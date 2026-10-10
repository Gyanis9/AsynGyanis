// 中间件单元测试：管道顺序与短路、日志、CORS、HTTP/3 端点通告、协作式超时、体积与速率限制
#include "Net/Http/Middleware.h"

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Http/Gzip.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

#include "CoreTestSupport.h"

#include "NetTestSupport.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 等待类断言的轮询上限，避免固定 sleep 硬等，同时防止用例卡死
        constexpr auto kConditionTimeout = std::chrono::milliseconds(2000);

        /// 协作式 handler 的自查上界：即便中间件彻底失灵，业务侧也不会永远占着事件循环
        constexpr auto kHandlerHardStop = std::chrono::milliseconds(800);

        /// 触发超时用的截止时长，量级取「比一个睡眠分片大、比测试上界小得多」
        constexpr auto kShortTimeout = std::chrono::milliseconds(20);

        /// 不触发超时用的宽松截止时长
        constexpr auto kGenerousTimeout = std::chrono::milliseconds(500);

        /// 速率限制窗口过期用例用的窗口长度
        constexpr auto kRateLimitWindow = std::chrono::milliseconds(20);

        /**
         * @brief 在超时上限内逐毫秒轮询等待条件成立（定义见 CoreTestSupport.h）
         * @note 本组用例都是内存内驱动，等待上界比模块默认更短，调用点一律显式传 kConditionTimeout
         */
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 构造一条只填了方法、URI 与版本的请求，中间件读的就是这几样
         * @param method 请求方法
         * @param uri    原始 URI
         * @return HttpRequest 请求对象
         */
        HttpRequest makeRequest(const HttpMethod method, std::string uri)
        {
            HttpRequest request;
            request.setMethod(method);
            request.setUri(std::move(uri));
            request.setHttpVersion("HTTP/1.1");
            return request;
        }

        /**
         * @brief 什么都不做的终点处理器，用于只关心中间件自身行为的用例
         * @return TerminalHandler 空终点
         */
        TerminalHandler emptyTerminal()
        {
            return []() -> Core::Task<void> { co_return; };
        }

        /**
         * @brief 生成一个「写下固定正文并计数」的终点处理器
         * @param response    要被写入的响应对象，生命周期必须覆盖整条链
         * @param bodyText    正文文本
         * @param callCounter 调用次数计数器，可为空指针
         * @return TerminalHandler 管道终点
         */
        TerminalHandler terminalWriting(HttpResponse &response, std::string bodyText, std::atomic<int> *callCounter = nullptr)
        {
            return [&response, bodyText = std::move(bodyText), callCounter]() -> Core::Task<void>
            {
                if (callCounter != nullptr)
                {
                    callCounter->fetch_add(1);
                }
                response.setBody(bodyText);
                co_return;
            };
        }

        /**
         * @brief 同步跑完一条中间件链（链上没有任何真实挂起点时使用）
         * @param pipeline 被测管道
         * @param request  请求对象
         * @param response 响应对象
         * @param handler  终点处理器，必须活到链路结束
         */
        void runPipeline(MiddlewarePipeline &pipeline, HttpRequest &request, HttpResponse &response, const TerminalHandler &handler)
        {
            Core::Task<> chainTask = pipeline.run(request, response, handler);
            chainTask.handle().resume();
            ASSERT_TRUE(chainTask.isReady()) << "中间件链未在同步路径上跑完：测试链里不得真实挂起";
            chainTask.handle().promise().result();
        }

        /**
         * @brief 记录型 Sink 的共享容器：Sink 被日志器接管后仍能读回内容
         */
        struct LogRecords
        {
            std::mutex                  mutex;    ///< 保护下面两个向量
            std::vector<std::string>    messages; ///< 已记录的日志正文
            std::vector<Base::LogLevel> levels;   ///< 已记录的日志等级
        };

        /**
         * @brief 只在内存里累积日志事件的测试 Sink
         */
        class RecordingSink final : public Base::LogSink
        {
        public:
            /**
             * @brief 构造记录型 Sink
             * @param records 与测试共享的记录容器
             */
            explicit RecordingSink(std::shared_ptr<LogRecords> records) : m_records(std::move(records))
            {
            }

            /**
             * @brief 把事件正文与等级追加进共享容器
             * @param event 日志事件
             */
            void write(const Base::LogEvent &event) override
            {
                const std::lock_guard<std::mutex> recordLock(m_records->mutex);
                m_records->messages.push_back(event.message);
                m_records->levels.push_back(event.level);
            }

            /**
             * @brief 内容全在内存里，没有缓冲需要落盘，故为空实现
             */
            void flush() override
            {
            }

        private:
            std::shared_ptr<LogRecords> m_records; ///< 与测试共享的记录容器
        };

        /**
         * @brief 造一个只挂记录型 Sink 的日志器
         * @param records 与测试共享的记录容器
         * @return std::shared_ptr<const Base::Logger> loggingMiddleware 可直接持有
         */
        std::shared_ptr<const Base::Logger> makeRecordingLogger(const std::shared_ptr<LogRecords> &records)
        {
            auto logger = std::make_shared<Base::Logger>("http-test");
            logger->addSink(std::make_unique<RecordingSink>(records));
            return logger;
        }

        /**
         * @brief 测试协程：周期性检查取消令牌，观察到取消后立刻收工
         * @details 这是协作式超时下业务 handler 的标准形态——自己让出 CPU，并定期看 stop_requested()。
         *          同时带一个硬上界，绝不把事件循环和测试一起吊死。
         * @param loop           事件循环，定时器取自它
         * @param request        请求对象，读取消令牌
         * @param response       响应对象
         * @param observedCancel 出参：handler 是否真的看到了取消信号
         * @return Core::Task<> 协程
         */
        Core::Task<void> cooperativeHandler(Core::EventLoop &loop, HttpRequest &request, HttpResponse &response, std::atomic<bool> &observedCancel)
        {
            Core::Timer timer(loop);
            const auto  hardStop = std::chrono::steady_clock::now() + kHandlerHardStop;

            while (!request.cancelToken().stop_requested())
            {
                if (std::chrono::steady_clock::now() >= hardStop)
                {
                    break;
                }
                co_await timer.waitFor(std::chrono::milliseconds(1));
            }

            observedCancel.store(request.cancelToken().stop_requested());
            response.setBody("业务链自己的正文");
            co_return;
        }

        /**
         * @brief 测试协程：跑完整条链并置位结束标志，供事件循环线程外的轮询使用
         * @param pipeline        被测管道
         * @param request         请求对象
         * @param response        响应对象
         * @param handler         终点处理器，必须活到链路结束
         * @param exceptionCaught 出参：链上是否抛出异常
         * @param isFinished      出参：链路是否收口
         * @return Core::Task<> 协程
         */
        Core::Task<void> runChainOnLoop(MiddlewarePipeline &pipeline, HttpRequest &request, HttpResponse &response, const TerminalHandler &handler,
                                        std::atomic<bool> &exceptionCaught, std::atomic<bool> &isFinished)
        {
            try
            {
                co_await pipeline.run(request, response, handler);
            } catch (const std::exception &)
            {
                exceptionCaught.store(true);
            }
            isFinished.store(true);
            co_return;
        }
    } // namespace

    /**
     * @brief 带后台事件循环线程的夹具：超时中间件需要真实的定时器与调度器驱动
     */
    class MiddlewareLoopFixture : public ::testing::Test
    {
    protected:
        /**
         * @brief 在后台线程启动事件循环，并轮询等待它真正进入运行态
         */
        void SetUp() override
        {
            m_worker = std::thread([this]() { m_loop.run(); });
            if (!waitForCondition([this]() { return m_loop.isRunning(); }, kConditionTimeout))
            {
                stopLoop();
                GTEST_SKIP() << "事件循环未能启动，超时相关用例无法驱动";
            }
        }

        /**
         * @brief 停止并回收事件循环线程
         */
        void TearDown() override
        {
            stopLoop();
        }

        /**
         * @brief 把协程投到事件循环上跑完，收口后停掉循环，再把结果交给调用方
         *
         * @details 判据只用协程自己在循环线程上置位的原子标记——**不能**顺手读 Task::isReady()：
         *          那是在外部线程读协程帧，与循环线程写同一块内存（TSan 的并发用例集报的正是它，
         *          Windows 上因为时序凑巧看不出来）。收口之后本方法会停掉并 join 循环：任务帧紧接着
         *          就会被调用方销毁，而销毁必须发生在循环线程停手之后，否则协程的收尾阶段仍在碰这块帧。
         * @param task         待执行的协程任务，所有权仍归调用方
         * @param finishedFlag 链路收口时由协程自己置位的原子标记
         * @return true 链路在时限内完成
         */
        bool runOnLoop(Core::Task<void> &task, std::atomic<bool> &finishedFlag)
        {
            m_loop.scheduler().scheduleRemote(task.handle());

            const bool isCompleted = waitForCondition([&finishedFlag]() { return finishedFlag.load(); }, kConditionTimeout);

            // 跑完与超时都停循环：前者是为了让调用方安全销毁任务帧，后者是为了别让后台线程
            // 继续碰测试栈上的对象。stopLoop() 幂等，TearDown 再调一次无副作用
            stopLoop();
            return isCompleted;
        }

        Core::EventLoop m_loop; ///< 供中间件与定时器使用的事件循环

    private:
        /**
         * @brief 幂等地停止循环并 join 工作线程
         */
        void stopLoop()
        {
            m_loop.stop();
            if (m_worker.joinable())
            {
                m_worker.join();
            }
        }

        std::thread m_worker; ///< 承载 loop.run() 的后台线程
    };

    // ============================================================================
    // MiddlewarePipeline：顺序与短路
    // ============================================================================

    TEST(MiddlewarePipeline, RunsTerminalHandlerWhenEmpty)
    {
        MiddlewarePipeline pipeline;
        HttpRequest        request = makeRequest(HttpMethod::GET, "/bare");
        HttpResponse       response;
        std::atomic<int>   handlerCalls{0};

        EXPECT_EQ(pipeline.middlewareCount(), 0U);
        runPipeline(pipeline, request, response, terminalWriting(response, "bare", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.body(), "bare");
    }

    TEST(MiddlewarePipeline, CountsRegisteredMiddlewares)
    {
        MiddlewarePipeline pipeline;
        const auto         passthrough = [](HttpRequest &, HttpResponse &, const std::function<Core::Task<void>()> next) -> Core::Task<> { co_await next(); };

        pipeline.use(passthrough);
        pipeline.use(passthrough);

        EXPECT_EQ(pipeline.middlewareCount(), 2U);
    }

    TEST(MiddlewarePipeline, EntersInRegistrationOrderAndExitsInReverse)
    {
        MiddlewarePipeline       pipeline;
        std::vector<std::string> executionOrder;

        const auto recordingMiddleware = [](const std::string &name, std::vector<std::string> &order) -> MiddlewareFunc
        {
            return [&order, name](HttpRequest &, HttpResponse &, const std::function<Core::Task<void>()> next) -> Core::Task<>
            {
                order.push_back(name + ":前置");
                co_await next();
                order.push_back(name + ":后置");
            };
        };
        pipeline.use(recordingMiddleware("outer", executionOrder));
        pipeline.use(recordingMiddleware("inner", executionOrder));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/onion");
        HttpResponse          response;
        const TerminalHandler handler = [&executionOrder, &response]() -> Core::Task<void>
        {
            executionOrder.push_back("handler");
            response.setBody("onion");
            co_return;
        };
        runPipeline(pipeline, request, response, handler);

        const std::vector<std::string> expectedOrder{"outer:前置", "inner:前置", "handler", "inner:后置", "outer:后置"};
        EXPECT_EQ(executionOrder, expectedOrder);
    }

    TEST(MiddlewarePipeline, StopsWhenMiddlewareSkipsNext)
    {
        MiddlewarePipeline       pipeline;
        std::atomic<int>         innerCalls{0};
        std::atomic<int>         handlerCalls{0};
        std::vector<std::string> executionOrder;

        pipeline.use(
                [&executionOrder](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()>) -> Core::Task<>
                {
                    executionOrder.push_back("outer:短路");
                    response.setStatus(503);
                    response.setBody("service unavailable");
                    co_return;
                });
        pipeline.use(
                [&innerCalls](HttpRequest &, HttpResponse &, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    innerCalls.fetch_add(1);
                    co_await next();
                });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/short");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "unreachable", &handlerCalls));

        EXPECT_EQ(innerCalls.load(), 0);
        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 503);
        EXPECT_EQ(response.body(), "service unavailable");
        const std::vector<std::string> expectedOrder{"outer:短路"};
        EXPECT_EQ(executionOrder, expectedOrder);
    }

    TEST(MiddlewarePipeline, LetsMiddlewareRewriteResponseAfterDownstream)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(
                [](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    co_await next();
                    // 后置改写落在业务之后：正文最终由它说了算
                    response.setHeader("x-powered-by", "middleware");
                    response.setBody("rewritten");
                });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/rewrite");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "original"));

        EXPECT_EQ(response.body(), "rewritten");
        EXPECT_EQ(response.getHeader("x-powered-by").value_or(""), "middleware");
    }

    TEST(MiddlewarePipeline, PropagatesHandlerExceptionToCaller)
    {
        MiddlewarePipeline    pipeline;
        HttpRequest           request = makeRequest(HttpMethod::GET, "/throws");
        HttpResponse          response;
        const TerminalHandler handler = []() -> Core::Task<void>
        {
            throw std::runtime_error("handler failed");
            co_return;
        };

        auto chainTask = pipeline.run(request, response, handler);
        chainTask.handle().resume();
        ASSERT_TRUE(chainTask.isReady());
        EXPECT_THROW(static_cast<void>(chainTask.handle().promise().result()), std::runtime_error);
    }

    // ============================================================================
    // loggingMiddleware
    // ============================================================================

    TEST(LoggingMiddleware, PassesRequestThroughWhenLoggerPointerIsNull)
    {
        MiddlewarePipeline pipeline;
        // 空指针即「不记录」：上层无需为「日志可选」再包一层条件判断
        pipeline.use(loggingMiddleware(nullptr));

        HttpRequest      request = makeRequest(HttpMethod::GET, "/quiet");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "quiet", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.body(), "quiet");
    }

    TEST(LoggingMiddleware, RecordsUriAndStatusCodeAfterDownstreamCompletes)
    {
        auto               records = std::make_shared<LogRecords>();
        MiddlewarePipeline pipeline;
        pipeline.use(loggingMiddleware(makeRecordingLogger(records)));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/logged?detail=1");
        HttpResponse response;
        response.setStatus(201);
        runPipeline(pipeline, request, response, terminalWriting(response, "logged"));

        const std::lock_guard<std::mutex> recordLock(records->mutex);
        ASSERT_EQ(records->messages.size(), 1U);
        EXPECT_EQ(records->levels[0], Base::LogLevel::Info);
        EXPECT_TRUE(containsText(records->messages[0], "/logged?detail=1"));
        EXPECT_TRUE(containsText(records->messages[0], "201"));
    }

    TEST(LoggingMiddleware, RecordsShortCircuitedResponseStatus)
    {
        auto               records = std::make_shared<LogRecords>();
        MiddlewarePipeline pipeline;
        // 日志中间件在最外层，业务被限流短路也要留下痕迹
        pipeline.use(loggingMiddleware(makeRecordingLogger(records)));
        pipeline.use(rateLimiterMiddleware(0, std::chrono::seconds(60)));

        HttpRequest      request = makeRequest(HttpMethod::GET, "/burst");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "unreachable", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        const std::lock_guard<std::mutex> recordLock(records->mutex);
        ASSERT_EQ(records->messages.size(), 1U);
        EXPECT_TRUE(containsText(records->messages[0], "429"));
    }

    // ============================================================================
    // corsMiddleware
    // ============================================================================

    TEST(CorsMiddleware, AnswersPreflightWithoutEnteringDownstream)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(CorsPolicy{}));

        HttpRequest request = makeRequest(HttpMethod::OPTIONS, "/api/data");
        request.addHeader("Origin", "https://client.test");
        request.addHeader("Access-Control-Request-Method", "POST");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "business", &handlerCalls));

        // 预检就地应答并短路，业务 handler 根本不该收到这个 OPTIONS
        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 204);
        EXPECT_TRUE(response.body().empty());
        EXPECT_EQ(response.getHeader("access-control-allow-origin").value_or(""), "*");
        EXPECT_EQ(response.getHeader("vary").value_or(""), "Origin");
    }

    /**
     * @brief 钉住 CORS 补 `Vary: Origin` 时不盖掉前一条中间件已经写上的 token
     * @details `Vary` 是列表头，而 `setHeader` 是整条换掉。外层中间件先写 `Accept-Language`、CORS 再写
     *          `Origin`，修复前这条响应上的 Vary 只剩 `Origin`——共享缓存据此分桶，等于把
     *          A 来源（或 A 语言）的应答发给另一个来源的请求方，而这是 CORS 与压缩串在一起时最容易
     *          撞上的形状：两条都要动 Vary，而其中一条按「整条换掉」的写法写。
     *          框架里动 Vary 的出口只留一份（`Detail::appendVaryToken`，压缩补 `accept-encoding` 走的
     *          就是它），本用例钉的就是「两条中间件串起来，两个 token 都在」。
     * @note 证伪：把 CORS 那两处换回 `response.setHeader("vary", "Origin")`，本用例红在
     *       「前一个 token 还在」那一格（只剩 `Origin`）。
     */
    TEST(CorsMiddleware, AppendsOriginTokenWithoutClobberingExistingVary)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(
                [](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    response.setHeader("vary", "Accept-Language");
                    co_await next();
                });
        pipeline.use(corsMiddleware(CorsPolicy{}));

        HttpRequest request = makeRequest(HttpMethod::GET, "/api/data");
        request.addHeader("Origin", "https://client.test");
        HttpResponse          response;
        const TerminalHandler handler = [&response]() -> Core::Task<void>
        {
            response.setBody("business");
            co_return;
        };
        runPipeline(pipeline, request, response, handler);

        const std::string vary = response.getHeader("vary").value_or("");
        EXPECT_NE(vary.find("Accept-Language"), std::string::npos) << "CORS 把前一条中间件写的 Vary 整条盖掉了：缓存分桶少了一个维度";
        EXPECT_NE(vary.find("Origin"), std::string::npos) << "CORS 自己的 token 也得在";
        EXPECT_EQ(response.body(), "business") << "短路判据的反面对照：业务应当照常跑完";
    }

    /**
     * @brief 钉住 CORS 补 `Origin` 时按大小写不敏感去重，不把别人写过的 token 再补一遍
     * @details 外层已经写了小写 `origin`（头部 token 大小写不敏感是 RFC 9110 §5.1 的口径），CORS 再补
     *          一次就成了 `origin, Origin`：读的人不会多拿信息，而这条头的长度会被缓存与代理逐跳复制。
     * @note 证伪：把 `Detail::appendVaryToken` 的命中即返回那句摘掉，本用例红在整串相等那一格。
     */
    TEST(CorsMiddleware, DoesNotDuplicateOriginTokenAlreadyPresentInVary)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(
                [](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    response.setHeader("vary", "origin");
                    co_await next();
                });
        pipeline.use(corsMiddleware(CorsPolicy{}));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse          response;
        const TerminalHandler handler = [&response]() -> Core::Task<void>
        {
            response.setBody("business");
            co_return;
        };
        runPipeline(pipeline, request, response, handler);

        EXPECT_EQ(response.getHeader("vary").value_or(""), "origin") << "同一个 token 被写了两遍：去重走的是追加那一份出口";
    }

    TEST(CorsMiddleware, ResetsResponseBeforeWritingPreflightAnswer)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(CorsPolicy{}));

        HttpRequest request = makeRequest(HttpMethod::OPTIONS, "/api/data");
        request.addHeader("Access-Control-Request-Method", "PUT");
        HttpResponse response;
        response.setStatus(500);
        response.setBody("上一轮残留");
        ASSERT_TRUE(response.setHeader("x-leftover", "1"));
        runPipeline(pipeline, request, response, emptyTerminal());

        EXPECT_FALSE(response.getHeader("x-leftover").has_value());
        EXPECT_TRUE(response.body().empty());
        EXPECT_EQ(response.status(), 204);
    }

    /**
     * @brief 钉住 CORS 策略在**装配期**就把「会被 setHeader 静默拒写的取值」挡下来
     * @details 三个取值都是原样上线的字段值。含 CR/LF/NUL 时 `HttpResponse::setHeader` 拒写并回
     *          false，而中间件不看那个 bool——留到请求期，现场表现只有「浏览器说 CORS 不通」，
     *          服务端一句日志都没有。判据复用 `setHeader` 内部那一份谓词（与 `altSvcMiddleware`
     *          对端点主机名那条同一把尺），所以装配期与写入期永远不会分出两套答案。
     * @note 拒因必须点名是哪个字段：运维拿着一份配置文件看不出自己错在哪，等于没给拒因。
     * @note 正面对照由既有的 `DeclaresConfiguredMethodsHeadersAndMaxAge` 承担（合法策略照装照写）；
     *       本例只钉拒绝面。
     * @note 证伪：把装配期那三行 `requireWritableFieldValue` 摘掉，本用例红在 `ADD_FAILURE`
     *       （不抛就等于让脏值走到请求期去被静默拒写）；把三处合成一处漏判一个字段，红的是
     *       那一格的「拒因点名」断言。
     */
    TEST(CorsMiddleware, RejectsPolicyValuesThatSetHeaderWouldRefuse)
    {
        for (const std::string_view fieldName: {"allowOrigin", "allowMethods", "allowHeaders"})
        {
            SCOPED_TRACE(fieldName);

            CorsPolicy policy;
            if (fieldName == "allowOrigin")
            {
                policy.allowOrigin = "https://a.example\r\nX-Injected: 1";
            } else if (fieldName == "allowMethods")
            {
                policy.allowMethods = "GET\r\nPost: 1";
            } else
            {
                policy.allowHeaders = "Content-Type\nX-Other";
            }

            try
            {
                static_cast<void>(corsMiddleware(policy));
                ADD_FAILURE() << "这种值会被 setHeader 静默拒写：装配期就该拒，而不是留到请求期丢头";
            } catch (const Base::InvalidArgumentException &failure)
            {
                EXPECT_NE(std::string(failure.what()).find(fieldName), std::string::npos) << "拒因没点名是哪个字段";
            }
        }
    }

    /**
     * @brief 钉住 CORS 的预检缓存秒数不接受负值
     * @details 0 是合法档位（要求每次都发预检），负数不是：浏览器按「不缓存」处理，
     *          配的人以为自己在设一个时限而实际上把这一格配废了。与 `altSvcMiddleware` 对
     *          `maxAge` 的那道界同一处置：装配期当场拒。
     * @note 证伪：把 `maxAge.count() < 0` 那道判定摘掉，本用例红（不再抛）。
     */
    TEST(CorsMiddleware, RejectsNegativePreflightMaxAge)
    {
        CorsPolicy policy;
        policy.maxAge = std::chrono::seconds{-5};
        EXPECT_THROW(static_cast<void>(corsMiddleware(policy)), Base::InvalidArgumentException) << "负秒数等于把 maxAge 这一格配废，装配期就该拒";
    }

    TEST(CorsMiddleware, DeclaresConfiguredMethodsHeadersAndMaxAge)
    {
        CorsPolicy policy;
        policy.allowMethods = "GET, POST";
        policy.allowHeaders = "X-Custom";
        policy.maxAge       = std::chrono::seconds(0);
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(policy));

        HttpRequest request = makeRequest(HttpMethod::OPTIONS, "/api/data");
        request.addHeader("Access-Control-Request-Method", "POST");
        HttpResponse response;
        runPipeline(pipeline, request, response, emptyTerminal());

        EXPECT_EQ(response.getHeader("access-control-allow-methods").value_or(""), "GET, POST");
        EXPECT_EQ(response.getHeader("access-control-allow-headers").value_or(""), "X-Custom");
        // maxAge 为 0 表示要求每次都发预检，仍要把 0 明确写出去
        EXPECT_EQ(response.getHeader("access-control-max-age").value_or(""), "0");
        // 预检应答没有正文，204 也不该被自动补上 content-length
        EXPECT_FALSE(containsText(response.toString(), "content-length"));
    }

    TEST(CorsMiddleware, OmitsCredentialsWhenOriginIsWildcard)
    {
        CorsPolicy policy;
        policy.allowCredentials = true;
        policy.allowOrigin      = "*";
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(policy));

        HttpRequest preflight = makeRequest(HttpMethod::OPTIONS, "/api/data");
        preflight.addHeader("Access-Control-Request-Method", "POST");
        HttpResponse preflightResponse;
        runPipeline(pipeline, preflight, preflightResponse, emptyTerminal());

        // 通配来源与凭据不能共存：一起发出去浏览器一定判失败
        EXPECT_FALSE(preflightResponse.getHeader("access-control-allow-credentials").has_value());

        HttpRequest  actual = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse actualResponse;
        runPipeline(pipeline, actual, actualResponse, emptyTerminal());
        EXPECT_FALSE(actualResponse.getHeader("access-control-allow-credentials").has_value());
    }

    TEST(CorsMiddleware, DeclaresCredentialsForSpecificOrigin)
    {
        CorsPolicy policy;
        policy.allowOrigin      = "https://app.example";
        policy.allowCredentials = true;
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(policy));

        HttpRequest      request = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "data", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.getHeader("access-control-allow-origin").value_or(""), "https://app.example");
        EXPECT_EQ(response.getHeader("access-control-allow-credentials").value_or(""), "true");
    }

    TEST(CorsMiddleware, LetsRequestWithoutRequestedMethodHeaderThrough)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(CorsPolicy{}));

        // 没有 Access-Control-Request-Method 的 OPTIONS 不是预检，应当交给业务路由
        HttpRequest      request = makeRequest(HttpMethod::OPTIONS, "/api/data");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "options-answer", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "options-answer");
    }

    TEST(CorsMiddleware, SetsOriginHeadersBeforeDownstreamRuns)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(CorsPolicy{}));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse          response;
        std::string           originSeenByHandler;
        const TerminalHandler handler = [&response, &originSeenByHandler]() -> Core::Task<void>
        {
            originSeenByHandler = response.getHeader("access-control-allow-origin").value_or("");
            response.setBody("data");
            co_return;
        };
        runPipeline(pipeline, request, response, handler);

        // 头要在下游之前挂好：业务既能读到也能自行改写
        EXPECT_EQ(originSeenByHandler, "*");
        EXPECT_EQ(response.getHeader("access-control-allow-origin").value_or(""), "*");
    }

    // ============================================================================
    // altSvcMiddleware（RFC 7838 的 HTTP/3 端点通告）
    // ============================================================================

    TEST(AltSvcMiddleware, AdvertisesHttp3PortWithDefaultMaxAge)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(altSvcMiddleware(8443));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/index.html");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        // 逐字节比对整条字段值：端口漏写、ma 单位写错、引号位置不对都会在这里露出来
        EXPECT_EQ(response.getHeader("alt-svc").value_or(""), "h3=\":8443\"; ma=86400");
    }

    TEST(AltSvcMiddleware, AdvertisesOnHttp2RequestAsWell)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(altSvcMiddleware(8443));

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        request.setHttpVersion("HTTP/2");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        EXPECT_EQ(response.getHeader("alt-svc").value_or(""), "h3=\":8443\"; ma=86400");
    }

    TEST(AltSvcMiddleware, SkipsRequestAlreadyServedOverHttp3)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(altSvcMiddleware(8443));

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        request.setHttpVersion("HTTP/3");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        EXPECT_FALSE(response.hasHeader("alt-svc"));
        EXPECT_EQ(response.body(), "index");
    }

    TEST(AltSvcMiddleware, DeclaresConfiguredAuthorityAndMaxAge)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(altSvcMiddleware(443, std::chrono::seconds{3600}, "edge.example.com"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/index.html");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        EXPECT_EQ(response.getHeader("alt-svc").value_or(""), "h3=\"edge.example.com:443\"; ma=3600");
    }

    TEST(AltSvcMiddleware, LetsHandlerOverrideAdvertisement)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(altSvcMiddleware(8443));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/index.html");
        HttpResponse          response;
        std::string           advertisementSeenByHandler;
        const TerminalHandler handler = [&response, &advertisementSeenByHandler]() -> Core::Task<void>
        {
            advertisementSeenByHandler = response.getHeader("alt-svc").value_or("");
            static_cast<void>(response.setHeader("alt-svc", "h3=\":9999\"; ma=60"));
            co_return;
        };
        runPipeline(pipeline, request, response, handler);

        // 与 x-request-id、CORS 同一个优先级口径：中间件在下游之前挂，业务显式写过就以业务为准
        EXPECT_EQ(advertisementSeenByHandler, "h3=\":8443\"; ma=86400");
        EXPECT_EQ(response.getHeader("alt-svc").value_or(""), "h3=\":9999\"; ma=60");
    }

    TEST(AltSvcMiddleware, RejectsZeroPort)
    {
        EXPECT_THROW(altSvcMiddleware(0), Base::InvalidArgumentException);
    }

    TEST(AltSvcMiddleware, RejectsNonPositiveMaxAge)
    {
        EXPECT_THROW(altSvcMiddleware(8443, std::chrono::seconds{0}), Base::InvalidArgumentException);
        EXPECT_THROW(altSvcMiddleware(8443, std::chrono::seconds{-1}), Base::InvalidArgumentException);
    }

    TEST(AltSvcMiddleware, RejectsAuthorityThatWouldSplitFieldValue)
    {
        // 双引号会提前结束 quoted-string，分号会被读成下一个参数，CR/LF 直接撕裂报文
        EXPECT_THROW(altSvcMiddleware(8443, kAltSvcDefaultMaxAge, "edge\".example.com"), Base::InvalidArgumentException);
        EXPECT_THROW(altSvcMiddleware(8443, kAltSvcDefaultMaxAge, "edge.example.com; ma=1"), Base::InvalidArgumentException);
        EXPECT_THROW(altSvcMiddleware(8443, kAltSvcDefaultMaxAge, "edge.example.com\r\nx"), Base::InvalidArgumentException);
    }

    // ============================================================================
    // traceContextMiddleware（W3C Trace Context 的链路上下文归一化）
    // ============================================================================

    /// 规范 §3.2 的示例取值，管道用例里当作「上游已经决定好的链路」
    constexpr std::string_view kSpecExampleTraceparent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

    /// 收集请求上的头部名，按到达顺序（钉「只覆盖不追加」与「一条都不多写」）
    std::vector<std::string> collectHeaderNames(const HttpRequest &request)
    {
        std::vector<std::string> names;
        request.forEachHeaderField([&names](const std::string_view name, const std::string_view) { names.push_back(std::string(name)); });
        return names;
    }

    TEST(TraceContextMiddleware, GeneratesTraceparentWhenAbsent)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware());

        HttpRequest  request = makeRequest(HttpMethod::GET, "/index.html");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
        ASSERT_TRUE(identifiers.has_value()) << "没带 traceparent 的请求应当被起一条新链路";
        EXPECT_EQ(identifiers->version, 0U);
        EXPECT_TRUE(identifiers->isSampled());
        // 生成的字段必须落在请求的头部上：处理器与下游读的是同一份状态，不是中间件私藏的副本
        EXPECT_EQ(request.getHeader(kTraceparentHeaderName).value_or(""), Traceparent::value(*identifiers));
        // 业务照常执行，中间件不短路
        EXPECT_EQ(response.body(), "index");
    }

    TEST(TraceContextMiddleware, LeavesAValidInboundValueByteForByteUntouched)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware());

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        static_cast<void>(request.setHeader(kTraceparentHeaderName, kSpecExampleTraceparent));
        const std::size_t fieldCountBefore = collectHeaderNames(request).size();
        HttpResponse      response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        // 合法就一个字都不写：值原样、条目数原样（改写会白白把头部缓冲再长一截）
        EXPECT_EQ(request.getHeader(kTraceparentHeaderName).value_or(""), kSpecExampleTraceparent);
        EXPECT_EQ(collectHeaderNames(request).size(), fieldCountBefore);
        // 采样位由上游定：这里把 flags 改成 00，本中间件不得自作主张抬高
        HttpRequest unsampledRequest = makeRequest(HttpMethod::GET, "/index.html");
        static_cast<void>(unsampledRequest.setHeader(kTraceparentHeaderName, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00"));
        HttpResponse unsampledResponse;
        runPipeline(pipeline, unsampledRequest, unsampledResponse, terminalWriting(unsampledResponse, "index"));
        const std::optional<TraceIdentifiers> unsampled = extractTraceContext(unsampledRequest);
        ASSERT_TRUE(unsampled.has_value());
        EXPECT_FALSE(unsampled->isSampled());
    }

    TEST(TraceContextMiddleware, PassesAnUnknownVersionWithExtraFieldsThroughUntouched)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware());

        // 版本 01 并多带一个附加字段：本实现认得前 55 字节，但绝不自作主张「升级到 00」或删掉看不懂的字段。
        // 这条比上一条更尖：如果实现是「无论如何都重渲染一遍」，尾字段就会在这里消失
        constexpr std::string_view kFutureVersionValue = "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-future";
        HttpRequest                request             = makeRequest(HttpMethod::GET, "/index.html");
        static_cast<void>(request.setHeader(kTraceparentHeaderName, kFutureVersionValue));
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        EXPECT_EQ(request.getHeader(kTraceparentHeaderName).value_or(""), kFutureVersionValue);
        const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_EQ(identifiers->version, 1U);
    }

    TEST(TraceContextMiddleware, ReplacesMalformedValueWithAFreshTrace)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware());

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        static_cast<void>(request.setHeader(kTraceparentHeaderName, "00-not-a-valid-traceparent-at-all------------01"));
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
        ASSERT_TRUE(identifiers.has_value()) << "畸形取值没有被换成一条新链路：下游还会读到坏字段";
        EXPECT_NE(request.getHeader(kTraceparentHeaderName).value_or(""), "00-not-a-valid-traceparent-at-all------------01");
    }

    TEST(TraceContextMiddleware, RestartsTraceWhenTheHeaderAppearsTwice)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware());

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        // 两条同名 traceparent 会被头部存储折成一条带逗号的取值：歧义不猜任何一种解释，按「上游没给」重起
        request.addHeader(kTraceparentHeaderName, kSpecExampleTraceparent);
        request.addHeader(kTraceparentHeaderName, "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        const std::vector<std::string> names = collectHeaderNames(request);
        EXPECT_EQ(std::ranges::count(names, "traceparent"), 1);
        const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_NE(identifiers->traceIdText(), "4bf92f3577b34da6a3ce929d0e0e4736");
        EXPECT_NE(identifiers->traceIdText(), "0af7651916cd43dd8448eb211c80319c");
    }

    TEST(TraceContextMiddleware, StaysSilentWhenGenerationIsDisabled)
    {
        TraceContextOptions options;
        options.isGeneratedWhenAbsent = false;
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware(std::move(options)));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/index.html");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        EXPECT_FALSE(request.hasHeader(kTraceparentHeaderName)) << "关掉生成就不该无中生有：链路要么来自上游，要么就没有";
        EXPECT_FALSE(extractTraceContext(request).has_value());
    }

    TEST(TraceContextMiddleware, MovesVendorEntryToFrontOfTraceState)
    {
        TraceContextOptions options;
        options.vendorKey = "asyn";
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware(std::move(options)));

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        static_cast<void>(request.setHeader(kTraceparentHeaderName, kSpecExampleTraceparent));
        static_cast<void>(request.setHeader(kTracestateHeaderName, "a=1,b=2"));
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        // 自己的键挪到最前，上游条目的相对次序不动（§3.2.4.1）；值取本段的 span-id
        EXPECT_EQ(request.getHeader(kTracestateHeaderName).value_or(""), "asyn=00f067aa0ba902b7,a=1,b=2");
    }

    TEST(TraceContextMiddleware, TreatsInvalidTraceStateAsAbsent)
    {
        TraceContextOptions options;
        options.vendorKey = "asyn";
        MiddlewarePipeline pipeline;
        pipeline.use(traceContextMiddleware(std::move(options)));

        HttpRequest request = makeRequest(HttpMethod::GET, "/index.html");
        static_cast<void>(request.setHeader(kTraceparentHeaderName, kSpecExampleTraceparent));
        static_cast<void>(request.setHeader(kTracestateHeaderName, "roto=abc,roto=xyz")); // 重复键：整条判废
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "index"));

        EXPECT_EQ(request.getHeader(kTracestateHeaderName).value_or(""), "asyn=00f067aa0ba902b7") << "无效 tracestate 应按缺席处理：不该把上游的坏字段继续往下传";
    }

    TEST(TraceContextMiddleware, RejectsInvalidVendorKeyAtRegistration)
    {
        TraceContextOptions options;
        options.vendorKey = "BadKey";
        EXPECT_THROW(traceContextMiddleware(std::move(options)), Base::InvalidArgumentException);

        TraceContextOptions reservedKey;
        reservedKey.vendorKey = "congo";
        EXPECT_THROW(traceContextMiddleware(std::move(reservedKey)), Base::InvalidArgumentException);
    }

    /**
     * @brief 通告要真的上线：走真实回环连接，逐字节比对线上那一行
     * @details 管道内的用例只证明「响应对象上有这条头」，客户端读到的是序列化后的字节。
     *          这里钉住线上形态——字段名按本框架口径小写、值带引号与 ma 参数，
     *          也就是浏览器与 curl 实际要解析的那串文本。
     */
    TEST(AltSvcMiddleware, AdvertisementReachesTheWireOnHttp1Response)
    {
        /// 线上用例的等待上限：要覆盖「起监听 + 建连 + 一来一回」，比内存内管道宽松得多
        constexpr auto kWireProbeTimeout = std::chrono::milliseconds{5000};

        const HttpTestSupport::RouteRegistrar registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.get("/hello",
                       [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                       {
                           response.setBody("hello");
                           co_return;
                       });
            router.addMiddleware(altSvcMiddleware(8443));
        };

        auto fixture = std::make_unique<HttpTestSupport::RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, HttpTestSupport::SlowRouteOptions{},
                                                                                   registerRoutes, HttpParserLimits{}, [](HttpTestSupport::TestHttpServer &) {});
        ASSERT_TRUE(fixture->awaitRunning(kWireProbeTimeout));

        const std::optional<HttpTestSupport::ParsedResponse> response =
                HttpTestSupport::sendAndReadResponse(fixture->listeningPort(), HttpTestSupport::makeRequestText("GET /hello HTTP/1.1"), kWireProbeTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        EXPECT_TRUE(HttpTestSupport::hasHeaderLine(response->headers, "alt-svc: h3=\":8443\"; ma=86400")) << "线上响应没有带上通告：\n" << response->headers;
    }

    // ============================================================================
    // timeoutMiddleware（需要真实事件循环驱动定时器）
    // ============================================================================

    TEST_F(MiddlewareLoopFixture, PassesThroughWhenHandlerFinishesBeforeDeadline)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, kGenerousTimeout));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/fast");
        HttpResponse          response;
        std::atomic<int>      handlerCalls{0};
        std::atomic<bool>     exceptionCaught{false};
        std::atomic<bool>     isFinished{false};
        const TerminalHandler handler = terminalWriting(response, "fast", &handlerCalls);

        Core::Task<void> chainTask = runChainOnLoop(pipeline, request, response, handler, exceptionCaught, isFinished);
        ASSERT_TRUE(runOnLoop(chainTask, isFinished));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "fast");
        EXPECT_FALSE(request.cancelToken().stop_requested());
        EXPECT_FALSE(exceptionCaught.load());
    }

    TEST_F(MiddlewareLoopFixture, RequestsCancelAndWritesGatewayTimeoutWhenDeadlineElapses)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, kShortTimeout));

        HttpRequest       request = makeRequest(HttpMethod::GET, "/slow");
        HttpResponse      response;
        std::atomic<bool> observedCancel{false};
        std::atomic<bool> exceptionCaught{false};
        std::atomic<bool> isFinished{false};

        const TerminalHandler handler = [this, &request, &response, &observedCancel]() -> Core::Task<void>
        { co_await cooperativeHandler(m_loop, request, response, observedCancel); };
        Core::Task<void> chainTask = runChainOnLoop(pipeline, request, response, handler, exceptionCaught, isFinished);
        ASSERT_TRUE(runOnLoop(chainTask, isFinished));

        // 协作式取消：先让业务看到 stop_requested，再由中间件单点改写 504
        EXPECT_TRUE(observedCancel.load());
        EXPECT_TRUE(request.cancelToken().stop_requested());
        EXPECT_EQ(response.status(), 504);
        EXPECT_EQ(response.body(), "Gateway Timeout");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), std::string(kPlainTextContentType));
        EXPECT_FALSE(exceptionCaught.load());
    }

    /**
     * @brief 钉住：流式响应的头部已上线时，超时中间件不得改写响应
     * @details 头部随首段正文上线之后状态码就定稿了；此时 reset() 会把流式标记一并清掉，
     *          会话据此以为这是「新响应」，会在分块正文中间再插一条完整的 504——对端直接报协议错。
     *          正确处置是记日志并按已发出的状态码收尾。
     */
    TEST_F(MiddlewareLoopFixture, DoesNotRewriteStreamingResponseWhoseHeadIsAlreadyOnTheWire)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, kShortTimeout));

        HttpRequest              request = makeRequest(HttpMethod::GET, "/sse");
        HttpResponse             response;
        std::vector<std::string> sentSegments;
        response.setChunkSender(
                [&sentSegments](const std::string_view segment) -> Core::Task<bool>
                {
                    sentSegments.emplace_back(segment);
                    co_return true;
                });

        std::atomic<bool>     observedCancel{false};
        std::atomic<bool>     exceptionCaught{false};
        std::atomic<bool>     isFinished{false};
        const TerminalHandler handler = [this, &request, &response, &observedCancel]() -> Core::Task<>
        {
            response.startChunkedResponse(200);
            static_cast<void>(co_await response.writeChunk("first-segment"));
            // 拖到超时之后：此时头部已经在对端手里
            co_await cooperativeHandler(m_loop, request, response, observedCancel);
        };

        Core::Task<void> chainTask = runChainOnLoop(pipeline, request, response, handler, exceptionCaught, isFinished);
        ASSERT_TRUE(runOnLoop(chainTask, isFinished));

        EXPECT_TRUE(observedCancel.load()) << "看门狗应当照常发取消信号";
        EXPECT_EQ(response.status(), 200) << "头部早已上线：不得改写成 504";
        EXPECT_TRUE(response.hasSentChunkedHead()) << "流式标记被 reset 清掉了：会话会在正文中间再发一条响应";
        EXPECT_EQ(response.body(), "");
        ASSERT_EQ(sentSegments.size(), 2U) << "线上应当只有「头部 + 一段分块帧」";
        EXPECT_NE(sentSegments.front().find("200"), std::string::npos);
    }

    TEST_F(MiddlewareLoopFixture, PassesThroughWithoutTimerForNonPositiveTimeout)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, std::chrono::milliseconds(0)));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/no-deadline");
        HttpResponse          response;
        std::atomic<int>      handlerCalls{0};
        std::atomic<bool>     exceptionCaught{false};
        std::atomic<bool>     isFinished{false};
        const TerminalHandler handler = terminalWriting(response, "no-deadline", &handlerCalls);

        Core::Task<void> chainTask = runChainOnLoop(pipeline, request, response, handler, exceptionCaught, isFinished);
        ASSERT_TRUE(runOnLoop(chainTask, isFinished));

        // 非正数等价于「不启用超时」：只透传，既不建定时器也不发取消信号
        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.status(), 200);
        EXPECT_FALSE(request.cancelToken().stop_requested());
    }

    TEST_F(MiddlewareLoopFixture, RethrowsHandlerExceptionAfterReapingWatchdog)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, kGenerousTimeout));

        HttpRequest           request = makeRequest(HttpMethod::GET, "/throws");
        HttpResponse          response;
        std::atomic<bool>     exceptionCaught{false};
        std::atomic<bool>     isFinished{false};
        const TerminalHandler handler = []() -> Core::Task<void>
        {
            throw std::runtime_error("business failed");
            co_return;
        };

        Core::Task<void> chainTask = runChainOnLoop(pipeline, request, response, handler, exceptionCaught, isFinished);
        ASSERT_TRUE(runOnLoop(chainTask, isFinished));

        // 超时中间件不改变业务的失败语义：异常照原样上抛，也不擅自写 504
        EXPECT_TRUE(exceptionCaught.load());
        EXPECT_NE(response.status(), 504);
    }

    // ============================================================================
    // bodySizeLimitMiddleware
    // ============================================================================

    TEST(BodySizeLimitMiddleware, AcceptsRequestWithoutContentLengthHeader)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(10));

        HttpRequest      request = makeRequest(HttpMethod::POST, "/upload");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.status(), 200);
    }

    /**
     * @brief 钉住：取值一致的重复 Content-Length 不算非法
     * @details RFC 9110 §8.6 允许同一取值重复写；h2/h3 的 framing 读的是**首条**。这道前置闸此前读的是
     *          合并视图，两条 `content-length: 8` 会被拼成「8, 8」而 from_chars 判它非法，
     *          于是本框架自己收得下的请求被中间件回一个 400——三层口径必须一致
     */
    TEST(BodySizeLimitMiddleware, AcceptsRepeatedIdenticalContentLength)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(10));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "8");
        request.addHeader("Content-Length", "8");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1) << "重复且一致的长度声明不该短路掉业务";
        EXPECT_EQ(response.status(), 200) << response.body();
    }

    TEST(BodySizeLimitMiddleware, AcceptsDeclaredLengthAtLimit)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(10));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "10");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        // 判据是「严格大于」，恰好等于上限属于放行区间
        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.status(), 200);
    }

    TEST(BodySizeLimitMiddleware, RejectsDeclaredLengthAboveLimitWith413)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(10));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "11");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 413);
        EXPECT_EQ(response.body(), "Payload Too Large");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), std::string(kPlainTextContentType));
    }

    TEST(BodySizeLimitMiddleware, RejectsNonNumericContentLengthWith400)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(1000));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "many");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 400);
        EXPECT_TRUE(containsText(response.body(), "Content-Length"));
    }

    TEST(BodySizeLimitMiddleware, RejectsOverflowingContentLengthWith400)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(1000));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "99999999999999999999999999");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 400);
    }

    // ============================================================================
    // requestDecompressionMiddleware（入站正文的 Content-Encoding）
    // ============================================================================
    // 出站客户端早就替调用方解响应正文，入站这一半此前一行都没有：对端 POST 一份 gzip，业务从
    // body() 拿到压缩字节，而 jsonBody() 只回一句「解析失败」。

    /**
     * @brief gzip 的请求正文解回明文，兑现掉的声明一并撤走
     * @details 三条都要钉：正文换成解出来的那份；content-encoding 删掉（留着下游会再解一次）；
     *          content-length 换成解码后的字节数（旧值描述的是压缩字节，给一份新正文配一个旧长度
     *          等于让任何按长度数正文的下游数错）
     */
    TEST(RequestDecompressionMiddleware, DecodesGzipBodyAndDropsFulfilledDeclaration)
    {
        std::string payload;
        while (payload.size() < 4000)
        {
            payload += R"({"items":[1,2,3],"note":"gzip only shrinks when there is redundancy"})";
        }
        const std::string compressed = gzipCompress(payload).value();
        ASSERT_LT(compressed.size(), payload.size()) << "前提：这份正文真的被压缩过，否则本用例什么都没钉住";

        HttpRequest request = makeRequest(HttpMethod::POST, "/ingest");
        request.addHeader("content-encoding", "gzip");
        request.addHeader("content-length", std::to_string(compressed.size()));
        request.setBody(compressed);

        std::string           bodySeenByHandler;
        const TerminalHandler handler = [&request, &bodySeenByHandler]() -> Core::Task<void>
        {
            bodySeenByHandler = std::string(request.body());
            co_return;
        };

        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        HttpResponse response;
        runPipeline(pipeline, request, response, handler);

        EXPECT_EQ(bodySeenByHandler, payload) << "业务该拿到解开的正文";
        EXPECT_FALSE(request.getHeader("content-encoding").has_value()) << "已兑现的声明要撤走";
        EXPECT_EQ(request.getHeader("content-length").value_or(""), std::to_string(payload.size())) << "长度该指向解开的正文";
        EXPECT_EQ(response.status(), 200);
    }

    /**
     * @brief 标 deflate 而实际发 gzip 容器：按容器解而不按标签建流
     * @details 线上确实有这种对端（`inflateHttpBody` 走 zlib 的自动识别档），判据不能改成
     *          「按标签选解码器」，否则这类对端从能用变成 400
     */
    TEST(RequestDecompressionMiddleware, DecodesGzipContainerLabeledAsDeflate)
    {
        const std::string payload(2000, 'x');

        HttpRequest request = makeRequest(HttpMethod::POST, "/ingest");
        request.addHeader("content-encoding", "deflate");
        request.setBody(gzipCompress(payload).value());

        std::string           bodySeenByHandler;
        const TerminalHandler handler = [&request, &bodySeenByHandler]() -> Core::Task<void>
        {
            bodySeenByHandler = std::string(request.body());
            co_return;
        };

        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        HttpResponse response;
        runPipeline(pipeline, request, response, handler);

        EXPECT_EQ(bodySeenByHandler, payload) << "标签与容器不一致时要按容器解：这类对端线上真有";
    }

    /**
     * @brief 本端没有解码器的编码回 415，而不是把压缩字节当正文交下去
     * @details RFC 9110 §8.4 的 MUST：源服务器不支持请求声明的编码就必须按 415 处置。
     *          交下去的现场是业务以为拿到文本而其实是 br 字节
     */
    TEST(RequestDecompressionMiddleware, RejectsUnsupportedCodingWith415)
    {
        HttpRequest request = makeRequest(HttpMethod::POST, "/ingest");
        request.addHeader("content-encoding", "br");
        request.setBody("some brotli bytes");

        HttpResponse       response;
        std::atomic<int>   handlerCalls{0};
        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        runPipeline(pipeline, request, response, terminalWriting(response, "handled", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0) << "解不了的正文不该交给业务";
        EXPECT_EQ(response.status(), 415);
        EXPECT_EQ(request.body(), "some brotli bytes") << "被拒的正文不许被改写";
    }

    /**
     * @brief 链式声明（一层以上）也判 415：半途解错一层的后果是「看着正常的坏数据」
     */
    TEST(RequestDecompressionMiddleware, RejectsChainedCodingWith415)
    {
        HttpRequest request = makeRequest(HttpMethod::POST, "/ingest");
        request.addHeader("content-encoding", "gzip, deflate");
        request.setBody(gzipCompress("payload").value());

        HttpResponse       response;
        std::atomic<int>   handlerCalls{0};
        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        runPipeline(pipeline, request, response, terminalWriting(response, "handled", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 415);
    }

    /**
     * @brief 声明了 gzip 而字节不是合法 gzip 流：400，且不把半截正文交给业务
     */
    TEST(RequestDecompressionMiddleware, RejectsUndecodableBodyWith400)
    {
        HttpRequest request = makeRequest(HttpMethod::POST, "/ingest");
        request.addHeader("content-encoding", "gzip");
        request.setBody("not a gzip stream at all");

        HttpResponse       response;
        std::atomic<int>   handlerCalls{0};
        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        runPipeline(pipeline, request, response, terminalWriting(response, "handled", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 400);
    }

    /**
     * @brief 解出来的长度受上界约束：到界即判失败，不交回前 N 字节
     * @details gzip 压缩比可上千倍，不设界等于让对端用几百字节决定本进程分配多少内存；
     *          而「交回前 N 字节」是一份长度对、内容错的数据，比失败更坏
     */
    TEST(RequestDecompressionMiddleware, BoundsDecompressedBodyLength)
    {
        RequestDecompressionOptions options;
        options.maximumDecompressedByteCount = 1024;

        HttpRequest request = makeRequest(HttpMethod::POST, "/ingest");
        request.addHeader("content-encoding", "gzip");
        request.setBody(gzipCompress(std::string(64U * 1024U, 'A')).value());

        HttpResponse       response;
        std::atomic<int>   handlerCalls{0};
        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware(options));
        runPipeline(pipeline, request, response, terminalWriting(response, "handled", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0) << "超出上界的正文不该被当成合法请求";
        EXPECT_EQ(response.status(), 400);
    }

    /**
     * @brief 没声明、声明 identity、声明了编码但没有正文——三型都原样交给业务
     * @details 反面判据：闸门不能宽到把没编码的请求也拦下，也不能在「压根没有正文」时报错
     */
    TEST(RequestDecompressionMiddleware, PassesThroughUnencodedAndEmptyBodies)
    {
        HttpRequest plain = makeRequest(HttpMethod::POST, "/ingest");
        plain.setBody("plain text");

        HttpRequest identity = makeRequest(HttpMethod::POST, "/ingest");
        identity.addHeader("content-encoding", "identity");
        identity.setBody("still plain");

        HttpRequest emptyWithCoding = makeRequest(HttpMethod::POST, "/ingest");
        emptyWithCoding.addHeader("content-encoding", "gzip");

        for (HttpRequest *caseRequest: {&plain, &identity, &emptyWithCoding})
        {
            HttpResponse      response;
            std::atomic<int>  handlerCalls{0};
            const std::string bodyBefore(caseRequest->body());

            MiddlewarePipeline pipeline;
            pipeline.use(requestDecompressionMiddleware());
            runPipeline(pipeline, *caseRequest, response, terminalWriting(response, "handled", &handlerCalls));

            EXPECT_EQ(handlerCalls.load(), 1) << "这一型没有要解的东西，业务必须照常被调用";
            EXPECT_EQ(response.status(), 200);
            EXPECT_EQ(caseRequest->body(), bodyBefore) << "正文字节不许被动过";
        }
    }

    /**
     * @brief 声明长度比手上的字节大（流式派发那一型）时原样交回，不许判成解不出
     * @details 流式路由在头部收齐时就派发，此刻 `body()` 只是已收而尚未交付的那一段。拿这一段去解整份
     *          gzip 流必然「解不出」，于是一条合法的流式上传会被这条中间件判 400——错判比不解更坏。
     *          判据用「声明长度 ≠ 实际字节」表达，这一层只有这一个依据
     */
    TEST(RequestDecompressionMiddleware, PassesThroughWhenBodyIsStillArriving)
    {
        const std::string wholeStream = gzipCompress(std::string(3000, 'z')).value();
        // 取整份流的一半：保证「声明的整份长度」严格大于「手上的字节」，这正是流式早期派发的现场。
        // gzip 的压缩比高时整份流只有几十字节，先钉住这个前提本身，别让它悄悄等于全量
        const std::string arrivedPart = wholeStream.substr(0, wholeStream.size() / 2);
        ASSERT_GT(wholeStream.size(), arrivedPart.size()) << "前提：这一段必须短于整份流";

        HttpRequest request = makeRequest(HttpMethod::POST, "/stream");
        request.addHeader("content-encoding", "gzip");
        request.addHeader("content-length", std::to_string(wholeStream.size()));
        request.setBody(arrivedPart);

        HttpResponse          response;
        std::atomic<int>      handlerCalls{0};
        std::string           bodySeenByHandler;
        const TerminalHandler handler = [&request, &bodySeenByHandler, &handlerCalls]() -> Core::Task<void>
        {
            handlerCalls.fetch_add(1);
            bodySeenByHandler = std::string(request.body());
            co_return;
        };

        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        runPipeline(pipeline, request, response, handler);

        EXPECT_EQ(handlerCalls.load(), 1) << "正文还在路上就该交给处理器，而不是回 400";
        EXPECT_EQ(response.status(), 200) << response.body();
        EXPECT_EQ(bodySeenByHandler, arrivedPart) << "没收到完整流时一个字节都不许动";
        EXPECT_EQ(request.getHeader("content-length").value_or(""), std::to_string(wholeStream.size())) << "没解成的正文不许被换成半截";
        EXPECT_TRUE(request.getHeader("content-encoding").has_value()) << "没兑现的声明不许撤";
    }

    /**
     * @brief 编码本身不合规那两档与正文到没到齐无关：先判 415
     * @details 这条钉的是判据的顺序——「本端解不了这种编码」是策略结论，不该被「字节还没收齐」这一格
     *          掩护过去；否则同一条请求在流式派发下变成静默交回，处理器拿着一段 br 字节继续跑
     */
    TEST(RequestDecompressionMiddleware, RejectsUnsupportedCodingEvenWhileBodyIsArriving)
    {
        HttpRequest request = makeRequest(HttpMethod::POST, "/stream");
        request.addHeader("content-encoding", "br");
        request.addHeader("content-length", "4096");
        request.setBody("only a prefix arrived");

        HttpResponse       response;
        std::atomic<int>   handlerCalls{0};
        MiddlewarePipeline pipeline;
        pipeline.use(requestDecompressionMiddleware());
        runPipeline(pipeline, request, response, terminalWriting(response, "handled", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0) << "解不了的编码不该因为正文没到齐就被放行";
        EXPECT_EQ(response.status(), 415);
    }

    // ============================================================================
    // rateLimiterMiddleware
    // ============================================================================
    // 计数容器按「同一个事件循环线程串行处理」的假设设计，本身不加锁：
    // 以下用例全部在当前线程内同步跑完整条链，不涉及跨线程并发计数。

    TEST(RateLimiterMiddleware, AllowsQuotaThenRejectsWithRetryAfter)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(rateLimiterMiddleware(2, std::chrono::seconds(60)));

        std::atomic<int> handlerCalls{0};
        HttpRequest      firstRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse     firstResponse;
        runPipeline(pipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest  secondRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse secondResponse;
        runPipeline(pipeline, secondRequest, secondResponse, terminalWriting(secondResponse, "ok", &handlerCalls));
        EXPECT_EQ(secondResponse.status(), 200);

        HttpRequest  thirdRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse thirdResponse;
        runPipeline(pipeline, thirdRequest, thirdResponse, terminalWriting(thirdResponse, "ok", &handlerCalls));
        EXPECT_EQ(handlerCalls.load(), 2);
        EXPECT_EQ(thirdResponse.status(), 429);
        EXPECT_EQ(thirdResponse.body(), "Too Many Requests");
        EXPECT_EQ(thirdResponse.getHeader("retry-after").value_or(""), "60");
    }

    TEST(RateLimiterMiddleware, RejectsEveryRequestWhenQuotaIsZero)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(rateLimiterMiddleware(0, std::chrono::seconds(60)));

        HttpRequest      request = makeRequest(HttpMethod::GET, "/closed");
        HttpResponse     response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "ok", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 429);
    }

    TEST(RateLimiterMiddleware, RestoresQuotaAfterWindowExpires)
    {
        const auto windowStarted = std::chrono::steady_clock::now();

        MiddlewarePipeline pipeline;
        pipeline.use(rateLimiterMiddleware(1, kRateLimitWindow));

        std::atomic<int> handlerCalls{0};
        HttpRequest      firstRequest = makeRequest(HttpMethod::GET, "/windowed");
        HttpResponse     firstResponse;
        runPipeline(pipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest  exhaustedRequest = makeRequest(HttpMethod::GET, "/windowed");
        HttpResponse exhaustedResponse;
        runPipeline(pipeline, exhaustedRequest, exhaustedResponse, terminalWriting(exhaustedResponse, "ok", &handlerCalls));
        EXPECT_EQ(exhaustedResponse.status(), 429);

        // 轮询等窗口真正过期，不做固定 sleep；上界由 waitForCondition 兜住
        ASSERT_TRUE(waitForCondition([&windowStarted]() { return std::chrono::steady_clock::now() - windowStarted > kRateLimitWindow; }, kConditionTimeout));

        HttpRequest  nextWindowRequest = makeRequest(HttpMethod::GET, "/windowed");
        HttpResponse nextWindowResponse;
        runPipeline(pipeline, nextWindowRequest, nextWindowResponse, terminalWriting(nextWindowResponse, "ok", &handlerCalls));
        EXPECT_EQ(nextWindowResponse.status(), 200);
        EXPECT_EQ(handlerCalls.load(), 2);
    }

    TEST(RateLimiterMiddleware, SharesWindowBucketBetweenCopiesOfSameMiddleware)
    {
        // 计数器随 shared_ptr 一起被复制：同一个中间件实例塞进多条管道仍是同一份额度
        MiddlewareFunc       limiter       = rateLimiterMiddleware(1, std::chrono::seconds(60));
        const MiddlewareFunc copiedLimiter = limiter;

        MiddlewarePipeline firstPipeline;
        firstPipeline.use(limiter);
        MiddlewarePipeline secondPipeline;
        secondPipeline.use(copiedLimiter);

        std::atomic<int> handlerCalls{0};
        HttpRequest      firstRequest = makeRequest(HttpMethod::GET, "/shared");
        HttpResponse     firstResponse;
        runPipeline(firstPipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest  secondRequest = makeRequest(HttpMethod::GET, "/shared");
        HttpResponse secondResponse;
        runPipeline(secondPipeline, secondRequest, secondResponse, terminalWriting(secondResponse, "ok", &handlerCalls));
        EXPECT_EQ(secondResponse.status(), 429);
    }

    // ============================================================================
    // 令牌桶限流（tokenBucketRateLimiterMiddleware）
    // ============================================================================

    TEST(TokenBucket, AllowsBurstUpToCapacityThenRejects)
    {
        // 速率极小：本用例只关心「容量决定瞬时突发」，补令牌在这几毫秒里可以忽略
        TokenBucket bucket(0.001, 3.0);

        EXPECT_TRUE(bucket.tryAcquire());
        EXPECT_TRUE(bucket.tryAcquire());
        EXPECT_TRUE(bucket.tryAcquire());
        EXPECT_FALSE(bucket.tryAcquire()) << "超出桶容量后应拒绝";
        EXPECT_LT(bucket.availableTokenCount(), 1.0);
    }

    TEST(TokenBucket, RefillsOverTime)
    {
        // 速率取大一些，让「补到 1 个令牌」的等待时间远小于用例的等待窗口，避免卡在机器抖动上
        TokenBucket bucket(50.0, 1.0);
        EXPECT_TRUE(bucket.tryAcquire());
        EXPECT_FALSE(bucket.tryAcquire()) << "刚取空就该拒绝";

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_TRUE(bucket.tryAcquire()) << "等待远超补一个令牌所需的时间后应当放行";
    }

    TEST(TokenBucket, ClampsAccumulatedTokensToCapacity)
    {
        // 速率 1000/s、容量 1：若空闲期间攒下的令牌不封顶，睡 50ms 后就能连续放行 50 次
        TokenBucket bucket(1000.0, 1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        EXPECT_TRUE(bucket.tryAcquire());
        EXPECT_FALSE(bucket.tryAcquire()) << "空闲攒下的令牌必须被封顶到桶容量";
    }

    TEST(TokenBucket, RejectsInvalidConfiguration)
    {
        // 速率非正无法补令牌；容量小于 1 永远攒不满一个令牌——两者都是配置错误，必须当场抛
        EXPECT_THROW(TokenBucket(0.0, 1.0), Base::InvalidArgumentException);
        EXPECT_THROW(TokenBucket(-1.0, 1.0), Base::InvalidArgumentException);
        EXPECT_THROW(TokenBucket(1.0, 0.5), Base::InvalidArgumentException);
    }

    TEST(TokenBucket, ReportsWaitTimeUntilNextToken)
    {
        TokenBucket bucket(2.0, 1.0);
        EXPECT_EQ(bucket.timeUntilTokenAvailable().count(), 0) << "桶里还有令牌时不必等待";

        EXPECT_TRUE(bucket.tryAcquire());
        // 速率 2/s ⇒ 补一个令牌要 500ms；允许实现细节带来的少量偏差，但必须是「几百毫秒」量级
        const auto waitMilliseconds = bucket.timeUntilTokenAvailable().count();
        EXPECT_GE(waitMilliseconds, 400);
        EXPECT_LE(waitMilliseconds, 600);
    }

    TEST(TokenBucket, LimitsRequestsThroughSharedBucketAcrossPipelines)
    {
        // 两条管道共享同一个桶：这正是「进程级全局 RPS 上限」的用法——各持一份的话上限会翻倍。
        // 速率为 1/s：两条紧接着的请求之间补不满一个令牌，因此第二次必然被拒
        auto               bucket = std::make_shared<TokenBucket>(1.0, 1.0);
        MiddlewarePipeline firstPipeline;
        firstPipeline.use(tokenBucketRateLimiterMiddleware(bucket));
        MiddlewarePipeline secondPipeline;
        secondPipeline.use(tokenBucketRateLimiterMiddleware(bucket));

        std::atomic<int> handlerCalls{0};

        HttpRequest  firstRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse firstResponse;
        runPipeline(firstPipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest  secondRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse secondResponse;
        runPipeline(secondPipeline, secondRequest, secondResponse, terminalWriting(secondResponse, "ok", &handlerCalls));
        EXPECT_EQ(secondResponse.status(), 429);

        // 超限时不得唤醒业务：短路发生在中间件里
        EXPECT_EQ(handlerCalls.load(), 1);
        // 速率 1/s ⇒ 下一个令牌要等 1 秒，Retry-After 就该报 1（而不是 0——那等于让客户端立刻重试）
        EXPECT_EQ(secondResponse.getHeader("retry-after").value_or(""), "1");
    }

    TEST(TokenBucket, RejectsNullBucketAtFactoryTime)
    {
        // 空桶是用法错误，不是「不限制」：静默放行会让人以为限流生效
        EXPECT_THROW(tokenBucketRateLimiterMiddleware(nullptr), Base::InvalidArgumentException);
    }

    TEST(TokenBucket, ConcurrentAcquireNeverExceedsCapacity)
    {
        // 速率取到几乎不补（0.001/s），让「成功次数」有一个可断言的确定值：
        // 任何超出容量的成功都意味着桶状态在并发下被破坏
        constexpr int kCapacity          = 8;
        constexpr int kThreadCount       = 8;
        constexpr int kAttemptsPerThread = 200;

        TokenBucket bucket(0.001, static_cast<double>(kCapacity));

        std::atomic<int>         successCount{0};
        std::vector<std::thread> workers;
        workers.reserve(kThreadCount);
        for (int index = 0; index < kThreadCount; ++index)
        {
            workers.emplace_back(
                    [&bucket, &successCount]
                    {
                        for (int attempt = 0; attempt < kAttemptsPerThread; ++attempt)
                        {
                            if (bucket.tryAcquire())
                            {
                                successCount.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    });
        }
        for (std::thread &worker: workers)
        {
            worker.join();
        }

        EXPECT_EQ(successCount.load(), kCapacity) << "并发取令牌的成功次数必须恰好等于桶容量";
    }

    // ============================================================================
    // securityHeadersMiddleware：安全响应头
    // ============================================================================

    namespace
    {
        /**
         * @brief 用给定的安全响应头配置跑一趟管道，交回响应
         * @param options 待测的安全响应头取值
         * @param isSecure 这条请求是否经由 TLS（HSTS 的唯一开关）
         * @param outerOverride 排在安全头之前的那层中间件要写的头（名字, 取值）；空名字表示没有这一层
         * @return HttpResponse 链路跑完后的响应
         */
        HttpResponse runSecurityHeaders(const SecurityHeadersOptions &options, const bool isSecure, const std::pair<std::string_view, std::string> &outerOverride = {})
        {
            MiddlewarePipeline pipeline;
            if (!outerOverride.first.empty())
            {
                const std::string fieldName  = std::string(outerOverride.first);
                const std::string fieldValue = outerOverride.second;
                pipeline.use(
                        [fieldName, fieldValue](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                        {
                            static_cast<void>(response.setHeader(fieldName, fieldValue));
                            co_await next();
                        });
            }
            pipeline.use(securityHeadersMiddleware(options));

            HttpRequest  request = makeRequest(HttpMethod::GET, "/guarded");
            HttpResponse response;
            request.setOverTls(isSecure);
            runPipeline(pipeline, request, response, terminalWriting(response, "guarded"));
            return response;
        }
    } // namespace

    /**
     * @brief 钉住：默认档把三条「收紧」的头挂上，没配的三条一条都不发
     * @details 这三条（nosniff / DENY / no-referrer）默认取收紧侧是刻意的：它们兜的是「浏览器多做一步
     *          猜测」，放松的代价远大于收紧。而 CSP 与 HSTS 各部署结论相反，本层不替调用方选边
     */
    TEST(SecurityHeadersMiddleware, AppliesHardenedDefaultsAndSkipsUnconfiguredOnes)
    {
        const HttpResponse response = runSecurityHeaders(SecurityHeadersOptions{}, false);

        EXPECT_EQ(response.getHeader("x-content-type-options"), "nosniff");
        EXPECT_EQ(response.getHeader("x-frame-options"), "DENY");
        EXPECT_EQ(response.getHeader("referrer-policy"), "no-referrer");
        EXPECT_FALSE(response.hasHeader("content-security-policy")) << "没配策略却发了一张空 CSP：浏览器按「什么都不许」执行";
        EXPECT_FALSE(response.hasHeader("strict-transport-security"));
        EXPECT_FALSE(response.hasHeader("permissions-policy"));
        EXPECT_FALSE(response.hasHeader("cross-origin-resource-policy"));
        // 刻意不发的一条：那条早被规范废弃，发它会诱导人以为有防护
        EXPECT_FALSE(response.hasHeader("x-xss-protection")) << "X-XSS-Protection 早已废弃，不该由本层继续发";
    }

    /**
     * @brief 钉住：明文连接上不发 HSTS
     * @details RFC 6797 §7.2 要求浏览器忽略非安全传输上收到的这一条，明文发出去既不生效，
     *          又会让运维以为站点已经有了这道保护——比不发更糟
     */
    TEST(SecurityHeadersMiddleware, DoesNotAdvertiseHstsOnACleartextRequest)
    {
        SecurityHeadersOptions options;
        options.strictTransportSecurityMaxAgeSeconds     = 31'536'000;
        options.strictTransportSecurityIncludeSubDomains = true;
        const HttpResponse response                      = runSecurityHeaders(options, false);

        EXPECT_FALSE(response.hasHeader("strict-transport-security")) << "明文连接被挂了 HSTS";
        EXPECT_TRUE(response.hasHeader("x-frame-options")) << "其余的头不该被这一条连带不发";
    }

    /**
     * @brief 钉住：TLS 连接上 HSTS 的文案逐格拼对
     * @details 指令顺序与分隔符都是浏览器解析的对象；includeSubDomains 与 preload 两格各自只在
     *          被显式要求时出现（默认不扩大到子域）
     */
    TEST(SecurityHeadersMiddleware, AdvertisesHstsOnASecureRequestWithTheExactDirectives)
    {
        SecurityHeadersOptions options;
        options.strictTransportSecurityMaxAgeSeconds = 31'536'000;
        EXPECT_EQ(runSecurityHeaders(options, true).getHeader("strict-transport-security"), "max-age=31536000") << "只配了秒数时不该带上子域";

        options.strictTransportSecurityIncludeSubDomains = true;
        EXPECT_EQ(runSecurityHeaders(options, true).getHeader("strict-transport-security"), "max-age=31536000; includeSubDomains");

        options.strictTransportSecurityPreload = true;
        EXPECT_EQ(runSecurityHeaders(options, true).getHeader("strict-transport-security"), "max-age=31536000; includeSubDomains; preload");
    }

    /**
     * @brief 钉住：配了 preload 却没配 includeSubDomains 当场抛
     * @details preload 是提交给浏览器内置名单的声明，那份名单硬性要求同时带 includeSubDomains；
     *          少这一格的表现是「配了 preload 却永远进不了名单」，静默放行比构造期拒绝难查得多
     */
    TEST(SecurityHeadersMiddleware, RefusesPreloadWithoutIncludeSubDomains)
    {
        SecurityHeadersOptions options;
        options.strictTransportSecurityMaxAgeSeconds = 31'536'000;
        options.strictTransportSecurityPreload       = true;
        EXPECT_THROW(securityHeadersMiddleware(options), Base::InvalidArgumentException);

        options.strictTransportSecurityIncludeSubDomains = true;
        EXPECT_NO_THROW(securityHeadersMiddleware(options));
    }

    /**
     * @brief 钉住：带 CR/LF/NUL 的取值在构造期就被拒
     * @details 这类文案常由配置拼出来；写进响应头就是 HTTP 响应拆分——同一份判据与
     *          HttpResponse::setHeader() 拒这三个字节同源，这里要在**拼串之前**响
     */
    TEST(SecurityHeadersMiddleware, RefusesValuesCarryingHeaderBreakingBytes)
    {
        SecurityHeadersOptions withSplit;
        withSplit.contentSecurityPolicy = "default-src 'self';\r\nX-Fake: 1";
        EXPECT_THROW(securityHeadersMiddleware(withSplit), Base::InvalidArgumentException);

        SecurityHeadersOptions withNul;
        withNul.frameOptions = std::string("SAMEORIGIN") + '\0';
        EXPECT_THROW(securityHeadersMiddleware(withNul), Base::InvalidArgumentException);

        SecurityHeadersOptions withLineFeedOnly;
        withLineFeedOnly.referrerPolicy = "no-referrer\n";
        EXPECT_THROW(securityHeadersMiddleware(withLineFeedOnly), Base::InvalidArgumentException);
    }

    /**
     * @brief 钉住：已经存在的头不覆盖，业务与外层中间件才是取值的主人
     * @details 需要内嵌 iframe 的管理台会把 DENY 改成 SAMEORIGIN 或干脆不发；这一层如果无条件
     *          setHeader，就会把改过的值又刷回默认，那种「改了没用」最难归因到中间件
     */
    TEST(SecurityHeadersMiddleware, LeavesHeadersAlreadySetByOuterLayersAlone)
    {
        const HttpResponse overridden = runSecurityHeaders(SecurityHeadersOptions{}, false, {"x-frame-options", "SAMEORIGIN"});
        EXPECT_EQ(overridden.getHeader("x-frame-options"), "SAMEORIGIN") << "外层写定的值被安全头中间件刷回了 DENY";
    }

    /**
     * @brief 把 Basic 认证中间件跑一趟管道，交回响应
     * @param options 认证取值
     * @param authorization 要送出的 Authorization 头原文；空串表示根本不带这个头
     * @param isSecure 这条请求是否经由 TLS（Basic 的默认闸门）
     * @param observedUser / observedSecret 可空的出参：verify 实际看到的拆分结果
     * @param path 请求路径
     * @return HttpResponse 链路跑完后的响应
     */
    HttpResponse runBasicAuth(const BasicAuthOptions &options, const std::string &authorization, const bool isSecure, const std::string &path = "/admin")
    {
        MiddlewarePipeline pipeline;
        pipeline.use(basicAuthMiddleware(options));

        HttpRequest  request = makeRequest(HttpMethod::GET, path);
        HttpResponse response;
        request.setOverTls(isSecure);
        if (!authorization.empty())
        {
            if (!request.setHeader("authorization", authorization))
            {
                ADD_FAILURE() << "这条 Authorization 原文被头部校验拒了，用例测的就不再是中间件：" << authorization;
            }
        }

        runPipeline(pipeline, request, response, terminalWriting(response, "granted"));
        return response;
    }

    /**
     * @brief 钉住：verify 认下的凭据放行，且它拿到的拆分正是 `user:secret`
     */
    TEST(BasicAuthMiddleware, AdmitsTheCredentialsTheVerifierAccepts)
    {
        std::string      seenUser;
        std::string      seenSecret;
        BasicAuthOptions options;
        options.realm  = "admin";
        options.verify = [&](std::string_view user, std::string_view secret)
        {
            seenUser   = std::string(user);
            seenSecret = std::string(secret);
            return user == "ops" && secret == "s3cr3t";
        };

        const HttpResponse granted = runBasicAuth(options, "Basic " + Base::base64Encode("ops:s3cr3t"), true);
        EXPECT_EQ(granted.status(), 200);
        EXPECT_EQ(granted.body(), "granted");
        EXPECT_EQ(seenUser, "ops");
        EXPECT_EQ(seenSecret, "s3cr3t");
    }

    /**
     * @brief 钉住：没给凭据时回 401 带 realm 与 charset，且不调用下游
     * @details 401 的形状是给客户端「重一次」用的：少了 `WWW-Authenticate` 客户端就无从知道该问谁要凭据
     */
    TEST(BasicAuthMiddleware, ChallengesWithRealmAndCharsetWhenNothingIsPresented)
    {
        BasicAuthOptions options;
        options.realm  = "metrics-box";
        options.verify = [](std::string_view, std::string_view) { return false; };

        const HttpResponse challenged = runBasicAuth(options, "", true);
        EXPECT_EQ(challenged.status(), 401);
        EXPECT_EQ(challenged.getHeader("www-authenticate"), "Basic realm=\"metrics-box\", charset=\"US-ASCII\"");
        EXPECT_EQ(challenged.body().find("口令不对"), std::string::npos) << "没给凭据时不该说「口令不对」：" << challenged.body();
    }

    /**
     * @brief 钉住：另一种方案不被顺着手解释成 Basic
     * @details `Bearer xxx` 那一段是令牌不是 base64(user:secret)；顺手解会把一段任意字节当成凭据，
     *          现场还会表现成「口令怎么都不对」
     */
    TEST(BasicAuthMiddleware, RefusesAnotherSchemeAsIfNothingWereGiven)
    {
        BasicAuthOptions options;
        options.verify = [](std::string_view, std::string_view) { return true; };

        EXPECT_EQ(runBasicAuth(options, "Bearer sometoken", true).status(), 401);
        EXPECT_EQ(runBasicAuth(options, "Basic @@not-base64@@", true).status(), 401) << "解不开的 base64 该按没给凭据处理";
        EXPECT_EQ(runBasicAuth(options, "Token " + Base::base64Encode("ops:s3cr3t"), true).status(), 401) << "凭据段本身合法也不是 Basic 方案：按固定偏移顺手剥前缀就会把它收下";
    }

    /**
     * @brief 钉住：auth-scheme 大小写不敏感（RFC 9110 §11.2）
     * @details 客户端写 `Basic`/`basic`/`BASIC` 都合规，只认一种拼法就是按实现细节挑客户端
     */
    TEST(BasicAuthMiddleware, AcceptsTheSchemeInAnyCase)
    {
        BasicAuthOptions options;
        options.verify = [](std::string_view user, std::string_view secret) { return user == "ops" && secret == "s3cr3t"; };

        const std::string credential = Base::base64Encode("ops:s3cr3t");
        for (const std::string_view scheme: {"Basic ", "basic ", "BASIC ", "bAsIc "})
        {
            EXPECT_EQ(runBasicAuth(options, std::string(scheme) + credential, true).status(), 200) << "方案拼法：" << scheme;
        }
    }

    /**
     * @brief 钉住：按第一个冒号切分，含冒号的口令因此可用
     * @details RFC 7617 §2 规定 user-id 里不许出现冒号，所以冒号之后的整体都是口令；
     *          反过来切最后一刀会把口令截短，表现成「明明对了却不通」
     */
    TEST(BasicAuthMiddleware, SplitsOnTheFirstColonOnly)
    {
        std::string      seenUser;
        std::string      seenSecret;
        BasicAuthOptions options;
        options.verify = [&](std::string_view user, std::string_view secret)
        {
            seenUser   = std::string(user);
            seenSecret = std::string(secret);
            return true;
        };

        static_cast<void>(runBasicAuth(options, "Basic " + Base::base64Encode("root:a:b:c"), true));
        EXPECT_EQ(seenUser, "root");
        EXPECT_EQ(seenSecret, "a:b:c");
    }

    /**
     * @brief 钉住：默认不在明文连接上收 Basic 凭据，回的是 403 而不是 401
     * @details 401 会附那句「请给 Basic 凭据」的提示，等于邀请客户端把口令发进不加密的信道；
     *          403 明确「这条路不接受」，同时放开的方式是显式置 requireSecureTransport=false
     */
    TEST(BasicAuthMiddleware, KeepsCredentialsOffCleartextUntilTheOperatorOptsIn)
    {
        BasicAuthOptions options;
        options.verify = [](std::string_view, std::string_view) { return true; };

        const HttpResponse refused = runBasicAuth(options, "Basic " + Base::base64Encode("ops:pw"), false);
        EXPECT_EQ(refused.status(), 403);
        EXPECT_FALSE(refused.hasHeader("www-authenticate")) << "403 还附要凭据的提示，等于一边拒一边催";

        BasicAuthOptions optedOut       = options;
        optedOut.requireSecureTransport = false;
        EXPECT_EQ(runBasicAuth(optedOut, "Basic " + Base::base64Encode("ops:pw"), false).status(), 200) << "显式放开之后明文通路该照常走";
    }

    /**
     * @brief 钉住：名单之外的路径不被闸住
     * @details `protectedPaths` 逐条精确匹配，与运维端点那道闸同一口径；默认「空名单＝全部都要鉴权」
     *          是收紧侧，反过来就会让忘记填名单的配置静默全放行
     */
    TEST(BasicAuthMiddleware, LeavesPathsOutsideTheListAlone)
    {
        BasicAuthOptions options;
        options.protectedPaths = {"/admin"};
        options.verify         = [](std::string_view, std::string_view) { return false; };

        EXPECT_EQ(runBasicAuth(options, "", true, "/public").status(), 200) << "名单之外的路径被顺带闸住了";
        EXPECT_EQ(runBasicAuth(options, "", true, "/admin").status(), 401);
        const BasicAuthOptions emptyList = []
        {
            BasicAuthOptions fresh;
            fresh.verify = [](std::string_view, std::string_view) { return false; };
            return fresh;
        }();
        EXPECT_EQ(runBasicAuth(emptyList, "", true, "/anything").status(), 401);
    }

    /**
     * @brief 钉住：缺 verify 与可注入的 realm 在构造期就抛
     * @details 缺回调的表现是「所有人都被拒」，比放行危险但更难归因；realm 会被拼进带引号的头取值里，
     *          自己带引号或控制字符就是在写响应头的时候开口子
     */
    TEST(BasicAuthMiddleware, RefusesAMissingVerifierAndAnInjectableRealm)
    {
        BasicAuthOptions noVerifier;
        EXPECT_THROW(basicAuthMiddleware(noVerifier), Base::InvalidArgumentException);

        BasicAuthOptions quotedRealm;
        quotedRealm.verify = [](std::string_view, std::string_view) { return true; };
        quotedRealm.realm  = "a\", Set-Cookie: x=1";
        EXPECT_THROW(basicAuthMiddleware(quotedRealm), Base::InvalidArgumentException);

        BasicAuthOptions lineFeedRealm;
        lineFeedRealm.verify = [](std::string_view, std::string_view) { return true; };
        lineFeedRealm.realm  = "two\r\nlines";
        EXPECT_THROW(basicAuthMiddleware(lineFeedRealm), Base::InvalidArgumentException);
    }

    /**
     * @brief 钉住：解出来但没有冒号的凭据按未授权处理，且不把异常漏给上层
     * @details 冒号切分找不到分隔符时若不设这道闸，`substr(separator + 1)` 就是 `npos + 1`，
     *          抛的是 `std::out_of_range`——现场表现成「带某个奇怪口令就 500」
     */
    TEST(BasicAuthMiddleware, TreatsACredentialWithoutColonAsMalformedRatherThanThrowing)
    {
        int              verifierCalls = 0;
        BasicAuthOptions options;
        options.verify = [&](std::string_view, std::string_view)
        {
            ++verifierCalls;
            return true;
        };

        const HttpResponse malformed = runBasicAuth(options, "Basic " + Base::base64Encode("nocolonuser"), true);
        EXPECT_EQ(malformed.status(), 401);
        EXPECT_EQ(verifierCalls, 0) << "连用户与口令都分不开时不该把任意一段字节当成 user 去问 verify";
    }

    /**
     * @brief 钉住：多条 Authorization 按未授权处理，不回显差在哪一位
     * @details 与运维端点那道闸同一条纪律：只取第一条来解释，会让「塞一条对的再塞一条错的」
     *          随头部顺序时通时不通
     */
    TEST(BasicAuthMiddleware, TreatsAmbiguousHeadersAsNoCredential)
    {
        BasicAuthOptions options;
        options.verify = [](std::string_view user, std::string_view secret) { return user == "ops" && secret == "pw"; };

        MiddlewarePipeline pipeline;
        pipeline.use(basicAuthMiddleware(options));
        HttpRequest  request = makeRequest(HttpMethod::GET, "/admin");
        HttpResponse response;
        request.setOverTls(true);
        ASSERT_TRUE(request.setHeader("authorization", "Basic " + Base::base64Encode("ops:pw")));
        request.addHeader("authorization", "Basic " + Base::base64Encode("ops:wrong"));
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "granted", &handlerCalls));

        EXPECT_EQ(response.status(), 401) << "两条互相矛盾的凭据不该被当成一条";
        EXPECT_EQ(handlerCalls.load(), 0);
    }
} // namespace AsynGyanis::Net
