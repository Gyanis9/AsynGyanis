/**
 * @file AcmeStubAuthority.h
 * @brief 进程内的 ACME 桩颁发机构：真验 JWS 签名、真管一次性 nonce、真回取自证令牌、真签证书
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AcmeTestSupport.h"
#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Net/Acme/AcmeKeyPair.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class HttpServer;
} // namespace AsynGyanis::Net

namespace AsynGyanis::Net::TestSupport
{
    /**
     * @brief ACME 协议用例的裁判替身
     *
     * @details 为什么要有它而不是直接测 Let's Encrypt：状态机的每一条出口（nonce 用坏、授权只给
     *          别的挑战类型、限流、账户已删、CSR 被拒）都得由故障注入才出得来，而真机构不会替我们
     *          制造这些现场，且它的速率限制会让一轮回归变成一小时的等待。真机构那条路另有按环境变量
     *          门控的用例覆盖。
     * @details 它同时是**独立实现**：签名用「按 JWK 重建公钥再验」这条与被测实现不同的路，
     *          指纹用通用 JSON 序列化的按键排序而不是本框架手写的成员顺序。两边都错在同一种理解上时，
     *          这里会不一致而不是一起绿。
     * @note 桩走明文 http：出站 TLS 已由 HttpClient 的既有用例覆盖，把两件事捆在一条判据上只会让
     *       失败原因更难归。地址固定用 127.0.0.1。
     * @note 只在所属事件循环的线程上调用与读证据；用例必须在循环停下之后再读 evidence()
     */
    class AcmeStubAuthority
    {
    public:
        /**
         * @brief 桩的行为开关：每一条都对应一条被测的判据
         */
        struct Settings
        {
            /// 自证令牌要去的地址（机构按域名解析到的那台机器）："127.0.0.1:8088" 这种形式
            std::string validationAuthority{"127.0.0.1"};
            /// 目录里是否给 meta.termsOfService，且注册时必须带 termsOfServiceAgreed
            bool isTermsOfServicePublished{true};
            /// 是否要求外部账户绑定（meta.externalAccountRequired）
            bool isExternalAccountRequired{false};
            /// 桩核对 EAB 用的账户标识
            std::string externalAccountKeyId;
            /// 桩核对 EAB 用的 HMAC 密钥（base64url 文本）
            std::string externalAccountKeySecret;
            /// 只给这一种挑战类型；写成 "dns-01" 既能测「机构不给 http-01」那条拒绝面，也能测 DNS-01 的整条通路
            std::string offeredChallengeType{"http-01"};
            /// 触发校验之后前 N 次轮询仍报 processing：测客户端真在轮询，而不是赌一次就 valid
            std::size_t challengeProcessingPolls{0};
            /// 前 N 次合法 nonce 也故意回一次 badNonce：测客户端重取 nonce 再发，而不是就此失败
            std::size_t badNonceFaults{0};
            /// 下单里出现这些域名就按 rejectedIdentifier 拒掉
            std::vector<std::string> rejectedDomainNames;
            /// 下一个 newOrder 直接 429 带 Retry-After：测限流这一档被折成 RateLimited
            bool isOrderRateLimited{false};
            /**
             * @brief 同一域名的下一张订单直接复用上一轮已 valid 的授权
             * @details Boulder 与 Pebble 都会复用（Pebble 默认按概率复用）。开这一档是为了钉住
             *          「客户端对着已 valid 的授权又去触发挑战」那条出口——真机构对此回 400，不是幂等
             */
            bool reusesValidAuthorizations{false};
            /// 签出的叶证书有效期天数
            long certificateValidDays{90};
        };

        /**
         * @brief 一轮事务结束后用例读的证据
         */
        struct Evidence
        {
            std::size_t verifiedSignatureCount{0};       ///< 验过的 JWS 条数
            std::size_t rejectedSignatureCount{0};       ///< 验不过而拒掉的 JWS 条数
            std::size_t jwkBearingRequestCount{0};       ///< 头部带 jwk 的请求条数：只有账户注册那一次该带
            std::size_t badNonceReplyCount{0};           ///< 桩回出 badNonce 的条数
            std::size_t missingUserAgentRequestCount{0}; ///< 缺 User-Agent 的请求条数：真机构（Boulder/Pebble）对这种请求直接 400
            std::size_t revalidatedChallengeCount{0};    ///< 对已 valid 的挑战再触发校验的条数：真机构对此回 400
            std::size_t challengeFetchCount{0};          ///< 取自证令牌的次数
            std::size_t dns01ValidationCount{0};         ///< 按 dns-01 口径核过一次 TXT 的次数
            std::size_t issuedOrderCount{0};             ///< 建出的订单条数
            std::size_t issuedCertificateCount{0};       ///< 签出的证书条数
            std::string lastChallengeFetchPath;          ///< 最后一次取令牌用的路径，用于断言 well-known 位置
            std::string lastFetchedBody;                 ///< 最后一次取令牌读到的正文
            std::string issuedCertificatePem;            ///< 最近签出的叶证书 PEM
            std::string firstAccountUrl;                 ///< 第一个建出来的账户 URL
            std::string registeredAccountContactText;    ///< 注册载荷里第一个 contact 的原文；空表示载荷没带联系人
        };

        /**
         * @brief 构造桩机构
         * @param loop 承载它的那条事件循环
         * @param settings 行为开关；要哪一档默认行为就逐字段写出来
         * @note 此处刻意不给默认实参：嵌套聚合体的默认成员初始化要到本类闭合后才就绪，
         *       默认实参里用到它时 GCC 直接拒绝编译（MSVC 却放行）
         */
        explicit AcmeStubAuthority(Core::EventLoop &loop, Settings settings);

        ~AcmeStubAuthority();

        AcmeStubAuthority(const AcmeStubAuthority &) = delete;

        AcmeStubAuthority &operator=(const AcmeStubAuthority &) = delete;

        /**
         * @brief 起监听并注册全部路由
         * @param port 端口；0 表示让系统挑一个空闲端口（并行用例不互踩）
         * @return true 已可服务
         */
        bool start(std::uint16_t port);

        /// 收掉监听
        void stop();

        /**
         * @brief 实际生效的端口（start(0) 之后用例要拿它拼验证地址）
         * @return std::uint16_t 端口号；未启动时为 0
         */
        [[nodiscard]] std::uint16_t port() const noexcept;

        /**
         * @brief 这个机构的目录 URL，直接喂给 AcmeClient::Configuration::directoryUrl
         * @return std::string 形如 http://127.0.0.1:PORT/directory；未启动时为空
         */
        [[nodiscard]] std::string directoryUrl() const;

        /**
         * @brief 证据快照
         * @return const Evidence & 只在循环停止后读（见类注释的线程约束）
         */
        [[nodiscard]] const Evidence &evidence() const noexcept;

        /**
         * @brief 告诉桩「去哪台机器取自证令牌」
         * @param authority 形如 127.0.0.1:8088 的地址
         * @details 端口要等接受协程把监听建起来才可见，而那时循环已经在跑，因此应答方的地址只能
         *          在循环上告诉桩。只在所属循环的线程上调用。
         */
        void setValidationAuthority(std::string authority);

        /**
         * @brief 交给桩一条「现在权威侧答得出的 TXT 是什么」的读法，dns-01 校验靠它
         * @param reader 返回当前已发布的那条 TXT 正文；没有就返回空
         * @details 桩不接真 DNS，所以由用例把被测方那副假 DNS 的记录口递进来。桩要核的是
         *          `base64url(SHA-256(令牌.指纹))`，其中**指纹用桩自己那份独立实现算**——
         *          被测实现若把令牌或指纹接错，这里就会判 invalid 而不是跟着一起错
         */
        void setPublishedTxtReader(std::function<std::optional<std::string>()> reader);

        /**
         * @brief 一个不存在账户的 URL，形状合法但桩没记过账
         * @details 测「本机记的账户已被机构删掉」那条回落：客户端要重新建号而不是报错
         * @return std::string 形如 http://127.0.0.1:PORT/acme/acct/999999
         */
        [[nodiscard]] std::string unknownAccountUrl() const;

    private:
        /// 一份已注册的账户
        struct Account
        {
            std::string url;        ///< 账户 URL，之后的 kid 就指它
            std::string jwkText;    ///< 注册时交来的公钥 JWK，验后续签名靠它重建公钥
            std::string thumbprint; ///< 该 JWK 的指纹，自证串的后半段
        };

        /// 一条自证挑战
        struct Challenge
        {
            std::string url;                         ///< 挑战 URL
            std::string token;                       ///< 令牌
            std::string status;                      ///< pending / processing / valid / invalid
            std::string detail;                      ///< 失败时给客户端看的原因
            std::size_t remainingProcessingPolls{0}; ///< 还要报几轮 processing 才真去校验
            bool        isValidated{false};          ///< 是否已经取过令牌
        };

        /// 一个域名的授权
        struct Authorization
        {
            std::string url;
            std::string identifier;
            std::string status;
            std::string challengeUrl;
        };

        /// 一张订单
        struct Order
        {
            std::string              url;
            std::string              accountUrl;
            std::string              status;
            std::string              finalizeUrl;
            std::string              certificateUrl;
            std::vector<std::string> authorizationUrls;
        };

        /// 一段解开的 JWS：字段按原样留着，签名要用**原文**重算，不能解码后再拼
        struct ParsedJws
        {
            std::string protectedText; ///< 解开的 protected 头 JSON
            std::string payloadText;   ///< 解开的正文（POST-as-GET 时为空）
            std::string signingInput;  ///< "protected.payload" 的 base64 原文拼接
            std::string signature;     ///< base64url 的签名段
            std::string algorithm;     ///< 头部 alg
            std::string nonce;         ///< 头部 nonce
            std::string url;           ///< 头部 url
            std::string jwkText;       ///< 头部 jwk（账户注册时给）
            std::string kid;           ///< 头部 kid（其余请求给）
        };

        /// 一次认证成功之后的结果，供各端点接着用
        struct AuthenticatedJws
        {
            Account     account;     ///< 签名所属的账户
            std::string payloadText; ///< 解开的正文
            std::string nonce;       ///< 本次用掉的 nonce
            ParsedJws   jws;         ///< 原始字段，EAB 那一步还要再看
        };

        Core::Task<void> handleDirectory(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleNewNonce(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleNewAccount(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleNewOrder(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleAccount(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleAuthorization(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleChallenge(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleOrder(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleFinalize(HttpRequest &request, HttpResponse &response);
        Core::Task<void> handleCertificate(HttpRequest &request, HttpResponse &response);

        /**
         * @brief 解出一段 JWS 并核签名、核 nonce、核 url
         * @param bodyText 请求正文原文
         * @param requestUrl 本次请求的 URL，与头里的 url 必须逐字相同
         * @param[out] failure 失败时要写出的响应
         * @return std::optional<AuthenticatedJws> 认证成功的结果；不成立时为空且 failure 已填好
         */
        std::optional<AuthenticatedJws> authenticate(std::string_view bodyText, const std::string &requestUrl, HttpResponse &failure);

        /**
         * @brief 真去取一次 HTTP-01 的自证令牌并按结果定挑战状态
         * @details 期望正文是 "令牌.账户公钥指纹"，指纹按**桩自己**那份独立规范化算出：
         *          被测实现手写的成员顺序若与之不同，这里就会判 invalid 而不是跟着一起错
         */
        Core::Task<void> validateHttp01(Challenge &challenge, const Account &account);

        /**
         * @brief 按 dns-01 的口径核一次已发布的 TXT，并按结果定挑战状态
         * @details 不去取 HTTP 令牌：这条通路上「答案在 DNS 里」，而桩读的是用例给的那个记录口
         */
        Core::Task<void> validateDns01(Challenge &challenge, const Account &account);

        /// 发一条 RFC 8555 §6.7 的问题文档
        void writeProblem(HttpResponse &response, int statusCode, std::string_view problemType, std::string_view detail);

        /// 发一条带 JSON 正文的成功应答，并顺带发一个新 nonce
        void writeJson(HttpResponse &response, int statusCode, const Base::ConfigValue &body, std::string_view location = {});

        /// 造一个新 nonce 并记进待用集合
        [[nodiscard]] std::string issueNonce();

        /// 把一份 JWK 文本按桩的口径规范化（只留必需成员、按 JSON 序列化的键序）
        [[nodiscard]] static std::optional<std::string> canonicalJsonWebKeyText(std::string_view jwkText);

        /// 订单的 JSON 表示
        [[nodiscard]] Base::ConfigValue orderBody(const Order &order) const;

        Core::EventLoop                               &m_loop;
        Settings                                       m_settings;                    ///< 行为开关
        std::unique_ptr<HttpServer>                    m_server;                      ///< 承载这些端点的明文 HTTP 服务（端口由它现报，见 port()）
        std::optional<Core::Task<void>>                m_startTask;                   ///< 接受循环的协程帧：start() 只把它交出来，得有人持有到循环停下
        std::string                                    m_validationAuthority;         ///< 取自证令牌要去的那台机器（由用例在循环上填）
        std::function<std::optional<std::string>()>    m_publishedTxtReader{};        ///< dns-01 时读「已发布的 TXT」的那只口
        TestCertificateAuthority                       m_authority;                   ///< 签证书用的测试 CA
        std::vector<Account>                           m_accounts;                    ///< 已注册账户，URL 是键
        std::unordered_map<std::string, Authorization> m_authorizations;              ///< 授权 URL → 记录
        std::unordered_map<std::string, Challenge>     m_challenges;                  ///< 挑战 URL → 记录
        std::unordered_map<std::string, Order>         m_orders;                      ///< 订单 URL → 记录
        std::unordered_map<std::string, std::string>   m_validAuthorizationsByDomain; ///< 域名 → 已 valid 的授权 URL，复用档按它找回
        std::unordered_map<std::string, std::string>   m_certificates;                ///< 证书 URL → PEM
        std::set<std::string>                          m_outstandingNonces;           ///< 发过、还没用掉的 nonce
        std::size_t                                    m_sequence{0};                 ///< 各类资源编号的来源
        long                                           m_certificateSerial{1000};     ///< 每次签发换一个序列号，用例靠它区分新旧证书
        Evidence                                       m_evidence;                    ///< 累计证据
    };
} // namespace AsynGyanis::Net::TestSupport
