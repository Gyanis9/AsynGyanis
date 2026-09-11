/**
 * @file AsyncSocket.cpp
 * @brief 异步套接字实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/AsyncSocket.h"
#include "Core/EventLoop/EpollAwaiter.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Base/Exception/SystemException.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <cerrno>

namespace AsynGyanis::Core
{
    AsyncSocket::AsyncSocket(EventLoop &loop, const int fileDescriptor) :
        m_loop(loop), m_fileDescriptor(fileDescriptor)
    {
        if (m_fileDescriptor >= 0)
        {
            setNonBlocking();
        }
    }

    AsyncSocket::~AsyncSocket()
    {
        close();
    }

    AsyncSocket::AsyncSocket(AsyncSocket &&other) noexcept :
        m_loop(other.m_loop), m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1))
    {
    }

    AsyncSocket &AsyncSocket::operator=(AsyncSocket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_fileDescriptor = std::exchange(other.m_fileDescriptor, -1);
        }
        return *this;
    }

    AsyncSocket AsyncSocket::create(EventLoop &loop, const int domain, const int type)
    {
#ifdef _WIN32
        const int fileDescriptor = ::socket(domain, type, 0);
#else
        const int fileDescriptor = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
        if (fileDescriptor < 0)
        {
            throw Base::SystemException("socket creation failed");
        }

#ifdef _WIN32
        Platform::FileDescriptor::setNonBlocking(fileDescriptor);
#endif

        if (type == SOCK_STREAM)
        {
            constexpr int opt = 1;
            setsockopt(fileDescriptor, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
        }

        return AsyncSocket(loop, fileDescriptor);
    }

    bool AsyncSocket::bind(const sockaddr *const address, const socklen_t addressLength) const
    {
        constexpr int opt = 1;
        setsockopt(m_fileDescriptor, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
        return ::bind(m_fileDescriptor, address, addressLength) == 0;
    }

    bool AsyncSocket::bind(const InetAddress &address) const
    {
        return bind(address.nativeAddress(), address.nativeAddressLength());
    }

    bool AsyncSocket::listen(const int backlog) const
    {
        return ::listen(m_fileDescriptor, backlog) == 0;
    }

    Task<AsyncSocket> AsyncSocket::asyncAccept() const
    {
        while (true)
        {
            sockaddr_storage address{};

            socklen_t addressLength = sizeof(address);
            if (const int fileDescriptor = Platform::Socket::accept(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength);
                fileDescriptor >= 0)
            {
                constexpr int opt = 1;
                setsockopt(fileDescriptor, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
                co_return AsyncSocket(m_loop, fileDescriptor);
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLIN);
                continue;
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kConnectionAborted)
            {
                continue;
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kTooManyOpenFiles || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kSystemFileTableFull || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kNoBufferSpace || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kOutOfMemory)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLIN);
                continue;
            }

            throw Base::SystemException("accept failed");
        }
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const address, const socklen_t addressLength) const
    {
        if (const int ret = ::connect(m_fileDescriptor, address, addressLength); ret == 0)
        {
            co_return;
        } else if (Platform::PlatformError::lastSocketErrorCode() != Platform::PlatformError::kInProgress)
        {
            throw Base::SystemException("connect failed");
        }

        co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLOUT);

        int       error = 0;
        socklen_t length = sizeof(error);
        getsockopt(m_fileDescriptor, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);
        if (error != 0)
        {
            throw Base::SystemException("connect failed", std::error_code(error, std::system_category()));
        }
        co_return;
    }

    Task<> AsyncSocket::asyncConnect(const InetAddress &address) const
    {
        co_return co_await asyncConnect(address.nativeAddress(), address.nativeAddressLength());
    }

    Task<ssize_t> AsyncSocket::asyncReceive(void *const buffer, const size_t length) const
    {
        if (length == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const ssize_t n = ::recv(m_fileDescriptor, static_cast<char *>(buffer), static_cast<int>(length), MSG_NOSIGNAL);
            if (n > 0)
                co_return n;
            if (n == 0)
                co_return 0;
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLIN);
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
                continue;
            throw Base::SystemException("recv failed");
        }
    }

    Task<ssize_t> AsyncSocket::asyncSend(const void *const buffer, const size_t length) const
    {
        if (length == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const ssize_t n = ::send(m_fileDescriptor, static_cast<const char *>(buffer), static_cast<int>(length), MSG_NOSIGNAL);
            if (n > 0)
                co_return n;
            if (n == 0)
                co_return -1; // 对端已关闭连接
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock || Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLOUT);
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
                continue;
            throw Base::SystemException("send failed");
        }
    }

    void AsyncSocket::close()
    {
        if (m_fileDescriptor >= 0)
        {
            ::shutdown(m_fileDescriptor, SHUT_RDWR);
            Platform::FileDescriptor::close(m_fileDescriptor);
            m_fileDescriptor = -1;
        }
    }

    int AsyncSocket::fileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }

    void AsyncSocket::setNonBlocking() const
    {
        if (m_fileDescriptor >= 0)
        {
            Platform::FileDescriptor::setNonBlocking(m_fileDescriptor);
        }
    }

    bool AsyncSocket::setSockOpt(const int level, const int opt, const void *const value, const socklen_t length) const
    {
        return setsockopt(m_fileDescriptor, level, opt, static_cast<const char *>(value), length) == 0;
    }

    InetAddress AsyncSocket::remoteAddress() const
    {
        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getpeername(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            throw Base::SystemException("getpeername failed");
        }
        return InetAddress(address, addressLength);
    }

    InetAddress AsyncSocket::localAddress() const
    {
        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getsockname(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            throw Base::SystemException("getsockname failed");
        }
        return InetAddress(address, addressLength);
    }
}
