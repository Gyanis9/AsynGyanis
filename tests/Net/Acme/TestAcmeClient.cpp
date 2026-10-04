// ACME 客户端用例：拿一份进程内的桩颁发机构（真验签、真管 nonce、真回取令牌、真签证书）走完整台状态机，
// 再逐条测它的拒绝面与故障面。用例不联网，因此每一轮都能跑；真机构那条路另有按环境变量门控的用例。
#include "AcmeStubAuthority.h"
#include "AcmeTestSupport.h"
#include "Base/Coding/Base64.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Acme/AcmeClient.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
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

        /// 缺联系人那条 WARN 的识别串：取实现文案里独有的这一段，文案改了用例要点名跟着改
        constexpr std::string_view kMissingContactWarningMarker = "账户没有登记";

        /**
         * @brief 与 Sink 共享的消息表
         * @details 表与锁放在一起：写侧是记录日志的线程，用例只能读锁内拷出来的副本
         */
        struct MessageTable
        {
            mutable std::mutex       mutex;    ///< 保护下面那张表
            std::vector<std::string> messages; ///< 已收到的日志原文
        };

        /**
         * @brief 把 root 日志器收到的消息原文收进共享表，用于断言「这条提示到底报了没有」
         */
        class RecordingSink final : public Base::LogSink
        {
        public:
            /**
             * @brief 绑定共享表
             * @param table 用例创建并持有的表，Sink 只借它写字
             */
            explicit RecordingSink(std::shared_ptr<MessageTable> table) : m_table(std::move(table))
            {
            }

            /// @brief 记下一条消息的原文（本用例只关心 message 字段）
            void write(const Base::LogEvent &event) override
            {
                const std::lock_guard lock(m_table->mutex);
                m_table->messages.push_back(event.message);
            }

            /// 不落盘，没有缓冲需要刷新
            void flush() override
            {
            }

        private:
            std::shared_ptr<MessageTable> m_table; ///< 与用例共享的消息表
        };

        /**
         * @brief 数出含某个标记的消息条数
         * @param table 消息表
         * @param marker 要匹配的片段
         * @return std::size_t 命中的条数
         */
        std::size_t countMessagesContaining(const MessageTable &table, const std::string_view marker)
        {
            const std::lock_guard lock(table.mutex);
            return static_cast<std::size_t>(std::ranges::count_if(table.messages, [marker](const std::string &message) { return message.find(marker) != std::string::npos; }));
        }

        /**
         * @brief 往 root 日志器挂一个记录型 Sink，交出它写的那张表
         * @return std::shared_ptr<MessageTable> 用例侧的读取句柄，存在期独立于 Sink
         */
        std::shared_ptr<MessageTable> attachRecordingSink()
        {
            auto table = std::make_shared<MessageTable>();
            Base::LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<RecordingSink>(table));
            return table;
        }

        /**
         * @brief 作用域结束时换掉整棵 root 日志器，摘掉本用例挂上去的 Sink
         * @details Logger 只有 clearSinks() 而没有「摘掉单个 Sink」的口，沿用仓库既有的「整份换掉 root」做法；
         *          表由用例持有，换掉 root 之后仍读得到已收下的那些消息
         */
        class RootSinkScope
        {
        public:
            RootSinkScope()                                 = default;
            RootSinkScope(const RootSinkScope &)            = delete;
            RootSinkScope &operator=(const RootSinkScope &) = delete;

            ~RootSinkScope()
            {
                Base::LoggerRegistry::instance().clear();
            }
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
            /// 失败时机构给的 `Retry-After`（限流一档才有值，其余为空）
            std::optional<std::chrono::seconds> failureRetryAfter{};
            std::string                         certificatePem; ///< 成功时拿到的证书链
            std::string                         accountUrl;     ///< 本轮生效的账户 URL，供下一轮复用
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
        std::unique_ptr<AcmeKeyPair> m_accountKey;                                           ///< 账户密钥
        bool                         m_isBrokenDirectory{};                                  ///< 把目录地址换成应答方上一个不存在的路径
        bool                         m_isDirectoryMissingNewNonce{};                         ///< 目录换成「合法 JSON 但没有 newNonce」
        bool                         m_isPersistedAccountUnknown{};                          ///< 拿一个桩没记过账的账户 URL 去复用
        bool                         m_isTermsOfServiceAccepted{true};                       ///< 是否替调用方接受条款
        std::string                  m_externalAccountKeyId{};                               ///< 客户端侧的 EAB 标识
        std::string                  m_externalAccountKeySecret{};                           ///< 客户端侧的 EAB HMAC 密钥
        std::string                  m_contactEmailAddress{"mailto:acme-tests@example.com"}; ///< 账户联系人；留空即「不登记联系人」那一档
        /// 客户端要答哪一种自证挑战；默认 HTTP-01（与加这个字段之前的行为逐字相同），只有那两条
        /// 「配置与机构实际给的对不上」的用例改它
        AcmeChallengeKind m_challengeKind{AcmeChallengeKind::Http01};

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
            configuration.contactEmailAddress      = m_contactEmailAddress;
            configuration.isTermsOfServiceAccepted = m_isTermsOfServiceAccepted;
            configuration.requestTimeout           = kRequestTimeout;
            configuration.externalAccountKeyId     = m_externalAccountKeyId;
            configuration.externalAccountKeySecret = m_externalAccountKeySecret;
            configuration.challengeKind            = m_challengeKind;
            return configuration;
        }

        /// 把失败并上这之前已经拿到的部分结果一起记下，并交出「还要不要接着跑」
        template<typename Expected>
        bool recordFailure(const std::size_t index, const Expected &result, FlowOutcome outcome)
        {
            outcome.failureKind       = result.error().kind;
            outcome.failureMessage    = result.error().message;
            outcome.failureRetryAfter = result.error().retryAfter;
            m_outcomes[index]         = std::move(outcome);
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
                    // 被挑中的那一种一定存在：fetchAuthorization 里两种都没挑到就返回失败而不是交回空
                    const AcmeChallenge &challenge = authorization->http01.has_value() ? *authorization->http01 : *authorization->dns01;
                    auto                 solved    = co_await client.solveChallenge(challenge.challengeUrl, kPollInterval, kPollTimeout);
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
     * @brief 钉住：登记了联系人的那次注册把地址原样带上，且不报「没登记」
     * @details 这条是下一条的对照：桩记不到 contact（字段恒空）、或「没登记」的提示见人就报时，红的只有这条。
     *          缺联系人在 RFC 8555 下本来就合法，判据必须两半都有才分得开
     */
    TEST_F(AcmeClientTest, SendsTheConfiguredContactAndStaysQuietAboutIt)
    {
        RootSinkScope sinkScope;
        const auto    messages = attachRecordingSink();
        startFixtures({});

        const FlowOutcome outcome = runDefaultFlow();
        ASSERT_TRUE(outcome.isSuccess) << static_cast<int>(outcome.failureKind) << " " << outcome.failureMessage;

        EXPECT_EQ(evidence().registeredAccountContactText, m_contactEmailAddress) << "配置里的联系人没有出现在注册载荷里";
        EXPECT_EQ(countMessagesContaining(*messages, kMissingContactWarningMarker), 0U) << "登记了联系人还报「没登记」，那条提示就成了噪声";
    }

    /**
     * @brief 钉住：不登记联系人照样能建账户（RFC 合法），但要说一次原因
     * @details 拒掉会让「本来就留空联系人」的机构用不起来，静默放过又让 90 天寿命漏续变成无人预知的线上事故，
     *          所以这一档的形状是「继续做 + 出声」。出声只在注册那一次，重复提醒会让日志里全是同一条
     */
    TEST_F(AcmeClientTest, RegistersWithoutContactButSaysSoOnce)
    {
        RootSinkScope sinkScope;
        const auto    messages = attachRecordingSink();
        startFixtures({});
        m_contactEmailAddress.clear();

        const FlowOutcome outcome = runDefaultFlow();
        ASSERT_TRUE(outcome.isSuccess) << "缺联系人不该被拒：" << static_cast<int>(outcome.failureKind) << " " << outcome.failureMessage;

        EXPECT_EQ(evidence().registeredAccountContactText, "") << "配置里没给联系人，注册载荷里却带了：那是实现自己造的默认值";
        EXPECT_EQ(countMessagesContaining(*messages, kMissingContactWarningMarker), 1U) << "缺联系人要么没出声，要么每条请求都在重复提醒（只该在注册那一次说）";
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
     * @brief 钉住：机构 problem document 里的自由文本被折过才交出去
     * @details `detail` 是**远程自由文本**，JSON 里合法地可以携带换行（那边写成转义形式），解析回来
     *          就是真换行；而这条消息最终由续期管理器以 `LOG_ERROR_FMT("AcmeCertificateManager: {}", ...)`
     *          落成一行日志。原样带过去，就等于让一个不守规矩（或被劫持）的机构在本进程的日志里
     *          伪造记录——伪造的那半行可以顶着别的时间戳与级别。两条断言各管一头：消息里不许出现
     *          真换行（这是目的），且必须出现可见转义（这证明是「折起来」而不是「整段丢掉」）。
     */
    TEST_F(AcmeClientTest, EscapesControlCharactersComingFromTheAuthority)
    {
        AcmeStubAuthority::Settings settings;
        settings.rejectedDomainNames   = {"stub-first.example.com"};
        settings.injectedProblemDetail = std::string{"机构说明里的\n换行与"} + static_cast<char>(0x1B) + "这段 ESC";
        startFixtures(settings);

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::RejectedByAuthority) << outcome.failureMessage;
        EXPECT_EQ(outcome.failureMessage.find('\n'), std::string::npos) << "机构 detail 里的真换行原样进了消息：落到日志就是两条记录";
        EXPECT_NE(outcome.failureMessage.find("\\x0A"), std::string::npos) << "换行没被折成可见转义";
        EXPECT_NE(outcome.failureMessage.find("\\x1B"), std::string::npos) << "ESC 没被折掉：原样打出来会污染终端";
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
     * @brief 钉住：机构只给非「配置要的那一种」时拒绝，而不是挑一条答不了的去做
     */
    TEST_F(AcmeClientTest, RefusesAChallengeItCannotAnswer)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startFixtures(settings);

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::UnexpectedResponse);
        // 要的是「按配置要的是 http-01」这条拒绝：挑了 dns-01 再去报缺 token，是同一种退化却被
        // 说成机构的毛病，判据不能允许
        EXPECT_NE(outcome.failureMessage.find("要的是 http-01"), std::string::npos) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().challengeFetchCount, 0U) << "拒了就不该再去答任何一条挑战";
    }

    /**
     * @brief 钉住：配置切到 DNS-01 而机构只给 http-01 时，拒绝话术点名的是 dns-01
     * @details 与上一条凑成一对反向判据：文案跟着配置走，才说明「要哪一种」真被消费了，
     *          而不是写死在实现里的一句老话
     */
    TEST_F(AcmeClientTest, RefusesHttp01OnlyAuthorityWhenConfiguredForDns01)
    {
        AcmeStubAuthority::Settings settings;
        startFixtures(settings);
        m_challengeKind = AcmeChallengeKind::Dns01;

        const auto outcome = runDefaultFlow();
        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::UnexpectedResponse) << outcome.failureMessage;
        EXPECT_NE(outcome.failureMessage.find("要的是 dns-01"), std::string::npos) << outcome.failureMessage;
        EXPECT_NE(outcome.failureMessage.find("http-01"), std::string::npos) << "拒绝话术要列出机构实际给了哪几种";
        EXPECT_EQ(this->evidence().challengeFetchCount, 0U) << "没挑到挑战就不该去答";
    }

    /**
     * @brief 钉住：配置与机构对得上时不再走「挑不到挑战」那条拒绝
     * @details 这条只判「没被拒绝」，不判整单走通——桩机构答不了 DNS-01 的校验（它按取令牌那条路
     *          判 valid），把 TXT 那条路真接上是 DNS 提供方那一轮的事。放在这里是因为少了它，
     *          上一条与「选择永远失败」那种实现就分不开
     */
    TEST_F(AcmeClientTest, SelectsDns01WhenBothSidesAgreeOnIt)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startFixtures(settings);
        m_challengeKind = AcmeChallengeKind::Dns01;

        const auto outcome = runDefaultFlow();
        EXPECT_EQ(outcome.failureMessage.find("只提供了这些挑战类型"), std::string::npos) << outcome.failureMessage;
    }

    /**
     * @brief 钉住：DNS-01 的 TXT 正文等于独立实现算出的 base64url(SHA-256(keyAuthorization))
     * @details 期望值来自 Node 的 crypto（与 OpenSSL/base64 通路都不同源），写死成字面量：
     *          用被测同一个 helper 算期望，等于「规范怎么改都绿」。第三个向量是空串，钉的是
     *          「无填充」这条——带 `=` 的写法机构直接判 invalid
     */
    TEST(AcmeDns01ValidationText, MatchesIndependentlyComputedDigests)
    {
        EXPECT_EQ(dns01ValidationText("EVstcErua-a8EKJvihxZd1nbLFiqsFf7.wwdt6DDcpLZcmWN7ZU8hgpjPvjz3w24xpXDWYA7BA8U"), "xUlhfYWyr3ScAdMmWf7gs_oiljhsvAxhGEi45qQiz8o");
        EXPECT_EQ(dns01ValidationText("token.thumbprint"), "61rBZ_4knHblO0MNoxFsXZ_eTFUHum0B6IVRbhvUn5I");
        EXPECT_EQ(dns01ValidationText(""), "47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU");

        const std::string text = dns01ValidationText("token.thumbprint");
        EXPECT_EQ(text.size(), 43U) << "SHA-256 的 URL-safe 无填充 Base64 恒为 43 字符";
        EXPECT_EQ(text.find('='), std::string::npos) << "带填充的 TXT 值机构不认";
    }

    /**
     * @brief 钉住：单次出站请求的时限以「那轮操作的总时限」为上界，两边都不许越过
     * @details 签发管理器曾把总时限原样赋给单次请求的时限，于是内层等于外层：一次卡住的请求就能吃满
     *          整轮预算，而外层那道闸每轮之间才查一次，永远轮不到它开火——报错里那句「在总时限内没走到
     *          终局」也就永远不会由它自己说出来。一轮签发是一串请求（目录、nonce、账户、订单、每次轮询），
     *          总耗时因此成了总时限的若干倍。
     * @details 反向的那一格同样要成立：总时限配得比默认单次时限还小时，它就是上界——「配得更小」必须
     *          等于「更严」，否则把 180 秒改成 3 秒不会让任何东西提前收口。
     * @details 这条钉的是折算出口本身，钉不到签发管理器里那一行装配：管理器用例的总时限是 8 秒，比默认
     *          单次时限还小，「调折算」与「原样赋值」算出同一个数。要连那一行一起钉，得让桩机构对某一次
     *          请求拖过 10 秒——那是一条 10 秒起步的用例，这一轮没付这个代价。
     */
    TEST(AcmeClientConfiguration, BoundsPerRequestTimeoutByTheOverallBudget)
    {
        using Config = AcmeClient::Configuration;

        // 正向：总时限很大，也不能把单次请求抬过本类的默认值
        EXPECT_EQ(Config::boundedRequestTimeout(std::chrono::seconds{180}), Config::kDefaultRequestTimeout);
        // 反向：总时限比默认单次时限还小时，取总时限
        EXPECT_EQ(Config::boundedRequestTimeout(std::chrono::seconds{3}), std::chrono::seconds{3});
        // 边界：两边相等时不该出现「取哪边都一样」之外的漂移
        EXPECT_EQ(Config::boundedRequestTimeout(Config::kDefaultRequestTimeout), Config::kDefaultRequestTimeout);

        // 不变式：折算出来的数既不超过总时限，也不超过默认单次时限（两条任缺一条就是这次的缺陷形状）
        for (const std::chrono::milliseconds overall:
             {std::chrono::milliseconds{0}, std::chrono::milliseconds{1000}, Config::kDefaultRequestTimeout, std::chrono::milliseconds{300000}})
        {
            const auto bounded = Config::boundedRequestTimeout(overall);
            EXPECT_LE(bounded, overall) << "折算后的单次时限超过了总时限：" << bounded.count() << " > " << overall.count();
            EXPECT_LE(bounded, Config::kDefaultRequestTimeout);
        }
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
        // 桩在 429 上带的是 `Retry-After: 3600`（RFC 8555 §6.8 要求机构必须带、客户端必须照办）。
        // 这一项以前被解析出来又当场丢掉，于是「该退避」只写在注释里
        ASSERT_TRUE(outcome.failureRetryAfter.has_value()) << "机构的 Retry-After 没有交回给调用方：退避只能靠猜";
        EXPECT_EQ(outcome.failureRetryAfter->count(), 3600) << "Retry-After 的秒数应当原样交回，不夹本地默认";
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
     * @brief 钉住：换成 dns-01 之后，通配标识不再被本地拦（RFC 8738 就是为通配开的这条路）
     * @details 判据挂在「挑了哪种挑战」上而不是配置字段名：同一份域名表，http-01 拒、dns-01 放行
     */
    TEST_F(AcmeClientTest, AllowsWildcardIdentifiersWhenConfiguredForDns01)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startFixtures(settings);
        m_challengeKind = AcmeChallengeKind::Dns01;

        FlowRequest request;
        request.domainNames = {"*.stub.example.com"};
        const auto outcome  = runFlow(std::move(request));

        EXPECT_EQ(this->evidence().issuedOrderCount, 1U) << "dns-01 验得了通配，本地不该拦：" << outcome.failureMessage;
        EXPECT_EQ(outcome.failureMessage.find("通配"), std::string::npos) << outcome.failureMessage;
    }

    /**
     * @brief 钉住：dns-01 下通配仍只认「* 占满最左一段标签」那一种写法
     * @details 机构对 `a*b.example.com` 这类写法回的是 rejectedIdentifier，而那条原文不 telling
     *          是形状不对还是权限不对；本地按形状拒更快也更准
     */
    TEST_F(AcmeClientTest, RejectsMalformedWildcardIdentifiers)
    {
        AcmeStubAuthority::Settings settings;
        settings.offeredChallengeType = "dns-01";
        startFixtures(settings);
        m_challengeKind = AcmeChallengeKind::Dns01;

        FlowRequest request;
        request.domainNames = {"a*b.stub.example.com"};
        const auto outcome  = runFlow(std::move(request));

        EXPECT_FALSE(outcome.isSuccess);
        EXPECT_EQ(outcome.failureKind, AcmeErrorKind::InvalidConfiguration);
        EXPECT_NE(outcome.failureMessage.find("最左一段标签"), std::string::npos) << outcome.failureMessage;
        EXPECT_EQ(this->evidence().issuedOrderCount, 0U) << "形状不合的通配不该发去机构";
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
