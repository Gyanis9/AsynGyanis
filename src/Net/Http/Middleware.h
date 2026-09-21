/**
 * @file Middleware.h
 * @brief HTTP 请求/响应中间件管道与常用中间件工厂
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Http/Compression.h"
#include "Net/Http/Gzip.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Base/Log/SourceLocation.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 中间件函数类型。
     *
     * @details next 是「继续走下游」的入口：不调用它即短路，下游中间件与业务处理器都不再执行
     *          （限流、CORS 预检就是这么做的）。
     *
     * @note next 只在本次中间件调用期间有效：它捕获的是管道内部的局部状态，
     *       中间件不得把它存起来留到别的请求里再调用，那是悬垂引用。
     */
    using MiddlewareFunc = std::function<Core::Task<void>(HttpRequest &, HttpResponse &, std::function<Core::Task<void>()>)>;

    /**
     * @brief 中间件管道的终点：已经绑定好业务处理器的可调用对象。
     *
     * @details Router 把「调用命中的 handler」包装成这个类型交给管道，
     *          管道全程按 const 引用传递，避免每条请求拷贝一次 std::function。
     */
    using TerminalHandler = std::function<Core::Task<void>()>;

    /**
     * @brief 中间件管道，按注册顺序依次执行中间件，最终执行业务处理器。
     *
     * @details 执行顺序为「注册顺序正向进入、逆向退出」的洋葱模型：
     *          middleware[0] 的前置 → middleware[1] 的前置 → … → handler → … → middleware[1] 的后置 → middleware[0] 的后置。
     *          任一中间件不 `co_await next()` 即短路，其后的中间件与业务处理器都不再执行。
     *
     * @note 管道本身不做任何加锁：一条 HTTP 连接从头到尾在同一个事件循环线程上串行执行，
     *       中间件内部若要放共享状态，请自行保证与「单循环线程」假设一致（见 rateLimiterMiddleware）。
     */
    class MiddlewarePipeline
    {
    public:
        /**
         * @brief 默认构造函数，创建一个空管道。
         */
        MiddlewarePipeline() = default;

        /**
         * @brief 注册一个中间件到管道末尾。
         * @param middleware 中间件函数，所有权转移给管道
         */
        void use(MiddlewareFunc middleware);

        /**
         * @brief 已注册的中间件数量。
         * @return std::size_t 中间件条数，空管道返回 0
         */
        [[nodiscard]] std::size_t middlewareCount() const noexcept;

        /**
         * @brief 运行中间件管道，从第一个中间件开始依次执行，最终调用业务处理器。
         * @param request  HTTP 请求对象（可被中间件修改）
         * @param response HTTP 响应对象（可被中间件修改）
         * @param handler  终点业务处理器；按 const 引用贯穿整条链，本函数不接管其生命周期
         * @return Core::Task<> 协程任务，整条链结束后完成
         * @throws std::exception 中间件或业务处理器抛出的异常原样向上传播
         * @note handler 必须活到返回的协程结束，调用方（Router）是在自己的协程帧里 co_await 本任务的，
         *       因此把 handler 放在调用方栈上就是安全的
         */
        Core::Task<> run(HttpRequest &request, HttpResponse &response, const TerminalHandler &handler);

    private:
        /**
         * @brief 递归调用中间件链
         * @param index    当前要执行的中间件下标，等于 m_middlewares.size() 时执行终点处理器
         * @param request  请求对象
         * @param response 响应对象
         * @param handler  终点业务处理器（const 引用，逐层继续按引用下传）
         * @return Core::Task<> 协程
         */
        Core::Task<> invoke(std::size_t index, HttpRequest &request, HttpResponse &response, const TerminalHandler &handler);

        std::vector<MiddlewareFunc> m_middlewares; ///< 已注册的中间件，按注册顺序排列
    };

    // ============================================================================
    // MiddlewarePipeline 内联实现
    // ============================================================================

    inline void MiddlewarePipeline::use(MiddlewareFunc middleware)
    {
        // 直接 push_back 并转移所有权：中间件对象可能持有捕获，拷贝一次是白给的开销
        m_middlewares.push_back(std::move(middleware));
    }

    inline std::size_t MiddlewarePipeline::middlewareCount() const noexcept
    {
        return m_middlewares.size();
    }

    inline Core::Task<> MiddlewarePipeline::run(HttpRequest &request, HttpResponse &response, const TerminalHandler &handler)
    {
        // 空管道也要 co_await：终点处理器的异常必须沿同一条传播路径回到调用方
        co_await invoke(0, request, response, handler);
    }

    inline Core::Task<> MiddlewarePipeline::invoke(const std::size_t index, HttpRequest &request, HttpResponse &response, const TerminalHandler &handler)
    {
        // 递归出口：中间件用尽，执行绑定了业务处理器的终点回调
        if (index >= m_middlewares.size())
        {
            co_await handler();
            co_return;
        }

        // next 只按引用捕获下游状态，不进 std::function 的对象里再复制一份 handler，
        // 否则每层中间件都会拷贝一次终点 std::function（大 lambda 的捕获会退化成每请求堆分配）
        co_await m_middlewares[index](request, response, [this, index, &request, &response, &handler]() -> Core::Task<void>
        {
            co_await invoke(index + 1, request, response, handler);
        });
    }

    // ============================================================================
    // 常用中间件工厂
    // ============================================================================

    namespace detail
    {
        /// 看门狗首个睡眠分片，单位毫秒：短请求只多等一个分片就能被回收，量级取「人眼不可察」的 1ms
        inline constexpr std::chrono::milliseconds::rep kTimeoutWatchdogInitialSliceMs = 1;

        /// 看门狗睡眠分片上限，单位毫秒：再长也不会让已完成请求的回收等待超过一个数量级，同时避免高频醒来
        inline constexpr std::chrono::milliseconds::rep kTimeoutWatchdogMaxSliceMs = 25;

        /**
         * @brief 超时看门狗与中间件之间共享的状态
         *
         * @details 只放「标志位」，不放响应对象指针：响应由中间件在链路收口后单点改写，
         *          避免看门狗协程与业务协程在各自的挂起间隙交替写同一个 HttpResponse。
         *          用 shared_ptr 持有是为了让看门狗协程在任何一条退出路径上都不会读到已销毁的状态。
         */
        struct TimeoutGuardState
        {
            bool isChainFinished{false};      ///< 业务链是否已经跑完（看门狗据此提前收工）
            bool isDeadlineReached{false};    ///< 到期标志，由看门狗在超时点位置位
            bool isTimerUnavailable{false};   ///< 定时器创建失败，本轮不做超时约束
        };

        /**
         * @brief 超时看门狗协程：分片睡眠到「业务链结束」或「截止时刻到达」
         * @param loop 事件循环，定时器与调度器都取自它
         * @param request 请求对象，到期时对它发出协作式取消
         * @param timeout 超时时长
         * @param state 与中间件共享的标志位状态
         * @return Core::Task<> 协程，两条正常退出路径（结束/到期）都会正常完成
         * @note 睡眠分片采用「指数增长 + 上限封顶」：短请求只花一个最小分片就能被发现结束，
         *       长请求也不会因为高频醒来而烧 CPU。最后一跳总是夹到截止时刻，超时判定因此是准的。
         */
        inline Core::Task<> timeoutWatchdog(Core::EventLoop &loop, HttpRequest &request, const std::chrono::milliseconds timeout, const std::shared_ptr<TimeoutGuardState> &state)
        {
            try
            {
                // 每请求一个定时器：Timer 内部只有一个描述符，多个并发请求共用会互相覆盖 epoll 注册
                Core::Timer timer(loop);
                const auto deadline = std::chrono::steady_clock::now() + timeout;

                // 指数退避的分片长度，单位毫秒；初值与上限见调用处的常量说明
                std::chrono::milliseconds slice{kTimeoutWatchdogInitialSliceMs};

                while (true)
                {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

                    // 已到截止时刻：发出协作式取消后收工，响应改写交给中间件本体（单点写入）
                    if (remaining <= std::chrono::milliseconds::zero())
                    {
                        state->isDeadlineReached = true;
                        // 这里只「请求」取消：底层业务是否中断取决于它是否检查 cancelToken()
                        request.requestCancel();
                        co_return;
                    }

                    // 挂起点：醒来要么是定时器到期，要么是循环被其他事件唤醒后重新排队
                    co_await timer.waitFor(std::min(slice, remaining));

                    // 业务链已经跑完，中间件马上会回收本协程，无需再等到截止时刻
                    if (state->isChainFinished)
                    {
                        co_return;
                    }

                    // 分片翻倍但封顶，兼顾「短请求少等待」与「长请求少空转」
                    slice = std::min(slice * 2, std::chrono::milliseconds{kTimeoutWatchdogMaxSliceMs});
                }
            } catch (const std::exception &)
            {
                // 定时器描述符创建失败（文件描述符耗尽一类）：退化成「本轮不做超时约束」，
                // 绝不因为超时中间件本身失败而让业务请求直接失败
                state->isTimerUnavailable = true;
                co_return;
            }
        }
    } // namespace detail

    /**
     * @brief 创建一个日志中间件，记录请求 URI、响应状态码与处理耗时。
     * @param logger 日志器；为空指针时本中间件退化为透传（不记日志）
     * @return MiddlewareFunc 中间件函数
     *
     * @details 前置什么都不做，只在 `co_await next()` 返回后记一条 Info 日志，
     *          因此业务短路响应（429、504）同样会被记录，方便压障。
     *
     * @note 参数按 shared_ptr 收取：中间件会被塞进类型擦除的 std::function 里长期存活，
     *       按引用捕获外部对象等于把生命周期交给调用方口头保证，一旦对方传的是局部 logger 就是悬垂引用。
     *       借用 Base::LoggerRegistry 持有的日志器时，可用「空 shared_ptr + 裸指针」构造一个不拥有的
     *       共享指针（注册表单例的生命周期覆盖全进程）；若将来会 unregisterLogger()/clear()，
     *       请改为调用方自己拥有的 shared_ptr。
     */
    inline MiddlewareFunc loggingMiddleware(std::shared_ptr<const Base::Logger> logger)
    {
        return [logger = std::move(logger)](HttpRequest &request, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            const auto startTimePoint = std::chrono::steady_clock::now();
            co_await next();

            // 空指针即「不记录」：让上层无需为「日志可选」这件事再包一层条件判断
            if (logger == nullptr)
            {
                co_return;
            }

            // 耗时用毫秒整数：日志里读数量级够用，浮点秒反而把噪声写进眼睛
            const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTimePoint).count();
            logger->logFormat(Base::LogLevel::Info, Base::SourceLocation::current(),
                              "HTTP 请求 {} -> {}（{}ms）", request.uri(), response.status(), elapsedMilliseconds);
        };
    }

    /**
     * @brief CORS 策略，corsMiddleware 的可选项集合
     */
    struct CorsPolicy
    {
        std::string allowOrigin{"*"};       ///< Access-Control-Allow-Origin 取值，默认放开任意来源
        std::string allowMethods{"GET, POST, PUT, DELETE, PATCH, OPTIONS"}; ///< 预检应答里声明的方法集合，ASCII 逗号分隔
        std::string allowHeaders{"Content-Type, Authorization"};            ///< 预检应答里允许的申请头集合
        std::chrono::seconds maxAge{86400}; ///< 预检结果缓存秒数，0 表示要求每次都发预检
        bool allowCredentials{false};       ///< 是否声明 Access-Control-Allow-Credentials
    };

    /**
     * @brief 创建一个 CORS 中间件，处理跨域请求与 OPTIONS 预检。
     * @param policy 跨域策略，默认值为「允许任意来源、不携带凭据」
     * @return MiddlewareFunc 中间件函数
     *
     * @details 预检请求（OPTIONS 且带 Access-Control-Request-Method）就地生成 204 预检应答，
     *          **不进入业务路由**：业务 handler 收下一个它根本不该处理的 OPTIONS 只会回 404/405，
     *          浏览器侧表现为「CORS 一直不通」。实际请求先挂上允许来源的头再放行下游。
     *
     * @warning 凭据与通配来源不能共存：`Access-Control-Allow-Origin: *` 与
     *          `Access-Control-Allow-Credentials: true` 的组合会被浏览器直接判为失败，
     *          而若为了「让它通」把 `*` 换成回显请求头里的 Origin，就等于允许任意站点
     *          带着 Cookie 读取你的接口（CSRF 的翻版）。因此本实现在 allowOrigin 为 `*` 时
     *          一律不输出 allow-credentials，并附一条 `Vary: Origin` 说明应答随来源而变。
     *          确实要带凭据，请把 allowOrigin 写成具体站点并且自己维护白名单。
     */
    inline MiddlewareFunc corsMiddleware(CorsPolicy policy = {})
    {
        return [policy = std::move(policy)](HttpRequest &request, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            const bool isPreflightRequest = request.method() == HttpMethod::OPTIONS && request.hasHeader("access-control-request-method");

            // 预检分支：应答只描述「允许的跨域方式」，与业务资源无关，所以直接短路
            if (isPreflightRequest)
            {
                response.reset();
                // 204 不带正文：预检应答没有内容体，省掉一次无意义的 Content-Length 计算
                response.setStatus(204);
                response.setHeader("access-control-allow-methods", policy.allowMethods);
                response.setHeader("access-control-allow-headers", policy.allowHeaders);
                response.setHeader("access-control-max-age", std::to_string(policy.maxAge.count()));
                response.setHeader("access-control-allow-origin", policy.allowOrigin);
                response.setHeader("vary", "Origin");

                // 通配来源下不声明允许凭据，理由见 @warning
                if (policy.allowCredentials && policy.allowOrigin != "*")
                {
                    response.setHeader("access-control-allow-credentials", "true");
                }
                co_return;
            }

            // 实际请求分支：头要在下游之前设好，业务若自行改写 origin 也仍有机会覆盖
            response.setHeader("access-control-allow-origin", policy.allowOrigin);
            response.setHeader("vary", "Origin");
            if (policy.allowCredentials && policy.allowOrigin != "*")
            {
                response.setHeader("access-control-allow-credentials", "true");
            }

            co_await next();
        };
    }

    /**
     * @brief 创建一个协作式超时中间件：到期即请求取消业务链并回 504。
     * @param loop    事件循环，用于创建定时器与投递看门狗协程，必须比路由器活得久
     * @param timeout 超时时长；非正值等价于「不启用超时」，只做透传
     * @return MiddlewareFunc 中间件函数
     *
     * @details 看门狗协程投递到调度器上睡到截止时刻，醒来调用 HttpRequest::requestCancel()
     *          并把「已超时」写进共享状态；业务链返回时中间件据该标志把响应重置为 504。
     *          只比实际耗时的写法既取消不了业务、也改不动本轮已经发出的响应。
     *
     * @warning 这是**协作式**超时。单线程协程模型里没有抢占：
     *          @li 业务 handler 必须周期性检查 `request.cancelToken().stop_requested()`
     *              并尽快 `co_return`，否则中间件只能等它跑完再改写 504；
     *          @li 全程不让出 CPU 的 handler（死循环、长同步计算、阻塞调用）既不会被定时唤醒打断，
     *              也会把看门狗一起饿死，因为它同样需要循环空出来才能被调度。
     *          需要「硬超时」请在业务侧使用可中断的等待，或把长任务放到 ThreadPool 并配合 Core::Cancelable。
     *
     * @note 取消信号的落点是 HttpRequest；连接的统一取消入口是 Core::Connection::cancelable()，
     *       会话在每次路由前注册了一次转发，两个来源到期都会体现在同一个 request.cancelToken() 上。
     * @note 每条在途请求会额外占一个定时器描述符与一个协程帧，因此本中间件是按需安装的，
     *       不作为默认管道成员。
     */
    inline MiddlewareFunc timeoutMiddleware(Core::EventLoop &loop, const std::chrono::milliseconds timeout)
    {
        return [&loop, timeout](HttpRequest &request, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            // 非正数视为「不超时」：省掉一次定时器与协程帧，语义比「立刻超时」更符合调用方直觉
            if (timeout <= std::chrono::milliseconds::zero())
            {
                co_await next();
                co_return;
            }

            auto state = std::make_shared<detail::TimeoutGuardState>();

            // 业务链的起始时刻：正常路径由看门狗判到期，这里只给「定时器不可用」的退化路径兜底
            const auto chainStartTimePoint = std::chrono::steady_clock::now();

            // 用 optional 持有：业务跑完之后直接销毁它（见下方收尾处），不必等它按分片醒来
            std::optional<Core::Task<>> watchdogTask = detail::timeoutWatchdog(loop, request, timeout, state);
            // Core::Task 是惰性协程：只构造不会开跑，必须把句柄显式投给调度器
            loop.scheduler().schedule(watchdogTask->handle());

            // 业务链的异常先存起来：无论它正常返回还是抛出，都必须先把看门狗收干净再往上抛
            std::exception_ptr chainException = nullptr;
            try
            {
                co_await next();
            } catch (...)
            {
                chainException = std::current_exception();
            }

            // 置位结束标志：看门狗最迟在当前睡眠分片末尾看到它并正常完成
            state->isChainFinished = true;

            // 回收看门狗协程帧。两条路都不能带着未完成的子协程返回——否则请求对象与响应对象
            // 都可能在协程余下的生命周期里被会话复用掉：
            // @li 已经开跑（挂在定时器上或已结束）：**直接销毁**，不必等它自己按分片醒来
            //     （最长 25ms，而此刻已经不需要它了）。销毁帧即停掉它挂着的定时器。
            // @li 还没开跑：它的句柄仍在调度器的就绪队列里，销毁帧会让队列里留下一个悬空句柄
            //     （实测 ASan 报 resume 未知地址）。这条路 co_await 它——它一被取出就会看到
            //     结束标志并立刻返回，同样不会等满一个分片。
            if (watchdogTask->handle().promise().m_isStarted)
            {
                watchdogTask.reset();
            } else
            {
                co_await *watchdogTask;
            }

            // 定时器申请不到描述符时看门狗根本没能跑起来，退化成「事后按实际耗时判超时」：
            // 至少不会悄悄丢掉超时语义；两条判据互斥，正常路径只走前者
            const bool isElapsedOverTimeout = std::chrono::steady_clock::now() - chainStartTimePoint > timeout && state->isTimerUnavailable;

            // 超时判定只在此处单点改写响应：晚于业务的一切写入，保证线上发的就是 504
            if (state->isDeadlineReached || isElapsedOverTimeout)
            {
                // 流式响应已经发过至少一段时**不能改写**：头部（状态码与头字段）早在上线那一刻定稿，
                // 而 reset() 会把 isChunked/hasSentChunkedHead 一并清掉——会话据此以为这是「新响应」，
                // 于是分块正文中间又插进一条完整的 504，对端直接报协议错。
                // 此时唯一正确的动作是记一条日志、让这条流按已发出的状态码收尾
                if (response.isChunkedResponse() && response.hasSentChunkedHead())
                {
                    LOG_WARN_FMT("TimeoutMiddleware: 请求已超时，但流式响应的头部早已上线，无法改写成 504；"
                                 "该流将按已发出的状态码收尾（对端不会看到超时语义）");
                }
                else
                {
                    // 先重置再填：业务在半路上写的头与正文都可能带着「已经成功」的痕迹，留着会误导客户端
                    response.reset();
                    response.setStatus(504);
                    response.setBody("Gateway Timeout");
                    response.setHeader("content-type", "text/plain");
                }
            }

            // 异常原样上抛，由会话统一转成 500：超时中间件不改变业务的失败语义
            if (chainException)
            {
                std::rethrow_exception(chainException);
            }
        };
    }

    /**
     * @brief 创建一个速率限制中间件。
     * @param maximumRequestCount 在 windowDuration 时间窗口内允许的最大请求数，0 表示全部拒绝
     * @param windowDuration      时间窗口长度；以毫秒为单位接收，传秒字面量会自动换算
     * @return MiddlewareFunc 中间件函数
     *
     * @details 固定窗口计数（窗口到期整体清零）。超出上限时直接回 429 Too Many Requests
     *          并带上 Retry-After，**不调用下游**，因此业务 handler 不会被无谓唤醒。
     *
     * @note 计数器为「每个中间件实例一份」，进程内全局共享，非线程安全：
     *       它假定所有会话跑在同一个事件循环线程上。多循环/多进程部署请换用原子计数或外部存储。
     * @warning 固定窗口有两个坑：
     *          @li **临界突发**：窗口切换的前后各放行一整批，瞬时速率可达配置值的两倍；
     *          @li **额度按实例算**：本中间件注册到哪个服务器，额度就只属于那个服务器。多监听器
     *             部署（每循环一个服务器）时若逐个注册，进程级的实际上限会乘上监听器数量。
     *          要一个真正的全局上限，用下面基于共享令牌桶的 tokenBucketRateLimiterMiddleware()。
     */
    inline MiddlewareFunc rateLimiterMiddleware(const std::size_t maximumRequestCount, const std::chrono::milliseconds windowDuration)
    {
        struct WindowBucket
        {
            std::chrono::steady_clock::time_point windowStartTimePoint{std::chrono::steady_clock::now()}; ///< 本窗口起点
            std::size_t requestCount{0};                                                                  ///< 本窗口已放行的请求数
        };

        auto bucket = std::make_shared<WindowBucket>();

        return [bucket, maximumRequestCount, windowDuration]([[maybe_unused]] HttpRequest &request, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            // 先滑动窗口再计数：窗口刚过期的请求应该落进新窗口，而不是被旧窗口的余额拒绝
            const auto nowTimePoint = std::chrono::steady_clock::now();
            if (nowTimePoint - bucket->windowStartTimePoint > windowDuration)
            {
                bucket->windowStartTimePoint = nowTimePoint;
                bucket->requestCount         = 0;
            }

            ++bucket->requestCount;

            // 超限分支：429 + Retry-After，正文 ASCII，原因短语由 HttpResponse 侧统一给出
            if (bucket->requestCount > maximumRequestCount)
            {
                response.setStatus(429);
                response.setBody("Too Many Requests");
                response.setHeader("content-type", "text/plain");
                // Retry-After 按 RFC 9110 §10.2.3 是「秒」为单位的 delta-seconds，
                // 窗口以毫秒配置时向上取整换算，避免亚秒窗口被截成 0 让客户端立刻重试
                const auto retryAfterSeconds = (windowDuration.count() + 999) / 1000;
                response.setHeader("retry-after", std::to_string(retryAfterSeconds));
                co_return;
            }

            co_await next();
        };
    }

    /**
     * @brief 令牌桶：按恒定速率补令牌，取不到即拒绝
     *
     * @details 与固定窗口的分工：固定窗口在窗口切换的前后各放行一整批（瞬时速率可达配置值的两倍），
     *          令牌桶按经过的时间连续补令牌、超出容量的部分丢弃，长期速率精确等于配置值，
     *          桶容量则决定能容忍多大的瞬时突发。
     *
     * @note 线程安全（一把互斥量护住桶状态）：会话可能跑在不同循环线程上。**要在所有监听器之间
     *       共享同一实例**才能构成进程级的全局 RPS 上限——每个服务器各持一份的话，实际上限会乘上
     *       监听器数量（与 PerIpConnectionLimiter 同一类坑）。
     */
    class TokenBucket
    {
    public:
        /**
         * @brief 构造令牌桶
         * @param tokensPerSecond 补令牌速率（每秒多少个），必须为正
         * @param burstCapacity 桶容量，即瞬时允许的突发量，必须 ≥ 1
         * @throws Base::InvalidArgumentException 速率非正或容量小于 1
         * @note 容量必须 ≥ 1：容量小于 1 的桶永远攒不满一个令牌，等于把所有请求都拒掉——
         *       那是配置错误，不是「严格限流」，所以直接抛而不是静默全拒
         */
        TokenBucket(const double tokensPerSecond, const double burstCapacity) :
            m_tokensPerSecond(tokensPerSecond), m_burstCapacity(burstCapacity), m_tokenCount(burstCapacity)
        {
            if (!(m_tokensPerSecond > 0.0))
            {
                throw Base::InvalidArgumentException("TokenBucket: 补令牌速率必须为正数");
            }
            if (!(m_burstCapacity >= 1.0))
            {
                throw Base::InvalidArgumentException("TokenBucket: 桶容量必须不小于 1，否则永远放行不了任何请求");
            }
        }

        /**
         * @brief 尝试取走一个令牌
         * @return true 取到（放行）；false 桶里不足一个令牌（拒绝）
         */
        [[nodiscard]] bool tryAcquire()
        {
            std::lock_guard<std::mutex> guard(m_mutex);

            // 先按经过的时间补令牌，再封顶到桶容量：不封顶的话，空闲一天攒下的令牌够放行一整天的流量，
            // 限流形同虚设。补令牌只算经过的时间，因此长期平均速率恰为配置值
            const auto nowTimePoint = std::chrono::steady_clock::now();
            const double elapsedSeconds = std::chrono::duration<double>(nowTimePoint - m_lastRefillTimePoint).count();
            m_tokenCount = std::min(m_burstCapacity, m_tokenCount + elapsedSeconds * m_tokensPerSecond);
            m_lastRefillTimePoint = nowTimePoint;

            if (m_tokenCount < 1.0)
            {
                return false;
            }
            m_tokenCount -= 1.0;
            return true;
        }

        /**
         * @brief 当前桶里的令牌数（观测用，不参与判定）
         * @return double 令牌数，取值在 [0, 桶容量] 内
         */
        [[nodiscard]] double availableTokenCount() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_tokenCount;
        }

        /**
         * @brief 距离下一个令牌可用还有多久
         * @return std::chrono::milliseconds 等待时长；桶里已有令牌时为 0
         * @note 给 Retry-After 用：按「还差多少令牌 ÷ 补令牌速率」折算，不四舍五入成 0
         */
        [[nodiscard]] std::chrono::milliseconds timeUntilTokenAvailable() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (m_tokenCount >= 1.0)
            {
                return std::chrono::milliseconds::zero();
            }

            const double secondsUntilAvailable = (1.0 - m_tokenCount) / m_tokensPerSecond;
            const auto millisecondsUntilAvailable =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(secondsUntilAvailable));
            // 亚毫秒的等待向上取整到 1ms：返回 0 会让调用方以为「立刻就能重试」，反而制造空转
            return std::max(millisecondsUntilAvailable, std::chrono::milliseconds{1});
        }

    private:
        mutable std::mutex                    m_mutex;                ///< 保护下面几项；桶可能被多个循环线程同时取
        double                                m_tokensPerSecond;      ///< 补令牌速率（个/秒）
        double                                m_burstCapacity;        ///< 桶容量（瞬时突发上限）
        double                                m_tokenCount;           ///< 当前令牌数
        std::chrono::steady_clock::time_point m_lastRefillTimePoint{std::chrono::steady_clock::now()}; ///< 上次补令牌的时刻
    };

    /**
     * @brief 创建基于共享令牌桶的速率限制中间件（超限回 429 Too Many Requests）
     * @param bucket 令牌桶；**由调用方在所有监听器之间共享同一份**才能构成全局 RPS 上限
     * @return MiddlewareFunc 中间件函数
     * @throws Base::InvalidArgumentException bucket 为空指针
     *
     * @details 取不到令牌时直接回 429 并附 Retry-After，**不调用下游**：业务 handler 不会被唤醒。
     *
     * @note 空指针是用法错误而不是「不限制」：静默放行会让调用方以为限流生效，实际完全没有保护；
     *       要「不限制」就不要注册本中间件。
     * @see TokenBucket, rateLimiterMiddleware()
     */
    inline MiddlewareFunc tokenBucketRateLimiterMiddleware(std::shared_ptr<TokenBucket> bucket)
    {
        if (bucket == nullptr)
        {
            throw Base::InvalidArgumentException("tokenBucketRateLimiterMiddleware: 令牌桶不能为空指针；不需要限流就不要注册本中间件");
        }

        return [bucket = std::move(bucket)]([[maybe_unused]] HttpRequest &request, HttpResponse &response,
                                           const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            if (!bucket->tryAcquire())
            {
                response.setStatus(429);
                response.setBody("Too Many Requests");
                response.setHeader("content-type", "text/plain");

                // Retry-After 的单位是秒（RFC 9110 §10.2.3 的 delta-seconds），因此把毫秒向上取整到秒、
                // 且不低于 1：报 0 等于告诉客户端「立刻重试」，那正是限流要避免的
                const auto waitMilliseconds = bucket->timeUntilTokenAvailable().count();
                const auto retryAfterSeconds = std::max<std::int64_t>(1, (waitMilliseconds + 999) / 1000);
                response.setHeader("retry-after", std::to_string(retryAfterSeconds));
                co_return;
            }

            co_await next();
        };
    }

    /**
     * @brief 创建一个请求体大小限制中间件。
     * @param maximumBodySize 允许的最大请求体字节数
     * @return MiddlewareFunc 中间件函数
     *
     * @details 只看 Content-Length 声明值，在路由到业务处理器之前拦下超限请求并回 413。
     *          这是一道「便宜的前置闸」：真正的硬上限由 HttpParser 在收字节时把关，
     *          所以即使客户端谎报 Content-Length，也不会绕过解析器那一层。
     *
     * @note 与解析器策略保持一致：Content-Length 非法（非数字、越界）判 400 而不是放行，
     *       谎报长度的报文要么走私要么残缺，交给业务处理没有好处。
     */
    inline MiddlewareFunc bodySizeLimitMiddleware(const std::size_t maximumBodySize)
    {
        return [maximumBodySize](HttpRequest &request, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            if (const auto contentLengthHeader = request.getHeader("content-length"))
            {
                const std::string &declaredText = contentLengthHeader.value();

                // Content-Length 必须是纯十进制数字串（RFC 9110 §8.6）。不用 std::stoull：
                // "-1" 会绕进 unsigned long long 变成 ULLONG_MAX，"12abc" 按前缀解析成 12，
                // 只有越界才抛异常，三类畸形口径不一致。from_chars 判据统一：解析失败或有残留即不合法
                unsigned long long declaredBodyLength = 0;
                const std::from_chars_result parseResult = std::from_chars(declaredText.data(), declaredText.data() + declaredText.size(), declaredBodyLength);

                if (parseResult.ec != std::errc{} || parseResult.ptr != declaredText.data() + declaredText.size())
                {
                    response.setStatus(400);
                    response.setBody("Bad Request: Invalid Content-Length");
                    response.setHeader("content-type", "text/plain");
                    co_return;
                }

                // 声明值超上限：立刻短路，后续字节连解析都不用进
                if (declaredBodyLength > maximumBodySize)
                {
                    response.setStatus(413);
                    response.setBody("Payload Too Large");
                    response.setHeader("content-type", "text/plain");
                    co_return;
                }
            }

            co_await next();
        };
    }

    namespace detail
    {
        /**
         * @brief 判断 Accept-Encoding 是否接受某个编码（含 q 值语义）
         * @details 既要认出目标编码名（大小写不敏感），也要认出 `*`（通配）与 `gzip;q=0`（明确拒绝）。
         *          q=0 是**拒绝**而不是「优先级最低」：按 RFC 9110 §12.5.3 必须当作不可接受，
         *          发该编码的正文等于违反协商。
         * @param acceptEncoding Accept-Encoding 头部的值，缺头时传空串
         * @param targetEncoding 待判定的编码名（小写，如 "gzip"/"br"/"zstd"）
         * @return true 可以使用该编码
         */
        [[nodiscard]] inline bool acceptsEncoding(const std::string_view acceptEncoding, const std::string_view targetEncoding)
        {
            std::size_t offset = 0;
            bool        wildcardAccepted = false;

            while (offset < acceptEncoding.size())
            {
                const std::size_t commaPosition = acceptEncoding.find(',', offset);
                const std::size_t entryEnd      = commaPosition == std::string_view::npos ? acceptEncoding.size() : commaPosition;

                std::string_view entry = acceptEncoding.substr(offset, entryEnd - offset);
                offset                 = entryEnd + 1;

                // 拆出编码名与参数（形如 gzip;q=0.5）
                const std::size_t semicolonPosition = entry.find(';');
                std::string_view  encodingName      = entry.substr(0, semicolonPosition);
                const std::string_view parameters   = semicolonPosition == std::string_view::npos ? std::string_view{}
                                                                                                   : entry.substr(semicolonPosition + 1);

                // 去掉首尾空白：头部里 "gzip ; q=0" 这种写法同样合法
                while (!encodingName.empty() && (encodingName.front() == ' ' || encodingName.front() == '\t'))
                {
                    encodingName.remove_prefix(1);
                }
                while (!encodingName.empty() && (encodingName.back() == ' ' || encodingName.back() == '\t'))
                {
                    encodingName.remove_suffix(1);
                }

                bool       isRejected = false;
                std::size_t parameterOffset = 0;
                while (parameterOffset < parameters.size())
                {
                    const std::size_t nextSeparator = parameters.find(';', parameterOffset);
                    const std::size_t parameterEnd =
                            nextSeparator == std::string_view::npos ? parameters.size() : nextSeparator;
                    std::string_view parameter = parameters.substr(parameterOffset, parameterEnd - parameterOffset);
                    parameterOffset                  = parameterEnd + 1;

                    // 参数自身也要去空白："gzip ; q=0" 里 q 之前有空格，不裁掉就认不出这次拒绝
                    while (!parameter.empty() && (parameter.front() == ' ' || parameter.front() == '\t'))
                    {
                        parameter.remove_prefix(1);
                    }
                    while (!parameter.empty() && (parameter.back() == ' ' || parameter.back() == '\t'))
                    {
                        parameter.remove_suffix(1);
                    }

                    if (parameter.size() >= 2 && (parameter[0] == 'q' || parameter[0] == 'Q') && parameter[1] == '=')
                    {
                        const std::string_view quality = parameter.substr(2);
                        // q=0 与 q=0.0、q=0.000 都是拒绝；只判「值是否全为 0 与小数点」足够，不必真解析
                        bool isZeroQuality = !quality.empty();
                        for (const char character: quality)
                        {
                            if (character != '0' && character != '.')
                            {
                                isZeroQuality = false;
                                break;
                            }
                        }
                        isRejected = isRejected || isZeroQuality;
                    }
                }

                if (encodingName == "*")
                {
                    wildcardAccepted = !isRejected;
                    continue;
                }

                // 编码名按 token 语义大小写不敏感（RFC 9110 §12.5.3）
                if (equalsIgnoringCase(encodingName, targetEncoding))
                {
                    return !isRejected;
                }
            }

            return wildcardAccepted;
        }

        /**
         * @brief 判断内容类型是否属于「已经压过、再压没用」的一类
         * @param contentType Content-Type 头部的值（含可能的 charset 参数）
         * @return true 不值得压缩
         */
        [[nodiscard]] inline bool isIncompressibleContentType(const std::string_view contentType)
        {
            // 只比前缀，参数（;charset=...）不参与比较；媒体类型大小写不敏感
            constexpr std::string_view kIncompressiblePrefixes[] = {"image/", "video/", "audio/", "font/",
                                                                    "application/zip", "application/gzip", "application/x-gzip",
                                                                    "application/x-7z-compressed", "application/x-rar-compressed"};
            for (const std::string_view prefix: kIncompressiblePrefixes)
            {
                if (contentType.size() < prefix.size())
                {
                    continue;
                }

                bool isSamePrefix = true;
                for (std::size_t index = 0; index < prefix.size(); ++index)
                {
                    const char actual   = contentType[index];
                    const char expected = prefix[index];
                    const char lowered  = (actual >= 'A' && actual <= 'Z') ? static_cast<char>(actual - 'A' + 'a') : actual;
                    if (lowered != expected)
                    {
                        isSamePrefix = false;
                        break;
                    }
                }
                if (isSamePrefix)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 把 Vary 补上 accept-encoding（已含则不重复）
         * @details 压缩后的表示与 Accept-Encoding 有关，缓存必须按它分桶，否则会把压缩副本发给
         *          不接受压缩的客户端（反之亦然）
         * @param response 响应
         */
        inline void appendVaryAcceptEncoding(HttpResponse &response)
        {
            const std::optional<std::string> existingVary = response.getHeader("vary");
            if (!existingVary.has_value())
            {
                response.setHeader("vary", "accept-encoding");
                return;
            }

            // 已有 Vary：逐 token 找 accept-encoding，大小写不敏感
            std::string_view remaining = *existingVary;
            while (!remaining.empty())
            {
                const std::size_t commaPosition = remaining.find(',');
                std::string_view  token         = remaining.substr(0, commaPosition);
                remaining = commaPosition == std::string_view::npos ? std::string_view{} : remaining.substr(commaPosition + 1);

                while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                {
                    token.remove_prefix(1);
                }
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                {
                    token.remove_suffix(1);
                }

                if (token.size() == 15)
                {
                    bool isAcceptEncoding = true;
                    constexpr std::string_view kAcceptEncoding = "accept-encoding";
                    for (std::size_t index = 0; index < kAcceptEncoding.size(); ++index)
                    {
                        const char actual  = token[index];
                        const char lowered = (actual >= 'A' && actual <= 'Z') ? static_cast<char>(actual - 'A' + 'a') : actual;
                        if (lowered != kAcceptEncoding[index])
                        {
                            isAcceptEncoding = false;
                            break;
                        }
                    }
                    if (isAcceptEncoding)
                    {
                        return;
                    }
                }
            }

            response.setHeader("vary", *existingVary + ", accept-encoding");
        }

        /// 压缩算法偏好顺序（对端都接受时按此挑选）：zstd 压缩率与速度综合最好、brotli 次之
        /// （静态内容尤佳）、gzip 兜底兼容。加算法按偏好插进这张表即可
        inline constexpr std::string_view kCompressionPreference[] = {"zstd", "br", "gzip"};
    } // namespace detail

    /**
     * @brief 响应压缩选项
     * @details 三种算法各有自己的档位语义：gzip 级别 1..9（zlib 的 6 是折中）、brotli 质量 0..11
     *          （6 与 gzip 6 同档）、zstd 级别 1..22（3 是库自身的平衡点）。越界由各压缩构件夹取
     */
    struct CompressionOptions
    {
        std::size_t minimumBodySize = 1024;                  ///< 正文达到该字节数才压缩；小正文压缩后往往更大，白烧 CPU
        int         gzipLevel       = kDefaultGzipLevel;     ///< gzip 压缩级别，1..9
        int         brotliQuality   = kDefaultBrotliQuality; ///< brotli 压缩质量，0..11
        int         zstdLevel       = kDefaultZstdLevel;     ///< zstd 压缩级别，1..22
    };

    namespace detail
    {
        /**
         * @brief 按选定编码压一次正文，失败（含内存不足）返回空
         * @details 就地压与外置到工作线程压共用这一份实现：两条路径的产物必须逐字节相同，
         *          否则开关一拨就换了一套编码语义
         * @param encoding 选定的编码名，取值见 kCompressionPreference
         * @param body 待压的正文
         * @param options 各算法的档位
         * @return std::optional<std::string> 压缩结果；为空表示没压成
         */
        [[nodiscard]] inline std::optional<std::string> compressWithEncoding(const std::string_view encoding,
                                                                            const std::string_view body,
                                                                            const CompressionOptions options)
        {
            if (encoding == "zstd")
            {
                return zstdCompress(body, options.zstdLevel);
            }
            if (encoding == "br")
            {
                return brotliCompress(body, options.brotliQuality);
            }
            return gzipCompress(body, options.gzipLevel);
        }
    } // namespace detail

    namespace detail
    {
        /**
         * @brief 响应压缩中间件的实现体：就地压与外置到工作线程压共用同一套判断与头部改写
         * @details 两条路径必须产出一样的字节与头，否则「换一种执行位置」本身就成了行为变更
         * @param offloadExecutor 非空时把压缩交给它的工作线程，为空时在调用协程所在线程上压完
         * @param completionLoop 外置时恢复协程用的事件循环，须是处理器所属的那条循环
         * @param options 压缩选项（阈值与各算法档位）
         * @return MiddlewareFunc 中间件
         */
        inline MiddlewareFunc compressionMiddlewareImplementation(Core::AsyncExecutor *offloadExecutor,
                                                                Core::EventLoop *completionLoop,
                                                                const CompressionOptions options)
        {
            return [offloadExecutor, completionLoop, options](HttpRequest &request, HttpResponse &response,
                                                              const std::function<Core::Task<void>()> next) -> Core::Task<>
            {
                co_await next();

                // 已经声明过编码（业务自己压的，或上游中间件压的）：再压一层对端解不开
                if (response.hasHeader("content-encoding"))
                {
                    co_return;
                }

                // 流式响应逐段写出、长度对序列化层未知，压不了整块；无正文的状态码没有可压的内容
                if (response.isChunkedResponse() || response.carriesNoContent())
                {
                    co_return;
                }

                // 区间响应：正文只是某个区间的一段字节，压缩它会让对端的区间语义错乱（206 必带 content-range）
                if (response.hasHeader("content-range"))
                {
                    co_return;
                }

                // 协商：按偏好顺序（zstd > br > gzip）挑第一个被对端接受的编码；
                // q=0 视为明确拒绝，`*` 视为接受（语义见 detail::acceptsEncoding）
                const std::string acceptEncodingHeader = request.getHeader("accept-encoding").value_or(std::string{});
                std::string_view  selectedEncoding;
                for (const std::string_view candidate: detail::kCompressionPreference)
                {
                    if (detail::acceptsEncoding(acceptEncodingHeader, candidate))
                    {
                        selectedEncoding = candidate;
                        break;
                    }
                }
                if (selectedEncoding.empty())
                {
                    co_return;
                }

                const std::string_view body = response.body();
                if (body.size() < options.minimumBodySize)
                {
                    co_return;
                }

                if (const std::optional<std::string> contentType = response.getHeader("content-type");
                    contentType.has_value() && detail::isIncompressibleContentType(*contentType))
                {
                    co_return;
                }

                std::optional<std::string> compressed;
                if (offloadExecutor != nullptr)
                {
                    // 交给工作线程的只有这份副本与编码名：响应对象、协程帧都属于循环线程，
                    // 跨线程碰它们就是数据竞争。恢复落在 completionLoop 上，因此下面的改写仍在原线程
                    std::string bodyCopy{body};
                    const std::string_view encoding = selectedEncoding;
                    compressed = co_await offloadExecutor->submit<std::optional<std::string>>(
                            *completionLoop,
                            [bodyCopy, encoding, options]
                            {
                                return compressWithEncoding(encoding, bodyCopy, options);
                            });
                } else
                {
                    compressed = compressWithEncoding(selectedEncoding, body, options);
                }
                if (!compressed.has_value())
                {
                    // 压缩失败（内存不足）不是错误响应：照原样发未压缩正文，别让对端拿到半截数据
                    co_return;
                }

                // 正文表示变了：强 ETag 必须降级为弱校验器（RFC 9110 §8.8.1），否则缓存会把
                // 压缩副本与未压缩副本当成同一份表示
                if (const std::optional<std::string> entityTag = response.getHeader("etag");
                    entityTag.has_value() && !entityTag->starts_with("W/"))
                {
                    response.setHeader("etag", "W/" + *entityTag);
                }

                detail::appendVaryAcceptEncoding(response);
                response.setHeader("content-encoding", std::string(selectedEncoding));
                // 正文表示变了，业务此前显式声明过的 content-length（如静态文件对 HEAD 用的
                // 「先声明长度、不读正文」）此刻描述的是未压缩正文的字节数，必须按压缩后的
                // 实际字节数改写：留着旧值就是「头部说一万字节、实际只有三千」，keep-alive 上
                // 对端按旧长度截断，余下字节被当成下一条响应。
                // setBody 刻意保留调用方声明过的长度（那是 HEAD/静态文件路径赖以省一次读的手段），
                // 因此改写这条头是替换正文的一方——也就是本中间件——自己的责任
                response.setHeader("content-length", std::to_string(compressed->size()));
                // 压缩结果此刻只被本中间件持有，交出所有权直接移动进响应，省掉一整份压缩字节的拷贝与再分配
                response.setOwnedBody(std::move(*compressed));
                co_return;
            };
        }
    } // namespace detail

    /**
     * @brief 响应压缩中间件（zstd / brotli / gzip），压缩在调用协程所在线程上做完
     *
     * @details 在业务处理完之后判断并压缩响应正文；对所有路由生效，静态文件命中压缩时从 mmap 零拷贝换成
     *          内存正文（阈值默认 1 KiB，是「省带宽」与「多一次拷贝 + 一次压缩」的取舍）。对端接受多种编码时
     *          按固定偏好挑选：zstd > br > gzip，不支持按 q 值重排。
     * @param options 压缩选项（阈值与各算法档位），见 CompressionOptions
     * @return MiddlewareFunc 中间件
     * @note 不压缩的情形（都会原样发正文）：对端不接受任何受支持的编码（含 `gzip;q=0` 这类明确拒绝）、
     *       响应已带 content-encoding、流式响应、无正文的状态码、区间响应（206/content-range）、
     *       正文小于阈值、内容类型属于已压缩媒体
     * @note 压缩会改写 ETag 为弱校验器（RFC 9110 §8.8.1）：正文表示变了，强校验器不能再复用；
     *       h1 与 h2 共用同一份响应序列化，因此两条路径都生效
     * @warning 正文可能很大时别用这一版：HTTP 三条路径的协程都跑在事件循环线程上，一条正文压多久，
     *          同一条循环上的其他连接就等多久。256 KiB 近似真实正文的占用是 gzip6 4.4 ms、
     *          brotli6 4.2 ms、gzip1 1.3 ms、zstd3 0.45 ms（微基准四条 `*-response-compress-256k`）；
     *          线上同结论：单条循环上只跑一路持续要 gzip 大正文的客户端，就能把同循环小请求的 p50
     *          从 37us 顶到 5.9ms。改用下面带执行器的那一版把压缩挪出循环线程
     * @see compressionMiddleware(Core::EventLoop &, Core::AsyncExecutor &, CompressionOptions)
     */
    inline MiddlewareFunc compressionMiddleware(const CompressionOptions options = {})
    {
        return detail::compressionMiddlewareImplementation(nullptr, nullptr, options);
    }

    /**
     * @brief 响应压缩中间件，压缩交给工作线程、完成后回到指定事件循环
     * @details 判断、头部改写与就地版完全同源（同一份实现体），差别只在压缩那一步落在哪个线程：
     *          循环线程因此不会被一次压缩占住数毫秒，代价是多一份正文副本（按值交给工作线程）与
     *          一次跨线程恢复。
     * @param completionLoop 恢复协程用的事件循环，必须是处理器所属的那条循环——否则响应改写会
     *        发生在别的线程上，与那条循环的会话状态构成数据竞争
     * @param offloadExecutor 承载压缩的工作线程池，见 Core::AsyncExecutor
     * @param options 压缩选项（阈值与各算法档位）
     * @return MiddlewareFunc 中间件
     * @warning 执行器与循环都必须比返回的中间件活得久：中间件只握着它们的裸指针，
     *          注册进服务器后由每条请求复用
     * @warning 前提是这条传输路径的会话收尾会**等**处理器协程跑完（HTTP/1.1 与 HTTP/2 的请求路径
     *          按结构成立：帧在会话帧里、服务器只在 isReady() 后回收）。挂起的帧若被提前销毁，
     *          已经投回循环的那次恢复就是悬垂句柄。HTTP/3 的连接异常关闭目前不满足这条
     *          （收口只看连接级在途动作，不看会话里分离的派发协程），因此 h3 一侧暂用就地版
     */
    inline MiddlewareFunc compressionMiddleware(Core::EventLoop &completionLoop, Core::AsyncExecutor &offloadExecutor,
                                              const CompressionOptions options = {})
    {
        return detail::compressionMiddlewareImplementation(&offloadExecutor, &completionLoop, options);
    }

} // namespace AsynGyanis::Net
