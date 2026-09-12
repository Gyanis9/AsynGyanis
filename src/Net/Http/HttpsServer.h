/**
 * @file HttpsServer.h
 * @brief HTTPS 服务器：持有 TlsContext，为每条连接派生 TLS 包装的 HttpsSession
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsContext.h"

#include "Net/Http/Router.h"
#include "Net/Tcp/TcpServer.h"

#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTPS 服务器类。
     *
     * @details 与 HttpServer 的分工一致：接受循环与连接生命周期由 TcpServer 负责，
     *          本类额外持有一个 TLS 上下文，并在 createConnection() 里为每条新连接
     *          申请一个 SSL 对象、包成 Core::TlsSocket，再交给 HttpsSession。
     *
     * @note 证书在构造时就加载完毕：加载失败直接抛异常，不会留下一个「看似在监听、
     *       每条连接都握手失败」的服务器。
     * @warning 一个 SSL_CTX 被本服务器的所有连接共享，OpenSSL 保证 SSL 对象可并发使用；
     *          会话本身仍全部跑在同一个事件循环线程上。
     * @see HttpsSession, Core::TlsContext, HttpServer
     */
    class HttpsServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTPS 服务器并加载证书。
         * @param loop 事件循环，用于管理 I/O 事件与调度会话协程
         * @param address 监听的本地地址（IP 与端口）
         * @param certificateFile 服务器证书文件路径（PEM 格式，可含证书链）
         * @param keyFile 服务器私钥文件路径（PEM 格式）
         * @throws Base::SystemException 基类创建监听套接字失败
         * @throws Base::Exception 证书或私钥加载失败（文件缺失、格式不对、口令不符）
         */
        HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数与中间件。
         * @return Router& 路由器对象，生命周期跟随本服务器
         * @note 必须在 start() 之前完成注册
         */
        [[nodiscard]] Router &router();

        /**
         * @brief 为一条新连接创建 TLS 加密的 HTTP 会话。
         *
         * @details 重写 TcpServer::createConnection()（基类纯虚钩子）。与基类契约的差异：
         *          @li 基类只要求「接管 socket 或让它随参数析构关闭」，本实现在此之前还要
         *              向 TLS 上下文申请一个 SSL 对象——申请失败时**抛异常**而不是返回空指针，
         *              基类约定的处置是把这一条连接丢弃并记录中文错误，接受循环不受影响；
         *          @li socket 的所有权先随 Core::TlsSocket 转移（TLS 通道是描述符的唯一所有者），
         *              会话交给基类 Core::Connection 的是一条占位套接字，细节见 HttpsSession；
         *          @li 与 HttpSession 不同，本函数不做任何网络动作，握手留在会话协程里做，
         *              以免在事件循环线程上阻塞（基类明令禁止）。
         *
         * @param socket 已 accept 且已设为非阻塞的套接字，所有权就此转移
         * @return std::shared_ptr<Core::Connection> 实际类型为 HttpsSession，永不为空
         * @throws Base::Exception SSL 对象申请失败（此时 socket 随形参析构被关闭）
         * @see TcpServer::createConnection(), HttpsSession, Core::TlsContext::createSSL()
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

    private:
        Router m_router;          ///< 路由器，存储 HTTP 路由表与处理函数
        Core::TlsContext m_tlsContext; ///< TLS 上下文，管理 SSL_CTX 与证书，被所有连接共享
    };
} // namespace AsynGyanis::Net
