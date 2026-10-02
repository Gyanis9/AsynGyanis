/**
 * @file AcmeCertificateManager.h
 * @brief 证书自动化的一层运维壳：什么时候签、签完放哪儿、装回哪台服务
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Acme/AcmeDns01TxtWriter.h"
#include "Net/Acme/AcmeError.h"
#include "Net/Acme/AcmeHttp01ChallengeStore.h"
#include "Net/Acme/AcmeKeyPair.h"

#include <array>
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class Router;
    class AcmeClient;
    struct AcmeAuthorization;

    /**
     * @brief 一次签发或续期之后的落点
     */
    struct ASYN_NET_API AcmeIssuedCertificate
    {
        std::filesystem::path                 certificateFile;  ///< 证书链路径（PEM，首张是叶证书）
        std::filesystem::path                 privateKeyFile;   ///< 域名私钥路径（PEM，0600）
        std::chrono::system_clock::time_point expiry{};         ///< 叶证书的到期时刻
        bool                                  wasIssued{false}; ///< 假表示沿用磁盘上已有的一张，这一轮没去问机构
    };

    /**
     * @brief 证书自动化的可读状态，供健康端点与运维读数
     * @note 各计数是原子量、lastFailureMessage 走加锁快照：跨线程读安全，但「读到的一对数」
     *       之间不保证原子关系（要看严格配对就订阅签发完成点）
     * @note 这三条计数同时挂在 `Core::ProcessMetricsRegistry` 上，抓 `/metrics` 就能看见
     *       （名字是 `asyn_acme_*`，进程级，不随监听器前缀变）；本结构是给应用自己读的，
     *       两条通道报的是同一份原子量，不会各算一遍
     */
    struct ASYN_NET_API AcmeManagerStatus
    {
        std::size_t issuanceCount{0};                ///< 成功签发或续期的次数
        std::size_t failureCount{0};                 ///< 失败的轮次数（含被拒绝的配置）
        long long   certificateExpiryUnixSeconds{0}; ///< 磁盘上那张证书的到期时刻（构造即按文件填）；0 表示读不出
        long long   backoffUntilUnixSeconds{0};      ///< 失败退避门槛：早于这个时刻不会再试；0 表示没在退避中
        std::string lastFailureMessage;              ///< 最近一次失败的中文文案；没有失败时为空
    };

    /**
     * @brief 把 ACME 协议层接成一项常驻运维：按到期时间签发、原子落盘、叫服务重装
     *
     * @details 与 AcmeClient 的分工是有意的：协议状态机不该知道「证书放在哪个路径」「这台服务的
     *          监听器怎么重装身份」，而那些事也不该散落在每个应用的 main 里——一处写歪就会出现
     *          「磁盘上是新证书、内存里还是旧的」这种最难查的现场。
     *
     * @details 落盘一律走原子替换（写临时文件再 rename），且**先写私钥再写证书链**：中途崩了
     *          最坏情况是旧证书配新私钥，下一次启动加载配对校验就会失败并说清原因，
     *          而不是留下一个能加载却身份不明的组合。
     *
     * @note 绑定在构造时给的事件循环上：签发流程里的每一次出站请求与每一段等待都在那条线程上跑。
     *       runRenewalLoop() 交回的协程帧要由调用方持有到循环停下（与 TcpServer 的清扫协程同一形状），
     *       停下这件事由 stopRenewalLoop() 发起——停放被切成 1 秒一片，所以「等到帧退出再拆对象」
     *       是有界的（约一片 + 调度），不会等满一整拍（默认可达 12 小时）
     * @warning 自证走哪一条由「有没有交来 DNS-01 的 TXT 写入动作」决定，两条的前提不同：
     *          HTTP-01（默认）要机构能从公网按 80 端口取到 `/.well-known/acme-challenge/` 下的令牌，
     *          registerChallengeRoutes() 必须在**明文 80 端口那台服务**上调用（或让 80 重定向到本服务，
     *          Let's Encrypt 会跟随重定向），只挂在 443 上等于没答；
     *          DNS-01 不需要任何入站通路，但要求那对凭据真的能改域名记录，且这台机器能出公网 443。
     *          被备案拦截、80 端口拿不到的部署（国内云上常见）只能走后者
     */
    class ASYN_NET_API AcmeCertificateManager
    {
    public:
        /**
         * @brief 续期完成后要通知的对象：把新证书装回正在服务的那台服务器
         * @details 交回失败值表示「磁盘上是新证书，但服务还在用旧的」。这一档会被记成
         *          ReloadRejected 而不是被忽略——证书自动化的意义就是让线上身份也跟着换新
         */
        using ReloadHandler = std::function<std::expected<void, std::string>()>;

        /**
         * @brief 运维层的配置
         */
        struct Configuration
        {
            /// 要覆盖的域名，至少一条；多张证书不在本期（每张证书要一条独立的自证，路径也得各一份）
            std::vector<std::string> domainNames;
            /// 证书链落点，同时是服务加载证书的那条路径（TlsContext 的 reloadCertificate 按原路径重读）
            std::filesystem::path certificateFile;
            /// 域名私钥落点
            std::filesystem::path privateKeyFile;
            /// 账户私钥落点；不存在时新生成并写在这里（0600）
            std::filesystem::path accountKeyFile;
            /// 记账户 URL 的小文件；沿用同一账户靠它，不存在表示第一次跑
            std::filesystem::path accountStateFile;

            /// 机构的目录 URL
            std::string directoryUrl;
            /// 联系邮箱，写法 "mailto:ops@example.com"
            std::string contactEmailAddress;
            /// 服务条款的显式接受（建账户时要用），见 AcmeClient::Configuration 同一判据
            bool isTermsOfServiceAccepted{false};
            /// 外部账户绑定的标识与 HMAC 密钥（两条同时给才启用）
            std::string externalAccountKeyId;
            std::string externalAccountKeySecret;

            /// 账户密钥的算法：默认 RS256，理由见 AcmeKeyAlgorithm 的 @warning
            AcmeKeyAlgorithm accountKeyAlgorithm{AcmeKeyAlgorithm::Rs256};
            /// 域名密钥的算法（进 CSR 的那把）
            AcmeKeyAlgorithm domainKeyAlgorithm{AcmeKeyAlgorithm::Rs256};

            /// 每隔多久查一次到期时间
            std::chrono::milliseconds renewalCheckInterval{std::chrono::hours{12}};
            /// 到期前多久就该续：Let's Encrypt 的证书是 90 天，社区惯例是三分之一寿命即 30 天
            std::chrono::milliseconds renewBeforeExpiry{std::chrono::hours{24 * 30}};
            /// 一轮签发的总时限（含机构侧的自证等待）
            std::chrono::milliseconds issuanceTimeout{std::chrono::seconds{180}};
            /// 自证状态的轮询间隔
            std::chrono::milliseconds challengePollInterval{std::chrono::milliseconds{500}};
            /// 失败后的最小重试间隔：机构侧有速率限制，查得再勤也不该每小时都去撞一次
            std::chrono::milliseconds minimumRetryInterval{std::chrono::hours{1}};
        };

        /**
         * @brief 构造管理器
         * @param loop 承载签发流程与定时器的那个事件循环
         * @param configuration 见 Configuration：本构造不校验，校验排在每次签发入口里，
         *        好让配置错误与其他失败从同一条 expected 通道交回
         * @param reloadHandler 装回服务的动作；可先不设（首签时服务往往还没起来，拿到路径再构造），
         *        但**不设就跑续期循环会被拒绝**：那只等于把证书下到磁盘却没人装
         * @param dns01TxtWriter 发布与撤回 TXT 的动作对。给了它就走 DNS-01（机构侧挑 dns-01 挑战，
         *        且只在这条通道上应答），不给就走 HTTP-01；两档之间没有回落——一次签发中途换通道
         *        只会让机构拿着另一条挑战的答案判 invalid
         */
        AcmeCertificateManager(Core::EventLoop &loop, Configuration configuration, ReloadHandler reloadHandler = {}, AcmeDns01TxtWriter dns01TxtWriter = {});

        AcmeCertificateManager(const AcmeCertificateManager &) = delete;

        AcmeCertificateManager &operator=(const AcmeCertificateManager &) = delete;

        /**
         * @brief 需要就签一张，不需要就把磁盘上那张的信息交回去
         * @details 判据是「磁盘上那张证书的到期时刻减去现在，是否还够 renewBeforeExpiry」；
         *          证书文件读不出、解析不动或压根不存在都按「该签」处理——覆盖一份坏文件是恢复动作，
         *          不是需要人批准的事故。
         * @return Core::Task<std::expected<AcmeIssuedCertificate, AcmeError>> 落点与到期时刻；
         *         失败时磁盘上的旧证书原样不动（新证书没写成功就报错，不会写半份）
         */
        Core::Task<std::expected<AcmeIssuedCertificate, AcmeError>> issueIfRequired();

        /**
         * @brief 常驻续期循环：按 renewalCheckInterval 查到期、必要时签发，失败按最小间隔退避
         * @details 交给调用方投进循环并把帧持有到退出（与 TcpServer::idleSweepLoop 同一形状）。
         *          没有装回服务的动作就直接收口并把原因记进 status()：静默不干活是这类常驻协程
         *          最坏的失败模式——磁盘上的证书每月都在换，线上身份却永远是那张旧的。
         * @return Core::Task<void> 循环协程；stopRenewalLoop() 或被拒绝的启动都会让它退出
         */
        Core::Task<void> runRenewalLoop();

        /**
         * @brief 叫停续期循环：正在停放的那一片睡满就退出（不超过约 1 秒），正在进行的那一轮签发不打断
         * @details 不打断是有意的——半途掐掉一次下单会留下半个订单状态。本方法只落一个原子标志，
         *          因此任何线程上调都安全；它刻意不「叫醒」那条帧，跨线程取消定时器违反循环的线程契约。
         *          调用方等到帧退出再销毁本对象即可，等待是有界的
         */
        void stopRenewalLoop() noexcept;

        /**
         * @brief 把 http-01 的自证路由注册到给定路由器
         * @param router 明文 80 端口那台服务的路由表（见类注释的 @warning），须在 start() 之前
         */
        void registerChallengeRoutes(Router &router);

        /**
         * @brief 当前的运维读数
         * @return AcmeManagerStatus 跨线程可读（见该结构的 @note）
         */
        [[nodiscard]] AcmeManagerStatus status() const;

        /// 自证令牌的暂存处（观测与用例读它，写入由签发流程负责）
        [[nodiscard]] const AcmeHttp01ChallengeStore &challengeStore() const noexcept;

    private:
        /**
         * @brief 读磁盘上那张证书的到期时刻
         * @return std::optional 文件不存在、读不出或不是合法 X509 时为空
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> readExpiryFromDisk() const;

        /**
         * @brief 把账户 URL 记到状态文件里（供下一次启动复用同一账户）
         * @return std::optional<AcmeError> 失败时带上原因；成功时为空
         */
        [[nodiscard]] std::optional<AcmeError> persistAccountState(const std::string &accountUrl) const;

        /**
         * @brief 读回上次记下的账户 URL
         * @return std::optional<std::string> 文件不存在或内容不合形时为空（按「第一次跑」处理）
         */
        [[nodiscard]] std::optional<std::string> readPersistedAccountUrl() const;

        /**
         * @brief 让账户密钥就位：文件里有就加载，没有就新生成并落盘
         * @return std::expected<AcmeKeyPair *, AcmeError> 指向本对象持有的那把密钥；文件读不出、
         *         不是合法私钥或落盘失败都交回原因（expected 的值类型不能是引用，故给指针）
         * @details 排在签发流程里而不是构造函数里：构造函数没有错误通道，而这三种失败都是
         *          「运维得改点东西」的现场，必须带着中文文案从 expected 交回
         */
        [[nodiscard]] std::expected<AcmeKeyPair *, AcmeError> ensureAccountKey();

        /**
         * @brief 走完一条 dns-01 授权：写 TXT → 叫机构校验 → 撤 TXT
         * @details 撤这一步排在「无论前面成没成」的那条出口上，而不是每条提前返回里手写一次：
         *          漏掉一条就留下一条永久有效的 TXT，下一轮签发的机构在同一名字上看到两条不同答案，
         *          它的答复只会是 DNS 校验失败，而真正的原因在上一次
         * @param client 协议层客户端（已按 DNS-01 配置）
         * @param authorization 本条授权，其 dns01 挑战必须已经填好
         * @return Core::Task<std::optional<AcmeError>> 成功时为空；失败时是第一条要报的失败
         */
        Core::Task<std::optional<AcmeError>> authorizeDns01(AcmeClient &client, const AcmeAuthorization &authorization);

        /// 记一次失败：计数加一、文案留下、日志说出来（三种渠道都写，免得只在一处可见），并按本地的
        /// 最小重试间隔与机构给的 `Retry-After` 设退避门槛（见 remoteOrLocalDelaySeconds）
        void recordFailure(std::string message, std::optional<std::chrono::seconds> remoteRetryAfter = {}) noexcept;

        /**
         * @brief 这一次失败该退避多久（秒）
         * @details 本地的 `minimumRetryInterval` 与机构的 `Retry-After` 取较大者：前者是我们对速率
         *          限制的估计，后者是机构明说的答复，两者都不该被对方覆盖。机构的说法另外夹在 24 小时
         *          之内——胡乱答复的邻居不该能把续期推到证书过期。
         * @param remoteRetryAfter 机构在 429/503 上给的等待时长，没有则为空
         * @return long long 至少 1 秒
         */
        [[nodiscard]] long long remoteOrLocalDelaySeconds(std::optional<std::chrono::seconds> remoteRetryAfter) const noexcept;

        /**
         * @brief 失败退避的门槛时刻：早于它不再试；0 表示没在退避中
         * @details 这条算式只留一份：常驻循环用它决定下一轮什么时候醒，`status()` 用同一份回答运维
         *          「还会不会再试」。两处各写一遍就会出现「循环其实还会来，面板说没在退避」这类拆脸
         */
        [[nodiscard]] long long backoffGateUnixSeconds() const noexcept;

        /**
         * @brief 把下层交回的失败记下来再原样交回
         * @param error 协议层或密钥层交回的失败
         * @return AcmeError 同一条失败
         * @details 少了这一步，协议层的失败就只出现在 expected 里：常驻循环没人接的时候等于静默
         */
        [[nodiscard]] AcmeError notedFailure(AcmeError error);

        /**
         * @brief 记下失败并把它折成 expected 的失败值
         * @param kind 失败种类
         * @param message 中文可操作文案（同时进日志与 status()）
         * @return AcmeError 原样的那条失败，供调用方 std::unexpected(...) 用
         */
        [[nodiscard]] AcmeError failWith(AcmeErrorKind kind, std::string message);

        Core::EventLoop           &m_loop;               ///< 承载签发流程与定时器的循环
        Configuration              m_configuration;      ///< 运维层配置
        ReloadHandler              m_reloadHandler;      ///< 装回服务的动作，可空（空则拒绝跑续期循环）
        std::optional<AcmeKeyPair> m_accountKey;         ///< 账户密钥：首次用到时才加载或生成（见 ensureAccountKey）
        mutable std::mutex         m_lastFailureMutex;   ///< 保护下面那条文案
        std::string                m_lastFailureMessage; ///< 最近一次失败的中文文案

        std::atomic<std::size_t> m_issuanceCount{0};            ///< 成功签发或续期的次数
        std::atomic<std::size_t> m_failureCount{0};             ///< 失败轮次数
        std::atomic<long long>   m_expiryUnixSeconds{0};        ///< 磁盘上那张证书的到期时刻；0 表示还没有
        std::atomic<bool>        m_isStopping{false};           ///< 续期循环的停位
        std::atomic<long long>   m_notBeforeNextAttemptUnix{0}; ///< 失败退避：早于这个时刻不再尝试

        AcmeHttp01ChallengeStore m_challengeStore; ///< 自证令牌的暂存处
        AcmeDns01TxtWriter       m_dns01TxtWriter; ///< TXT 的发布与撤回动作；两格都空就按 HTTP-01 走

        /**
         * @brief 挂在进程级指标注册表上的把手：到期时刻、签发次数、失败次数
         * @details 构造时就登记，而不是等第一次签发：常驻进程里这三条**长期为 0** 本身就是最要紧的
         *          信号——自动化没跑成与自动化还没跑，从外面看得是同一个形状
         */
        std::array<Core::ProcessMetricHandle, 3> m_metricHandles{};
    };

    /**
     * @brief 读一份 PEM 证书链文件里叶证书的到期时刻
     * @param certificateFile 证书链路径（读第一张）
     * @return std::optional<std::chrono::system_clock::time_point> 到期时刻；文件读不出或
     *         不是合法 X509 时为空
     * @details 单独开放这一句是因为续期判据与运维读数该用同一个数：各处自己解析一遍 X509，
     *          迟早会出现「判据认为还有三十天、面板显示已过期」这种两个解释
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::chrono::system_clock::time_point> readCertificateExpiry(const std::filesystem::path &certificateFile);

    /**
     * @brief 跟着磁盘上那张证书走的常驻协程：身份变了就叫本进程重装一次
     * @details 存在的理由是「一张单签完、多个进程都要用」那一档部署。签发只需要一个进程去做（机构的
     *          速率限制按账户计，不是按进程），可 TLS 身份是每个进程各自握着的 SSL_CTX，而签发进程
     *          写完那两张文件之后没有任何通知别人的通道——**原子替换本身就是通知**：本协程按
     *          `pollInterval` 用 `Platform::queryFileBasicInfo` 读回 (字节数, 修改秒, 身份标记) 三元组，
     *          任一与上次不同就调用 `reloadHandler` 一次。三元组与静态文件缓存用的是同一处口径，
     *          不在这里另造一份「怎么算换了」。
     *
     * @details 装回失败时**不把新身份认下**，下一拍再试：线上还在用旧证书，放过一次就是永远放过。
     *          同一次变化只报一条 ERROR（重试静默），否则 15 秒一次的节拍会把日志刷成一堵墙。
     *
     * @param loop 承载本协程定时器的事件循环（循环对象只在它所属的那条线程上碰）
     * @param certificateFile 证书链路径，必须与签发方的落点同一条
     * @param privateKeyFile 私钥路径，同上
     * @param pollInterval 查一次的间隔；停放按 1 秒切片，叫停因此是有界的
     * @param reloadHandler 装回动作，与 `AcmeCertificateManager` 用的是同一个形状；为空时本协程当场退出
     *        并留一条 ERROR（跑一条永远什么都做不了的协程比不跑更糟）
     * @param isStopping 叫停标志：置真后本协程最多再等一片就退出
     * @return Core::Task<void> 协程帧，调用方投进循环并持有到退出（与 `runRenewalLoop()` 同一形状）
     */
    ASYN_NET_API Core::Task<void> followCertificateRotation(Core::EventLoop &loop, std::filesystem::path certificateFile, std::filesystem::path privateKeyFile,
                                                            std::chrono::milliseconds pollInterval, AcmeCertificateManager::ReloadHandler reloadHandler,
                                                            const std::atomic<bool> &isStopping);
} // namespace AsynGyanis::Net
