/**
 * @file AcmeCertificateManager.h
 * @brief 证书自动化的一层运维壳：什么时候签、签完放哪儿、装回哪台服务
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Acme/AcmeError.h"
#include "Net/Acme/AcmeHttp01ChallengeStore.h"
#include "Net/Acme/AcmeKeyPair.h"

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

    /**
     * @brief 一次签发或续期之后的落点
     */
    struct AcmeIssuedCertificate
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
     */
    struct AcmeManagerStatus
    {
        std::size_t issuanceCount{0};                ///< 成功签发或续期的次数
        std::size_t failureCount{0};                 ///< 失败的轮次数（含被拒绝的配置）
        long long   certificateExpiryUnixSeconds{0}; ///< 当前磁盘上那张证书的到期时刻；0 表示还没有
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
     *       runRenewalLoop() 交回的协程帧要由调用方持有到循环停下（与 TcpServer 的清扫协程同一形状）
     * @warning 自证走 HTTP-01：必须让机构能从公网按 80 端口取到 `/.well-known/acme-challenge/` 下的
     *          令牌。registerChallengeRoutes() 要在**明文 80 端口那台服务**上调用（或让 80 重定向到
     *          本服务，Let's Encrypt 会跟随重定向），只挂在 443 上等于没答
     */
    class AcmeCertificateManager
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
         */
        AcmeCertificateManager(Core::EventLoop &loop, Configuration configuration, ReloadHandler reloadHandler = {});

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

        /// 叫停续期循环：下一轮检查点就会退出，正在进行的那一轮签发不打断（打断会留下半个订单状态）
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

        /// 记一次失败：计数加一、文案留下、日志说出来（三种渠道都写，免得只在一处可见）
        void recordFailure(std::string message) noexcept;

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
    };

    /**
     * @brief 读一份 PEM 证书链文件里叶证书的到期时刻
     * @param certificateFile 证书链路径（读第一张）
     * @return std::optional<std::chrono::system_clock::time_point> 到期时刻；文件读不出或
     *         不是合法 X509 时为空
     * @details 单独开放这一句是因为续期判据与运维读数该用同一个数：各处自己解析一遍 X509，
     *          迟早会出现「判据认为还有三十天、面板显示已过期」这种两个解释
     */
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> readCertificateExpiry(const std::filesystem::path &certificateFile);
} // namespace AsynGyanis::Net
