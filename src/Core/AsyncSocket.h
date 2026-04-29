/**
 * @file AsyncSocket.h
 * @brief 异步非阻塞 TCP socket — 基于 epoll 边缘触发的协程式 I/O
 *
 * 封装了 create/bind/listen/accept/connect/recv/send 等 socket 操作,
 * 所有 I/O 方法返回 Task<> 类型, 通过 co_await 实现异步等待。
 * 内部使用 EpollAwaiter 处理 EAGAIN/EWOULDBLOCK 情况。
 *
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_ASYNCSOCKET_H
#define CORE_ASYNCSOCKET_H

#include "Task.h"
#include <sys/socket.h>
#include <string>

namespace Core
{
    class InetAddress;
    class EventLoop;

    /**
     * @class AsyncSocket
     * @brief 异步 TCP socket
     *
     * 持有 EventLoop 引用和非阻塞 fd, 提供协程式异步 I/O 方法。
     * 所有 async* 方法内部通过 while(true) 循环处理 EAGAIN,
     * 在不可用状态时通过 EpollAwaiter 挂起协程等待 fd 就绪。
     *
     * @note 移动语义, 不可复制
     * @note close() 会先调用 shutdown(SHUT_RDWR) 再 close, 避免 TCP RST
     */
    class AsyncSocket
    {
    public:
        /** @brief 从已有 fd 构造 (通常用于 accept 返回的 socket) */
        AsyncSocket(EventLoop &loop, int fd);
        /** @brief 析构时自动 close() */
        ~AsyncSocket();

        AsyncSocket(const AsyncSocket &)            = delete;
        AsyncSocket &operator=(const AsyncSocket &) = delete;
        /** @brief 移动构造 */
        AsyncSocket(AsyncSocket &&other) noexcept;
        /** @brief 移动赋值 */
        AsyncSocket &operator=(AsyncSocket &&other) noexcept;

        /**
         * @brief 创建新 socket 的工厂方法
         * @param loop 关联的 EventLoop
         * @param domain 协议族 (AF_INET / AF_INET6)
         * @param type socket 类型, 默认 SOCK_STREAM
         * @return AsyncSocket 实例 (已设置 SOCK_NONBLOCK | SOCK_CLOEXEC)
         * @throws SystemException 创建失败时抛出
         */
        static AsyncSocket create(EventLoop &loop, int domain = AF_INET, int type = SOCK_STREAM);

        /** @brief 绑定 socket 到 sockaddr */
        bool bind(const sockaddr *addr, socklen_t addrLen) const;
        /** @brief 绑定到 InetAddress */
        bool bind(const InetAddress &addr) const;

        /**
         * @brief 开始监听
         * @param backlog 连接等待队列最大长度, 默认 SOMAXCONN
         * @return 成功返回 true
         */
        bool listen(int backlog = SOMAXCONN) const;

        /**
         * @brief 异步接受新连接 (协程式)
         * @return Task<AsyncSocket> — co_await 获得新的客户端 socket
         *
         * 内部循环 accept4(), EAGAIN 时通过 EpollAwaiter(EPOLLIN) 挂起。
         * 接受的 socket 自动设置 SOCK_NONBLOCK | SOCK_CLOEXEC。
         */
        Task<AsyncSocket> asyncAccept() const;

        /**
         * @brief 异步连接 (协程式)
         * @return Task<> — co_await 等待连接完成
         *
         * 使用非阻塞 connect() + EpollAwaiter(EPOLLOUT) 模式。
         */
        Task<> asyncConnect(const sockaddr *addr, socklen_t addrLen) const;
        /** @brief 异步连接到 InetAddress */
        Task<> asyncConnect(const InetAddress &addr) const;

        /**
         * @brief 异步接收数据 (协程式)
         * @param buf 接收缓冲区
         * @param len 缓冲区长度
         * @return Task<ssize_t> — 实际接收字节数, 0 表示对端关闭
         *
         * 内部循环 recv(MSG_NOSIGNAL), EAGAIN 时挂起等待 EPOLLIN。
         */
        Task<ssize_t> asyncRecv(void *buf, size_t len);

        /**
         * @brief 异步发送数据 (协程式)
         * @param buf 发送缓冲区
         * @param len 待发送字节数
         * @return Task<ssize_t> — 实际发送字节数
         *
         * 内部循环 send(MSG_NOSIGNAL), EAGAIN 时挂起等待 EPOLLOUT。
         */
        Task<ssize_t> asyncSend(const void *buf, size_t len);

        /** @brief 关闭 socket (先 shutdown 再 close, 避免 TCP RST) */
        void close();
        /** @brief 获取原始 fd */
        [[nodiscard]] int fd() const noexcept;
        /** @brief 设置 O_NONBLOCK 标志 */
        void setNonBlocking() const;
        /** @brief 设置 socket 选项 */
        bool setSockOpt(int level, int opt, const void *val, socklen_t len) const;
        /** @brief 获取对端地址 */
        InetAddress remoteAddress() const;
        /** @brief 获取本地地址 */
        InetAddress localAddress() const;

    private:
        EventLoop &m_loop;
        int        m_fd;
    };

}

#endif
