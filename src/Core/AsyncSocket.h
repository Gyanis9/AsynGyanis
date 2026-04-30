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
     * @brief 异步 TCP socket 封装，支持协程式 I/O
     *
     * 持有 EventLoop 引用和非阻塞 fd，提供协程式异步 I/O 方法。
     * 所有 async* 方法内部通过 while(true) 循环处理 EAGAIN,
     * 在不可用状态时通过 EpollAwaiter 挂起协程等待 fd 就绪。
     *
     * @note 支持移动语义，不可复制
     * @note close() 会先调用 shutdown(SHUT_RDWR) 再 close，避免 TCP RST 异常断开
     * @note 所有异步操作均通过 EventLoop 中的 epoll 实例等待事件，不会阻塞线程
     */
    class AsyncSocket
    {
    public:
        /**
         * @brief 从已有文件描述符构造（通常用于 accept 返回的 socket）
         * @param loop 关联的 EventLoop，用于事件注册与等待
         * @param fd   已打开且已设置为非阻塞的 socket 文件描述符
         */
        AsyncSocket(EventLoop &loop, int fd);

        /**
         * @brief 析构函数，自动调用 close() 关闭 socket
         */
        ~AsyncSocket();

        // 禁止拷贝
        AsyncSocket(const AsyncSocket &)            = delete;
        AsyncSocket &operator=(const AsyncSocket &) = delete;

        /**
         * @brief 移动构造函数，转移资源所有权
         * @param other 被移动的 AsyncSocket 对象
         */
        AsyncSocket(AsyncSocket &&other) noexcept;

        /**
         * @brief 移动赋值运算符，转移资源所有权
         * @param other 被移动的 AsyncSocket 对象
         * @return *this
         */
        AsyncSocket &operator=(AsyncSocket &&other) noexcept;

        /**
         * @brief 创建新 socket 的工厂方法
         *
         * 内部调用 socket() 并自动设置 SOCK_NONBLOCK | SOCK_CLOEXEC 标志，
         * 确保 socket 是非阻塞的，且子进程不会继承该 fd。
         *
         * @param loop   关联的 EventLoop
         * @param domain 协议族，通常为 AF_INET（IPv4）或 AF_INET6（IPv6）
         * @param type   socket 类型，默认为 SOCK_STREAM（TCP）
         * @return AsyncSocket 实例
         * @throws SystemException 当 socket() 系统调用失败时抛出
         */
        static AsyncSocket create(EventLoop &loop, int domain = AF_INET, int type = SOCK_STREAM);

        /**
         * @brief 绑定 socket 到指定的 sockaddr 地址
         * @param addr    指向 sockaddr 结构的指针（IPv4 或 IPv6）
         * @param addrLen 地址结构的长度
         * @return 成功返回 true，失败返回 false（errno 仍可获取）
         */
        bool bind(const sockaddr *addr, socklen_t addrLen) const;

        /**
         * @brief 绑定 socket 到 InetAddress 对象表示的地址
         * @param addr InetAddress 地址（内含协议族、IP、端口）
         * @return 成功返回 true，失败返回 false
         */
        bool bind(const InetAddress &addr) const;

        /**
         * @brief 开始监听 socket（用于服务端）
         * @param backlog 连接等待队列的最大长度，默认使用 SOMAXCONN（系统允许的最大值）
         * @return 成功返回 true，失败返回 false
         */
        bool listen(int backlog = SOMAXCONN) const;

        /**
         * @brief 异步接受新连接（协程式）
         * @return Task<AsyncSocket> — co_await 后获得新的客户端 AsyncSocket 对象
         *
         * 内部循环调用 accept4()，当没有新连接时（EAGAIN）会通过 EpollAwaiter 挂起协程，
         * 等待 EPOLLIN 事件触发后恢复。
         * 接受的 socket 会自动设置 SOCK_NONBLOCK | SOCK_CLOEXEC 标志。
         */
        Task<AsyncSocket> asyncAccept() const;

        /**
         * @brief 异步连接远端服务器（协程式）
         * @param addr    远端地址的 sockaddr 指针
         * @param addrLen 地址结构的长度
         * @return Task<> — co_await 等待连接建立完成
         *
         * 使用非阻塞 connect()，如果立即成功则直接返回；如果返回 EINPROGRESS，
         * 则通过 EpollAwaiter 挂起等待 EPOLLOUT 事件，连接完成后恢复。
         * @note 必须在绑定本地地址（可选）之后调用
         */
        Task<> asyncConnect(const sockaddr *addr, socklen_t addrLen) const;

        /**
         * @brief 异步连接远端服务器（InetAddress 版本）
         * @param addr InetAddress 类型的远端地址
         * @return Task<> — co_await 等待连接完成
         */
        Task<> asyncConnect(const InetAddress &addr) const;

        /**
         * @brief 异步接收数据（协程式）
         * @param buf 接收数据的缓冲区指针
         * @param len 缓冲区最大能接收的字节数
         * @return Task<ssize_t> — co_await 返回实际接收的字节数；0 表示对端正常关闭连接
         *
         * 内部循环调用 recv(MSG_NOSIGNAL)，当没有数据可读（EAGAIN）时，
         * 挂起等待 EPOLLIN 事件。
         * @note 无数据时不产生 SIGPIPE 信号（MSG_NOSIGNAL）
         */
        Task<ssize_t> asyncRecv(void *buf, size_t len) const;

        /**
         * @brief 异步发送数据（协程式）
         * @param buf 发送数据的缓冲区指针
         * @param len 待发送的字节数
         * @return Task<ssize_t> — co_await 返回实际发送的字节数（可能小于 len）
         *
         * 内部循环调用 send(MSG_NOSIGNAL)，当发送缓冲区满（EAGAIN）时，
         * 挂起等待 EPOLLOUT 事件。
         * @note 发送失败（如连接断开）时会返回 -1，上层需检查 errno
         */
        Task<ssize_t> asyncSend(const void *buf, size_t len) const;

        /**
         * @brief 关闭 socket（先 shutdown 再 close，避免 TCP RST）
         *
         * 首先调用 shutdown(SHUT_RDWR) 优雅关闭读写通道，
         * 然后调用 close() 释放文件描述符。这样可以避免在未读取完数据时
         * 直接 close 导致对端收到 RST 异常。
         */
        void close();

        /**
         * @brief 获取原始文件描述符
         * @return socket fd，若已关闭则返回 -1
         */
        [[nodiscard]] int fd() const noexcept;

        /**
         * @brief 设置 O_NONBLOCK 标志（通常创建时已设置，此处保留以备重新设置）
         */
        void setNonBlocking() const;

        /**
         * @brief 设置 socket 选项（如 SO_REUSEADDR、TCP_NODELAY 等）
         * @param level 协议层（如 SOL_SOCKET、IPPROTO_TCP）
         * @param opt   选项名
         * @param val   选项值的指针
         * @param len   选项值的长度
         * @return 成功返回 true，失败返回 false
         */
        bool setSockOpt(int level, int opt, const void *val, socklen_t len) const;

        /**
         * @brief 获取对端地址（已连接的 socket）
         * @return InetAddress 对象，包含对端的 IP 和端口
         * @throws std::runtime_error 如果获取失败（如未连接或 socket 无效）
         */
        InetAddress remoteAddress() const;

        /**
         * @brief 获取本地地址（绑定的地址）
         * @return InetAddress 对象，包含本地的 IP 和端口
         * @throws std::runtime_error 如果获取失败
         */
        InetAddress localAddress() const;

    private:
        EventLoop &m_loop; ///< 关联的事件循环，用于异步等待和事件注册
        int        m_fd;   ///< 底层 socket 文件描述符，-1 表示无效
    };
}

#endif
