/**
 * @file Router.h
 * @brief URL 路由器，支持路径模式匹配和中间件
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
    /**
     * @brief HTTP 路由器，支持按 HTTP 方法和路径模式注册处理函数，并集成中间件管道。
     *
     * 路由器内部维护路由表，支持路径参数提取（如 `/user/:id`），
     * 并在调用 route() 时按方法和路径匹配最佳路由，执行对应的处理函数。
     * 支持为所有路由统一添加中间件。
     */
    class Router
    {
    public:
        /**
         * @brief 路由处理函数类型。
         *
         * 接收请求和响应对象，返回协程任务。通常在路由处理函数内部生成响应内容。
         */
        using Handler = std::function<Core::Task<>(HttpRequest &, HttpResponse &)>;

        /**
         * @brief 构造函数，创建空路由器。
         */
        Router();

        /**
         * @brief 注册 GET 路由。
         * @param path    路径模式（支持 `:param` 形式的路径参数）
         * @param handler 处理函数
         */
        void get(const std::string &path, Handler handler);

        /**
         * @brief 注册 POST 路由。
         * @param path    路径模式
         * @param handler 处理函数
         */
        void post(const std::string &path, Handler handler);

        /**
         * @brief 注册 PUT 路由。
         * @param path    路径模式
         * @param handler 处理函数
         */
        void put(const std::string &path, Handler handler);

        /**
         * @brief 注册 DELETE 路由。
         * @param path    路径模式
         * @param handler 处理函数
         */
        void del(const std::string &path, Handler handler);

        /**
         * @brief 注册 PATCH 路由。
         * @param path    路径模式
         * @param handler 处理函数
         */
        void patch(const std::string &path, Handler handler);

        /**
         * @brief 注册 HEAD 路由。
         * @param path    路径模式
         * @param handler 处理函数
         */
        void head(const std::string &path, Handler handler);

        /**
         * @brief 注册 OPTIONS 路由。
         * @param path    路径模式
         * @param handler 处理函数
         */
        void options(const std::string &path, Handler handler);

        /**
         * @brief 添加全局中间件，将应用于所有路由。
         * @param middleware 中间件函数
         *
         * 中间件按添加顺序执行，可以在路由处理前/后添加通用逻辑（如日志、鉴权、CORS）。
         */
        void addMiddleware(MiddlewareFunc middleware);

        /**
         * @brief 路由入口，匹配请求方法和路径，执行对应的处理函数。
         * @param req HTTP 请求对象（可能被中间件或处理函数修改）
         * @param res HTTP 响应对象（由中间件或处理函数填充）
         * @return Core::Task<> 协程任务，完成后返回
         *
         * 若匹配成功，则通过中间件管道最终调用处理函数；
         * 若没有匹配的路由，内部应设置 404 响应（该实现细节取决于路由实现）。
         */
        Core::Task<> route(HttpRequest &req, HttpResponse &res);

    private:
        /**
         * @brief 内部路由条目结构。
         */
        struct Route
        {
            HttpMethod  method;  ///< HTTP 方法
            std::string pattern; ///< 路径模式（如 `/api/:version/user/:id`）
            Handler     handler; ///< 对应的处理函数
        };

        /**
         * @brief 检查路由是否匹配请求路径，并提取路径参数到请求对象中。
         * @param route 待匹配的路由条目
         * @param path  请求的实际路径（不含 query string）
         * @param req   请求对象，用于存储提取的路径参数
         * @return true  匹配成功
         * @return false 匹配失败
         */
        static bool matchRoute(const Route &route, const std::string &path, HttpRequest &req);

        std::vector<Route> m_routes;   ///< 注册的路由表（顺序重要，先匹配先服务）
        MiddlewarePipeline m_pipeline; ///< 全局中间件管道
    };
}

#endif
