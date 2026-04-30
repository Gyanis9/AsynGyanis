/**
 * @file TcpAcceptor.h
 * @brief TCP 监听套接字 — 异步 accept
 * @copyright Copyright (c) 2026
 */
#ifndef NET_TCPACCEPTOR_H
#define NET_TCPACCEPTOR_H

#include "Core/AsyncSocket.h"
#include "Core/Task.h"
#include "Core/InetAddress.h"

#include <deque>
#include <optional>

namespace Net
{
    using Core::InetAddress;

    /**
     * @brief TCP 监听器（Acceptor），封装非阻塞的监听套接字，支持协程异步 accept。
     *
     * 该类创建并管理一个 TCP 监听套接字，将其注册到事件循环中，
     * 并提供 accept() 协程，当新连接到达时恢复并返回 AsyncSocket。
     * 支持在单线程事件循环中使用，适合与服务端配合。
     */
    class TcpAcceptor
    {
    public:
        /**
         * @brief 构造 TcpAcceptor 对象。
         * @param loop  关联的事件循环，负责 I/O 事件监控
         * @param addr  要监听的本地地址（IP 和端口）
         *
         * 构造后未自动绑定或监听，需显式调用 bind() 和 listen()。
         */
        TcpAcceptor(Core::EventLoop &loop, const InetAddress &addr);

        // 禁止拷贝，允许移动
        TcpAcceptor(const TcpAcceptor &)            = delete;
        TcpAcceptor &operator=(const TcpAcceptor &) = delete;
        TcpAcceptor(TcpAcceptor &&)                 = default;
        TcpAcceptor &operator=(TcpAcceptor &&)      = delete;

        /**
         * @brief 默认析构函数，自动调用 close() 释放资源。
         */
        ~TcpAcceptor() = default;

        /**
         * @brief 绑定监听地址和端口。
         * @return 绑定成功返回 true，失败返回 false（可通过 errno 获取原因）
         */
        bool bind();

        /**
         * @brief 开始监听连接请求。
         * @param backlog 连接队列的最大长度，默认使用系统最大值 SOMAXCONN
         * @return 成功返回 true，失败返回 false
         */
        bool listen(int backlog = SOMAXCONN) const;

        /**
         * @brief 异步接受一个新连接。
         * @return Task<std::optional<Core::AsyncSocket>>
         *         如果成功接受，返回包含 AsyncSocket 的 optional；
         *         如果监听套接字已关闭或出现非可恢复错误，返回 std::nullopt。
         *
         * 该协程会挂起直到有新连接到达，被事件循环唤醒后立即接受连接。
         * 适用于协程服务器循环。
         */
        Core::Task<std::optional<Core::AsyncSocket>> accept();

        /**
         * @brief 关闭监听套接字，并从事件循环中移除。
         */
        void close();

        /**
         * @brief 获取监听套接字的本地地址。
         * @return InetAddress 对象
         */
        [[nodiscard]] Core::InetAddress localAddress() const;

        /**
         * @brief 获取监听套接字的文件描述符。
         * @return 文件描述符，若未创建或已关闭则返回 -1
         */
        [[nodiscard]] int fd() const;

    private:
        Core::EventLoop &             m_loop;         ///< 关联的事件循环
        Core::AsyncSocket             m_listenSocket; ///< 非阻塞监听套接字
        Core::InetAddress             m_addr;         ///< 绑定的本地地址
        std::deque<Core::AsyncSocket> m_pending;      ///< 暂未使用的队列（预留扩展）
        bool                          m_bound{false}; ///< 是否已成功绑定
    };
}

#endif
