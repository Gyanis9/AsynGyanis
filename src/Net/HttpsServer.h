/**
 * @file HttpsServer.h
 * @brief HTTPS 服务器 — 持有 TlsContext，为每个连接创建 TLS 包装的 HttpsSession
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSSERVER_H
#define NET_HTTPSSERVER_H

#include "Core/EventLoop.h"
#include "Core/TlsContext.h"

#include "Router.h"
#include "TcpServer.h"

#include <memory>
#include <string>

namespace Net
{
    /**
     * @brief HTTPS 服务器类。
     *
     * 继承自 TcpServer，支持 TLS 加密。内部持有 TlsContext 用于管理 SSL_CTX，
     * 并为每个新连接创建 HttpsSession（TLS 包装的 HTTP 会话）。适用于安全 HTTP 服务。
     */
    class HttpsServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTPS 服务器。
         * @param loop      事件循环，用于管理 I/O 事件
         * @param addr      监听的本地地址（IP 和端口）
         * @param certFile  服务器证书文件路径（PEM 格式）
         * @param keyFile   服务器私钥文件路径（PEM 格式）
         */
        HttpsServer(Core::EventLoop &loop, const Core::InetAddress &addr, const std::string &certFile, const std::string &keyFile);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数。
         * @return Router& 路由器对象
         */
        Router &router();

        /**
         * @brief 创建 TLS 加密的 HTTP 连接对象（HttpsSession）。
         * @param socket 已建立的异步 socket（原始 TCP socket）
         * @return 新创建的 Connection 智能指针，实际类型为 HttpsSession
         * @override 覆盖 TcpServer 的方法
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

    private:
        Router           m_router;     ///< 路由器，存储 HTTP 路由表和处理函数
        Core::TlsContext m_tlsContext; ///< TLS 上下文，管理证书和 SSL 对象
    };
}

#endif
