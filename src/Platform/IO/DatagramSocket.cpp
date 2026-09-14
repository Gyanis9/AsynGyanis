#include "Platform/IO/DatagramSocket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <utility>

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 套接字地址是否已被设置（长度与地址族都得合法）
        bool isAddressSet(const SocketAddress &address) noexcept
        {
            return address.length > 0 && (address.storage.ss_family == AF_INET || address.storage.ss_family == AF_INET6);
        }
    } // namespace

    DatagramSocket::~DatagramSocket()
    {
        close();
    }

    DatagramSocket::DatagramSocket(DatagramSocket &&other) noexcept :
        m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1))
    {
    }

    DatagramSocket &DatagramSocket::operator=(DatagramSocket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_fileDescriptor = std::exchange(other.m_fileDescriptor, -1);
        }
        return *this;
    }

    DatagramSocket DatagramSocket::bindTo(const SocketAddress &localAddress) noexcept
    {
        DatagramSocket socket;

        if (!isAddressSet(localAddress))
        {
            // 地址没给或地址族不认识：不猜协议族也不去绑「任意地址」，当场说明这是用法错误
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return socket;
        }

        // 地址族跟着调用方给的地址走：IPv4 与 IPv6 各自建自己的套接字，不做双栈推断
        socket.m_fileDescriptor = static_cast<int>(::socket(localAddress.storage.ss_family, SOCK_DGRAM, IPPROTO_UDP));
        if (!FileDescriptor::isValid(socket.m_fileDescriptor))
        {
            // socket/bind/getsockname 都是套接字族调用：Windows 上错误在 WSAGetLastError，
            // POSIX 上与 errno 同源，因此统一取套接字错误码
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            socket.m_fileDescriptor = -1;
            return socket;
        }

        // 地址复用：服务端重启时端口可能还在被上一个实例占着，不允许复用会让重启失败
        static_cast<void>(Socket::setReuseAddress(socket.m_fileDescriptor));

        if (::bind(socket.m_fileDescriptor, reinterpret_cast<const sockaddr *>(&localAddress.storage), localAddress.length) != 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            FileDescriptor::close(socket.m_fileDescriptor);
            socket.m_fileDescriptor = -1;
            return socket;
        }

        // 非阻塞：收发都由事件循环驱动，任何一次调用都不许在这里等
        if (!FileDescriptor::setNonBlocking(socket.m_fileDescriptor))
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            FileDescriptor::close(socket.m_fileDescriptor);
            socket.m_fileDescriptor = -1;
            return socket;
        }
        return socket;
    }

    bool DatagramSocket::isValid() const noexcept
    {
        return FileDescriptor::isValid(m_fileDescriptor);
    }

    int DatagramSocket::fileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }

    SocketAddress DatagramSocket::localAddress() const noexcept
    {
        SocketAddress localAddress;
        if (!isValid())
        {
            return localAddress;
        }

        // 端口给 0 时真正在用的端口要问内核：getsockname 是唯一来源
        localAddress.length = sizeof(localAddress.storage);
        if (::getsockname(m_fileDescriptor, reinterpret_cast<sockaddr *>(&localAddress.storage), &localAddress.length) != 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            localAddress = SocketAddress{};
        }
        return localAddress;
    }

    ssize_t DatagramSocket::receive(void *const buffer, const std::size_t capacity, SocketAddress &peerAddress) const noexcept
    {
        // 输出参数先清空：失败或没数据时调用方不该读到上一次的地址
        peerAddress = SocketAddress{};
        if (!isValid() || buffer == nullptr || capacity == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        // 缓冲区比单条报文上限还大也不会收到更多：把交给系统调用的长度钳进 int 范围，
        // 免得把大 size_t 静默窄化（UDP 语义下超长缓冲既没有额外好处，也没有额外代价）
        const std::size_t receiveCapacity = capacity > kMaximumDatagramBytes ? kMaximumDatagramBytes : capacity;
        peerAddress.length = sizeof(peerAddress.storage);
        const ssize_t receivedByteCount = static_cast<ssize_t>(
                ::recvfrom(m_fileDescriptor, static_cast<char *>(buffer), static_cast<int>(receiveCapacity), 0,
                           reinterpret_cast<sockaddr *>(&peerAddress.storage), &peerAddress.length));
        if (receivedByteCount < 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            peerAddress = SocketAddress{};
            return -1;
        }
        return receivedByteCount;
    }

    ssize_t DatagramSocket::send(const SocketAddress &peerAddress, const void *const buffer, const std::size_t length) const noexcept
    {
        if (!isValid() || buffer == nullptr || length == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }
        if (length > kMaximumDatagramBytes)
        {
            // 超限交给系统调用只会拿到平台各自的错误码（EMSGSIZE / WSAEMSGSIZE），
            // 不如在这一层统一判错并给出上限，调用方看到的文案与两端一致
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }
        if (!isAddressSet(peerAddress))
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        const ssize_t sentByteCount = static_cast<ssize_t>(
                ::sendto(m_fileDescriptor, static_cast<const char *>(buffer), static_cast<int>(length), 0,
                         reinterpret_cast<const sockaddr *>(&peerAddress.storage), peerAddress.length));
        if (sentByteCount < 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return -1;
        }
        return sentByteCount;
    }

    void DatagramSocket::close() noexcept
    {
        if (isValid())
        {
            FileDescriptor::close(m_fileDescriptor);
            m_fileDescriptor = -1;
        }
    }
} // namespace AsynGyanis::Platform
