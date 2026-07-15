#include "Router.h"

#include <string_view>

namespace Net
{

    Router::Router() = default;

    bool Router::isExactPath(const std::string &path)
    {
        return path.find(':') == std::string::npos && path.find('*') == std::string::npos;
    }

    std::string Router::makeExactKey(const HttpMethod method, const std::string &path)
    {
        std::string key;
        key.reserve(16 + path.size());
        key.append(std::to_string(static_cast<int>(method)));
        key.push_back(':');
        key.append(path);
        return key;
    }

    void Router::addRoute(const HttpMethod method, const std::string &path, Handler handler)
    {
        // 精确路径进入哈希表实现 O(1) 查找
        if (isExactPath(path) && method != HttpMethod::UNKNOWN)
        {
            m_exactRoutes[makeExactKey(method, path)] = std::move(handler);
        } else
        {
            // 参数化路径（":id"）、通配符（"*"）或 UNKNOWN method 进入线性扫描列表
            m_patternRoutes.push_back({method, path, std::move(handler)});
        }
    }

    void Router::get(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::GET, path, std::move(handler));
    }

    void Router::post(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::POST, path, std::move(handler));
    }

    void Router::put(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::PUT, path, std::move(handler));
    }

    void Router::del(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::DELETE, path, std::move(handler));
    }

    void Router::patch(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::PATCH, path, std::move(handler));
    }

    void Router::head(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::HEAD, path, std::move(handler));
    }

    void Router::options(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::OPTIONS, path, std::move(handler));
    }

    void Router::addMiddleware(MiddlewareFunc middleware)
    {
        m_pipeline.use(std::move(middleware));
    }

    Core::Task<> Router::route(HttpRequest &request, HttpResponse &response)
    {
        const std::string path = request.path();

        // 一级：精确路径 O(1) 哈希查找
        if (const auto exactIt = m_exactRoutes.find(makeExactKey(request.method(), path));
            exactIt != m_exactRoutes.end())
        {
            auto handler = exactIt->second;
            co_await m_pipeline.run(request, response, [&request, &response, handler]() -> Core::Task<void>
            {
                co_await handler(request, response);
            });
            finalizeResponse(request, response);
            co_return;
        }

        // 二级：参数化/通配符路径线性扫描
        for (auto it = m_patternRoutes.rbegin(); it != m_patternRoutes.rend(); ++it)
        {
            if (it->method != request.method() && it->method != HttpMethod::UNKNOWN)
                continue;

            if (matchRoute(*it, path, request))
            {
                auto handler = it->handler;
                co_await m_pipeline.run(request, response, [&request, &response, handler]() -> Core::Task<void>
                {
                    co_await handler(request, response);
                });
                finalizeResponse(request, response);
                co_return;
            }
        }

        // 404 Not Found
        const auto notFoundResponse = HttpResponse::notFound();
        response.setStatus(notFoundResponse.status());
        response.setBody(notFoundResponse.body());
    }

    void Router::finalizeResponse(const HttpRequest &request, HttpResponse &response)
    {
        // RFC 7231 §4.3.2: HEAD 响应 MUST NOT 包含 body
        if (request.method() == HttpMethod::HEAD)
        {
            response.setBody(std::string_view{});
        }
    }

    bool Router::matchRoute(const Route &route, const std::string &path, HttpRequest &request)
    {
        const auto &pattern = route.pattern;

        if (pattern.empty())
            return path.empty() || path == "/";

        if (pattern == path)
            return true;

        if (pattern.back() == '*')
        {
            std::string_view prefix(pattern.data(), pattern.size() - 1);
            if (!prefix.empty() && prefix.back() == '/')
                prefix = std::string_view(prefix.data(), prefix.size() - 1);
            return path.starts_with(prefix);
        }

        std::string_view patternView(pattern);
        std::string_view pathView(path);

        while (!patternView.empty() && !pathView.empty())
        {
            if (patternView.front() == '/')
            {
                if (pathView.front() != '/')
                    return false;
                patternView = patternView.substr(1);
                pathView    = pathView.substr(1);
                continue;
            }

            const auto patternSlash = patternView.find('/');
            const auto pathSlash    = pathView.find('/');

            std::string_view patternSeg = patternView.substr(0, patternSlash);
            std::string_view pathSeg    = pathView.substr(0, pathSlash);

            if (patternSeg.starts_with(':'))
            {
                std::string paramName(patternSeg.substr(1));
                request.setParam(std::move(paramName), std::string(pathSeg));
            } else if (patternSeg != pathSeg)
            {
                return false;
            }

            patternView = (patternSlash != std::string_view::npos)
                              ? patternView.substr(patternSlash)
                              : std::string_view{};
            pathView = (pathSlash != std::string_view::npos)
                           ? pathView.substr(pathSlash)
                           : std::string_view{};
        }

        return patternView.empty() && pathView.empty();
    }

}
