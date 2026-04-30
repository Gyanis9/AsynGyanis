/**
 * @file HttpServer.h
 * @brief 完整的 HTTP 服务器，继承自 TcpServer
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSERVER_H
#define NET_HTTPSERVER_H

#include "Core/EventLoop.h"

#include "Router.h"
#include "TcpServer.h"

#include <memory>
#include <optional>
#include <string>

namespace Net
{
    /**
     * @brief HTTP 服务器类。
     *
     * 继承自 TcpServer，处理 HTTP 请求，内置路由器（Router），支持静态文件服务。
     * 当接收到新连接时，会创建 HttpConnection 对象，
     * 该对象使用 HttpParser 解析请求，并通过 Router 分发给对应的处理函数。
     */
    class HttpServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTP 服务器。
         * @param loop 事件循环，用于管理 I/O 事件
         * @param addr 监听的本地地址（IP 和端口）
         */
        HttpServer(Core::EventLoop &loop, const Core::InetAddress &addr);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数。
         * @return Router& 路由器对象
         */
        Router &router();

        /**
         * @brief 创建 HTTP 连接对象。
         * @param socket 已建立的异步 socket
         * @return 新创建的 Connection 智能指针（实际为 HttpConnection 对象）
         * @override 覆盖 TcpServer 的方法
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

        /**
         * @brief 设置静态文件目录。
         * @param path 静态文件的根目录路径（如 "/var/www/html"）
         *
         * 当请求路径匹配到静态文件时，服务器会尝试从该目录读取文件并返回。
         * 若未设置或路径无效，静态文件功能将被禁用。
         */
        void staticFileDir(const std::string &path);

    private:
        Router                     m_router;    ///< 路由器，存储路由表和处理函数
        std::optional<std::string> m_staticDir; ///< 静态文件根目录，若为 std::nullopt 表示未启用
    };
}

#endif
