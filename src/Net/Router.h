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
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Net
{
    /**
     * @brief HTTP 路由器，支持按 HTTP 方法和路径模式注册处理函数，并集成中间件管道。
     *
     * 路由匹配采用两级策略：
     * - 一级：精确路径哈希表 O(1) 查找（覆盖大多数生产场景）
     * - 二级：参数化/通配符路由线性扫描（":id"、"*" 等模式）
     *
     * 支持路径参数提取（如 "/user/:id"），并在调用 route() 时按方法和路径匹配最佳路由。
     * 支持为所有路由统一添加中间件。
     */
    class Router
    {
    public:
        /**
         * @brief 路由处理函数类型。
         */
        using Handler = std::function<Core::Task<>(HttpRequest &, HttpResponse &)>;

        Router();

        void get(const std::string &path, Handler handler);
        void post(const std::string &path, Handler handler);
        void put(const std::string &path, Handler handler);
        void del(const std::string &path, Handler handler);
        void patch(const std::string &path, Handler handler);
        void head(const std::string &path, Handler handler);
        void options(const std::string &path, Handler handler);

        /**
         * @brief 添加全局中间件，将应用于所有路由。
         * @param middleware 中间件函数
         */
        void addMiddleware(MiddlewareFunc middleware);

        /**
         * @brief 路由入口，匹配请求方法和路径，执行对应的处理函数。
         * @param req HTTP 请求对象（可能被中间件或处理函数修改）
         * @param res HTTP 响应对象（由中间件或处理函数填充）
         * @return Core::Task<> 协程任务，完成后返回
         */
        Core::Task<> route(HttpRequest &req, HttpResponse &res);

    private:
        struct Route
        {
            HttpMethod  method;
            std::string pattern;
            Handler     handler;
        };

        static bool matchRoute(const Route &route, const std::string &path, HttpRequest &req);

        /**
         * @brief 判断路径是否为精确匹配（不含 ":param" 或 "*" 模式）。
         */
        static bool isExactPath(const std::string &path);

        /**
         * @brief 生成精确路径查找的哈希键："method:path"。
         */
        static std::string makeExactKey(HttpMethod method, const std::string &path);

        /**
         * @brief 将路由注册到内部数据结构。
         */
        void addRoute(HttpMethod method, const std::string &path, Handler handler);

        /**
         * @brief 路由后的响应收尾处理（HEAD 请求剥离 body 等）。
         */
        static void finalizeResponse(const HttpRequest &req, HttpResponse &res);

        // 一级索引：精确路径 hash -> handler（O(1) 查找）
        std::unordered_map<std::string, Handler> m_exactRoutes;

        // 二级索引：参数化/通配符路由列表（含 UNKNOWN method 路由）
        std::vector<Route> m_patternRoutes;

        MiddlewarePipeline m_pipeline; ///< 全局中间件管道
    };
}

#endif
