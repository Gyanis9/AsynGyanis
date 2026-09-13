/**
 * @file HttpsSession.h
 * @brief HTTPS 会话：先完成 TLS 握手，再在加密通道上跑与 HTTP 相同的事务循环
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/Connection.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"

#include <memory>
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
     *       占位套接字（fileDescriptor() 为 -1）。
     * @note 凡是要看真实描述符的基类接口都已重写（见本类的 close()、isAlive()、
     *       remoteAddress() 与 localAddress()）：Core::Connection 把它们声明为虚函数，
     *       所以经基类指针调用时（ConnectionManager::shutdown()、TcpServer 的空闲清扫）
     *       作用到的是 TLS 通道，而不是那个占位套接字。
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
         * @param limits 连接级限额的共享只读配置；传空指针表示按 HttpServerLimits 的默认值执行
         * @param metrics 统计采集端；传空指针表示本会话不采集统计（请求计数、状态码分类、延迟直方图
         *        与 WebSocket 各项计数都不更新）
         * @param requestIdGenerator request-id 生成器；传空指针表示本会话不为请求落定 request-id
         * @param parserLimits 解析器资源上限；默认取 HttpParserLimits 的缺省字段。它按值交给本会话的
         *        解析器并在构造时固定，因此只影响此后新建的会话（见 HttpParserLimits 的 @note）
         * @note 构造函数不做握手：握手是协程动作，放进构造函数就等于要求调用方在构造点 co_await
         */
        HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router,
                     std::shared_ptr<const HttpServerLimits> limits = nullptr,
                     std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                     std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr,
                     HttpParserLimits parserLimits = {});

        /**
         * @brief 启动会话主协程：TLS 握手 → 保持活跃事务循环 → 关闭通道。
         *
         * @details 重写 Core::Connection::start()：比基类的空协程多一次 TLS 握手（失败即关通道并
         *          直接返回，不进任何 HTTP 事务）；进事务循环前先在基类取消源上注册停止回调，使
         *          close() 真能掐断这条 TLS 通道；存活谓词看 TLS 描述符是否有效，收尾统一走基类 close()。
         *
         * @return Core::Task<> 协程任务，连接结束时完成
         * @throws 基类 close() 之外的异常不做处理，原样抛给 TcpServer::handleConnection()
         * @see Core::Connection::start(), detail::httpKeepAliveLoop(), closeTlsTransport()
         */
        Core::Task<> start() override;

        /**
         * @brief 真正关闭 TLS 通道：发出 close_notify 并关掉底层描述符。
         *
         * @details 由重写后的 close() 直接调用，因此 TcpServer::close() →
         *          ConnectionManager::shutdown() → 虚函数派发到本类的这条强制停链路
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

        /**
         * @brief 关闭连接：先收 TLS 通道，再走基类关闭流程
         * @details 重写 Core::Connection::close()：基类实现只会关闭自己的套接字，而本类的
         *          描述符归 TlsSocket 所有，基类那个套接字是不持有描述符的占位对象。
         *          因此这里先 closeTlsTransport() 发出 close_notify 并关掉真实描述符，
         *          再调用基类实现复位存活位与取消源。顺序不能反：SSL_shutdown 需要底层
         *          描述符仍然有效。
         * @note 经基类指针调用同样有效（ConnectionManager::shutdown() 即走这条路径）。
         */
        void close() override;

        /**
         * @brief 检查会话是否存活
         * @details 重写 Core::Connection::isAlive()：除基类的存活位之外还要看 TLS 描述符
         *          是否有效。只信基类标志的话，描述符被外部关掉后本类仍会自称存活，
         *          事务循环要等到下一次读写失败才能退出。
         * @return true 基类存活位为真且 TLS 通道仍然打开
         */
        [[nodiscard]] bool isAlive() const noexcept override;

        /**
         * @brief 获取对端的 IP 地址与端口
         * @details 重写 Core::Connection::remoteAddress()：基类实现读的是那条不持有描述符的
         *          占位套接字，取地址必然失败（getpeername 返回「不是套接字」）。这里改问
         *          TLS 通道，它是真实描述符的所有者。
         * @return std::string 字符串格式 "ip:port"
         * @throws Base::SystemException TLS 通道已关闭，底层套接字无法提供地址
         */
        [[nodiscard]] std::string remoteAddress() const override;

        /**
         * @brief 获取本端的 IP 地址与端口
         * @details 重写 Core::Connection::localAddress()：理由同 remoteAddress()，
         *          基类的占位套接字取不到任何地址。
         * @return std::string 字符串格式 "ip:port"
         * @throws Base::SystemException TLS 通道已关闭，底层套接字无法提供地址
         */
        [[nodiscard]] std::string localAddress() const override;

        /**
         * @brief 本连接被空闲清扫协程按超时关闭时上报到所属服务器的统计
         *
         * @details 重写 Core::Connection::onIdleTimeoutClosed()：把这次收口累加进 timeoutClosedCount。
         *          基类默认实现是空操作，这里的差异只多一次原子自增，不再写日志——清扫协程已经
         *          记下了「哪条连接、超时误差多大」。
         * @note 未持有统计对象时（例如只关心协议的调用方直接构造会话）什么都不做
         */
        void onIdleTimeoutClosed() noexcept override;

    private:
        Core::TlsSocket m_tlsSocket;      ///< TLS 通道，持有 SSL 对象与真实描述符
        Router &m_router;                 ///< 路由器引用，用于分发请求
        HttpParser m_parser;              ///< HTTP 增量解析器（资源上限构造时固定），两条报文之间由会话显式 reset()
        std::vector<char> m_receiveBuffer; ///< 跨次读取存续的接收缓冲（存的是解密后的明文 HTTP 字节）
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，与服务器共享、只读（构造时保证非空）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端，与服务器共享；空指针表示本会话不采集
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器，与服务器共享；空指针表示不落定 request-id
    };
} // namespace AsynGyanis::Net
