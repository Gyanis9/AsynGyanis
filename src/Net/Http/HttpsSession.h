/**
 * @file HttpsSession.h
 * @brief HTTPS 会话：先完成 TLS 握手，再在加密通道上跑与 HTTP 相同的事务循环
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/Connection.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/Router.h"

#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTPS 会话类：一条 TLS 连接对应一个 HttpsSession。
     *
     * @details start() 先做 TLS 握手，握手成功后把加密通道交给 detail::httpKeepAliveLoop()，
     *          之后的定界、解析、路由与应答与 HttpSession 完全同一份实现，二者只在传输层不同。
     *
     * @note 关于基类 Core::Connection 持有的套接字：一条 TLS 连接的描述符只能有一个所有者，
     *       而那个所有者必须是 Core::TlsSocket（它的析构要先 SSL_shutdown 再关描述符，
     *       顺序反了就把 TLS 会话票据连同描述符一起丢了）。因此传给基类的是一条**不持有描述符**的
     *       占位套接字（fileDescriptor() 为 -1），基类的 close() 只负责置存活位与发出停止请求。
     * @warning 由此带来两条使用约束：
     *          @li 不要在本对象上调用基类的 remoteAddress()/localAddress()——占位套接字取不到地址，
     *              需要地址请从接受连接的那一层拿（TcpServer 的 accept 结果）；
     *          @li 强制关闭必须走 Core::Connection::close()：start() 在基类的取消源上注册了停止回调，
     *              回调里才真正关掉 TLS 通道（见 closeTlsTransport()）。绕开它直接析构会话，
     *              描述符会等 TlsSocket 自己收尾。
     * @see HttpSession, Core::TlsSocket
     */
    class HttpsSession : public Core::Connection
    {
    public:
        /**
         * @brief 构造 HTTPS 会话。
         * @param loop 事件循环。仅用于给基类 Core::Connection 造一条不持有描述符的占位套接字，
         *             TLS 通道自己带着它所需的事件循环引用，不从本参数取
         * @param tlsSocket 已创建但尚未握手的 TlsSocket，所有权转移给本会话
         * @param router 全局路由器，用于分发 HTTP 请求；生命周期必须不短于本会话
         * @note 构造函数不做握手：握手是协程动作，放进构造函数就等于要求调用方在构造点 co_await
         */
        HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router);

        /**
         * @brief 启动会话主协程：TLS 握手 → 保持活跃事务循环 → 关闭通道。
         *
         * @details 重写 Core::Connection::start()。与基类默认实现（一个立即完成的空协程）的差异：
         *          @li 多了一次握手，握手失败即关闭通道并直接返回，不进入任何 HTTP 事务；
         *          @li 进入事务循环之前，先在基类的取消源上注册停止回调，使
         *              Core::Connection::close() 能够真正掐断这条 TLS 通道——基类的占位套接字
         *              做不到这一点，这是本类比 HttpSession 多出的一步；
         *          @li 存活谓词除基类的存活位之外还要看 TLS 描述符是否有效，
         *              这样「描述符已被关掉」一定能结束循环，不必等下一次读写失败；
         *          @li 收尾统一调用基类 close()，由停止回调把 TLS 通道关掉，
         *              使「自然结束」与「被强制关闭」走同一条清理路径。
         *
         * @return Core::Task<> 协程任务，连接结束时完成
         * @throws 基类 close() 之外的异常不做处理，原样抛给 TcpServer::handleConnection()
         * @see Core::Connection::start(), detail::httpKeepAliveLoop(), closeTlsTransport()
         */
        Core::Task<> start() override;

        /**
         * @brief 真正关闭 TLS 通道：发出 close_notify 并关掉底层描述符。
         *
         * @details 由 start() 注册的停止回调调用，因此 TcpServer::close() →
         *          ConnectionManager::shutdown() → Core::Connection::close() 这条强制停链路
         *          能够唤醒阻塞在 epoll 上的 TLS 读，会话随即退出。
         *          调用方一般不需要直接用本函数；重复调用是安全的空操作。
         */
        void closeTlsTransport();

        /**
         * @brief TLS 通道是否仍然打开。
         * @return true 底层描述符有效，通道可能被使用
         * @return false 描述符已失效（已关闭或从未成功建立）
         */
        [[nodiscard]] bool isTlsTransportOpen() const noexcept;

    private:
        Core::TlsSocket m_tlsSocket;      ///< TLS 通道，持有 SSL 对象与真实描述符
        Router &m_router;                 ///< 路由器引用，用于分发请求
        HttpParser m_parser;              ///< HTTP 增量解析器，两条报文之间由会话显式 reset()
        std::vector<char> m_receiveBuffer; ///< 跨次读取存续的接收缓冲（存的是解密后的明文 HTTP 字节）
    };
} // namespace AsynGyanis::Net
