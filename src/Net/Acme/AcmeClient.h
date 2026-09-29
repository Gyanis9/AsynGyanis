/**
 * @file AcmeClient.h
 * @brief RFC 8555 的 ACME 客户端：目录、账户、下单、挑战、定稿与取证这一段协议状态机
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Net/Acme/AcmeError.h"
#include "Net/Acme/AcmeKeyPair.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 一段 ACME 应答：正文原文加上本通路还要读的两个响应头
     */
    struct ASYN_NET_API AcmeReply
    {
        std::string bodyText;      ///< 响应正文原文（证书那一步是 PEM 而不是 JSON，故正文一并留着）
        std::string locationUrl;   ///< Location 头，无则为空
        std::string replayNonce;   ///< Replay-Nonce 头，无则为空
        int         statusCode{0}; ///< HTTP 状态码
    };

    /**
     * @brief 一份 ACME 订单（RFC 8555 §7.4）
     */
    struct ASYN_NET_API AcmeOrder
    {
        std::string              orderUrl;          ///< 订单自身的 URL，轮询状态就读它
        std::string              finalizeUrl;       ///< 交 CSR 的 URL
        std::vector<std::string> authorizationUrls; ///< 每个域名一条自证授权
        std::string              status;            ///< pending / ready / processing / valid / invalid
        std::string              certificateUrl;    ///< 仅在状态为 valid 时有值
    };

    /**
     * @brief 自证挑战的种类（RFC 8555 §7.1.6 与 RFC 8738 §3）
     */
    enum class AcmeChallengeKind
    {
        Http01, ///< HTTP-01：把 keyAuthorization 原文发布到 `/.well-known/acme-challenge/<token>`
        Dns01,  ///< DNS-01：把 keyAuthorization 的摘要发布到 `_acme-challenge.<域名>` 的 TXT
    };

    /**
     * @brief 一条自证挑战（HTTP-01 与 DNS-01 共用同一对字段）
     */
    struct ASYN_NET_API AcmeChallenge
    {
        std::string challengeUrl; ///< 让机构开始校验时 POST 的 URL
        std::string token;        ///< HTTP-01 要发布的那段串；DNS-01 是参与算 TXT 值的那段串
    };

    /**
     * @brief 算 DNS-01 要发布到 `_acme-challenge.<域名>` 的 TXT 正文
     * @details 值是 `base64url(SHA-256(keyAuthorization))`（RFC 8738 §3，URL-safe 字母表且**不带填充**），
     *          与 HTTP-01 直接发布 keyAuthorization 原文只差这一层哈希；少一位或留了 `=` 填充，机构就判
     *          invalid，而它给的原文里往往不会说差在哪，所以这一处只留一份实现。
     * @param keyAuthorization `token` + `.` + 账户密钥指纹，即 HTTP-01 要发布的那整串
     * @return std::string 43 字符的 URL-safe 无填充 Base64 摘要文本
     */
    [[nodiscard]] ASYN_NET_API std::string dns01ValidationText(std::string_view keyAuthorization);

    /**
     * @brief 算 dns-01 要挂 TXT 记录的那个完整域名
     * @details 名字是 `_acme-challenge.<域名>`；通配符 `*.example.com` 要先去掉 `*.`，
     *          于是它与 `example.com` 落在同一条记录上（RFC 8738 §3）。一张证书覆盖多个域名时
     *          每个授权各一条，谁都不能替谁答。
     * @param domainName 授权里的 identifier 原文（可能是 `*.` 开头的通配符）
     * @return std::string 交给 DNS 提供方的记录名，形如 `_acme-challenge.gyanis.space`
     */
    [[nodiscard]] ASYN_NET_API std::string dns01RecordName(std::string_view domainName);

    /**
     * @brief 一个域名的授权记录，带着「本客户端按配置挑中的那一种」挑战
     * @details 解析时只挑 `AcmeClient::Configuration::challengeKind` 要的那一种，另一条留空——
     *          刻意不把两种都填上：调用方一旦拿到两条，就得自己决定发哪条给机构，而「发了一条、
     *          答的却是另一条」正是最难查的那种错。tls-alpn-01 仍然不做：它要在握手里交出带
     *          `acme-tls/1` 的自签证书，与本服务的 ALPN 装配是两套。
     */
    struct ASYN_NET_API AcmeAuthorization
    {
        std::string                  identifier; ///< 这条授权对应的域名
        std::string                  status;     ///< pending / valid / invalid / deactivated / expired / revoked
        std::optional<AcmeChallenge> http01;     ///< 配置要 HTTP-01 且机构给了才有值
        std::optional<AcmeChallenge> dns01;      ///< 配置要 DNS-01 且机构给了才有值
    };

    /**
     * @brief ACME 协议的客户端：把 RFC 8555 那一套请求与应答的形状和判定收在一处
     *
     * @details 与 AcmeCertificateManager 的分工：本类只管**协议**（一单怎么走完），不管证书落在
     *          哪个文件、什么时候该续、该通知谁重载——那些是运维的事。分成两层的代价是多一个类型，
     *          收益是协议侧可以拿一份进程内桩服务端逐条判据测，而不必为测协议去搭一台真 CA。
     *
     * @note 绑定在构造时给的那个事件循环上，只能在循环所属线程调用（出站请求与定时器都在那条线程上跑）
     * @note 账户密钥按引用持有：它必须活得比本对象久——签名发生在协程挂起之后，
     *       调用方交来一个临时对象会静默读到已释放的密钥材料
     * @warning 支持 HTTP-01 与 DNS-01 两种自证的**协议侧**：挑挑战、算校验值、按机构的答复轮询。
     *          「把答案放出去」不在本类职责内——HTTP-01 要靠已注册的路由，DNS-01 要靠 DNS 提供方；
     *          配置要的那一种机构没给时，本类在读授权时就判失败并列出机构实际给了哪些类型，
     *          不会挑一条自己答不了的挑战去 POST（tls-alpn-01 一律不支持）
     */
    class ASYN_NET_API AcmeClient
    {
    public:
        /**
         * @brief 客户端的配置
         */
        struct Configuration
        {
            /// 机构的目录 URL。生产一律是 https；http 只在测试的进程内桩上允许，本类不拦
            std::string directoryUrl;
            /// 联系邮箱，按 RFC 8555 §7.3 的写法带前缀："mailto:ops@example.com"。可空
            std::string contactEmailAddress;
            /// 是否已读过并接受目录里那份服务条款。**不为 true 时不建账户**：
            /// 接受条款是一个有法律含义的动作，不能由库替调用方默认
            bool isTermsOfServiceAccepted{false};
            /// 外部账户绑定（EAB，RFC 8555 §7.3.4）的标识；与密钥同时为空表示不用 EAB
            std::string externalAccountKeyId;
            /// 外部账户绑定的 HMAC 密钥（base64url 文本，机构发账户时给的那份）
            std::string externalAccountKeySecret;
            /// 单次出站请求的时限
            std::chrono::milliseconds requestTimeout{std::chrono::milliseconds{10000}};
            /// 要答哪一种自证挑战。默认 HTTP-01（与加这个字段之前的行为逐字相同）；选 DNS-01 时
            /// 调用方必须另外具备发布 TXT 记录的能力，本客户端只负责挑对挑战与算对 TXT 值
            AcmeChallengeKind challengeKind{AcmeChallengeKind::Http01};
            /// 撞上 badNonce 时的自动重试上限：nonce 由机构发、一次性，用坏了重取就行，
            /// 但无限重试会把一次网络抖动变成永久卡住
            std::size_t maximumNonceRetries{3};
        };

        /**
         * @brief 构造客户端
         * @param loop 承载出站请求的事件循环
         * @param configuration 见 Configuration：本构造不校验它，校验排在 prepareAccount() 里做，
         *        好让「配置不合法」与「网络失败」都从同一条 expected 通道交回
         * @param accountKey 账户密钥，按引用持有（存在期要求见类注释）
         */
        AcmeClient(Core::EventLoop &loop, Configuration configuration, const AcmeKeyPair &accountKey);

        AcmeClient(const AcmeClient &) = delete;

        AcmeClient &operator=(const AcmeClient &) = delete;

        /**
         * @brief 取目录并确保账户可用：这一步之后其余方法才允许调
         * @param persistedAccountUrl 上次记账的账户 URL；空表示本机第一次跑，要新建账户
         * @return Core::Task<std::expected<void, AcmeError>> 就绪与否。机构侧那个账户已被删除时，
         *         本函数按 RFC 的口径重新建一个账户并把新 URL 记在 accountUrl() 上，
         *         不要求人工去删状态文件
         */
        Core::Task<std::expected<void, AcmeError>> prepareAccount(std::optional<std::string> persistedAccountUrl);

        /**
         * @brief 当前生效的账户 URL，供调用方记账（下次启动原样传回 prepareAccount 即复用同一账户）
         * @return 还没建号或建号失败时为空视图
         */
        [[nodiscard]] std::string_view accountUrl() const noexcept;

        /**
         * @brief 目录里那份服务条款的 URL
         * @return 机构没给时为空视图（此时配置里的接受标志仍必须为 true：见 prepareAccount 的判据）
         */
        [[nodiscard]] std::string_view termsOfServiceUrl() const noexcept;

        /**
         * @brief 为这批域名下一张证书
         * @details 域名先按 RFC 8553 的字符集校验：不合法的名字送出去只会收到一条
         *          「rejectedIdentifier」的远端告状，而原因在本地一眼就能判。
         * @param domainNames 要覆盖的域名，至少一条
         * @return Core::Task<std::expected<AcmeOrder, AcmeError>> 带着授权与定稿 URL 的订单
         */
        Core::Task<std::expected<AcmeOrder, AcmeError>> createOrder(const std::vector<std::string> &domainNames);

        /**
         * @brief 读一份授权记录（POST-as-GET）
         * @param authorizationUrl 订单里的授权 URL
         * @return Core::Task<std::expected<AcmeAuthorization, AcmeError>> 授权与其中的 http-01 挑战
         */
        Core::Task<std::expected<AcmeAuthorization, AcmeError>> fetchAuthorization(const std::string &authorizationUrl);

        /**
         * @brief 叫机构去校验一条挑战，并轮询到它给出终局
         * @details 先 POST 空正文（这是「开始校验」的触发动作），再按 POST-as-GET 轮询同一个 URL。
         *          机构侧的校验是异步的，且要真去取 HTTP-01 的令牌——令牌没在应答时它会把挑战判成
         *          invalid，本函数据实交回 ChallengeNotAnswered 而不是含糊成「超时」。
         * @param challengeUrl 挑战的 URL
         * @param pollInterval 两次轮询之间的间隔
         * @param overallTimeout 总时限，超了交回 Transport 一档
         * @return Core::Task<std::expected<std::string, AcmeError>> 成功时交回终局状态（"valid"）
         */
        Core::Task<std::expected<std::string, AcmeError>> solveChallenge(const std::string &challengeUrl, std::chrono::milliseconds pollInterval,
                                                                         std::chrono::milliseconds overallTimeout);

        /**
         * @brief 交 CSR 定稿一张订单，并轮询到证书可取
         * @param order createOrder() 交回的那份订单
         * @param encodedRequest base64url 的 DER CSR（AcmeKeyPair::createCertificateSigningRequest 的产物）
         * @param pollInterval 两次轮询之间的间隔
         * @param overallTimeout 总时限
         * @return Core::Task<std::expected<AcmeOrder, AcmeError>> 状态为 valid 时带着 certificateUrl
         */
        Core::Task<std::expected<AcmeOrder, AcmeError>> finalizeOrder(const AcmeOrder &order, std::string_view encodedRequest, std::chrono::milliseconds pollInterval,
                                                                      std::chrono::milliseconds overallTimeout);

        /**
         * @brief 取回证书链（PEM 文本，首张是叶证书）
         * @param certificateUrl valid 订单里的 certificateUrl
         * @return Core::Task<std::expected<std::string, AcmeError>> PEM 原文
         * @note 按「机构给什么就存什么」交回，不重排也不摘根证书：链的顺序与内容直接影响对端能否
         *       补上最后一跳，重排一份凭据不是本类该做的判断
         */
        Core::Task<std::expected<std::string, AcmeError>> fetchCertificateChain(const std::string &certificateUrl);

    private:
        /**
         * @brief 带 JWS 签名发一次 POST，含 badNonce 的自动重试
         * @param url 目标 URL（同时写进 JWS 头的 "url"，两者不一致会被机构判死）
         * @param payloadJson 正文 JSON；传空视图表示 POST-as-GET（RFC 8555 §6.3）
         * @param isNewAccountRequest 是否账户注册那一次：只有它带 jwk，其余都带 kid
         * @return Core::Task<std::expected<AcmeReply, AcmeError>> **只要收到了应答就交回应答本身**，
         *         状态码是 4xx/5xx 也一样；只有「一条应答都没拿到」与「nonce 重试用尽」才落到失败值。
         *         故意不把非 2xx 折成失败：调用方里有需要按机构的问题类型分支的场合
         *         （账户已不存在就是一条 401，处置是重新建号而不是报错）
         */
        Core::Task<std::expected<AcmeReply, AcmeError>> postSignedRequest(const std::string &url, std::string_view payloadJson, bool isNewAccountRequest = false);

        /**
         * @brief 向机构的 newNonce 端点再取一个新鲜 nonce（HEAD，不带签名）
         */
        Core::Task<std::expected<void, AcmeError>> fetchFreshNonce();

        /**
         * @brief 攒一个可用的 nonce：池空时先去取一个
         */
        Core::Task<std::expected<std::string, AcmeError>> takeNonce();

        /**
         * @brief 按 RFC 8555 §6.7 把一段失败应答折成错误：状态码 + type + detail 拼出中文文案
         * @param reply 已收齐的应答
         * @param step 走到哪一步了（写进文案，便于运维定位是哪一次请求）
         * @return AcmeError 种类按限流与判死分开，机构原文附在文案里
         */
        [[nodiscard]] static AcmeError describeFailure(const AcmeReply &reply, std::string_view step);

        Core::EventLoop         &m_loop;                             ///< 承载出站请求的循环
        Configuration            m_configuration;                    ///< 本客户端的配置
        const AcmeKeyPair       &m_accountKey;                       ///< 账户密钥，按引用持有，存在期由调用方保证
        std::vector<std::string> m_noncePool;                        ///< 机构发过、还没用掉的 nonce，从尾部取
        std::string              m_accountUrl;                       ///< 当前账户 URL；空表示还没建号
        std::string              m_newNonceUrl;                      ///< 目录里的 newNonce
        std::string              m_newAccountUrl;                    ///< 目录里的 newAccount
        std::string              m_newOrderUrl;                      ///< 目录里的 newOrder
        std::string              m_termsOfServiceUrl;                ///< 目录 meta 里的服务条款 URL；机构没给则空
        bool                     m_isExternalAccountRequired{false}; ///< 目录 meta 是否声明必须要 EAB
    };
} // namespace AsynGyanis::Net
