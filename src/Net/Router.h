/**
 * @file Router.h
 * @brief URL router with pattern matching and middleware support
 * @copyright Copyright (c) 2026
 */
#ifndef NET_ROUTER_H
#define NET_ROUTER_H

#include "Core/Task.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Middleware.h"

#include <functional>
#include <string>
#include <vector>

namespace Net
{

    class Router
    {
    public:
        using Handler = std::function<Core::Task<>(HttpRequest &, HttpResponse &)>;

        Router();

        void get(const std::string &path, Handler handler);
        void post(const std::string &path, Handler handler);
        void put(const std::string &path, Handler handler);
        void del(const std::string &path, Handler handler);
        void patch(const std::string &path, Handler handler);
        void head(const std::string &path, Handler handler);
        void options(const std::string &path, Handler handler);

        void addMiddleware(MiddlewareFunc middleware);

        Core::Task<> route(HttpRequest &req, HttpResponse &res);

    private:
        struct Route
        {
            HttpMethod  method;
            std::string pattern;
            Handler     handler;
        };

        bool matchRoute(const Route &route, const std::string &path, HttpRequest &req) const;

        std::vector<Route> m_routes;
        MiddlewarePipeline m_pipeline;
    };

}

#endif
