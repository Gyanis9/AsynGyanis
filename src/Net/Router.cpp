#include "Router.h"

#include <string_view>

namespace Net
{

    Router::Router() = default;

    void Router::get(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::GET, path, std::move(handler)});
    }

    void Router::post(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::POST, path, std::move(handler)});
    }

    void Router::put(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::PUT, path, std::move(handler)});
    }

    void Router::del(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::DELETE, path, std::move(handler)});
    }

    void Router::patch(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::PATCH, path, std::move(handler)});
    }

    void Router::head(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::HEAD, path, std::move(handler)});
    }

    void Router::options(const std::string &path, Handler handler)
    {
        m_routes.push_back({HttpMethod::OPTIONS, path, std::move(handler)});
    }

    void Router::addMiddleware(MiddlewareFunc middleware)
    {
        m_pipeline.use(std::move(middleware));
    }

    Core::Task<> Router::route(HttpRequest &req, HttpResponse &res)
    {
        const std::string path = req.path();

        for (auto it = m_routes.rbegin(); it != m_routes.rend(); ++it)
        {
            if (it->method != req.method() && it->method != HttpMethod::UNKNOWN)
                continue;

            if (matchRoute(*it, path, req))
            {
                auto handler = it->handler;
                co_await m_pipeline.run(req, res, [&req, &res, handler]() -> Core::Task<void>
                {
                    co_await handler(req, res);
                });
                co_return;
            }
        }

        const auto notFoundRes = HttpResponse::notFound();
        res.setStatus(notFoundRes.status());
        res.setBody(notFoundRes.body());
    }

    bool Router::matchRoute(const Route &route, const std::string &path, HttpRequest &req)
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
                req.setParam(std::move(paramName), std::string(pathSeg));
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
