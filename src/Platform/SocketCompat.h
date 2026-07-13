/**
 * @file SocketCompat.h
 * @brief Socket 操作跨平台兼容函数
 * @copyright Copyright (c) 2026
 */

#ifndef PLATFORM_SOCKETCOMPAT_H
#define PLATFORM_SOCKETCOMPAT_H

#include "Platform.h"

namespace Platform
{
    // ========================================================================
    // 非阻塞设置
    // ========================================================================

#ifdef _WIN32
    inline bool setNonBlocking(int fd)
    {
        u_long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode) == 0;
    }
#else
    inline bool setNonBlocking(int fd)
    {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            return false;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }
#endif

    // ========================================================================
    // 关闭 fd
    // ========================================================================

#ifdef _WIN32
    inline int closeFd(int fd)
    {
        return ::closesocket(fd);
    }
#else
    inline int closeFd(int fd)
    {
        return ::close(fd);
    }
#endif

    // ========================================================================
    // read / write（用于 eventfd / timerfd / socket pair）
    // ========================================================================

#ifdef _WIN32
    inline ssize_t readFd(int fd, void *buf, size_t len)
    {
        return ::recv(fd, static_cast<char *>(buf), static_cast<int>(len), 0);
    }

    inline ssize_t writeFd(int fd, const void *buf, size_t len)
    {
        return ::send(fd, static_cast<const char *>(buf), static_cast<int>(len), 0);
    }
#else
    inline ssize_t readFd(int fd, void *buf, size_t len)
    {
        return ::read(fd, buf, len);
    }

    inline ssize_t writeFd(int fd, const void *buf, size_t len)
    {
        return ::write(fd, buf, len);
    }
#endif

    // ========================================================================
    // accept（Windows 无 accept4，用 accept + setNonBlocking）
    // ========================================================================

#ifdef _WIN32
    inline int acceptSocket(int listenFd, sockaddr *addr, socklen_t *addrLen)
    {
        const int fd = ::accept(listenFd, addr, addrLen);
        if (fd >= 0)
        {
            setNonBlocking(fd);
        }
        return fd;
    }
#else
    inline int acceptSocket(int listenFd, sockaddr *addr, socklen_t *addrLen)
    {
        return ::accept4(listenFd, addr, addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    }
#endif

    // ========================================================================
    // 创建 socket pair（用于 Windows eventfd/timerfd 替代）
    // ========================================================================

#ifdef _WIN32
    inline bool createSocketPair(int &readFd, int &writeFd)
    {
        // 通过 TCP loopback 创建一对已连接的 socket
        const int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0)
            return false;

        BOOL opt = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<char *>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            ::closesocket(listener);
            return false;
        }

        socklen_t addrLen = sizeof(addr);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &addrLen) < 0)
        {
            ::closesocket(listener);
            return false;
        }

        if (::listen(listener, 1) < 0)
        {
            ::closesocket(listener);
            return false;
        }

        const int client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client < 0)
        {
            ::closesocket(listener);
            return false;
        }

        if (::connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            ::closesocket(listener);
            ::closesocket(client);
            return false;
        }

        const int server = ::accept(listener, nullptr, nullptr);
        ::closesocket(listener);

        if (server < 0)
        {
            ::closesocket(client);
            return false;
        }

        readFd  = server;
        writeFd = client;
        return true;
    }
#else
    inline bool createSocketPair(int &readFd, int &writeFd)
    {
        (void)readFd;
        (void)writeFd;
        return false; // Linux 使用 eventfd，不需要 socket pair
    }
#endif

    // ========================================================================
    // Winsock 初始化/清理
    // ========================================================================

#ifdef _WIN32
    inline void initWinsock()
    {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }

    inline void cleanupWinsock()
    {
        WSACleanup();
    }
#else
    inline void initWinsock()
    {
    }

    inline void cleanupWinsock()
    {
    }
#endif

} // namespace Platform

#endif // PLATFORM_SOCKETCOMPAT_H
