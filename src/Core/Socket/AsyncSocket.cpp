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
#if ASYN_PLATFORM_WIN32
        const int fileDescriptor = ::socket(domain, type, 0);
#else
        const int fileDescriptor = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
        if (fileDescriptor < 0)
        {
            throw Base::SystemException("创建套接字失败");
        }

#if ASYN_PLATFORM_WIN32
        // Windows 没有 SOCK_NONBLOCK，只能在创建后补设非阻塞
        Platform::FileDescriptor::setNonBlocking(fileDescriptor);
#endif

        if (type == SOCK_STREAM)
        {
            // 流式套接字默认关掉 Nagle：框架承载的是小包请求/响应，攒包会明显抬高首字节延迟
            [[maybe_unused]] const bool isNoDelaySet = Platform::Socket::setNoDelay(fileDescriptor);
        }

        return AsyncSocket(loop, fileDescriptor);
    }

    bool AsyncSocket::bind(const sockaddr *const address, const socklen_t addressLength) const
    {
        // 统一开启地址复用：服务重启时上一代连接的 TIME_WAIT 会占住端口
        [[maybe_unused]] const bool isReuseAddressSet = Platform::Socket::setReuseAddress(m_fileDescriptor);
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
                // 新连接同样关闭 Nagle；设置失败只影响延迟，不丢弃这条连接
                [[maybe_unused]] const bool isNoDelaySet = Platform::Socket::setNoDelay(fileDescriptor);
                co_return AsyncSocket(m_loop, fileDescriptor);
            }

            // 暂无待接受连接：挂起等监听描述符可读。
            // 原先这里把 kWouldBlock 比较写了两遍（另一处本意是别的错误码），收敛成一次判断
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLIN);
                continue;
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted ||
                Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kConnectionAborted)
            {
                continue;
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kTooManyOpenFiles ||
                Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kSystemFileTableFull ||
                Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kNoBufferSpace ||
                Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kOutOfMemory)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLIN);
                continue;
            }

            throw Base::SystemException("接受新连接失败");
        }
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const address, const socklen_t addressLength) const
    {
        if (const int ret = ::connect(m_fileDescriptor, address, addressLength); ret == 0)
        {
            co_return;
        } else if (Platform::PlatformError::lastSocketErrorCode() != Platform::PlatformError::kInProgress)
        {
            throw Base::SystemException("发起连接失败");
        }

        co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLOUT);

        // 非阻塞 connect 完成后靠 SO_ERROR 判定成败，该读取由 Platform 统一封装
        if (const int pendingError = Platform::Socket::takePendingError(m_fileDescriptor); pendingError != 0)
        {
            throw Base::SystemException("连接对端失败", std::error_code(pendingError, std::system_category()));
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
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLIN);
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
                continue;
            throw Base::SystemException("接收数据失败");
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
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fileDescriptor, EPOLLOUT);
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
                continue;
            throw Base::SystemException("发送数据失败");
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
            throw Base::SystemException("获取对端地址失败");
        }
        return InetAddress(address, addressLength);
    }

    InetAddress AsyncSocket::localAddress() const
    {
        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getsockname(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            throw Base::SystemException("获取本地地址失败");
        }
        return InetAddress(address, addressLength);
    }
}
