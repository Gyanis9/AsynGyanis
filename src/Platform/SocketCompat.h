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
    inline bool setNonBlocking(int fileDescriptor)
    {
        u_long mode = 1;
        return ioctlsocket(fileDescriptor, FIONBIO, &mode) == 0;
    }
#else
    inline bool setNonBlocking(int fileDescriptor)
    {
        const int flags = fcntl(fileDescriptor, F_GETFL, 0);
        if (flags < 0)
            return false;
        return fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) == 0;
    }
#endif

    // ========================================================================
    // 关闭文件描述符
    // ========================================================================

#ifdef _WIN32
    inline int closeFileDescriptor(int fileDescriptor)
    {
        return ::closesocket(fileDescriptor);
    }
#else
    inline int closeFileDescriptor(int fileDescriptor)
    {
        return ::close(fileDescriptor);
    }
#endif

    // ========================================================================
    // read / write（用于 eventfd / timerfd / socket pair）
    // ========================================================================

#ifdef _WIN32
    inline ssize_t readFileDescriptor(int fileDescriptor, void *buffer, size_t length)
    {
        return ::recv(fileDescriptor, static_cast<char *>(buffer), static_cast<int>(length), 0);
    }

    inline ssize_t writeFileDescriptor(int fileDescriptor, const void *buffer, size_t length)
    {
        return ::send(fileDescriptor, static_cast<const char *>(buffer), static_cast<int>(length), 0);
    }
#else
    inline ssize_t readFileDescriptor(int fileDescriptor, void *buffer, size_t length)
    {
        return ::read(fileDescriptor, buffer, length);
    }

    inline ssize_t writeFileDescriptor(int fileDescriptor, const void *buffer, size_t length)
    {
        return ::write(fileDescriptor, buffer, length);
    }
#endif

    // ========================================================================
    // accept（Windows 无 accept4，用 accept + setNonBlocking）
    // ========================================================================

#ifdef _WIN32
    inline int acceptSocket(int listenFileDescriptor, sockaddr *address, socklen_t *addressLength)
    {
        const int fileDescriptor = ::accept(listenFileDescriptor, address, addressLength);
        if (fileDescriptor >= 0)
        {
            setNonBlocking(fileDescriptor);
        }
        return fileDescriptor;
    }
#else
    inline int acceptSocket(int listenFileDescriptor, sockaddr *address, socklen_t *addressLength)
    {
        return ::accept4(listenFileDescriptor, address, addressLength, SOCK_NONBLOCK | SOCK_CLOEXEC);
    }
#endif

    // ========================================================================
    // 创建 socket pair（用于 Windows eventfd/timerfd 替代）
    // ========================================================================

#ifdef _WIN32
    inline bool createSocketPair(int &readFileDescriptor, int &writeFileDescriptor)
    {
        // 通过 TCP loopback 创建一对已连接的 socket
        const int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0)
            return false;

        BOOL opt = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<char *>(&opt), sizeof(opt));

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
        {
            ::closesocket(listener);
            return false;
        }

        socklen_t addressLength = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) < 0)
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

        if (::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
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

        readFileDescriptor  = server;
        writeFileDescriptor = client;
        return true;
    }
#else
    inline bool createSocketPair(int &readFileDescriptor, int &writeFileDescriptor)
    {
        (void)readFileDescriptor;
        (void)writeFileDescriptor;
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
