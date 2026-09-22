#include "Platform/IO/DatagramSocket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <cstdint>
#include <utility>

#if ASYN_PLATFORM_WIN32
    #include <windows.h>
#endif

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

        // 地址族跟着调用方给的地址走：IPv4 与 IPv6 各自建自己的套接字，不做双栈推断。
        // 建完立刻把它标成「不随 spawn 传下去」：多进程 worker 走 fork+exec 或
        // CreateProcess(bInheritHandles=TRUE)，继承下来的套接字会让父进程退出后端口仍被占着，
        // 而且子进程与父进程共用同一条接收队列，会把报文读进一个永远不会处理它的进程里
        const int socketType = SOCK_DGRAM
#if !ASYN_PLATFORM_WIN32
                               | SOCK_CLOEXEC
#endif
            ;
        socket.m_fileDescriptor = static_cast<int>(::socket(localAddress.storage.ss_family, socketType, IPPROTO_UDP));
#if ASYN_PLATFORM_WIN32
        // Winsock 的句柄默认可继承，而带 WSA_FLAG_NO_HANDLE_INHERIT 的 WSASocketW 要求老系统上
        // 另走一条建法；这里只在建好之后取消继承位，语义相同且不会因缺标志而整个建不出套接字
        static_cast<void>(FileDescriptor::markNonInheritable(socket.m_fileDescriptor));
#endif
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

        // 端口复用：多个监听器共用一个 UDP 端口时，只设 SO_REUSEADDR 的内核会让每一个都「绑定成功」，
        // 却把全部报文交给最后绑上的那一个——前面的监听器一句错误都不报，永远收不到报文。
        // 多进程 worker 各自绑同一端口做 h3 横向扩展正好落在这个坑上。Linux 3.9+ 才有这个选项，
        // Windows 上必然失败——按「平台不支持即降级」处理，与 TcpAcceptor 同一惯例，不当作绑定失败
        [[maybe_unused]] const bool isReusePortSet = Socket::setReusePort(socket.m_fileDescriptor);

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
#if ASYN_PLATFORM_WIN32
            // Windows 在报文大于缓冲时返回 WSAEMSGSIZE，缓冲里是截断后的数据。按文档承诺的
            // 「多出的字节被丢弃、返回值即容量」交付，与 Linux 的 recvfrom 语义对齐；
            // 当作硬错误会把一条本可读的报文连同语义一起丢掉
            if (PlatformError::lastSocketErrorCode() == WSAEMSGSIZE)
            {
                return static_cast<ssize_t>(receiveCapacity);
            }
#endif
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            peerAddress = SocketAddress{};
            return -1;
        }
        return receivedByteCount;
    }

    ssize_t DatagramSocket::send(const SocketAddress &peerAddress, const void *const buffer, const std::size_t length) const noexcept
    {
        // 零长数据报是合法的（RFC 768：最小报文就是 8 字节头、零负载），接收侧一直照收，
        // 这里也照发——用零长报文做保活探测是常见做法，拒掉它会让两种方向的行为不对称
        if (!isValid() || buffer == nullptr)
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
