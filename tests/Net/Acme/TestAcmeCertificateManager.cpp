// ACME 证书管理器用例：首签落盘、够用时不打扰机构、到期前换新并装回服务、失败不留半张证书。
// 对端仍是那份进程内的桩机构。EventLoop::run() 只能回一次，因此「先签一张、再判要不要续」这类
// 场景在同一趟驱动里按轮跑完，每轮换一个新管理器，等价于进程重启之后接着看磁盘上留下的状态。
#include "AcmeStubAuthority.h"
#include "AcmeTestSupport.h"
#include "CommonTestSupport.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Net/Acme/AcmeCertificateManager.h"
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
#include <memory>
#include <optional>
#include <string>
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
            bool                      probesTokenAfterwards{false};                ///< 之后再回取一次令牌，看撤没撤
            bool                      usesFreshPaths{false};                       ///< 换一批落点：同一台机器上的另一张证书
            bool                      runsRenewalLoop{false};                      ///< 这一轮跑常驻循环而不是单次签发
        };

        /**
         * @brief 一轮的产出
         */
        struct Run
        {
            std::optional<std::expected<AcmeIssuedCertificate, AcmeError>> result{};               ///< 签发出口；空表示这一轮没跑到
            std::size_t                                                    installCalls{0};        ///< 这一轮里装回动作被调了几次
            std::optional<int>                                             tokenProbeStatusCode{}; ///< 事后回取令牌的状态码
            std::string                                                    tokenProbeBody;         ///< 事后回取到的正文
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
            // 用例统一用 EC：RSA 2048 每把都要几百毫秒，全跑完会把用例时长抬高一个量级
            configuration.accountKeyAlgorithm = AcmeKeyAlgorithm::Es256;
            configuration.domainKeyAlgorithm  = AcmeKeyAlgorithm::Es256;

            auto manager = std::make_unique<AcmeCertificateManager>(*m_loop, std::move(configuration),
                                                                    round.withInstallStep ? makeInstallStep() : AcmeCertificateManager::ReloadHandler{});
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
                m_installStepFails = round.installStepFails;
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
                }
                run.installCalls = m_installCalls.load(std::memory_order_relaxed) - installCallsBefore;

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
} // namespace AsynGyanis::Net
