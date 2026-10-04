// ACME 证书管理器用例：首签落盘、够用时不打扰机构、到期前换新并装回服务、失败不留半张证书。
// 对端仍是那份进程内的桩机构。EventLoop::run() 只能回一次，因此「先签一张、再判要不要续」这类
// 场景在同一趟驱动里按轮跑完，每轮换一个新管理器，等价于进程重启之后接着看磁盘上留下的状态。
#include "AcmeStubAuthority.h"
#include "AcmeTestSupport.h"
#include "CommonTestSupport.h"
#include "CoreTestSupport.h"
#include "MetricsTestSupport.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Net/Acme/AcmeCertificateManager.h"
#include "Net/Acme/AcmeClient.h"
#include "Net/Acme/AcmeDns01TxtWriter.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileContents.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 桩机构住在测试支持那一层，用例里按短名引用
        using AcmeStubAuthority = TestSupport::AcmeStubAuthority;

        /// 桩与本机之间一切等待的本轮上限：桩是毫秒级答完的，这里只兜住「协议卡住」
        constexpr std::chrono::milliseconds kIssuanceTimeout{8000};

        /// 自证轮询的节拍
        constexpr std::chrono::milliseconds kPollInterval{10};

        /// 等端口发布的预算
        constexpr std::chrono::milliseconds kPortBudget{2000};

        /// 默认覆盖的域名
        const std::vector<std::string> kDomainNames{"auto-first.example.com", "auto-second.example.com"};

        /// 装回服务的动作里那条固定失败文案
        constexpr std::string_view kInstallFailureText = "监听器拒绝了这份新证书";

        /// 假 DNS 那两格里失败时要交回的固定文案
        constexpr std::string_view kDnsPublishFailureText  = "假 DNS 拒绝写入这条 TXT";
        constexpr std::string_view kDnsWithdrawFailureText = "假 DNS 拒绝撤回这条 TXT";
    } // namespace

    /**
     * @brief 夹具：桩机构、挂着自证路由的明文服务与管理器同挂一条循环，由测试线程驱动
     * @details 内核分配的端口要等接受协程跑起来才可见，而管理器要知道桩的目录 URL 才能构造，
     *          因此「等端口 → 建管理器 → 挂自证路由 → 签发」都排在同一趟循环里做。
     *          路由注册落在 start() 之后是本夹具的刻意安排：那之前不会有任何请求进来。
     */
    class AcmeCertificateManagerTest : public ::testing::Test
    {
    public:
        /**
         * @brief 一轮的一副样子：阈值、域名与装回动作都可以逐轮不同
         */
        struct Round
        {
            std::chrono::milliseconds renewThreshold{std::chrono::hours{24 * 20}}; ///< 这一轮的到期阈值
            bool                      sendsEmptyDomainList{false};                 ///< 域名列表交空，测本地判据排在前面
            bool                      withInstallStep{true};                       ///< 要不要给「装回服务」的动作
            bool                      installStepFails{false};                     ///< 有动作，但让它失败
            bool                      retriesReloadAfterFailure{false};            ///< 失败后把装回翻回成功，再走一次入口（同一个管理器）
            bool                      probesTokenAfterwards{false};                ///< 之后再回取一次令牌，看撤没撤
            bool                      usesFreshPaths{false};                       ///< 换一批落点：同一台机器上的另一张证书
            bool                      runsRenewalLoop{false};                      ///< 这一轮跑常驻循环而不是单次签发
            bool                      usesDns01{false};                            ///< 这一轮走 DNS-01：交一副记着发布与撤回的假 DNS
            bool                      dnsPublishFails{false};                      ///< 让写入那一步失败，看撤有没有照跑
            bool                      dnsWithdrawFails{false};                     ///< 让撤回那一步失败
            bool                      dnsPublishesWrongValue{false};               ///< 写入成功但正文算错，机构会把挑战判 invalid
            std::chrono::milliseconds minimumRetryInterval{std::chrono::hours{1}}; ///< 失败后的最小重试间隔；默认与管理器一致
        };

        /**
         * @brief 一轮的产出
         */
        struct Run
        {
            std::optional<std::expected<AcmeIssuedCertificate, AcmeError>> result{};                         ///< 签发出口；空表示这一轮没跑到
            std::optional<std::expected<AcmeIssuedCertificate, AcmeError>> retryResult{};                    ///< 装回翻回成功后再走一次入口的出口
            bool                                                           pendingReloadAfterFailure{false}; ///< 第一次失败之后，管理器记不记得这笔欠账
            bool                                                           pendingReloadAfterRetry{false};   ///< 补装成功之后，这笔欠账结没结清
            std::size_t                                                    ordersAfterFailure{0};            ///< 第一次失败时机构建出的订单条数
            std::size_t                                                    ordersAfterRetry{0};              ///< 补装之后机构建出的订单条数
            std::size_t                                                    installCalls{0};                  ///< 这一轮里装回动作被调了几次
            std::optional<int>                                             tokenProbeStatusCode{};           ///< 事后回取令牌的状态码
            std::string                                                    tokenProbeBody;                   ///< 事后回取到的正文
            std::size_t                                                    dnsPublishCalls{0};               ///< 这一轮里 TXT 被写入几次
            std::size_t                                                    dnsWithdrawCalls{0};              ///< 这一轮里 TXT 被撤回几次
            std::vector<std::string>                                       dnsPublishedNames;                ///< 每次写入用的记录名，按调用顺序
            std::vector<std::string>                                       dnsPublishedValues;               ///< 每次写入的正文，按调用顺序
        };

    protected:
        void SetUp() override
        {
            m_loop  = std::make_unique<Core::EventLoop>();
            m_paths = std::make_unique<AsynGyanis::TestSupport::TemporaryDirectory>("AcmeCertificateManager");
            // 落点绑在这轮的临时目录上：成员初始化跑在 SetUp 之前，那时目录还不存在
            m_certificatePath  = m_paths->path() / "certificate.pem";
            m_privateKeyPath   = m_paths->path() / "domain-key.pem";
            m_accountKeyPath   = m_paths->path() / "account-key.pem";
            m_accountStatePath = m_paths->path() / "account-state.json";
        }

        void TearDown() override
        {
            if (m_challengeServer)
            {
                m_challengeServer->stop();
            }
            if (m_authority)
            {
                m_authority->stop();
            }
        }

        /**
         * @brief 摆出桩机构与那台承载自证路由的明文服务，并把接受协程投进循环
         */
        void startServers(const AcmeStubAuthority::Settings &settings)
        {
            m_challengeServer = std::make_unique<HttpServer>(*m_loop, Core::InetAddress::localhost(0));
            m_challengeTask   = m_challengeServer->start();
            m_loop->scheduler().schedule(m_challengeTask->handle());

            m_authority = std::make_unique<AcmeStubAuthority>(*m_loop, settings);
            // 桩自己的 start() 已经把接受协程投进循环，并把帧持在自己的成员里
            ASSERT_TRUE(m_authority->start(0));
        }

        /**
         * @brief 一趟循环里按序跑完若干轮
         */
        std::vector<Run> driveRounds(std::vector<Round> rounds)
        {
            m_rounds = std::move(rounds);
            m_runs.clear();
            m_installCalls.store(0, std::memory_order_relaxed);

            m_task = driveAllRounds();
            m_loop->scheduler().schedule(m_task->handle());
            m_loop->run();
            m_task.reset();
            m_rounds.clear();
            return std::move(m_runs);
        }

        /// 单轮的简写。默认实参用不上 Round 的默认成员初始化（GCC 要到本类闭合才认），故要调用方自己写出来
        Run driveIssue(const Round &round)
        {
            auto runs = driveRounds({round});
            return runs.empty() ? Run{} : std::move(runs.front());
        }

        /// 最近一轮的管理器：驱动跑完才存在（构造要在循环上，那时端口才可见）
        [[nodiscard]] AcmeCertificateManager &manager() const
        {
            return *m_manager;
        }

        [[nodiscard]] const std::filesystem::path &certificatePath() const noexcept
        {
            return m_certificatePath;
        }

        [[nodiscard]] const std::filesystem::path &privateKeyPath() const noexcept
        {
            return m_privateKeyPath;
        }

        [[nodiscard]] const std::filesystem::path &accountKeyPath() const noexcept
        {
            return m_accountKeyPath;
        }

        [[nodiscard]] const std::filesystem::path &accountStatePath() const noexcept
        {
            return m_accountStatePath;
        }

        [[nodiscard]] AcmeStubAuthority::Evidence stubEvidence() const
        {
            return m_authority->evidence();
        }

        [[nodiscard]] std::string readFileOf(const std::filesystem::path &path) const
        {
            const auto contents = Platform::readFileContents(path, 0U, static_cast<std::size_t>(std::filesystem::file_size(path)));
            return contents.has_value() ? *contents : std::string{};
        }

    private:
        /**
         * @brief 等两台服务都把内核分配的端口发布出来
         */
        Core::Task<bool> awaitPorts()
        {
            Core::Timer timer(*m_loop);
            const auto  deadline = std::chrono::steady_clock::now() + kPortBudget;
            while (m_challengeServer->listeningPort() == 0 || m_authority->port() == 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    co_return false;
                }
                co_await timer.waitFor(std::chrono::milliseconds{1});
            }
            co_return true;
        }

        /// 装回服务的动作：成不成功按本轮参数决定（存不存在由调用点决定）。计数要被它改，故非常量成员函数
        [[nodiscard]] AcmeCertificateManager::ReloadHandler makeInstallStep()
        {
            return [this]() -> std::expected<void, std::string>
            {
                m_installCalls.fetch_add(1, std::memory_order_relaxed);
                if (m_installStepFails)
                {
                    return std::unexpected(std::string(kInstallFailureText));
                }
                return {};
            };
        }

        /**
         * @brief 一副记着发布与撤回的假 DNS：dns-01 通路用它替掉真提供方
         * @details 「权威侧现在答得出什么」就存在夹具那一格里，桩机构取答案时读它。
         *          两格都按夹具上的开关决定成败，于是「写失败之后的撤」「自证失败之后的撤」
         *          这些出口都出得来 —— 它们正是手写 withdraw 最容易漏的那几条
         */
        [[nodiscard]] AcmeDns01TxtWriter makeFakeDnsWriter()
        {
            AcmeDns01TxtWriter writer;
            // 协程形参按值取：管理器持有的这个 std::function 会被复制进签发协程的帧里
            writer.publish = [this](std::string fqdn, std::string value) -> Core::Task<std::expected<void, std::string>>
            {
                ++m_dnsPublishCalls;
                m_dnsPublishedNames.push_back(fqdn);
                if (m_dnsPublishesWrongValue)
                {
                    value = "这一条正文是错的";
                }
                m_dnsPublishedValues.push_back(value);
                if (m_dnsPublishFails)
                {
                    co_return std::unexpected(std::string(kDnsPublishFailureText));
                }
                m_publishedTxt = value;
                co_return std::expected<void, std::string>{};
            };
            writer.withdraw = [this](std::string, std::string) -> Core::Task<std::expected<void, std::string>>
            {
                ++m_dnsWithdrawCalls;
                m_publishedTxt.reset();
                if (m_dnsWithdrawFails)
                {
                    co_return std::unexpected(std::string(kDnsWithdrawFailureText));
                }
                co_return std::expected<void, std::string>{};
            };
            return writer;
        }

        /**
         * @brief 按当前这轮的参数造管理器，并把自证路由挂上
         */
        std::unique_ptr<AcmeCertificateManager> buildManager(const Round &round)
        {
            AcmeCertificateManager::Configuration configuration;
            if (!round.sendsEmptyDomainList)
            {
                configuration.domainNames = kDomainNames;
            }
            configuration.certificateFile          = m_certificatePath;
            configuration.privateKeyFile           = m_privateKeyPath;
            configuration.accountKeyFile           = m_accountKeyPath;
            configuration.accountStateFile         = m_accountStatePath;
            configuration.directoryUrl             = m_authority->directoryUrl();
            configuration.contactEmailAddress      = "mailto:acme-auto@example.com";
            configuration.isTermsOfServiceAccepted = true;
            configuration.issuanceTimeout          = kIssuanceTimeout;
            configuration.challengePollInterval    = kPollInterval;
            configuration.renewalCheckInterval     = std::chrono::milliseconds{60};
            configuration.renewBeforeExpiry        = round.renewThreshold;
            configuration.minimumRetryInterval     = round.minimumRetryInterval;
            // 用例统一用 EC：RSA 2048 每把都要几百毫秒，全跑完会把用例时长抬高一个量级
            configuration.accountKeyAlgorithm = AcmeKeyAlgorithm::Es256;
            configuration.domainKeyAlgorithm  = AcmeKeyAlgorithm::Es256;

            auto manager =
                    std::make_unique<AcmeCertificateManager>(*m_loop, std::move(configuration), round.withInstallStep ? makeInstallStep() : AcmeCertificateManager::ReloadHandler{},
                                                             round.usesDns01 ? makeFakeDnsWriter() : AcmeDns01TxtWriter{});
            manager->registerChallengeRoutes(m_challengeServer->router());
            return manager;
        }

        /**
         * @brief 把每轮按参数配好、跑掉并记下结果
         * @details 常驻循环那一轮直接 co_await：它被拒绝时在任何 await 之前就收口，
         *          不会留下挂在定时器上没人唤醒的帧
         */
        Core::Task<void> driveAllRounds()
        {
            if (!co_await awaitPorts())
            {
                m_loop->stop();
                co_return;
            }
            m_authority->setValidationAuthority(std::format("127.0.0.1:{}", m_challengeServer->listeningPort()));

            for (const Round &round: m_rounds)
            {
                m_installStepFails       = round.installStepFails;
                m_dnsPublishFails        = round.dnsPublishFails;
                m_dnsWithdrawFails       = round.dnsWithdrawFails;
                m_dnsPublishesWrongValue = round.dnsPublishesWrongValue;
                if (round.usesDns01)
                {
                    m_dnsPublishCalls  = 0;
                    m_dnsWithdrawCalls = 0;
                    m_dnsPublishedNames.clear();
                    m_dnsPublishedValues.clear();
                    m_publishedTxt.reset();
                    // 桩取 dns-01 答案的那只口要在循环上交给它（同 setValidationAuthority 的理由）
                    m_authority->setPublishedTxtReader([this]() -> std::optional<std::string> { return m_publishedTxt; });
                }
                if (round.usesFreshPaths)
                {
                    m_certificatePath = m_paths->path() / "second-cert.pem";
                    m_privateKeyPath  = m_paths->path() / "second-key.pem";
                }

                m_manager                            = buildManager(round);
                const std::size_t installCallsBefore = m_installCalls.load(std::memory_order_relaxed);
                Run               run;
                if (round.runsRenewalLoop)
                {
                    // 先摆好停位再进循环：这一轮要的是「启动即被拒绝」这个判据，
                    // 常驻循环真跑起来就不是本用例的范围（那属于循环本体的节拍问题）
                    m_manager->stopRenewalLoop();
                    co_await m_manager->runRenewalLoop();
                } else
                {
                    run.result = co_await m_manager->issueIfRequired();
                    if (round.retriesReloadAfterFailure)
                    {
                        // 同一个管理器再走一次入口：装回翻回成功，看它是不是只补装而不重新下单
                        run.pendingReloadAfterFailure = m_manager->status().isReloadPending;
                        run.ordersAfterFailure        = m_authority->evidence().issuedOrderCount;
                        m_installStepFails            = false;
                        run.retryResult               = co_await m_manager->issueIfRequired();
                        run.pendingReloadAfterRetry   = m_manager->status().isReloadPending;
                        run.ordersAfterRetry          = m_authority->evidence().issuedOrderCount;
                    }
                }
                run.installCalls       = m_installCalls.load(std::memory_order_relaxed) - installCallsBefore;
                run.dnsPublishCalls    = m_dnsPublishCalls;
                run.dnsWithdrawCalls   = m_dnsWithdrawCalls;
                run.dnsPublishedNames  = m_dnsPublishedNames;
                run.dnsPublishedValues = m_dnsPublishedValues;

                if (round.probesTokenAfterwards)
                {
                    // 事后回取：令牌应当已经撤掉，那条路径该给 404
                    const std::string challengePath = m_authority->evidence().lastChallengeFetchPath;
                    const std::string url           = std::format("http://127.0.0.1:{}{}", m_challengeServer->listeningPort(), challengePath);
                    auto              fetched       = co_await HttpClient::get(*m_loop, url, kIssuanceTimeout);
                    if (fetched != nullptr)
                    {
                        run.tokenProbeStatusCode = fetched->statusCode;
                        run.tokenProbeBody       = fetched->body;
                    }
                }
                m_runs.push_back(std::move(run));
            }
            m_loop->stop();
        }

        std::unique_ptr<Core::EventLoop>                             m_loop;
        std::unique_ptr<AsynGyanis::TestSupport::TemporaryDirectory> m_paths;
        std::unique_ptr<HttpServer>                                  m_challengeServer; ///< 挂着自证路由的明文服务
        std::unique_ptr<AcmeStubAuthority>                           m_authority;       ///< 桩机构
        std::unique_ptr<AcmeCertificateManager>                      m_manager;         ///< 最近一轮的管理器

        std::optional<Core::Task<void>> m_challengeTask{};
        std::optional<Core::Task<void>> m_task{};

        std::filesystem::path m_certificatePath{};
        std::filesystem::path m_privateKeyPath{};
        std::filesystem::path m_accountKeyPath{};
        std::filesystem::path m_accountStatePath{};

        std::vector<Round>       m_rounds{};
        std::vector<Run>         m_runs{};
        std::atomic<std::size_t> m_installCalls{0};
        bool                     m_installStepFails{false};

        /// dns-01 那副假 DNS 的状态：m_publishedTxt 就是「权威侧现在答得出什么」
        std::optional<std::string> m_publishedTxt{};                ///< 当前已发布的那条 TXT 正文；空表示没发布或已撤回
        std::vector<std::string>   m_dnsPublishedNames{};           ///< 每次写入要求的记录名，按调用顺序
        std::vector<std::string>   m_dnsPublishedValues{};          ///< 每次写入的正文，按调用顺序
        std::size_t                m_dnsPublishCalls{0};            ///< 写入被调了几次
        std::size_t                m_dnsWithdrawCalls{0};           ///< 撤回被调了几次
        bool                       m_dnsPublishFails{false};        ///< 让写入那一步交回失败
        bool                       m_dnsWithdrawFails{false};       ///< 让撤回那一步交回失败
        bool                       m_dnsPublishesWrongValue{false}; ///< 写入算错正文，机构会判 invalid
    };

    /**
     * @brief 钉住：第一次跑会签出一张可用的证书，私钥、账户状态与到期时刻各归其位
     */
    TEST_F(AcmeCertificateManagerTest, IssuesAndPersistsACertificateOnFirstRun)
    {
        startServers({});
        const auto run = driveIssue(Round{});

        ASSERT_TRUE(run.result.has_value()) << "驱动没跑到签发这一步";
        ASSERT_TRUE(run.result->has_value()) << run.result->error().message;

        const AcmeIssuedCertificate &issued = run.result->value();
        EXPECT_TRUE(issued.wasIssued);
        EXPECT_EQ(issued.certificateFile, certificatePath());
        ASSERT_TRUE(std::filesystem::exists(certificatePath()));
        ASSERT_TRUE(std::filesystem::exists(privateKeyPath()));
        ASSERT_TRUE(std::filesystem::exists(accountKeyPath()));
        ASSERT_TRUE(std::filesystem::exists(accountStatePath()));

        // 证书带着下单的每个域名：校验侧只看 SAN
        const auto signedNames = TestSupport::subjectAlternativeNamesOfCertificatePem(readFileOf(certificatePath()));
        EXPECT_EQ(signedNames, kDomainNames);

        // 到期时刻判得出，且落盘这一对能被真实的 TLS 上下文接受（配对校验走生产代码，不在用例里另算一遍）
        ASSERT_TRUE(issued.expiry > std::chrono::system_clock::now());
        Core::TlsContext context;
        EXPECT_TRUE(context.loadCertificate(certificatePath().string(), privateKeyPath().string())) << "落盘的证书与私钥不是一对";

        EXPECT_EQ(run.installCalls, 1U) << "新证书没被装回服务";
        EXPECT_EQ(manager().status().issuanceCount, 1U);
        EXPECT_EQ(manager().challengeStore().presentedCount(), 0U) << "挑战已终局，令牌却不还挂着";
        EXPECT_EQ(stubEvidence().issuedCertificateCount, 1U);
        EXPECT_NE(readFileOf(accountStatePath()).find("accountUrl"), std::string::npos);
    }

    /**
     * @brief 钉住：证书还有富余时一次请求都不发，也不重复装回
     */
    TEST_F(AcmeCertificateManagerTest, SkipsTheAuthorityWhenTheCertificateStillHasRoom)
    {
        startServers({});
        const auto runs = driveRounds({Round{}, Round{}});
        ASSERT_EQ(runs.size(), 2U);
        const Run &first  = runs[0];
        const Run &second = runs[1];
        ASSERT_TRUE(first.result.has_value() && first.result->has_value());
        ASSERT_TRUE(second.result.has_value() && second.result->has_value());

        EXPECT_FALSE(second.result->value().wasIssued) << "还有富余却又签了一张：机构的速率限制是按周算的";
        EXPECT_EQ(second.installCalls, 0U);
        EXPECT_EQ(stubEvidence().issuedCertificateCount, 1U);
        EXPECT_EQ(manager().status().issuanceCount, 0U) << "这一轮的管理器什么都没签，读数却涨了";
    }

    /**
     * @brief 钉住：剩余寿命掉进阈值之内就换新，并把新的一张装回服务
     */
    TEST_F(AcmeCertificateManagerTest, RenewsWhenTheRemainingLifetimeDropsInsideTheWindow)
    {
        AcmeStubAuthority::Settings settings;
        settings.certificateValidDays = 1; // 桩签出来的证书只活一天
        startServers(settings);

        Round firstRound;
        firstRound.renewThreshold = std::chrono::hours{2}; // 剩一天 > 两小时：第一轮签完就该收手
        Round secondRound;
        secondRound.renewThreshold = std::chrono::hours{24 * 3}; // 同一张证书此刻已掉进阈值里
        const auto runs            = driveRounds({firstRound, secondRound});
        ASSERT_EQ(runs.size(), 2U);
        const Run &first  = runs[0];
        const Run &second = runs[1];
        ASSERT_TRUE(first.result.has_value() && first.result->has_value());
        ASSERT_TRUE(second.result.has_value() && second.result->has_value()) << second.result->error().message;

        EXPECT_TRUE(first.result->value().wasIssued);
        EXPECT_TRUE(second.result->value().wasIssued) << "剩余寿命已经掉进阈值却没重签";
        EXPECT_EQ(second.installCalls, 1U) << "换好的新证书没装回服务";
        EXPECT_EQ(stubEvidence().issuedCertificateCount, 2U);
        EXPECT_EQ(manager().status().issuanceCount, 1U) << "这一轮只签了一张，别把上一轮的账算进来";
    }

    /**
     * @brief 钉住：装回服务这一步失败要按失败交回，而不是「磁盘换了、线上没换」地报成功
     */
    TEST_F(AcmeCertificateManagerTest, ReportsFailureWhenTheInstallStepRefuses)
    {
        Round round;
        round.installStepFails = true;
        startServers({});
        const auto run = driveIssue(round);

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::ReloadRejected);
        EXPECT_NE(run.result->error().message.find(kInstallFailureText), std::string::npos) << run.result->error().message;

        // 磁盘上确实换了：这条失败的含义是「线上仍是旧的」，不是「什么都没发生」
        EXPECT_TRUE(std::filesystem::exists(certificatePath()));
        EXPECT_EQ(run.installCalls, 1U);
        EXPECT_EQ(manager().status().issuanceCount, 0U);
        EXPECT_EQ(manager().status().failureCount, 1U);
        EXPECT_NE(manager().status().lastFailureMessage.find(kInstallFailureText), std::string::npos);
    }

    /**
     * @brief 钉住：装回失败之后下一拍只补装，不重新下单
     * @details 磁盘上那张已经是新的了，到期判据因此会判「还够用」而一次都不再试——不记这笔欠账，
     *          这个进程会带着旧身份一路跑到下一个续期窗口。补装成功要把欠账结清，否则之后每次调用
     *          都会多装一次；而结清的判据不能靠「再过 30 天看看」那种口头承诺。
     */
    TEST_F(AcmeCertificateManagerTest, RetriesOnlyTheReloadAfterTheFirstInstallFailed)
    {
        Round round;
        round.installStepFails          = true;
        round.retriesReloadAfterFailure = true;
        startServers({});
        const auto run = driveIssue(round);

        ASSERT_TRUE(run.result.has_value()) << "驱动没跑到签发这一步";
        ASSERT_FALSE(run.result->has_value()) << "装回失败本该按失败交回";
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::ReloadRejected);
        EXPECT_TRUE(run.pendingReloadAfterFailure) << "欠账没记下来，下一拍没人再试";

        ASSERT_TRUE(run.retryResult.has_value());
        ASSERT_TRUE(run.retryResult->has_value()) << "补装这一步没成功：" << run.retryResult->error().message;
        EXPECT_FALSE(run.retryResult->value().wasIssued) << "补装不该重新下单：机构那张单已经用掉了";
        EXPECT_EQ(run.ordersAfterRetry, run.ordersAfterFailure) << "第二次调用向机构多下了一张单";
        EXPECT_FALSE(run.pendingReloadAfterRetry) << "已经装上了还记着欠账，之后每次调用都会多装一次";
        EXPECT_EQ(run.installCalls, 2U) << "两次入口各该叫一次装回：一次失败、一次补装";
    }

    /**
     * @brief 钉住：欠着装回时下一拍压到短片，而不是等到失败退避那一小时之后
     * @details 退避门槛管的是「别再撞机构」，与「把已经在盘上的那张装上去」是两件事；把后者也交给
     *          前者，等于让线上身份多等一小时——而这一轮根本没有下单的必要。
     */
    TEST_F(AcmeCertificateManagerTest, ShortensTheNextWakeWhileTheReloadIsPending)
    {
        Round round;
        round.installStepFails = true;
        startServers({});
        const auto run = driveIssue(round);

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        ASSERT_TRUE(manager().status().isReloadPending);
        const long long nowUnix                 = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const long long backoffRemainingSeconds = manager().status().backoffUntilUnixSeconds - nowUnix;
        EXPECT_GT(backoffRemainingSeconds, 60) << "这一轮的前置没搭对：退避门槛本该还在一小时附近";

        const auto delay = manager().nextRenewalDelay();
        EXPECT_LE(delay, std::chrono::seconds{30}) << "欠着装回却按退避那一小时等，旧身份会被一路用下去";
        EXPECT_GE(delay, std::chrono::milliseconds{1}) << "延迟为 0 会让这条循环空转";
    }

    /**
     * @brief 钉住：没有装回动作就不许跑常驻循环，而不是每月默默把证书写到磁盘上
     */
    TEST_F(AcmeCertificateManagerTest, RefusesTheRenewalLoopWithoutAnInstallStep)
    {
        Round loopRound;
        loopRound.withInstallStep = false;
        loopRound.runsRenewalLoop = true;
        startServers({});
        const auto runs = driveRounds({loopRound});
        ASSERT_EQ(runs.size(), 1U);

        EXPECT_EQ(manager().status().failureCount, 1U);
        EXPECT_NE(manager().status().lastFailureMessage.find("装回"), std::string::npos) << manager().status().lastFailureMessage;
        EXPECT_EQ(stubEvidence().issuedCertificateCount, 0U) << "循环被拒绝了却还是去签了一张";
    }

    /**
     * @brief 钉住：首签引导允许不设装回动作（那时服务还没起来，拿到路径再构造）
     */
    TEST_F(AcmeCertificateManagerTest, AllowsFirstIssuanceWithoutAnInstallStep)
    {
        Round round;
        round.withInstallStep = false;
        startServers({});
        const auto run = driveIssue(round);

        ASSERT_TRUE(run.result.has_value() && run.result->has_value()) << run.result->error().message;
        EXPECT_TRUE(run.result->value().wasIssued);
        EXPECT_TRUE(std::filesystem::exists(certificatePath()));
        EXPECT_EQ(manager().status().failureCount, 0U) << "合法的引导不该计成失败";
    }

    /**
     * @brief 钉住：换一批落点仍沿用记在状态文件里那个账户，不再注册一次
     */
    TEST_F(AcmeCertificateManagerTest, ReusesTheRecordedAccountAcrossCertificates)
    {
        Round firstRound;
        Round secondRound;
        secondRound.usesFreshPaths = true;
        secondRound.renewThreshold = std::chrono::hours{24 * 365}; // 新落点上什么都没有，必须重签
        startServers({});
        const auto runs = driveRounds({firstRound, secondRound});
        ASSERT_EQ(runs.size(), 2U);
        ASSERT_TRUE(runs[0].result.has_value() && runs[0].result->has_value());
        ASSERT_TRUE(runs[1].result.has_value() && runs[1].result->has_value()) << runs[1].result->error().message;

        EXPECT_TRUE(runs[1].result->value().wasIssued) << "换了落点却没签出新证书";
        EXPECT_EQ(stubEvidence().jwkBearingRequestCount, 1U) << "第二张证书又注册了一个账户";
    }

    /**
     * @brief 钉住：机构复用上一轮已 valid 的授权时，第二轮不得再去触发那条挑战
     * @details Boulder 与 Pebble 都会复用授权（Pebble 默认按概率复用），而对已 valid 的挑战再触发一次
     *          校验不是幂等而是 400「Cannot update challenge with status valid, only status pending」——
     *          这是 Pebble 真跑出来的失败形状（见 scripts/acme_pebble_cross_check.sh），桩这一侧把同一条
     *          判据搬进来，才不至于每次回归都要靠一台真机构才能发现。
     */
    TEST_F(AcmeCertificateManagerTest, LeavesAReusedAuthorizationUntouchedOnTheNextOrder)
    {
        AcmeStubAuthority::Settings settings;
        settings.reusesValidAuthorizations = true;
        startServers(settings);

        const Round firstRound; // 域名全新的：必须走完整自证
        Round       secondRound;
        secondRound.usesFreshPaths = true; // 换落点，逼第二轮真的再下一张订单
        secondRound.renewThreshold = std::chrono::hours{24 * 365};
        const auto runs            = driveRounds({firstRound, secondRound});

        ASSERT_EQ(runs.size(), 2U);
        ASSERT_TRUE(runs[0].result.has_value() && runs[0].result->has_value()) << runs[0].result->error().message;
        ASSERT_TRUE(runs[1].result.has_value() && runs[1].result->has_value()) << runs[1].result->error().message;

        EXPECT_EQ(stubEvidence().revalidatedChallengeCount, 0U) << "对着已 valid 的授权又触发了一次挑战：真机构对此回 400";
        // 取令牌只该发生在第一轮：域名有 kDomainNames.size() 个，第二轮全部走复用，一次都不该再取
        EXPECT_EQ(stubEvidence().challengeFetchCount, kDomainNames.size()) << "第二轮把自证重做了一遍：本该复用上一轮已 valid 的授权";
        EXPECT_EQ(stubEvidence().issuedCertificateCount, 2U) << "两轮各签一张，复用授权不该少签一张";
    }

    /**
     * @brief 钉住：中途失败时磁盘上不留半张证书，也不叫装回动作
     */
    TEST_F(AcmeCertificateManagerTest, LeavesNoPartialCertificateWhenIssuanceFails)
    {
        AcmeStubAuthority::Settings settings;
        settings.rejectedDomainNames = {"auto-second.example.com"};
        startServers(settings);
        const auto run = driveIssue(Round{});

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::RejectedByAuthority) << run.result->error().message;
        EXPECT_FALSE(std::filesystem::exists(certificatePath())) << "订单没走成就把证书写下去了";
        EXPECT_EQ(run.installCalls, 0U);
        EXPECT_EQ(manager().status().failureCount, 1U);
    }

    /**
     * @brief 钉住：配置不成形时一个请求都不发（本地判据排在机构判据之前）
     */
    TEST_F(AcmeCertificateManagerTest, RejectsAnEmptyDomainListBeforeAskingTheAuthority)
    {
        Round round;
        round.sendsEmptyDomainList = true;
        startServers({});
        const auto run = driveIssue(round);

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::InvalidConfiguration);
        EXPECT_EQ(stubEvidence().verifiedSignatureCount, 0U) << "配置不合法却已经签出请求去了";
    }

    /**
     * @brief 钉住：令牌只在挑战活着的那段时间可取，事后回取拿到 404
     */
    TEST_F(AcmeCertificateManagerTest, WithdrawsTheTokenOnceTheChallengeIsSettled)
    {
        Round round;
        round.probesTokenAfterwards = true;
        startServers({});
        const auto run = driveIssue(round);
        ASSERT_TRUE(run.result.has_value() && run.result->has_value());

        // 机构在自证时真取到过令牌，否则挑战走不成 valid
        EXPECT_GE(stubEvidence().challengeFetchCount, 1U);
        ASSERT_TRUE(run.tokenProbeStatusCode.has_value()) << "回取令牌没拿到应答";
        EXPECT_EQ(*run.tokenProbeStatusCode, 404) << "撤令牌之后那条路径还在答话：" << run.tokenProbeBody;
        EXPECT_EQ(manager().challengeStore().presentedCount(), 0U);
    }

    /**
     * @brief 钉住：dns-01 通路按名字发布**摘要后**的正文，机构取到过答案之后每条都撤
     * @details 这条同时管着三件事：走的是 dns-01 挑战、交出去的是 43 字符的 base64url 摘要而不是
     *          HTTP-01 那份 keyAuthorization 原文、以及每条授权各一次写各一次撤。
     *          桩机构那侧的期望值是拿它自己那份指纹算的，所以「名字与令牌对不上」也会在这里变红
     */
    TEST_F(AcmeCertificateManagerTest, PublishesTheDigestedTxtAndWithdrawsItAfterDns01Validation)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startServers(settings);

        Round round;
        round.usesDns01 = true;
        const auto run  = driveIssue(round);

        ASSERT_TRUE(run.result.has_value()) << "签发没跑到出口";
        ASSERT_TRUE(run.result->has_value()) << run.result->error().message;
        EXPECT_EQ(run.dnsPublishCalls, kDomainNames.size()) << "每条域名都要各写一次 TXT";
        EXPECT_EQ(run.dnsWithdrawCalls, kDomainNames.size()) << "写了几次就要撤几次，留下的一条会把下一轮堵死";

        for (std::size_t index = 0; index < kDomainNames.size(); ++index)
        {
            EXPECT_EQ(run.dnsPublishedNames[index], dns01RecordName(kDomainNames[index])) << "第 " << index << " 条记录名不对";
            ASSERT_LT(index, run.dnsPublishedValues.size());
            const std::string &value = run.dnsPublishedValues[index];
            // base64url 无填充的 SHA-256：43 个字符，且不含 HTTP-01 那个把 keyAuthorization 原样发出去的点号
            EXPECT_EQ(value.size(), 43U) << "正文不是 43 字符的无填充摘要：" << value;
            EXPECT_EQ(value.find('.'), std::string::npos) << "正文里出现了点号，看着像把 keyAuthorization 原文发出去了：" << value;
            EXPECT_EQ(value.find('='), std::string::npos) << "摘要带了 Base64 填充，机构会判 invalid";
        }
        EXPECT_EQ(stubEvidence().dns01ValidationCount, kDomainNames.size());
        EXPECT_EQ(stubEvidence().challengeFetchCount, 0U) << "走 dns-01 却去取了 HTTP 令牌";
    }

    /**
     * @brief 钉住：写入这一步就失败时，撤回照样要跑一次
     * @details 「报失败的写入其实已经落到权威侧」是这类控制面 API 的真实形状（响应超时的那一次最典型），
     *          留下的那条 TXT 会在下一轮与新的答案并存，而机构的原文只会说 DNS 校验失败
     */
    TEST_F(AcmeCertificateManagerTest, WithdrawsTheTxtWhenThePublishStepReportsFailure)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startServers(settings);

        Round round;
        round.usesDns01       = true;
        round.dnsPublishFails = true;
        const auto run        = driveIssue(round);

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::DnsRecordRejected) << run.result->error().message;
        EXPECT_NE(run.result->error().message.find(kDnsPublishFailureText), std::string::npos) << run.result->error().message;
        EXPECT_EQ(run.dnsPublishCalls, 1U);
        EXPECT_EQ(run.dnsWithdrawCalls, 1U) << "写入失败之后的那次撤回没跑：这是这条通路上最容易漏的出口";
        EXPECT_EQ(stubEvidence().dns01ValidationCount, 0U) << "写入都失败了，机构那边却已经来取过答案";
    }

    /**
     * @brief 钉住：机构判挑战失败之后，写进去的那条仍然被撤掉
     */
    TEST_F(AcmeCertificateManagerTest, WithdrawsTheTxtWhenTheAuthorityRejectsTheChallenge)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startServers(settings);

        Round round;
        round.usesDns01              = true;
        round.dnsPublishesWrongValue = true;
        const auto run               = driveIssue(round);

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::ChallengeNotAnswered) << run.result->error().message;
        EXPECT_EQ(run.dnsPublishCalls, 1U);
        EXPECT_EQ(run.dnsWithdrawCalls, 1U) << "自证失败之后没撤 TXT，那条错误正文会一直留在域名上";
        EXPECT_EQ(stubEvidence().dns01ValidationCount, 1U);
    }

    /**
     * @brief 钉住：撤回自己失败时要把这条报出来，而不是当成签发成功
     * @details 撤不干净的那条记录不影响这一张证书，但会影响下一轮 —— 只在日志里说一句等于让运维
     *          在三十天后撞上「同一个名字两条 TXT」时再来查一次现场
     */
    TEST_F(AcmeCertificateManagerTest, ReportsTheFailureWhenTheTxtCannotBeWithdrawn)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startServers(settings);

        Round round;
        round.usesDns01        = true;
        round.dnsWithdrawFails = true;
        const auto run         = driveIssue(round);

        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        EXPECT_EQ(run.result->error().kind, AcmeErrorKind::DnsRecordRejected) << run.result->error().message;
        EXPECT_NE(run.result->error().message.find(kDnsWithdrawFailureText), std::string::npos) << run.result->error().message;
    }

    namespace
    {
        using AsynGyanis::TestSupport::findRegistrySample;
    } // namespace

    /**
     * @brief 钉住：三条自动化读数在**构造时**就挂上，管理器析构就注销
     * @details 不等第一次签发才登记，是因为「常驻进程里这几条长期为 0」就是要报的事；
     *          而把手漏注销会留下一条谁也不持有的读数，下一轮抓取还在报旧对象的值
     */
    TEST(AcmeAutomationMetrics, RegistersAtConstructionAndReleasesOnDestruction)
    {
        Core::EventLoop                       loop;
        AcmeCertificateManager::Configuration configuration;
        configuration.certificateFile  = std::filesystem::temp_directory_path() / "asyn-acme-metrics-unused-chain.pem";
        configuration.privateKeyFile   = std::filesystem::temp_directory_path() / "asyn-acme-metrics-unused-key.pem";
        configuration.accountKeyFile   = std::filesystem::temp_directory_path() / "asyn-acme-metrics-unused-account.pem";
        configuration.accountStateFile = std::filesystem::temp_directory_path() / "asyn-acme-metrics-unused-state.json";

        EXPECT_FALSE(findRegistrySample("asyn_acme_issuances_total").has_value()) << "还没有管理器，导出里就先有了这条读数";
        {
            const AcmeCertificateManager manager(loop, configuration, {}, {});
            for (const char *const name: {"asyn_acme_certificate_expiry_seconds", "asyn_acme_issuances_total", "asyn_acme_failures_total", "asyn_acme_reload_pending"})
            {
                const auto lookup = findRegistrySample(name);
                ASSERT_TRUE(lookup.has_value()) << name;
                EXPECT_EQ(lookup->value, 0U) << "刚登记就带着一个来路不明的数";
            }
        }
        EXPECT_FALSE(findRegistrySample("asyn_acme_issuances_total").has_value()) << "管理器析构后这条读数还挂在导出里";
    }

    /**
     * @brief 钉住：构造就把磁盘上那张证书的到期时刻填进读数，不等第一次签发
     * @details 少了这一步，进程每次重启后面板都会先看到一段 0 并报成「证书没了」——那个文件其实
     *          一直在，而这条读数的报警口径恰恰是「长期为 0」。多管理器求最早时也才拿得到真数：
     *          没填过的那一格会把整条读数压成 0，把「另一张还有八十天」盖掉
     */
    TEST(AcmeAutomationMetrics, SeedsCertificateExpiryFromDiskAtConstruction)
    {
        Core::EventLoop                       loop;
        AcmeCertificateManager::Configuration configuration;
        configuration.certificateFile  = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";
        configuration.privateKeyFile   = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";
        configuration.accountKeyFile   = std::filesystem::temp_directory_path() / "asyn-acme-seed-account.pem";
        configuration.accountStateFile = std::filesystem::temp_directory_path() / "asyn-acme-seed-state.json";

        const auto expectedExpiry = readCertificateExpiry(configuration.certificateFile);
        ASSERT_TRUE(expectedExpiry.has_value()) << "夹具证书读不出到期时刻，这条用例的对照就没了";
        const auto expectedSeconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(expectedExpiry->time_since_epoch()).count());

        const AcmeCertificateManager manager(loop, configuration, {}, {});
        EXPECT_EQ(findRegistrySample("asyn_acme_issuances_total")->value, 0U) << "构造阶段不该凭空记上一次签发";
        EXPECT_EQ(findRegistrySample("asyn_acme_certificate_expiry_seconds")->value, expectedSeconds) << "到期时刻的读数没有按磁盘上那张证书填，重启后它会先报一段假的「没有证书」";
        EXPECT_EQ(static_cast<std::uint64_t>(manager.status().certificateExpiryUnixSeconds), expectedSeconds) << "同一次构造里对外读数与 status() 报的不是同一个到期时刻";
    }

    /**
     * @brief 钉住：还没签出的那个管理器，不把另一张已读得出的到期时刻压成 0
     * @details 多域名形态下进程里会有多个管理器，而「这条路径上没有读得出的证书」的对外表示就是 0。
     *          按 Min 合并时它一定赢，面板于是报「证书没了」——那张其实还有几十天。合并规则改成跳零之后，
     *          两边都读不出才交回 0；登记顺序的两个方向都在这一条里走一遍。
     */
    TEST(AcmeAutomationMetrics, UnissuedManagerDoesNotMaskTheReadExpiry)
    {
        Core::EventLoop loop;

        AcmeCertificateManager::Configuration pending;
        pending.certificateFile  = std::filesystem::temp_directory_path() / "asyn-acme-mask-pending-absent.pem";
        pending.privateKeyFile   = std::filesystem::temp_directory_path() / "asyn-acme-mask-pending-absent-key.pem";
        pending.accountKeyFile   = std::filesystem::temp_directory_path() / "asyn-acme-mask-a-key.pem";
        pending.accountStateFile = std::filesystem::temp_directory_path() / "asyn-acme-mask-a-state.json";

        AcmeCertificateManager::Configuration readable;
        readable.certificateFile  = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";
        readable.privateKeyFile   = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";
        readable.accountKeyFile   = std::filesystem::temp_directory_path() / "asyn-acme-mask-b-key.pem";
        readable.accountStateFile = std::filesystem::temp_directory_path() / "asyn-acme-mask-b-state.json";

        const auto expectedExpiry = readCertificateExpiry(readable.certificateFile);
        ASSERT_TRUE(expectedExpiry.has_value()) << "夹具证书读不出到期时刻，这条用例的对照就没了";
        const auto expectedSeconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(expectedExpiry->time_since_epoch()).count());

        const AcmeCertificateManager pendingManager(loop, pending, {}, {});
        EXPECT_EQ(findRegistrySample("asyn_acme_certificate_expiry_seconds")->value, 0U) << "只有一格读不出时就该是 0";

        const AcmeCertificateManager readableManager(loop, readable, {}, {});
        EXPECT_EQ(findRegistrySample("asyn_acme_certificate_expiry_seconds")->value, expectedSeconds) << "已经签出的那张被还没签出的那张盖掉了";
    }

    namespace
    {
        /**
         * @brief 常驻循环的观察协程：200 毫秒叫停，之后每 50 毫秒看一次帧有没有退，5 秒为上限
         * @details 写成具名函数按引用收参，不写成立即调用的 lambda：协程帧里存的是**闭包对象的引用**，
         *          立即调用的那个闭包在整条表达式结束时就销毁了，帧之后再碰它是
         *          stack-use-after-scope（Linux 侧 ASan 实测抓到，MSVC 上恰好看不出来）
         */
        Core::Task<void> observeRenewalLoopStop(Core::EventLoop &loop, Core::Task<void> &loopTask, AcmeCertificateManager &manager, std::atomic<bool> &wasParkedWhenStopped,
                                                std::atomic<bool> &exitedWithinBound)
        {
            Core::Timer timer(loop);
            co_await timer.waitFor(std::chrono::milliseconds{200});
            wasParkedWhenStopped.store(!loopTask.isReady(), std::memory_order_relaxed);
            manager.stopRenewalLoop();
            for (int tick = 0; tick < 100; ++tick) // 100 × 50ms：给「一片 + 调度」留出五倍余量
            {
                co_await timer.waitFor(std::chrono::milliseconds{50});
                if (loopTask.isReady())
                {
                    exitedWithinBound.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            loop.stop();
        }
    } // namespace

    /**
     * @brief 钉住：叫停真的能让睡下的续期循环退出，而不是等到下一拍
     * @details `stopRenewalLoop()` 只落一个原子标志，而这条帧唯一醒着的时刻是它自己的定时器到点——
     *          检查间隔默认 12 小时。按文档办事的调用方两种都会出事：等帧退出再拆对象的一直等
     *          （停机挂住），不等就拆对象的在几小时后被一条帧踩在已释放的对象上。
     *          判据用比值：这一拍摄意把节拍设成 30 秒（远大于用例上限），叫停在 200ms 落，
     *          退出必须在 5 秒内被看到——只有停放被切成不超过 1 秒一片才做得到。
     *          反向对照一起钉：叫停那一刻帧必须还睡着，否则用例其实在测「启动即收口」
     */
    TEST(AcmeCertificateManagerLoop, StopWakesTheParkedRenewalLoopInsideOneSlice)
    {
        Core::EventLoop                       loop;
        AcmeCertificateManager::Configuration configuration;
        configuration.certificateFile      = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";
        configuration.privateKeyFile       = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";
        configuration.accountKeyFile       = std::filesystem::temp_directory_path() / "asyn-acme-loop-account.pem";
        configuration.accountStateFile     = std::filesystem::temp_directory_path() / "asyn-acme-loop-state.json";
        configuration.domainNames          = {"loop.example"};
        configuration.directoryUrl         = "https://127.0.0.1:1/directory"; // 这一轮不打扰机构，这只口不会被碰
        configuration.renewBeforeExpiry    = std::chrono::hours{24};
        configuration.renewalCheckInterval = std::chrono::seconds{30};
        configuration.accountKeyAlgorithm  = AcmeKeyAlgorithm::Es256;
        configuration.domainKeyAlgorithm   = AcmeKeyAlgorithm::Es256;

        std::atomic<int>       reloadCalls{0};
        AcmeCertificateManager manager(loop, configuration,
                                       [&reloadCalls]() -> std::expected<void, std::string>
                                       {
                                           ++reloadCalls;
                                           return {};
                                       });

        Core::Task<void> loopTask = manager.runRenewalLoop();
        loop.scheduler().schedule(loopTask.handle());

        std::atomic<bool> wasParkedWhenStopped{false};
        std::atomic<bool> exitedWithinBound{false};
        Core::Task<void>  observer = observeRenewalLoopStop(loop, loopTask, manager, wasParkedWhenStopped, exitedWithinBound);
        loop.scheduler().schedule(observer.handle());

        loop.run();
        if (loopTask.isReady())
        {
            // 循环协程里抛出过就要在这里冒出来，别让用例静默通过
            loopTask.handle().promise().result();
        }

        EXPECT_TRUE(wasParkedWhenStopped.load(std::memory_order_relaxed)) << "叫停之前这条帧就已经退了：判据落不到停放上";
        EXPECT_TRUE(exitedWithinBound.load(std::memory_order_relaxed)) << "叫停之后 5 秒内这条帧没退出：停放没切片，停机要等到下一拍（这一轮设的是 30 秒）";
        EXPECT_EQ(reloadCalls.load(std::memory_order_relaxed), 0) << "这一轮证书还有富余，不该把装回动作碰一次";
    }

    /**
     * @brief 钉住：导出的读数与 `status()` 报的是同一份原子量，不是各算一遍
     * @details 两处各读各的会出现「面板说签成一张、status() 说没有」——这条断言把两者钉成同一个数
     */
    TEST_F(AcmeCertificateManagerTest, ExportsTheSameReadingsStatusReports)
    {
        startServers({});
        const auto run = driveIssue(Round{});
        ASSERT_TRUE(run.result.has_value() && run.result->has_value()) << run.result->error().message;

        const auto status = manager().status();
        EXPECT_EQ(findRegistrySample("asyn_acme_issuances_total")->value, static_cast<std::uint64_t>(status.issuanceCount));
        EXPECT_EQ(findRegistrySample("asyn_acme_failures_total")->value, static_cast<std::uint64_t>(status.failureCount));
        EXPECT_EQ(findRegistrySample("asyn_acme_certificate_expiry_seconds")->value, static_cast<std::uint64_t>(status.certificateExpiryUnixSeconds))
                << "到期时刻的对外读数与判据用的不是同一个数";
        EXPECT_GT(status.certificateExpiryUnixSeconds, 0) << "签成之后落点读不出到期时刻";
        EXPECT_EQ(findRegistrySample("asyn_acme_reload_pending")->value, static_cast<std::uint64_t>(status.isReloadPending ? 1U : 0U))
                << "装回欠账的对外读数与判据用的不是同一个数";
    }

    /**
     * @brief 钉住：「盘上已是新的、线上还没换上」这一格在导出里有形状
     * @details 面板上「自动化没跑成」与「还没跑」本来就长一个样，而这一种最该报警：证书每月在换、
     *          线上身份一动不动。只放在 status() 里就得有人去翻进程内对象，翻日志不算对外可读。
     */
    TEST_F(AcmeCertificateManagerTest, ExportsTheReloadDebtAsAGauge)
    {
        Round round;
        round.installStepFails = true;
        startServers({});
        const auto run = driveIssue(round);
        ASSERT_TRUE(run.result.has_value());
        ASSERT_FALSE(run.result->has_value());
        ASSERT_TRUE(manager().status().isReloadPending);

        EXPECT_EQ(findRegistrySample("asyn_acme_reload_pending")->value, 1U) << "装回欠账没有对外读数，只能靠翻日志发现";
    }

    /**
     * @brief 钉住：失败退避时刻交回给运维，成功之后清回零
     * @details 只有「上次失败」的文案而没有「什么时候再试」，值班就无从判断这条自动化是不是已经躺平
     */
    TEST_F(AcmeCertificateManagerTest, ReportsTheBackoffDeadlineAfterAFailure)
    {
        Round loopRound;
        loopRound.withInstallStep = false;
        loopRound.runsRenewalLoop = true;
        startServers({});
        const auto runs = driveRounds({loopRound});
        ASSERT_EQ(runs.size(), 1U);

        const long long nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        EXPECT_GT(manager().status().backoffUntilUnixSeconds, nowUnix) << "记了失败却没留下退避时刻：外部无法判断还会不会再试";
    }

    /**
     * @brief 钉住：签成之后退避时刻清零（留着旧值会让人以为下一轮还要等）
     */
    TEST_F(AcmeCertificateManagerTest, ClearsTheBackoffDeadlineAfterASuccess)
    {
        startServers({});
        const auto run = driveIssue(Round{});
        ASSERT_TRUE(run.result.has_value() && run.result->has_value()) << run.result->error().message;
        EXPECT_EQ(manager().status().backoffUntilUnixSeconds, 0);
    }

    /**
     * @brief 钉住：机构在 429 上给的 `Retry-After` 决定退避门槛（RFC 8555 §6.8 要求客户端照办）
     * @details 本地最小重试间隔在这里刻意调成 60 秒，而桩回的是 3600 秒——两个数不同才分得清门槛
     *          到底听的是谁。这一项以前被解析出来又当场丢掉，门槛实际是「下一秒」，于是
     *          `RateLimited` 注释里那句「该退避」与实现无关。
     */
    TEST_F(AcmeCertificateManagerTest, BacksOffForTheDelayTheAuthorityAskedFor)
    {
        AcmeStubAuthority::Settings settings;
        settings.isOrderRateLimited = true;
        startServers(settings);

        Round round;
        round.minimumRetryInterval = std::chrono::seconds{60};
        const auto run             = driveIssue(round);
        ASSERT_TRUE(run.result.has_value()) << "管理器没有把这轮的结果交回来";
        ASSERT_FALSE(run.result->has_value()) << "桩在限流档位，这轮本该失败";
        EXPECT_EQ(run.result->error().retryAfter, std::optional<std::chrono::seconds>{std::chrono::seconds{3600}}) << "这条失败没带上机构给的等待时长";
        EXPECT_NE(manager().status().lastFailureMessage.find("3600"), std::string::npos) << manager().status().lastFailureMessage;

        const long long nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const long long waiting = manager().status().backoffUntilUnixSeconds - nowUnix;
        EXPECT_GE(waiting, 3000) << "机构的 3600 秒没有进退避门槛，实际只等到 " << waiting << " 秒";
        EXPECT_LE(waiting, 3660) << "门槛超出机构给的那一段：等待时长不该被本地放大";
    }

    /**
     * @brief 钉住：机构没说话时，退避门槛按本地 `minimumRetryInterval` 算
     * @details 这个配置项此前**从来没有被读过**（探针与用例都能配它，行为却恒为「下一秒再试」）。
     *          这一条把它接上：60 秒的门槛要看得见，而 1 秒就是原来那个从未生效的形状。
     */
    TEST_F(AcmeCertificateManagerTest, UsesTheLocalMinimumWhenTheAuthoritySaysNothing)
    {
        startServers({});

        Round round;
        round.sendsEmptyDomainList = true; // 本地判据先拒：这条失败不带机构的答复
        round.minimumRetryInterval = std::chrono::seconds{60};
        const auto run             = driveIssue(round);
        ASSERT_TRUE(run.result.has_value()) << "管理器没有把这轮的结果交回来";
        ASSERT_FALSE(run.result->has_value()) << "空域名列表本该被本地拒掉";

        const long long nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const long long waiting = manager().status().backoffUntilUnixSeconds - nowUnix;
        EXPECT_GE(waiting, 30) << "本地的最小重试间隔没生效，实际只等到 " << waiting << " 秒";
        EXPECT_LE(waiting, 120) << "没有机构答复时不该把门槛拉到本地间隔之外";
    }

    namespace
    {
        /**
         * @brief 等到条件成立或时限到点
         * @details 用例自己构造成立条件、自己等到它，不赌调度时序；到点仍不成立时返回假，由调用方的
         *          断言把「等了多少」说出来。
         */
        [[nodiscard]] bool waitUntil(const std::function<bool()> &condition, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return condition();
        }
    } // namespace

    /**
     * @brief 跟随协程：磁盘上那张证书换了就叫装回一次，没变就一次都不叫
     * @details 钉的是多进程部署里除签发方之外那些进程的取新路径：签发进程改的是磁盘，别的进程要么跟住
     *          这次变化，要么永远用旧身份。「每拍都叫」和「从不叫」都是这台机器的故障，所以两头都判。
     */
    TEST(CertificateRotationFollower, ReloadsOncePerCertificateChange)
    {
        AsynGyanis::TestSupport::TemporaryDirectory paths("CertificateRotationFollower");
        ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v1"));
        ASSERT_TRUE(paths.writeBinaryFile("key.pem", "key-v1"));

        std::atomic<bool>                  isStopping{false};
        std::atomic<int>                   reloadCalls{0};
        Core::EventLoop                    loop;
        Core::TestSupport::EventLoopThread loopThread(loop);
        std::optional<Core::Task<>>        follower;
        follower.emplace(followCertificateRotation(
                loop, paths.path() / "chain.pem", paths.path() / "key.pem", std::chrono::milliseconds{20},
                [&reloadCalls]() -> std::expected<void, std::string>
                {
                    ++reloadCalls;
                    return {};
                },
                isStopping));
        loop.scheduler().schedule(follower->handle());

        // 什么也没改的一段时间里不该叫过一次：把「每次轮询都重装」这种形状挡在门外
        static_cast<void>(waitUntil([&loopThread] { return loopThread.isRunning(); }, std::chrono::seconds{2}));
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        EXPECT_EQ(reloadCalls.load(), 0) << "文件没变就叫了装回，跟随协程成了每拍重装";

        ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v2-longer"));
        if (!waitUntil([&reloadCalls] { return reloadCalls.load() >= 1; }, std::chrono::seconds{2}))
        {
            // 基线是在协程第一遍 stat 那一刻定的，而「已调度」不等于「已跑过」：满载的 runner 上这次写盘
            // 可能整个发生在定基线之前，对这条跟随通道就不算变化。换一份内容再写一次，让「有变化」由用例
            // 自己造出来而不是赌调度时序。下面仍只断言装回被叫到一次，两次写入不会都被看见——
            // 第一次没被看见正是因为基线里已经含着它
            ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v3-longer-longest"));
        }
        EXPECT_TRUE(waitUntil([&reloadCalls] { return reloadCalls.load() >= 1; }, std::chrono::seconds{5})) << "换了证书之后 5 秒内没有触发装回";
        EXPECT_EQ(reloadCalls.load(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        EXPECT_EQ(reloadCalls.load(), 1) << "同一次变化被按拍重复装回，等于身份每 20ms 重装一遍";

        isStopping.store(true);
        loopThread.join();
    }

    /**
     * @brief 装回失败时不认下新身份，下一拍再试
     * @details 线上还在用旧证书，放过一次就是永远放过；而重试不能每拍都喊，那会把日志刷成一堵墙。
     */
    TEST(CertificateRotationFollower, KeepsRetryingWhileTheReloadKeepsFailing)
    {
        AsynGyanis::TestSupport::TemporaryDirectory paths("CertificateRotationFollowerRetry");
        ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v1"));
        ASSERT_TRUE(paths.writeBinaryFile("key.pem", "key-v1"));

        std::atomic<bool>                  isStopping{false};
        std::atomic<int>                   reloadCalls{0};
        Core::EventLoop                    loop;
        Core::TestSupport::EventLoopThread loopThread(loop);
        std::optional<Core::Task<>>        follower;
        follower.emplace(followCertificateRotation(
                loop, paths.path() / "chain.pem", paths.path() / "key.pem", std::chrono::milliseconds{20},
                [&reloadCalls]() -> std::expected<void, std::string>
                {
                    ++reloadCalls;
                    return std::unexpected("用例里的装回总是失败");
                },
                isStopping));
        loopThread.loop().scheduler().schedule(follower->handle());
        static_cast<void>(waitUntil([&loopThread] { return loopThread.isRunning(); }, std::chrono::seconds{2}));

        ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v2-longer"));
        if (!waitUntil([&reloadCalls] { return reloadCalls.load() >= 1; }, std::chrono::seconds{2}))
        {
            // 同一处调度赌注：基线可能在这一次写盘之后才定下，那这次变化对它就不存在。换内容再写一次，
            // 让前提由用例自己造出来。**这里之后不再写盘**——「装回失败还认下新身份」那种退化要靠
            // 「同一次未处置的变化每拍重试」来红，再多写一次就把那条性质洗掉了
            ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v3-longer-longest"));
        }
        EXPECT_TRUE(waitUntil([&reloadCalls] { return reloadCalls.load() >= 2; }, std::chrono::seconds{5})) << "装回失败一次就不再重试，等于承认了没装上的新身份";

        isStopping.store(true);
        loopThread.join();
    }

    /**
     * @brief 没交装回动作时跟随协程当场退出，不跑成一条什么都做不了的常驻协程
     */
    TEST(CertificateRotationFollower, ExitsImmediatelyWithoutAReloadAction)
    {
        AsynGyanis::TestSupport::TemporaryDirectory paths("CertificateRotationFollowerNoHandler");
        ASSERT_TRUE(paths.writeBinaryFile("chain.pem", "cert-v1"));
        ASSERT_TRUE(paths.writeBinaryFile("key.pem", "key-v1"));

        std::atomic<bool>                  isStopping{false};
        Core::EventLoop                    loop;
        Core::TestSupport::EventLoopThread loopThread(loop);
        std::optional<Core::Task<>>        follower;
        follower.emplace(followCertificateRotation(loop, paths.path() / "chain.pem", paths.path() / "key.pem", std::chrono::milliseconds{20}, {}, isStopping));
        // 先把「循环已经在跑」这个前提造出来再投递：向一条尚未进入 run() 的循环投递这条帧，
        // 它可能永远没人唤醒——那是用例在赌调度，不是被测代码的问题
        ASSERT_TRUE(waitUntil([&loopThread] { return loopThread.isRunning(); }, std::chrono::seconds{2})) << "后台循环没起来，环境异常";
        loopThread.loop().scheduler().schedule(follower->handle());

        // 收口判据改到循环停干净之后单线程读：在主线程上轮询协程句柄本身就是跨线程碰循环对象。
        // 先叫停再等 200 ms（够这条帧被投出去跑完），循环一停就没人再动它
        isStopping.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        loopThread.join();
        EXPECT_TRUE(follower->isReady()) << "空的装回动作本该让这条协程当场收口，而不是挂在定时器上";
    }
} // namespace AsynGyanis::Net
