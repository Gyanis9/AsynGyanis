/**
 * @file Middleware.h
 * @brief 请求/响应处理的中间件管道
 * @copyright Copyright (c) 2026
 */
#ifndef NET_MIDDLEWARE_H
#define NET_MIDDLEWARE_H

#include "Core/Task.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

#include "Base/Logger.h"

#include <chrono>
#include <functional>
#include <vector>

namespace Net
{
    /**
     * @brief 中间件函数类型。
     *
     * 接收请求、响应和一个 next 函数，执行前置处理，调用 next() 继续后续逻辑，再进行后置处理。
     * 协程返回值 Task<void>。
     */
    using MiddlewareFunc = std::function<Core::Task<void>(HttpRequest &, HttpResponse &, std::function<Core::Task<void>()>)>;

    /**
     * @brief 中间件管道，按注册顺序依次执行中间件，最终执行业务处理器。
     *
     * 支持链式调用，每个中间件可以决定是否继续执行下一个中间件（通过 co_await next()）。
     */
    class MiddlewarePipeline
    {
    public:
        /**
         * @brief 默认构造函数，创建一个空的中间件管道。
         */
        MiddlewarePipeline() = default;

        /**
         * @brief 注册一个中间件到管道末尾。
         * @param middleware 中间件函数
         */
        void use(MiddlewareFunc middleware);

        /**
         * @brief 运行中间件管道，从第一个中间件开始依次执行，最终调用业务处理器。
         * @param req     HTTP 请求对象（可被中间件修改）
         * @param res     HTTP 响应对象（可被中间件修改）
         * @param handler 最终的业务处理器（协程任务）
         * @return Task<> 协程，完成后返回
         */
        Core::Task<> run(HttpRequest &req, HttpResponse &res, std::function<Core::Task<>()> handler);

    private:
        /**
         * @brief 递归调用中间件链。
         * @param index   当前中间件索引
         * @param req     请求对象
         * @param res     响应对象
         * @param handler 最终处理器
         * @return Task<> 协程
         */
        Core::Task<> invoke(size_t index, HttpRequest &req, HttpResponse &res, std::function<Core::Task<>()> handler);

        std::vector<MiddlewareFunc> m_middlewares; ///< 存储已注册的中间件
    };

    // ============================================================================
    // MiddlewarePipeline 内联实现
    // ============================================================================

    inline void MiddlewarePipeline::use(MiddlewareFunc middleware)
    {
        m_middlewares.push_back(std::move(middleware));
    }

    inline Core::Task<> MiddlewarePipeline::run(HttpRequest &req, HttpResponse &res, std::function<Core::Task<void>()> handler)
    {
        co_await invoke(0, req, res, std::move(handler));
    }

    inline Core::Task<> MiddlewarePipeline::invoke(size_t index, HttpRequest &req, HttpResponse &res, std::function<Core::Task<void>()> handler)
    {
        if (index >= m_middlewares.size())
        {
            co_await handler();
            co_return;
        }

        co_await m_middlewares[index](req, res, [this, index, &req, &res, handler]() -> Core::Task<void>
        {
            co_await invoke(index + 1, req, res, handler);
        });
    }

    // ============================================================================
    // 常用中间件工厂函数
    // ============================================================================

    /**
     * @brief 创建一个日志中间件，记录请求 URI、响应状态码和处理耗时。
     * @param logger 日志记录器引用
     * @return MiddlewareFunc 中间件函数
     *
     * 该中间件会在请求处理前记录开始时间，处理后计算耗时并输出日志。
     * 注意：中间件签名要求非 const 引用，但内部只读取，不修改请求/响应。
     */
    inline MiddlewareFunc loggingMiddleware(Base::Logger &logger)
    {
        return [&logger](HttpRequest &req, HttpResponse &res, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            const auto start = std::chrono::steady_clock::now();
            co_await next();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            logger.logFormat(Base::LogLevel::INFO, Base::SourceLocation::current(),
                "{} -> {} {}ms", req.uri(), res.status(), elapsed);
        };
    }

    /**
     * @brief 创建一个 CORS 中间件，为响应添加跨域资源共享头部。
     * @return MiddlewareFunc 中间件函数
     *
     * 添加的头部包括：
     * - access-control-allow-origin: *
     * - access-control-allow-methods: 常见 HTTP 方法
     * - access-control-allow-headers: Content-Type, Authorization
     * - access-control-max-age: 86400
     */
    inline MiddlewareFunc corsMiddleware()
    {
        return [](HttpRequest &req, HttpResponse &res, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            res.setHeader("access-control-allow-origin", "*");
            res.setHeader("access-control-allow-methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            res.setHeader("access-control-allow-headers", "Content-Type, Authorization");
            res.setHeader("access-control-max-age", "86400");

            co_await next();
        };
    }

    /**
     * @brief 创建一个超时中间件，若处理器在指定时间内未完成则返回 504 Gateway Timeout。
     * @param timeout 超时持续时间
     * @return MiddlewareFunc 中间件函数
     *
     * 使用 steady_clock 测量处理器耗时，超时后标记 504 响应。
     * 不创建额外线程，零开销。需要真正中断处理器的场景应在业务层使用 Core::Timer。
     */
    inline MiddlewareFunc timeoutMiddleware(const std::chrono::milliseconds timeout)
    {
        return [timeout](HttpRequest &req, HttpResponse &res, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            const auto start = std::chrono::steady_clock::now();
            co_await next();
            if (std::chrono::steady_clock::now() - start > timeout)
            {
                res.setStatus(504);
                res.setBody("Gateway Timeout");
                res.setHeader("content-type", "text/plain");
            }
        };
    }

    /**
     * @brief 创建一个速率限制中间件。
     * @param maxRequests 在 window 时间窗口内允许的最大请求数
     * @param window      时间窗口长度
     * @return MiddlewareFunc 中间件函数
     *
     * 使用滑动窗口计数器实现简单的速率限制。
     * 超出限制时返回 429 Too Many Requests，不调用下游处理器。
     *
     * @note 当前实现为进程内全局计数器，非线程安全（适用于单 EventLoop 线程模型）。
     *       多线程部署需替换为原子计数或外部存储（Redis）。
     */
    inline MiddlewareFunc rateLimiterMiddleware(const size_t maxRequests, const std::chrono::seconds window)
    {
        struct Bucket
        {
            std::chrono::steady_clock::time_point windowStart{std::chrono::steady_clock::now()};
            size_t                                count{0};
        };

        auto bucket = std::make_shared<Bucket>();

        return [bucket, maxRequests, window](HttpRequest &req, HttpResponse &res, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {

            // 滑动窗口：窗口过期则重置
            if (const auto now = std::chrono::steady_clock::now(); now - bucket->windowStart > window)
            {
                bucket->windowStart = now;
                bucket->count       = 0;
            }

            ++bucket->count;

            if (bucket->count > maxRequests)
            {
                res.setStatus(429);
                res.setBody("Too Many Requests");
                res.setHeader("content-type", "text/plain");
                res.setHeader("retry-after", std::to_string(window.count()));
                co_return;
            }

            co_await next();
        };
    }

    /**
     * @brief 创建一个请求体大小限制中间件。
     * @param maxBodySize 允许的最大请求体字节数
     * @return MiddlewareFunc 中间件函数
     *
     * 在路由到业务处理器之前检查 Content-Length 头部。
     * 超过限制时返回 413 Payload Too Large。
     */
    inline MiddlewareFunc bodySizeLimitMiddleware(const size_t maxBodySize)
    {
        return [maxBodySize](const HttpRequest &req, HttpResponse &res, const std::function<Core::Task<void>()> next) -> Core::Task<>
        {
            if (const auto contentLength = req.getHeader("content-length"))
            {
                try
                {
                    if (const auto size = std::stoull(contentLength.value()); size > maxBodySize)
                    {
                        res.setStatus(413);
                        res.setBody("Payload Too Large");
                        res.setHeader("content-type", "text/plain");
                        co_return;
                    }
                } catch (...)
                {
                    res.setStatus(400);
                    res.setBody("Bad Request: Invalid Content-Length");
                    res.setHeader("content-type", "text/plain");
                    co_return;
                }
            }
            co_await next();
        };
    }

}

#endif
