/**
 * @file AsyncSocket.h
 * @brief 异步 TCP socket（非阻塞 I/O + epoll）
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

    class AsyncSocket
    {
    public:
        AsyncSocket(EventLoop &loop, int fd);

        ~AsyncSocket();

        AsyncSocket(const AsyncSocket &)            = delete;
        AsyncSocket &operator=(const AsyncSocket &) = delete;

        AsyncSocket(AsyncSocket &&other) noexcept;
        AsyncSocket &operator=(AsyncSocket &&other) noexcept;

        static AsyncSocket create(EventLoop &loop, int domain = AF_INET, int type = SOCK_STREAM);

        bool bind(const sockaddr *addr, socklen_t addrLen) const;
        bool bind(const InetAddress &addr) const;
        bool listen(int backlog = SOMAXCONN) const;

        Task<AsyncSocket> asyncAccept() const;
        Task<>            asyncConnect(const sockaddr *addr, socklen_t addrLen) const;
        Task<>            asyncConnect(const InetAddress &addr) const;
        Task<ssize_t>     asyncRecv(void *buf, size_t len);
        Task<ssize_t>     asyncSend(const void *buf, size_t len);

        void              close();
        [[nodiscard]] int fd() const noexcept;
        void              setNonBlocking() const;
        bool              setSockOpt(int level, int opt, const void *val, socklen_t len) const;
        InetAddress       remoteAddress() const;
        InetAddress       localAddress() const;

    private:
        EventLoop &m_loop;
        int        m_fd;
    };

}

#endif
