#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <cstring>
#include <limits>
#include <mutex>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <sys/sendfile.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
#if ASYN_PLATFORM_WIN32
        /**
         * @brief Winsock 初始化引用计数状态
         *
         * @details WSAStartup 与 WSACleanup 必须严格配对，因此不能只记录「是否启动过」。
         *          多个持有网络资源的对象（如逐个析构的 IoContext）各自成对调用时，
         *          只有最后一个引用释放才允许真正清理，否则仍存活的 socket 会失去 Winsock 支撑。
         */
        struct WinsockReferenceCount
        {
            std::mutex mutex;                    ///< 保护 initializeCount，跨线程启停时串行化
            int        initializeCount = 0;      ///< 当前生效的 WSAStartup 引用数
        };

        /**
         * @brief 取得进程级 Winsock 引用计数状态
         * @return WinsockReferenceCount& 全局唯一的状态对象
         */
        WinsockReferenceCount &winsockReferenceCount()
        {
            // 函数内 static 由 C++11 起保证线程安全的惰性初始化
            static WinsockReferenceCount state;
            return state;
        }
#endif

        /**
         * @brief 设置一个 int 尺寸的套接字选项
         * @param descriptor 目标套接字描述符
         * @param optionLevel 选项层级，如 SOL_SOCKET / IPPROTO_TCP / IPPROTO_IPV6
         * @param optionName 选项名
         * @param optionValue 选项值
         * @return true 设置成功
         */
        bool setIntegerOption(const int descriptor, const int optionLevel, const int optionName, const int optionValue) noexcept
        {
            // Windows 的值参数是 const char*，POSIX 是 const void*：
            // 统一转成 const char* 对两边都成立，业务层因此不必再写平台分支
            return ::setsockopt(descriptor, optionLevel, optionName, reinterpret_cast<const char *>(&optionValue), static_cast<socklen_t>(sizeof(optionValue))) == 0;
        }
    }

    bool Socket::initialize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                            &state = winsockReferenceCount();
        const std::lock_guard<std::mutex> lock(state.mutex);
        // 仅首个引用真正启动 Winsock，后续调用累加计数即可
        if (state.initializeCount == 0)
        {
            WSADATA socketData{};
            if (::WSAStartup(MAKEWORD(2, 2), &socketData) != 0)
            {
                return false;
            }
        }
        ++state.initializeCount;
        return true;
#else
        // Linux 侧没有需要启动的全局子系统，但有一个进程级设置要在这里落地：忽略 SIGPIPE。
        // 本层所有发送都带 MSG_NOSIGNAL，唯独 sendfile 没有对应的 per-call 标志——向已关闭的
        // 对端发文件会以默认动作打死整个进程。忽略之后写失败改以 EPIPE 返回，交给既有的
        // 发送失败路径处理（与 TlsContext 对 OpenSSL 内部写入的处理同源，那边是另一条
        // 绕不开的信号来源）。signal() 幂等，重复调用没有额外影响
        std::signal(SIGPIPE, SIG_IGN);
        return true;
#endif
    }

    void Socket::finalize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                            &state = winsockReferenceCount();
        const std::lock_guard<std::mutex> lock(state.mutex);
        // 计数归零才清理；多余的 finalize 调用直接忽略，避免把计数减成负数
        if (state.initializeCount > 0 && --state.initializeCount == 0)
        {
            ::WSACleanup();
        }
#endif
    }

    int Socket::accept(const int listenDescriptor, sockaddr *address, socklen_t *addressLength) noexcept
    {
#if ASYN_PLATFORM_WIN32
        const auto fileDescriptor = ::accept(listenDescriptor, address, addressLength);
        if (static_cast<int>(fileDescriptor) >= 0)
        {
            // 置非阻塞失败不能静默放过：返回的阻塞套接字会被当作非阻塞套接字注册进事件循环，
            // 之后任何 send/recv 都可能把循环线程停住，而调用方无从察觉。失败即关闭并返回无效值
            if (!FileDescriptor::setNonBlocking(static_cast<int>(fileDescriptor)))
            {
                const int failureCode = PlatformError::lastSocketErrorCode();
                ::closesocket(fileDescriptor);
                PlatformError::setLastErrorCode(failureCode);
                return FileDescriptor::kInvalid;
            }
            // 与 POSIX 侧的 SOCK_CLOEXEC 取平：Windows 的 accept 句柄默认可继承，留着继承位就等于
            // 把一条已建立的连接交给随机一个子进程——父进程关掉它之后这一侧仍被陌生进程持着
            static_cast<void>(FileDescriptor::markNonInheritable(static_cast<int>(fileDescriptor)));
        }
        return static_cast<int>(fileDescriptor);
#else
        return ::accept4(listenDescriptor, address, addressLength, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
    }

    bool Socket::setReuseAddress(const int descriptor) noexcept
    {
        return setIntegerOption(descriptor, SOL_SOCKET, SO_REUSEADDR, 1);
    }

    bool Socket::setReusePort(const int descriptor) noexcept
    {
#ifdef SO_REUSEPORT
        // 用特性宏而不是操作系统宏判定：可用性取决于内核版本，不是取决于厂商
        return setIntegerOption(descriptor, SOL_SOCKET, SO_REUSEPORT, 1);
#else
        // Windows 没有该选项，返回 false 让调用方按「不支持」降级而不是当作失败
        (void) descriptor;
        return false;
#endif
    }

    bool Socket::setNoDelay(const int descriptor) noexcept
    {
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_NODELAY, 1);
    }

    bool Socket::setSendBufferSize(const int descriptor, const int byteCount) noexcept
    {
        if (byteCount <= 0)
        {
            // 非正值没有「按上限扩容」的语义，直接拒绝而不是把含糊的取值交给内核
            return false;
        }
        return setIntegerOption(descriptor, SOL_SOCKET, SO_SNDBUF, byteCount);
    }

    bool Socket::setReceiveBufferSize(const int descriptor, const int byteCount) noexcept
    {
        if (byteCount <= 0)
        {
            return false;
        }
        return setIntegerOption(descriptor, SOL_SOCKET, SO_RCVBUF, byteCount);
    }

    bool Socket::setDeferAccept(const int descriptor, const int seconds) noexcept
    {
#ifdef TCP_DEFER_ACCEPT
        if (seconds < 0)
        {
            return false;
        }
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_DEFER_ACCEPT, seconds);
#else
        // Windows 没有该选项，返回 false 让调用方按「不支持」降级而不是当作失败
        (void) descriptor;
        (void) seconds;
        return false;
#endif
    }

    bool Socket::setFastOpen(const int descriptor, const int queueLength) noexcept
    {
#ifdef TCP_FASTOPEN
        // 负值没有「允许 TFO 的等待队列长度」这一语义：直接拒绝，不把含糊取值交给内核
        if (queueLength < 0)
        {
            return false;
        }
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_FASTOPEN, queueLength);
#else
        // 头文件里没有该选项（老内核头 / 老 SDK）：按「平台不支持」返回 false 由调用方降级
        (void) descriptor;
        (void) queueLength;
        return false;
#endif
    }

    bool Socket::setIpv6Only(const int descriptor, const bool isOnlyV6) noexcept
    {
        // 布尔值在两个平台上都按 int 尺寸传递，写成 0/1 避免 sizeof(bool) 歧义
        return setIntegerOption(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, isOnlyV6 ? 1 : 0);
    }

    int Socket::takePendingError(const int descriptor) noexcept
    {
        int       pendingError = 0;
        socklen_t optionLength = static_cast<socklen_t>(sizeof(pendingError));

        // Windows 的值参数是 char*、POSIX 是 void*，统一转 char* 两边都能接受，
        // 因此这里同样不需要平台分支
        if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&pendingError), &optionLength) != 0)
        {
            // 连 SO_ERROR 都读不到说明描述符已经失效：如实交回该错误，
            // 而不是返回 0 让调用方误判为「连接已建立」
            return PlatformError::lastSocketErrorCode();
        }
        return pendingError;
    }

    ssize_t Socket::writeVectored(const int descriptor, const WriteBuffer *const buffers, const std::size_t bufferCount) noexcept
    {
        // 段数与长度先按平台上限校验：这两处超限都必须当场失败。
        // 段数超限静默拆分会让「一次系统调用」的收益悄悄消失；单段长度被截断更糟——
        // 线上字节流会缺一截，而返回值看起来一切正常
        if (!FileDescriptor::isValid(descriptor) || buffers == nullptr || bufferCount == 0 || bufferCount > kMaximumVectorCount)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        // 「有长度却没有数据」这一非法形状在两个平台上都得当场拒掉：交给 Windows 是 WSASend 报错，
        // 交给 POSIX 的 sendmsg 则只得到一个依赖地址取值的 EFAULT——判据放在平台分支之前，
        // 才不会同一份参数在一侧返回 kInvalidArgument、另一侧返回系统错误
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            if (!buffers[index].data && buffers[index].length != 0)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
        }

#if ASYN_PLATFORM_WIN32
        WSABUF windowsBuffers[kMaximumVectorCount]{};
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            // 长度形参是 ULONG：超限直接失败，静默截断会让线上字节流缺一截而返回值看着正常
            if (buffers[index].length > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            windowsBuffers[index].buf = static_cast<char *>(const_cast<void *>(buffers[index].data));
            windowsBuffers[index].len = static_cast<ULONG>(buffers[index].length);
        }

        DWORD sentLength = 0;
        if (::WSASend(static_cast<SOCKET>(descriptor), windowsBuffers, static_cast<DWORD>(bufferCount), &sentLength, 0, nullptr, nullptr) == SOCKET_ERROR)
        {
            return -1;
        }
        return static_cast<ssize_t>(sentLength);
#else
        iovec vectors[kMaximumVectorCount]{};
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            vectors[index].iov_base = const_cast<void *>(buffers[index].data);
            vectors[index].iov_len  = buffers[index].length;
        }

        // 用 sendmsg 而不是 writev：只有它带 MSG_NOSIGNAL，对端已关闭时不会把进程打死
        msghdr message{};
        message.msg_iov    = vectors;
        message.msg_iovlen = bufferCount;
        return ::sendmsg(descriptor, &message, MSG_NOSIGNAL);
#endif
    }

#if !ASYN_PLATFORM_WIN32
    ssize_t Socket::sendFileChunk(const int socketDescriptor, const int fileDescriptor, const std::uint64_t offset, const std::size_t length) noexcept
    {
        if (!FileDescriptor::isValid(socketDescriptor) || !FileDescriptor::isValid(fileDescriptor) || length == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        // 超限会让内核直接 EINVAL 而不是部分写入，先钳制到安全上限；调用方按返回值推进偏移
        const std::size_t clampedLength = length > kMaximumSendFileChunk ? kMaximumSendFileChunk : length;

        // 显式传偏移指针：传空指针会让 sendfile 改用并推进描述符自身的文件偏移，而同一个
        // 文件可能被多条响应并发发送，动共享偏移会让它们读到彼此的位置
        off_t sendOffset = static_cast<off_t>(offset);
        return ::sendfile(socketDescriptor, fileDescriptor, &sendOffset, clampedLength);
    }
#endif

    namespace
    {
        /// 移交消息的定长头：地址族与类型随载荷一起过去，接收侧据此校验「收到的确实是我等的那类套接字」
        struct HandoffHeader
        {
            std::uint16_t family{0};        ///< 地址族（AF_INET / AF_INET6 ...）
            std::uint16_t socketType{0};    ///< 套接字类型（SOCK_STREAM ...）
            std::uint32_t blobByteCount{0}; ///< 载荷字节数：Windows 是 WSAPROTOCOL_INFO，POSIX 恒为 0
        };

        /**
         * @brief 按阻塞语义把整段字节写完
         * @param descriptor 通道套接字
         * @param bytes 待写字节
         * @param length 字节数
         * @return true 全部写完
         * @note 只有 Windows 侧用到：那边把协议信息当字节流写过去，POSIX 侧的描述符随 sendmsg 的
         *       控制消息过、没有这段字节要写（匿名 namespace 里没人调用的函数在 GCC 是错误，故整体门控）
         */
#if ASYN_PLATFORM_WIN32
        bool writeAll(int descriptor, const char *bytes, std::size_t length)
        {
            std::size_t written = 0;
            while (written < length)
            {
                const int pieceLength = ::send(descriptor, bytes + written, static_cast<int>(length - written), 0);
                if (pieceLength <= 0)
                {
                    return false;
                }
                written += static_cast<std::size_t>(pieceLength);
            }
            return true;
        }

        /**
         * @brief 按阻塞语义把整段字节读满
         * @param descriptor 通道套接字
         * @param bytes 输出缓冲
         * @param length 期望字节数
         * @return true 读满；通道提前关闭或读坏返回 false
         * @note 读不满就是「消息不完整」，调用方必须整体作废而不是拿半截载荷去重建套接字
         */
        bool readAll(int descriptor, char *bytes, std::size_t length)
        {
            std::size_t read = 0;
            while (read < length)
            {
                const int pieceLength = ::recv(descriptor, bytes + read, static_cast<int>(length - read), 0);
                if (pieceLength <= 0)
                {
                    return false;
                }
                read += static_cast<std::size_t>(pieceLength);
            }
            return true;
        }
#endif

        /**
         * @brief 取一个套接字的地址族与类型，填进移交头
         * @param descriptor 目标套接字
         * @param header 输出：填好 family 与 socketType 的头
         * @return true 两项都取到
         * @note 地址族的问法两家不同：Linux 有 SO_DOMAIN，Windows 没有，只能从本地地址的
         *       sa_family 读回来——移交头只是给接收侧做一致性核对的，两条路都给得出同一个值
         */
        bool fillHandoffHeaderIdentity(int descriptor, HandoffHeader &header)
        {
            int type             = 0;
            // 长度参数的类型两家不同（Winsock 是 int*，POSIX 是 socklen_t*）：一律用 socklen_t，
            // 它在 Windows 上就是 winsock2 给的 int 别名
            socklen_t valueLength = static_cast<socklen_t>(sizeof(type));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &valueLength) != 0)
            {
                return false;
            }
            header.socketType = static_cast<std::uint16_t>(type);

#ifdef SO_DOMAIN
            int family = 0;
            valueLength = static_cast<socklen_t>(sizeof(family));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_DOMAIN, reinterpret_cast<char *>(&family), &valueLength) != 0)
            {
                return false;
            }
#else
            sockaddr_storage localAddress{};
            socklen_t        localAddressLength = static_cast<socklen_t>(sizeof(localAddress));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&localAddress), &localAddressLength) != 0)
            {
                return false;
            }
            const int family = localAddress.ss_family;
#endif
            header.family = static_cast<std::uint16_t>(family);
            return true;
        }
    } // namespace

    bool Socket::writeListeningSocketHandoff(const int channelDescriptor, const int listenDescriptor, const std::uint64_t targetProcessId) noexcept
    {
        if (channelDescriptor < 0 || listenDescriptor < 0)
        {
            PlatformError::setLastErrorCode(EINVAL);
            return false;
        }

        HandoffHeader header;
        if (!fillHandoffHeaderIdentity(listenDescriptor, header))
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return false;
        }

#if ASYN_PLATFORM_WIN32
        // Winsock 的复制接口按**进程号**认目标，不需要先把对方 OpenProcess 成句柄；
        // 交给本进程（自测与「同进程内换一份描述符」的用法）也是同一条路
        WSAPROTOCOL_INFOW protocolInfo{};
        const int duplicationResult = ::WSADuplicateSocketW(static_cast<SOCKET>(listenDescriptor), static_cast<DWORD>(targetProcessId), &protocolInfo);
        if (duplicationResult != 0)
        {
            PlatformError::setLastErrorCode(::WSAGetLastError());
            return false;
        }

        header.blobByteCount = sizeof(WSAPROTOCOL_INFOW);
        if (!writeAll(channelDescriptor, reinterpret_cast<const char *>(&header), sizeof(header)))
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return false;
        }
        if (!writeAll(channelDescriptor, reinterpret_cast<const char *>(&protocolInfo), sizeof(protocolInfo)))
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return false;
        }
        return true;
#else
        // POSIX 的载荷走控制消息而不是字节流：头里 blobByteCount 恒为 0，描述符随 SCM_RIGHTS 一起过，
        // 因此本平台上「交给哪个进程」由内核在传递时决定，参数不需要用
        (void) targetProcessId;
        header.blobByteCount = 0U;
        char controlBuffer[CMSG_SPACE(sizeof(int))] = {};
        iovec dataVector{reinterpret_cast<void *>(&header), sizeof(header)};
        msghdr message{};
        message.msg_iov        = &dataVector;
        message.msg_iovlen     = 1;
        message.msg_control    = controlBuffer;
        message.msg_controllen = sizeof(controlBuffer);

        cmsghdr *controlHeader = CMSG_FIRSTHDR(&message);
        controlHeader->cmsg_level = SOL_SOCKET;
        controlHeader->cmsg_type  = SCM_RIGHTS;
        controlHeader->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(controlHeader), &listenDescriptor, sizeof(listenDescriptor));

        if (::sendmsg(channelDescriptor, &message, 0) < 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return false;
        }
        return true;
#endif
    }

    int Socket::readListeningSocketHandoff(const int channelDescriptor) noexcept
    {
        if (channelDescriptor < 0)
        {
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }

#if ASYN_PLATFORM_WIN32
        HandoffHeader header{};
        if (!readAll(channelDescriptor, reinterpret_cast<char *>(&header), sizeof(header)))
        {
            // 头都没收齐就是「没有一条完整消息」，报通道的错而不是猜一个格式
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return -1;
        }
        if (header.blobByteCount != sizeof(WSAPROTOCOL_INFOW))
        {
            // 载荷长度不像本平台的载体：那是别的版本或别的平台写来的，猜着读只会拿到半个套接字
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }

        WSAPROTOCOL_INFOW protocolInfo{};
        if (!readAll(channelDescriptor, reinterpret_cast<char *>(&protocolInfo), sizeof(protocolInfo)))
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return -1;
        }

        // FROM_PROTOCOL_INFO 是交给 af/type/protocol 这三个参数的哨兵，意思是「三项都按协议信息里
        // 带的来」；交 0 会被当成「地址族 AF_UNSPEC 的空套接字」而建不出对端那个监听口。
        // dwFlags 交 0：重建出的套接字与交出方同一形态（阻塞），要挂进完成端口的调用方自己改重叠
        const SOCKET receivedSocket = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &protocolInfo, 0, 0);
        if (receivedSocket == INVALID_SOCKET)
        {
            PlatformError::setLastErrorCode(::WSAGetLastError());
            return -1;
        }
        return static_cast<int>(receivedSocket);
#else
        HandoffHeader header{};
        char          controlBuffer[CMSG_SPACE(sizeof(int))] = {};
        iovec         dataVector{reinterpret_cast<void *>(&header), sizeof(header)};
        msghdr        message{};
        message.msg_iov        = &dataVector;
        message.msg_iovlen     = 1;
        message.msg_control    = controlBuffer;
        message.msg_controllen = sizeof(controlBuffer);

        const ssize_t receivedLength = ::recvmsg(channelDescriptor, &message, 0);
        if (receivedLength < 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return -1;
        }
        if (static_cast<std::size_t>(receivedLength) < sizeof(header) || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 ||
            header.blobByteCount != 0U)
        {
            // 头没收全、或控制消息被截断，都等于「这份移交不可信」：整体作废，
            // 不要拿半个描述符去 accept——那比失败更难查
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }

        int receivedDescriptor = -1;
        for (const cmsghdr *controlHeader = CMSG_FIRSTHDR(&message); controlHeader != nullptr;
             controlHeader                = CMSG_NXTHDR(&message, const_cast<cmsghdr *>(controlHeader)))
        {
            if (controlHeader->cmsg_level == SOL_SOCKET && controlHeader->cmsg_type == SCM_RIGHTS &&
                controlHeader->cmsg_len >= CMSG_LEN(sizeof(int)))
            {
                std::memcpy(&receivedDescriptor, CMSG_DATA(controlHeader), sizeof(receivedDescriptor));
                break;
            }
        }
        if (receivedDescriptor < 0)
        {
            PlatformError::setLastErrorCode(EBADF);
            return -1;
        }
        return receivedDescriptor;
#endif
    }
} // namespace AsynGyanis::Platform
