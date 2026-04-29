/**
 * @file Middleware.h
 * @brief Middleware pipeline for request/response processing
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

    using MiddlewareFunc = std::function<Core::Task<void>(HttpRequest &, HttpResponse &, std::function<Core::Task<void>()>)>;

    class MiddlewarePipeline
    {
    public:
        MiddlewarePipeline() = default;

        void use(MiddlewareFunc middleware);

        Core::Task<> run(HttpRequest &req, HttpResponse &res, std::function<Core::Task<>()> handler);

    private:
        Core::Task<> invoke(size_t index, HttpRequest &req, HttpResponse &res, std::function<Core::Task<>()> handler);

        std::vector<MiddlewareFunc> m_middlewares;
    };

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

    inline MiddlewareFunc loggingMiddleware(Base::Logger &logger)
    {
        return [&logger](const HttpRequest &req, const HttpResponse &res, const std::function<Core::Task<>()> next) -> Core::Task<>
        {
            const auto start = std::chrono::steady_clock::now();
            co_await next();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                    .count();
            logger.logFormat(Base::LogLevel::INFO, Base::SourceLocation::current(),
                             "{} -> {} {}ms",
                             req.uri(), res.status(), elapsed);
        };
    }

    inline MiddlewareFunc corsMiddleware()
    {
        return [](HttpRequest &req, HttpResponse &res, const std::function<Core::Task<>()> next) -> Core::Task<>
        {
            res.setHeader("access-control-allow-origin", "*");
            res.setHeader("access-control-allow-methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            res.setHeader("access-control-allow-headers", "Content-Type, Authorization");
            res.setHeader("access-control-max-age", "86400");

            co_await next();
        };
    }

    inline MiddlewareFunc timeoutMiddleware(std::chrono::milliseconds timeout)
    {
        return [timeout](HttpRequest &req, HttpResponse &res, const std::function<Core::Task<>()> next) -> Core::Task<>
        {
            co_await next();
        };
    }

}

#endif
