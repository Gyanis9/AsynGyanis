// ACME 客户端用例：拿一份进程内的桩颁发机构（真验签、真管 nonce、真回取令牌、真签证书）走完整台状态机，
// 再逐条测它的拒绝面与故障面。用例不联网，因此每一轮都能跑；真机构那条路另有按环境变量门控的用例。
#include "AcmeStubAuthority.h"
#include "AcmeTestSupport.h"
#include "Base/Coding/Base64.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Acme/AcmeClient.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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

        /// 单次出站请求的时限
        constexpr std::chrono::milliseconds kRequestTimeout{5000};

        /// 挑战与订单轮询的间隔与上限：桩是秒内就答完的，这里的上限只用来兜住「机构侧一直没给终局」
        constexpr std::chrono::milliseconds kPollInterval{20};
        constexpr std::chrono::milliseconds kPollTimeout{5000};

        /// 默认要覆盖的域名
        const std::vector<std::string> kDefaultDomainNames{"stub-first.example.com"};

        /// 令牌应答方对机构取来的那个请求怎么回
        enum class TokenAnswer
        {
            Correct, ///< 交出 "令牌.账户公钥指纹"，即规范要求的 keyAuthorization
            Wrong,   ///< 交出别的内容
            Absent,  ///< 不注册这条路由（机构会读到 404）
        };
    } // namespace

    /**
     * @brief 一轮协议往返的夹具：桩机构、令牌应答方与客户端都挂在同一条循环上
     * @details 时限都由步骤参数给死（请求 5 秒、轮询 5 秒、桩取令牌 2 秒），因此一条流程的总时长天然有界，
     *          不需要再放一个看门狗协程 parked 在定时器里——那种协程在循环停下时帧还没销毁，会被 LSan 记成泄漏。
     */
    class AcmeClientTest : public ::testing::Test
    {
    public:
        /**
         * @brief 一条流程的输入
         */
        struct FlowRequest
        {
            std::optional<std::string> persistedAccountUrl;              ///< 已记账的账户 URL；空表示按新建账户走
            bool                       isAccountFromPreviousFlow{false}; ///< 用上一条流程拿到的账户 URL（同一条循环里连着跑）
            std::vector<std::string>   domainNames;                      ///< 要下单的域名
            bool                       solvesChallenges{true};           ///< 是否把自证做完再定稿（关掉就测「没自证就定稿」）
        };

        /**
         * @brief 一条流程的产出
         */
        struct FlowOutcome
        {
            bool          isSuccess{false};                                 ///< 是否拿到证书
            AcmeErrorKind failureKind{AcmeErrorKind::InvalidConfiguration}; ///< 失败种类
            std::string   failureMessage;                                   ///< 失败文案
            std::string   certificatePem;                                   ///< 成功时拿到的证书链
            std::string   accountUrl;                                       ///< 本轮生效的账户 URL，供下一轮复用
        };

    protected:
        /**
         * @brief 起令牌应答方与桩机构
         * @param settings 桩的行为开关（validationAuthority 由本函数填成应答方的实际地址）
         * @param answer 应答方对令牌请求的回法
         */
        void startFixtures(const AcmeStubAuthority::Settings &settings, const TokenAnswer answer = TokenAnswer::Correct)
        {
            m_loop = std::make_unique<Core::EventLoop>();

            // 应答方先起：桩要知道它的地址才能去取令牌
            m_responder = std::make_unique<HttpServer>(*m_loop, Core::InetAddress::localhost(0));
            if (m_isDirectoryMissingNewNonce)
            {
                // 一份合法但缺 newNonce 的目录：客户端要点名缺的是哪个端点，而不是拿空 URL 继续签
                m_responder->router().get("/partial-directory",
                                          [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                                          {
                                              response.setStatus(200);
                                              response.setHeader("content-type", "application/json");
                                              response.setBody(R"({"newAccount":"/acme/new-account","newOrder":"/acme/new-order"})");
                                              co_return;
                                          });
            }
            if (answer != TokenAnswer::Absent)
            {
                m_responder->router().get("/.well-known/acme-challenge/:token",
                                          [this, answer](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                                          {
                                              const std::string token = request.param("token").value_or(std::string{});
                                              response.setStatus(200);
                                              response.setHeader("content-type", "text/plain");
                                              // 规范串是「令牌.账户公钥指纹」，指纹由被测实现自己算出；
                                              // 桩那边按它自己那份规范化另算一遍，两边不同就判 invalid
                                              const std::string body =
                                                      answer == TokenAnswer::Correct ? token + "." + m_accountKey->jsonWebKeyThumbprint() : token + ".不是一份真的指纹";
                                              response.setBody(body);
                                              co_return;
                                          });
            }
            // start() 交回的是接受循环的协程本身：投进循环并持有到跑完，否则监听根本建不起来
            m_responderTask = m_responder->start();
            m_loop->scheduler().schedule(m_responderTask->handle());

            m_authority = std::make_unique<AcmeStubAuthority>(*m_loop, settings);
            ASSERT_TRUE(m_authority->start(0));
        }

        /**
         * @brief 依次跑完几条流程：同一条循环只 run() 一次，所以「复用账户」这类场景要放在一趟里
         */
        std::vector<FlowOutcome> runFlows(std::vector<FlowRequest> requests)
        {
            m_outcomes.assign(requests.size(), FlowOutcome{});
            m_requests = std::move(requests);

            m_flowTask = driveFlows();
            m_loop->scheduler().schedule(m_flowTask->handle());
            m_loop->run();
            // 驱动协程跑完（或某一步失败提前收口）之后循环才停，这里读到的都是已落定的值
            m_flowTask.reset();
            return std::move(m_outcomes);
        }

        /// 单条流程的简写
        FlowOutcome runFlow(FlowRequest request)
        {
            auto outcomes = runFlows({std::move(request)});
            EXPECT_EQ(outcomes.size(), 1U);
            return outcomes.empty() ? FlowOutcome{} : std::move(outcomes.front());
        }

        /// 单条流程用默认输入（新建账户、覆盖默认域名、把自证做完）
        FlowOutcome runDefaultFlow()
        {
            FlowRequest request;
            request.domainNames = kDefaultDomainNames;
            return runFlow(std::move(request));
        }

        void SetUp() override
        {
            auto key = AcmeKeyPair::generate(AcmeKeyAlgorithm::Es256);
            ASSERT_TRUE(key.has_value()) << key.error().message;
            m_accountKey = std::make_unique<AcmeKeyPair>(std::move(*key));
        }

        void TearDown() override
        {
            if (m_authority)
            {
                m_authority->stop();
            }
            if (m_responder)
            {
                m_responder->stop();
            }
        }

        /// 桩机构累计的证据：只在循环停下之后读
        [[nodiscard]] const AcmeStubAuthority::Evidence &evidence() const
        {
            return m_authority->evidence();
        }

        /// 客户端配置的可调面：各用例只改自己那一档
        std::unique_ptr<AcmeKeyPair> m_accountKey;                     ///< 账户密钥
        bool                         m_isBrokenDirectory{};            ///< 把目录地址换成应答方上一个不存在的路径
        bool                         m_isDirectoryMissingNewNonce{};   ///< 目录换成「合法 JSON 但没有 newNonce」
        bool                         m_isPersistedAccountUnknown{};    ///< 拿一个桩没记过账的账户 URL 去复用
        bool                         m_isTermsOfServiceAccepted{true}; ///< 是否替调用方接受条款
        std::string                  m_externalAccountKeyId{};         ///< 客户端侧的 EAB 标识
        std::string                  m_externalAccountKeySecret{};     ///< 客户端侧的 EAB HMAC 密钥

    private:
        /**
         * @brief 按本夹具的桩地址拼一份客户端配置
         */
        [[nodiscard]] AcmeClient::Configuration makeClientConfiguration() const
        {
            AcmeClient::Configuration configuration;
            // 端口要等接受协程发布，因此所有 URL 都在循环上现拼
            configuration.directoryUrl             = m_isBrokenDirectory            ? std::format("http://127.0.0.1:{}/no-such-directory", m_responder->listeningPort())
                                                     : m_isDirectoryMissingNewNonce ? std::format("http://127.0.0.1:{}/partial-directory", m_responder->listeningPort())
                                                                                    : m_authority->directoryUrl();
            configuration.contactEmailAddress      = "mailto:acme-tests@example.com";
            configuration.isTermsOfServiceAccepted = m_isTermsOfServiceAccepted;
            configuration.requestTimeout           = kRequestTimeout;
            configuration.externalAccountKeyId     = m_externalAccountKeyId;
            configuration.externalAccountKeySecret = m_externalAccountKeySecret;
            return configuration;
        }

        /// 把失败并上这之前已经拿到的部分结果一起记下，并交出「还要不要接着跑」
        template<typename Expected>
        bool recordFailure(const std::size_t index, const Expected &result, FlowOutcome outcome)
        {
            outcome.failureKind    = result.error().kind;
            outcome.failureMessage = result.error().message;
            m_outcomes[index]      = std::move(outcome);
            return false;
        }

        Core::Task<bool> runOneFlow(const std::size_t index)
        {
            const FlowRequest &request = m_requests[index];
            FlowOutcome        outcome;

            AcmeClient client(*m_loop, makeClientConfiguration(), *m_accountKey);
            auto       persisted = request.persistedAccountUrl;
            if (request.isAccountFromPreviousFlow && index > 0)
            {
                persisted = m_outcomes[index - 1].accountUrl;
            }
            if (m_isPersistedAccountUnknown)
            {
                persisted = std::optional<std::string>(m_authority->unknownAccountUrl());
            }

            auto prepared = co_await client.prepareAccount(persisted);
            if (!prepared.has_value())
            {
                co_return recordFailure(index, prepared, std::move(outcome));
            }
            outcome.accountUrl = std::string(client.accountUrl());

            auto order = co_await client.createOrder(request.domainNames);
            if (!order.has_value())
            {
                co_return recordFailure(index, order, std::move(outcome));
            }

            if (request.solvesChallenges)
            {
                for (const std::string &authorizationUrl: order->authorizationUrls)
                {
                    auto authorization = co_await client.fetchAuthorization(authorizationUrl);
                    if (!authorization.has_value())
                    {
                        co_return recordFailure(index, authorization, std::move(outcome));
                    }
                    // http01 一定存在：fetchAuthorization 里没有它就返回失败而不是交回空
                    auto solved = co_await client.solveChallenge(authorization->http01->challengeUrl, kPollInterval, kPollTimeout);
                    if (!solved.has_value())
                    {
                        co_return recordFailure(index, solved, std::move(outcome));
                    }
                }
            }

            auto domainKey = AcmeKeyPair::generate(AcmeKeyAlgorithm::Es256);
            if (!domainKey.has_value())
            {
                co_return recordFailure(index, domainKey, std::move(outcome));
            }
            auto encodedRequest = domainKey->createCertificateSigningRequest(request.domainNames);
            if (!encodedRequest.has_value())
            {
                co_return recordFailure(index, encodedRequest, std::move(outcome));
            }

            auto finalized = co_await client.finalizeOrder(*order, *encodedRequest, kPollInterval, kPollTimeout);
            if (!finalized.has_value())
            {
                co_return recordFailure(index, finalized, std::move(outcome));
            }

            auto chain = co_await client.fetchCertificateChain(finalized->certificateUrl);
            if (!chain.has_value())
            {
                co_return recordFailure(index, chain, std::move(outcome));
            }

            outcome.isSuccess      = true;
            outcome.certificatePem = std::move(*chain);
            m_outcomes[index]      = std::move(outcome);
            co_return true;
        }

        /**
         * @brief 把每条流程连着跑完再叫停循环：某一步失败就停在这里，后面的流程不再尝试
         */
        Core::Task<void> driveFlows()
        {
            if (!co_await awaitListeningPorts())
            {
                m_loop->stop();
                co_return;
            }
            m_authority->setValidationAuthority(std::format("127.0.0.1:{}", m_responder->listeningPort()));

            for (std::size_t index = 0; index < m_requests.size(); ++index)
            {
                const bool isContinued = co_await runOneFlow(index);
                if (!isContinued)
                {
                    break;
                }
            }
            m_loop->stop();
        }

        /**
         * @brief 等两台服务都把内核分配的端口发布出来（有界等待）
         * @return true 端口已可见；false 表示等满预算，各条流程按「夹具没起来」记下失败
         */
        Core::Task<bool> awaitListeningPorts()
        {
            Core::Timer                     timer(*m_loop);
            const std::chrono::milliseconds perPoll{1};
            const std::chrono::milliseconds budget{2000};
            const auto                      deadline = std::chrono::steady_clock::now() + budget;
            while (m_responder->listeningPort() == 0 || m_authority->port() == 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    for (FlowOutcome &outcome: m_outcomes)
                    {
                        outcome.failureKind    = AcmeErrorKind::Transport;
                        outcome.failureMessage = "夹具的监听端口在预算内没发布出来（接受协程没跑起来）";
                    }
                    co_return false;
                }
                co_await timer.waitFor(perPoll);
            }
            co_return true;
        }

        std::unique_ptr<Core::EventLoop>   m_loop;
        std::unique_ptr<HttpServer>        m_responder;     ///< 应答 http-01 令牌请求的那台明文服务
        std::unique_ptr<AcmeStubAuthority> m_authority;     ///< 桩颁发机构
        std::vector<FlowRequest>           m_requests;      ///< 本轮要跑的流程
        std::vector<FlowOutcome>           m_outcomes;      ///< 各流程的产出
        std::optional<Core::Task<void>>    m_flowTask;      ///< 驱动协程的帧：必须活到循环停下之后
        std::optional<Core::Task<void>>    m_responderTask; ///< 应答方接受循环的协程帧
    };

    /**
     * @brief 钉住：整台状态机能走通，签出的证书覆盖下单的每个域名
     */
    TEST_F(AcmeClientTest, IssuesACertificateForEveryRequestedDomain)
    {
        startFixtures({});

        // runFlow 是按值搬走 request 的：期望值要在搬走之前自己留一份，否则比对对象成了空列表（恒等式假绿）
        const std::vector<std::string> expectedNames{"stub-first.example.com", "stub-second.example.com"};
        FlowRequest                    request;
        request.domainNames = expectedNames;
        const auto outcome  = runFlow(std::move(request));

        ASSERT_TRUE(outcome.isSuccess) << static_cast<int>(outcome.failureKind) << " " << outcome.failureMessage;
        EXPECT_NE(outcome.certificatePem.find("BEGIN CERTIFICATE"), std::string::npos);

        const auto signedNames = TestSupport::subjectAlternativeNamesOfCertificatePem(outcome.certificatePem);
        EXPECT_EQ(signedNames, expectedNames) << "签出来的证书没覆盖下单时要的每一个域名";

        const AcmeStubAuthority::Evidence &evidence = this->evidence();
        EXPECT_GE(evidence.verifiedSignatureCount, 6U) << "整单至少要签六次";
        EXPECT_EQ(evidence.rejectedSignatureCount, 0U) << "桩验不过的签名一条都不该有：那说明实现侧的 JWS 写错了";
        EXPECT_EQ(evidence.jwkBearingRequestCount, 1U) << "只有账户注册那一次能带 jwk";
        // 真机构（Boulder 与 Pebble）对缺 User-Agent 的请求一律 400 malformed，取目录那一步就过不去；
        // 这条判据把「每条 ACME 请求都带客户端标识」钉住，而不是让它靠实现里记得加
        EXPECT_EQ(evidence.missingUserAgentRequestCount, 0U) << "有 ACME 请求没带客户端标识：换到真机构那侧会第一步就断";
        EXPECT_EQ(evidence.challengeFetchCount, 2U) << "每个域名各取一次自证令牌";
        EXPECT_EQ(evidence.issuedCertificateCount, 1U);
        EXPECT_NE(evidence.lastChallengeFetchPath.find("/.well-known/acme-challenge/"), std::string::npos);
    }

    /**
     * @brief 钉住：记下账户 URL 之后，第二轮不再注册、直接以 kid 复用同一账户
     */
    TEST_F(AcmeClientTest, ReusesThePersistedAccountWithoutRegisteringAgain)
    {
        startFixtures({});

        FlowRequest first;
        first.domainNames = {"stub-first.example.com"};
        FlowRequest second;
        second.domainNames = {"stub-second.example.com"};
        // 第二轮要拿第一轮的账户 URL，而同一条循环只能 run() 一次：两条流程放在一趟里连着跑
        second.isAccountFromPreviousFlow = true;

        const auto outcomes = runFlows({std::move(first), std::move(second)});
        ASSERT_EQ(outcomes.size(), 2U);
        ASSERT_TRUE(outcomes[0].isSuccess) << outcomes[0].failureMessage;
        ASSERT_TRUE(outcomes[1].isSuccess) << outcomes[1].failureMessage;
        EXPECT_FALSE(outcomes[1].accountUrl.empty());
        EXPECT_EQ(outcomes[0].accountUrl, outcomes[1].accountUrl) << "复用账户的那一轮又走了一次注册";

        // 两趟流程里 jwk 只出现在第一次：第二轮全程用 kid
        EXPECT_EQ(this->evidence().jwkBearingRequestCount, 1U) << "复用账户那一轮又在注册";
    }

    /**
     * @brief 钉住：没显式接受服务条款就不建账户，而不是替调用方默认接受
     */
    TEST_F(AcmeClientTest, RefusesToRegisterWithoutAcceptingTerms)
    {
        AcmeStubAuthority::Settings settings;
        settings.isTermsOfServicePublished = true;
        startFixtures(settings);

        m_isTermsOfServiceAccepted = false;
        const auto outcome         = runDefaultFlow();

        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::InvalidConfiguration);
        EXPECT_NE(outcome.failureMessage.find("服务条款"), std::string::npos) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().issuedOrderCount, 0U) << "没接受条款就不该走到下单";
    }

    /**
     * @brief 钉住：机构要求外部账户绑定而本机没配时，当场拒绝并说清要申领什么
     */
    TEST_F(AcmeClientTest, RefusesWhenExternalAccountBindingIsRequiredButAbsent)
    {
        AcmeStubAuthority::Settings settings;
        settings.isExternalAccountRequired = true;
        startFixtures(settings);

        const auto outcome = runDefaultFlow();

        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::InvalidConfiguration);
        EXPECT_NE(outcome.failureMessage.find("外部账户绑定"), std::string::npos) << outcome.failureMessage;
    }

    /**
     * @brief 钉住：配了正确的 EAB 就走通（桩会逐段核对 alg、kid、url、HMAC 与载荷）
     */
    TEST_F(AcmeClientTest, RegistersWithExternalAccountBinding)
    {
        AcmeStubAuthority::Settings settings;
        settings.isExternalAccountRequired = true;
        settings.externalAccountKeyId      = "stub-key-id-1";
        settings.externalAccountKeySecret  = Base::base64UrlEncode("stub-hmac-secret-material");
        startFixtures(settings);

        m_externalAccountKeyId     = settings.externalAccountKeyId;
        m_externalAccountKeySecret = settings.externalAccountKeySecret;

        const auto outcome = runDefaultFlow();
        ASSERT_TRUE(outcome.isSuccess) << static_cast<int>(outcome.failureKind) << " " << outcome.failureMessage;
        EXPECT_EQ(this->evidence().rejectedSignatureCount, 0U);
    }

    /**
     * @brief 钉住：EAB 的密钥配错时被机构拒掉，而不是拿着无效绑定把整单走完
     */
    TEST_F(AcmeClientTest, RejectsAWrongExternalAccountBindingSecret)
    {
        AcmeStubAuthority::Settings settings;
        settings.isExternalAccountRequired = true;
        settings.externalAccountKeyId      = "stub-key-id-1";
        settings.externalAccountKeySecret  = Base::base64UrlEncode("stub-hmac-secret-material");
        startFixtures(settings);

        m_externalAccountKeyId     = settings.externalAccountKeyId;
        m_externalAccountKeySecret = Base::base64UrlEncode("另一把不对的密钥");

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::RejectedByAuthority) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().issuedOrderCount, 0U) << "账户没建成就不该有订单";
    }

    /**
     * @brief 钉住：nonce 被机构判坏时客户端重取再发，而不是就此失败
     */
    TEST_F(AcmeClientTest, RetriesWhenTheAuthorityReportsBadNonce)
    {
        AcmeStubAuthority::Settings settings;
        settings.badNonceFaults = 1;
        startFixtures(settings);

        const auto outcome = runDefaultFlow();
        ASSERT_TRUE(outcome.isSuccess) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().badNonceReplyCount, 1U) << "故障注入的 badNonce 没出现，这条用例就没测到重试";
    }

    /**
     * @brief 钉住：机构先报 processing 时客户端真在轮询，直到终局才定稿
     */
    TEST_F(AcmeClientTest, PollsUntilTheAuthorityFinishesValidating)
    {
        AcmeStubAuthority::Settings settings;
        settings.challengeProcessingPolls = 3;
        startFixtures(settings);

        const auto outcome = runDefaultFlow();
        ASSERT_TRUE(outcome.isSuccess) << outcome.failureMessage;
        // 桩在授权没走成 valid 之前会拒掉定稿：能拿到证书就说明客户端确实等到了 valid
        EXPECT_EQ(this->evidence().issuedCertificateCount, 1U);
    }

    /**
     * @brief 钉住：机构只给非 http-01 的挑战时拒绝，而不是挑一条答不了的去做
     */
    TEST_F(AcmeClientTest, RefusesAChallengeItCannotAnswer)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startFixtures(settings);

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::UnexpectedResponse);
        // 要的是「本通路只实现 http-01」这条拒绝：挑了 dns-01 再去报缺 token，是同一种退化却被
        // 说成机构的毛病，判据不能允许
        EXPECT_NE(outcome.failureMessage.find("只实现 http-01"), std::string::npos) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().challengeFetchCount, 0U) << "拒了就不该再去答任何一条挑战";
    }

    /**
     * @brief 钉住：令牌没在应答时按「自证没答上」告状，而不是含糊成超时
     */
    TEST_F(AcmeClientTest, ReportsChallengeNotAnsweredWhenTheTokenIsMissing)
    {
        startFixtures({}, TokenAnswer::Absent);

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::ChallengeNotAnswered) << outcome.failureMessage;
        EXPECT_GE(this->evidence().challengeFetchCount, 1U);
    }

    /**
     * @brief 钉住：应答了错的 keyAuthorization 同样是自证失败，且文案带上桩读到的内容
     */
    TEST_F(AcmeClientTest, ReportsChallengeNotAnsweredWhenTheKeyAuthorizationIsWrong)
    {
        startFixtures({}, TokenAnswer::Wrong);

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::ChallengeNotAnswered);
        EXPECT_NE(outcome.failureMessage.find("keyAuthorization"), std::string::npos) << outcome.failureMessage;
    }

    /**
     * @brief 钉住：机构的 429 被折成限流一档，调用方据此退避而不是改配置
     */
    TEST_F(AcmeClientTest, MapsThrottlingToRateLimited)
    {
        AcmeStubAuthority::Settings settings;
        settings.isOrderRateLimited = true;
        startFixtures(settings);

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::RateLimited) << outcome.failureMessage;
        EXPECT_NE(outcome.failureMessage.find("退避"), std::string::npos) << outcome.failureMessage;
    }

    /**
     * @brief 钉住：没做完自证就去定稿会被机构拒掉，客户端如实交回而不是当成功
     */
    TEST_F(AcmeClientTest, FinalizeIsRefusedUntilAuthorizationsAreValid)
    {
        startFixtures({});

        FlowRequest request;
        request.domainNames      = kDefaultDomainNames;
        request.solvesChallenges = false;
        const auto outcome       = runFlow(std::move(request));

        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::RejectedByAuthority) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().issuedCertificateCount, 0U);
    }

    /**
     * @brief 钉住：本机记的账户已被机构删除时重新建号，而不是把 401 报给用户
     */
    TEST_F(AcmeClientTest, RegistersAgainWhenTheRecordedAccountIsGone)
    {
        startFixtures({});

        FlowRequest request;
        request.domainNames         = kDefaultDomainNames;
        m_isPersistedAccountUnknown = true;
        const auto outcome          = runFlow(std::move(request));

        ASSERT_TRUE(outcome.isSuccess) << static_cast<int>(outcome.failureKind) << " " << outcome.failureMessage;
        EXPECT_EQ(this->evidence().jwkBearingRequestCount, 1U) << "账户没了要重新注册，jwk 只该出现在这一次";
    }

    /**
     * @brief 钉住：不合规则的域名标识在本地就被拦下，一次下单都不发
     */
    TEST_F(AcmeClientTest, RejectsUnusableDomainIdentifiersLocally)
    {
        startFixtures({});

        FlowRequest request;
        request.domainNames = {"*.stub.example.com"};
        const auto outcome  = runFlow(std::move(request));

        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::InvalidConfiguration);
        EXPECT_NE(outcome.failureMessage.find("通配"), std::string::npos) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().issuedOrderCount, 0U) << "本地拦下了就不该去问机构";
    }

    /**
     * @brief 钉住：目录里缺必要端点时点名缺哪一个，而不是拿空 URL 继续签
     */
    TEST_F(AcmeClientTest, NamesTheMissingDirectoryEndpoint)
    {
        // 开关要在 startFixtures 之前置：那份代劳的目录是它注册的路由
        m_isDirectoryMissingNewNonce = true;
        startFixtures({});

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::UnexpectedResponse) << outcome.failureMessage;
        EXPECT_NE(outcome.failureMessage.find("newNonce"), std::string::npos) << "要点名缺的是哪个端点：" << outcome.failureMessage;
    }

    /**
     * @brief 钉住：目录地址根本不是机构时按「响应不成形」交回，文案带上那个地址
     */
    TEST_F(AcmeClientTest, NamesTheUnreachableDirectoryUrl)
    {
        startFixtures({});
        m_isBrokenDirectory = true;

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::UnexpectedResponse) << outcome.failureMessage;
        EXPECT_NE(outcome.failureMessage.find("no-such-directory"), std::string::npos) << outcome.failureMessage;
    }
} // namespace AsynGyanis::Net
