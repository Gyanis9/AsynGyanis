/**
 * @file TestMiddleware.cpp
 * @brief 中间件单元测试：管道顺序与短路、日志、CORS、协作式超时、体积与速率限制
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/Middleware.h"

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpMethod.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
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
         * @brief 在超时上限内逐毫秒轮询等待条件成立
         * @tparam Predicate 可调用对象，返回 bool
         * @param predicate 待轮询的条件
         * @param timeout   超时上限
         * @return true 条件在时限内成立
         */
        template<typename Predicate>
        bool waitForCondition(Predicate predicate, const std::chrono::milliseconds timeout = kConditionTimeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!predicate())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /**
         * @brief 判断文本里是否出现指定子串
         * @param haystack 待搜索文本
         * @param needle   目标子串
         * @return true 命中
         */
        bool containsText(const std::string_view haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string_view::npos;
        }

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
            return []() -> Core::Task<void>
            {
                co_return;
            };
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
            std::mutex mutex;                   ///< 保护下面两个向量
            std::vector<std::string> messages;  ///< 已记录的日志正文
            std::vector<Base::LogLevel> levels; ///< 已记录的日志等级
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
            explicit RecordingSink(std::shared_ptr<LogRecords> records) :
                m_records(std::move(records))
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
            const auto hardStop = std::chrono::steady_clock::now() + kHandlerHardStop;

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
            m_worker = std::thread([this]()
            {
                m_loop.run();
            });
            if (!waitForCondition([this]()
                {
                    return m_loop.isRunning();
                }))
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

            const bool isCompleted = waitForCondition([&finishedFlag]()
            {
                return finishedFlag.load();
            });

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
        HttpRequest request = makeRequest(HttpMethod::GET, "/bare");
        HttpResponse response;
        std::atomic<int> handlerCalls{0};

        EXPECT_EQ(pipeline.middlewareCount(), 0U);
        runPipeline(pipeline, request, response, terminalWriting(response, "bare", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.body(), "bare");
    }

    TEST(MiddlewarePipeline, CountsRegisteredMiddlewares)
    {
        MiddlewarePipeline pipeline;
        const auto passthrough = [](HttpRequest &, HttpResponse &, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            co_await next();
        };

        pipeline.use(passthrough);
        pipeline.use(passthrough);

        EXPECT_EQ(pipeline.middlewareCount(), 2U);
    }

    TEST(MiddlewarePipeline, EntersInRegistrationOrderAndExitsInReverse)
    {
        MiddlewarePipeline pipeline;
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

        HttpRequest request = makeRequest(HttpMethod::GET, "/onion");
        HttpResponse response;
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
        MiddlewarePipeline pipeline;
        std::atomic<int> innerCalls{0};
        std::atomic<int> handlerCalls{0};
        std::vector<std::string> executionOrder;

        pipeline.use([&executionOrder](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()>) -> Core::Task<>
        {
            executionOrder.push_back("outer:短路");
            response.setStatus(503);
            response.setBody("service unavailable");
            co_return;
        });
        pipeline.use([&innerCalls](HttpRequest &, HttpResponse &, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            innerCalls.fetch_add(1);
            co_await next();
        });

        HttpRequest request = makeRequest(HttpMethod::GET, "/short");
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
        pipeline.use([](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            co_await next();
            // 后置改写落在业务之后：正文最终由它说了算
            response.setHeader("x-powered-by", "middleware");
            response.setBody("rewritten");
        });

        HttpRequest request = makeRequest(HttpMethod::GET, "/rewrite");
        HttpResponse response;
        runPipeline(pipeline, request, response, terminalWriting(response, "original"));

        EXPECT_EQ(response.body(), "rewritten");
        EXPECT_EQ(response.getHeader("x-powered-by").value_or(""), "middleware");
    }

    TEST(MiddlewarePipeline, PropagatesHandlerExceptionToCaller)
    {
        MiddlewarePipeline pipeline;
        HttpRequest request = makeRequest(HttpMethod::GET, "/throws");
        HttpResponse response;
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

        HttpRequest request = makeRequest(HttpMethod::GET, "/quiet");
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "quiet", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.body(), "quiet");
    }

    TEST(LoggingMiddleware, RecordsUriAndStatusCodeAfterDownstreamCompletes)
    {
        auto records = std::make_shared<LogRecords>();
        MiddlewarePipeline pipeline;
        pipeline.use(loggingMiddleware(makeRecordingLogger(records)));

        HttpRequest request = makeRequest(HttpMethod::GET, "/logged?detail=1");
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
        auto records = std::make_shared<LogRecords>();
        MiddlewarePipeline pipeline;
        // 日志中间件在最外层，业务被限流短路也要留下痕迹
        pipeline.use(loggingMiddleware(makeRecordingLogger(records)));
        pipeline.use(rateLimiterMiddleware(0, std::chrono::seconds(60)));

        HttpRequest request = makeRequest(HttpMethod::GET, "/burst");
        HttpResponse response;
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
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "business", &handlerCalls));

        // 预检就地应答并短路，业务 handler 根本不该收到这个 OPTIONS
        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 204);
        EXPECT_TRUE(response.body().empty());
        EXPECT_EQ(response.getHeader("access-control-allow-origin").value_or(""), "*");
        EXPECT_EQ(response.getHeader("vary").value_or(""), "Origin");
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

    TEST(CorsMiddleware, DeclaresConfiguredMethodsHeadersAndMaxAge)
    {
        CorsPolicy policy;
        policy.allowMethods = "GET, POST";
        policy.allowHeaders = "X-Custom";
        policy.maxAge = std::chrono::seconds(0);
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
        policy.allowOrigin = "*";
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(policy));

        HttpRequest preflight = makeRequest(HttpMethod::OPTIONS, "/api/data");
        preflight.addHeader("Access-Control-Request-Method", "POST");
        HttpResponse preflightResponse;
        runPipeline(pipeline, preflight, preflightResponse, emptyTerminal());

        // 通配来源与凭据不能共存：一起发出去浏览器一定判失败
        EXPECT_FALSE(preflightResponse.getHeader("access-control-allow-credentials").has_value());

        HttpRequest actual = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse actualResponse;
        runPipeline(pipeline, actual, actualResponse, emptyTerminal());
        EXPECT_FALSE(actualResponse.getHeader("access-control-allow-credentials").has_value());
    }

    TEST(CorsMiddleware, DeclaresCredentialsForSpecificOrigin)
    {
        CorsPolicy policy;
        policy.allowOrigin = "https://app.example";
        policy.allowCredentials = true;
        MiddlewarePipeline pipeline;
        pipeline.use(corsMiddleware(policy));

        HttpRequest request = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse response;
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
        HttpRequest request = makeRequest(HttpMethod::OPTIONS, "/api/data");
        HttpResponse response;
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

        HttpRequest request = makeRequest(HttpMethod::GET, "/api/data");
        HttpResponse response;
        std::string originSeenByHandler;
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
    // timeoutMiddleware（需要真实事件循环驱动定时器）
    // ============================================================================

    TEST_F(MiddlewareLoopFixture, PassesThroughWhenHandlerFinishesBeforeDeadline)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, kGenerousTimeout));

        HttpRequest request = makeRequest(HttpMethod::GET, "/fast");
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        std::atomic<bool> exceptionCaught{false};
        std::atomic<bool> isFinished{false};
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

        HttpRequest request = makeRequest(HttpMethod::GET, "/slow");
        HttpResponse response;
        std::atomic<bool> observedCancel{false};
        std::atomic<bool> exceptionCaught{false};
        std::atomic<bool> isFinished{false};

        const TerminalHandler handler = [this, &request, &response, &observedCancel]() -> Core::Task<void>
        {
            co_await cooperativeHandler(m_loop, request, response, observedCancel);
        };
        Core::Task<void> chainTask = runChainOnLoop(pipeline, request, response, handler, exceptionCaught, isFinished);
        ASSERT_TRUE(runOnLoop(chainTask, isFinished));

        // 协作式取消：先让业务看到 stop_requested，再由中间件单点改写 504
        EXPECT_TRUE(observedCancel.load());
        EXPECT_TRUE(request.cancelToken().stop_requested());
        EXPECT_EQ(response.status(), 504);
        EXPECT_EQ(response.body(), "Gateway Timeout");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/plain");
        EXPECT_FALSE(exceptionCaught.load());
    }

    TEST_F(MiddlewareLoopFixture, PassesThroughWithoutTimerForNonPositiveTimeout)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(timeoutMiddleware(m_loop, std::chrono::milliseconds(0)));

        HttpRequest request = makeRequest(HttpMethod::GET, "/no-deadline");
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        std::atomic<bool> exceptionCaught{false};
        std::atomic<bool> isFinished{false};
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

        HttpRequest request = makeRequest(HttpMethod::GET, "/throws");
        HttpResponse response;
        std::atomic<bool> exceptionCaught{false};
        std::atomic<bool> isFinished{false};
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

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 1);
        EXPECT_EQ(response.status(), 200);
    }

    TEST(BodySizeLimitMiddleware, AcceptsDeclaredLengthAtLimit)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(10));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "10");
        HttpResponse response;
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
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 413);
        EXPECT_EQ(response.body(), "Payload Too Large");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/plain");
    }

    TEST(BodySizeLimitMiddleware, RejectsNonNumericContentLengthWith400)
    {
        MiddlewarePipeline pipeline;
        pipeline.use(bodySizeLimitMiddleware(1000));

        HttpRequest request = makeRequest(HttpMethod::POST, "/upload");
        request.addHeader("Content-Length", "many");
        HttpResponse response;
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
        HttpResponse response;
        std::atomic<int> handlerCalls{0};
        runPipeline(pipeline, request, response, terminalWriting(response, "stored", &handlerCalls));

        EXPECT_EQ(handlerCalls.load(), 0);
        EXPECT_EQ(response.status(), 400);
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
        HttpRequest firstRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse firstResponse;
        runPipeline(pipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest secondRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse secondResponse;
        runPipeline(pipeline, secondRequest, secondResponse, terminalWriting(secondResponse, "ok", &handlerCalls));
        EXPECT_EQ(secondResponse.status(), 200);

        HttpRequest thirdRequest = makeRequest(HttpMethod::GET, "/limited");
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

        HttpRequest request = makeRequest(HttpMethod::GET, "/closed");
        HttpResponse response;
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
        HttpRequest firstRequest = makeRequest(HttpMethod::GET, "/windowed");
        HttpResponse firstResponse;
        runPipeline(pipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest exhaustedRequest = makeRequest(HttpMethod::GET, "/windowed");
        HttpResponse exhaustedResponse;
        runPipeline(pipeline, exhaustedRequest, exhaustedResponse, terminalWriting(exhaustedResponse, "ok", &handlerCalls));
        EXPECT_EQ(exhaustedResponse.status(), 429);

        // 轮询等窗口真正过期，不做固定 sleep；上界由 waitForCondition 兜住
        ASSERT_TRUE(waitForCondition([&windowStarted]()
        {
            return std::chrono::steady_clock::now() - windowStarted > kRateLimitWindow;
        }));

        HttpRequest nextWindowRequest = makeRequest(HttpMethod::GET, "/windowed");
        HttpResponse nextWindowResponse;
        runPipeline(pipeline, nextWindowRequest, nextWindowResponse, terminalWriting(nextWindowResponse, "ok", &handlerCalls));
        EXPECT_EQ(nextWindowResponse.status(), 200);
        EXPECT_EQ(handlerCalls.load(), 2);
    }

    TEST(RateLimiterMiddleware, SharesWindowBucketBetweenCopiesOfSameMiddleware)
    {
        // 计数器随 shared_ptr 一起被复制：同一个中间件实例塞进多条管道仍是同一份额度
        MiddlewareFunc limiter = rateLimiterMiddleware(1, std::chrono::seconds(60));
        const MiddlewareFunc copiedLimiter = limiter;

        MiddlewarePipeline firstPipeline;
        firstPipeline.use(limiter);
        MiddlewarePipeline secondPipeline;
        secondPipeline.use(copiedLimiter);

        std::atomic<int> handlerCalls{0};
        HttpRequest firstRequest = makeRequest(HttpMethod::GET, "/shared");
        HttpResponse firstResponse;
        runPipeline(firstPipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest secondRequest = makeRequest(HttpMethod::GET, "/shared");
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
        auto bucket = std::make_shared<TokenBucket>(1.0, 1.0);
        MiddlewarePipeline firstPipeline;
        firstPipeline.use(tokenBucketRateLimiterMiddleware(bucket));
        MiddlewarePipeline secondPipeline;
        secondPipeline.use(tokenBucketRateLimiterMiddleware(bucket));

        std::atomic<int> handlerCalls{0};

        HttpRequest firstRequest = makeRequest(HttpMethod::GET, "/limited");
        HttpResponse firstResponse;
        runPipeline(firstPipeline, firstRequest, firstResponse, terminalWriting(firstResponse, "ok", &handlerCalls));
        EXPECT_EQ(firstResponse.status(), 200);

        HttpRequest secondRequest = makeRequest(HttpMethod::GET, "/limited");
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
        constexpr int kCapacity = 8;
        constexpr int kThreadCount = 8;
        constexpr int kAttemptsPerThread = 200;

        TokenBucket bucket(0.001, static_cast<double>(kCapacity));

        std::atomic<int> successCount{0};
        std::vector<std::thread> workers;
        workers.reserve(kThreadCount);
        for (int index = 0; index < kThreadCount; ++index)
        {
            workers.emplace_back([&bucket, &successCount]
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
} // namespace AsynGyanis::Net
