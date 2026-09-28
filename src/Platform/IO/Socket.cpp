#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <system_error>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/un.h>
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
            std::mutex mutex;               ///< 保护 initializeCount，跨线程启停时串行化
            int        initializeCount = 0; ///< 当前生效的 WSAStartup 引用数
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
    } // namespace

    bool Socket::initialize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                             &state = winsockReferenceCount();
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
        auto                             &state = winsockReferenceCount();
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
            int type = 0;
            // 长度参数的类型两家不同（Winsock 是 int*，POSIX 是 socklen_t*）：一律用 socklen_t，
            // 它在 Windows 上就是 winsock2 给的 int 别名
            socklen_t valueLength = static_cast<socklen_t>(sizeof(type));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &valueLength) != 0)
            {
                return false;
            }
            header.socketType = static_cast<std::uint16_t>(type);

#ifdef SO_DOMAIN
            int family  = 0;
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

        /**
         * @brief 核对重建出来的套接字与移交头里写的是同一类
         * @details 头里的地址族与类型是交出方从**同一个套接字**问出来的，因此这里对不上只可能是
         *          消息被写坏、版本或平台不对，或通道上跑的压根不是移交消息——那三种情况下交出去
         *          的套接字都不能用：宁可报「不像本平台的移交消息」，也不把一枚认不出来的描述符交给
         *          调用方去 accept 或 recvfrom。
         * @param descriptor 本进程重建出的描述符
         * @param header 收到的移交头
         * @return true 类型与地址族都对得上
         */
        bool handoffHeaderMatchesSocket(const int descriptor, const HandoffHeader &header) noexcept
        {
            int       type       = 0;
            socklen_t typeLength = static_cast<socklen_t>(sizeof(type));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &typeLength) != 0 || static_cast<std::uint16_t>(type) != header.socketType)
            {
                return false;
            }

            sockaddr_storage localAddress{};
            socklen_t        localAddressLength = static_cast<socklen_t>(sizeof(localAddress));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&localAddress), &localAddressLength) != 0 ||
                static_cast<std::uint16_t>(localAddress.ss_family) != header.family)
            {
                return false;
            }
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
        const int         duplicationResult = ::WSADuplicateSocketW(static_cast<SOCKET>(listenDescriptor), static_cast<DWORD>(targetProcessId), &protocolInfo);
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
        header.blobByteCount                          = 0U;
        char   controlBuffer[CMSG_SPACE(sizeof(int))] = {};
        iovec  dataVector{reinterpret_cast<void *>(&header), sizeof(header)};
        msghdr message{};
        message.msg_iov        = &dataVector;
        message.msg_iovlen     = 1;
        message.msg_control    = controlBuffer;
        message.msg_controllen = sizeof(controlBuffer);

        cmsghdr *controlHeader    = CMSG_FIRSTHDR(&message);
        controlHeader->cmsg_level = SOL_SOCKET;
        controlHeader->cmsg_type  = SCM_RIGHTS;
        controlHeader->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(controlHeader), &listenDescriptor, sizeof(listenDescriptor));

        const ssize_t writtenByteCount = ::sendmsg(channelDescriptor, &message, 0);
        // 契约是「true 等于完整交出一条消息」：部分写（被信号打断那一类，socket 调用不因
        // SA_RESTART 自动重启）会把半条头留在流里，对端只能读成「不像移交消息」；交出方若记成
        // 成功就会白等一次接手。通道本来就是一次性的，整条判失败最诚实
        if (writtenByteCount != static_cast<ssize_t>(sizeof(header)))
        {
            PlatformError::setLastErrorCode(writtenByteCount < 0 ? PlatformError::lastSocketErrorCode() : EPIPE);
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
        // dwFlags 同样按协议信息里的来（FROM_PROTOCOL_INFO 时这个参数被忽略），所以交出方是重叠
        // 套接字时接手方也是——实测判据：Windows 的多进程编排里 worker 接手监听口后照常挂在完成
        // 端口上接受连接
        const SOCKET receivedSocket = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &protocolInfo, 0, 0);
        if (receivedSocket == INVALID_SOCKET)
        {
            PlatformError::setLastErrorCode(::WSAGetLastError());
            return -1;
        }
        // 重建出来的套接字必须与头里写的是同一类：对不上说明这条消息不可信（被写坏、版本或平台不对，
        // 或通道上跑的根本不是移交消息）。交一枚认不出来的句柄出去，比在这里报失败难查得多
        if (!handoffHeaderMatchesSocket(static_cast<int>(receivedSocket), header))
        {
            PlatformError::setLastErrorCode(EINVAL);
            static_cast<void>(::closesocket(receivedSocket));
            return -1;
        }
        // 接手来的句柄一律取消「随进程创建继承」：新一代再往下派生任何进程时，那枚监听句柄不该跟着
        // 过去——端口只要还有一个持有者就永远关不掉。交出方的标志位传不过来（两侧都是新建的一份引用），
        // 所以这一格必须由接手侧补
        static_cast<void>(FileDescriptor::markNonInheritable(static_cast<int>(receivedSocket)));
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

        // 先把描述符从控制消息里摘出来：recvmsg 一成功返回，内核就已经把随消息装填的那枚描述符放进
        // 本进程了，此后**每一条**失败出口都得关掉它——拒掉一条不可信的移交却留下一枚活描述符，
        // 比不拒更糟（换代是旧进程交给新进程，交完还要按补位次数重复许多轮）
        int receivedDescriptor = -1;
        for (const cmsghdr *controlHeader = CMSG_FIRSTHDR(&message); controlHeader != nullptr; controlHeader = CMSG_NXTHDR(&message, const_cast<cmsghdr *>(controlHeader)))
        {
            if (controlHeader->cmsg_level == SOL_SOCKET && controlHeader->cmsg_type == SCM_RIGHTS && controlHeader->cmsg_len >= CMSG_LEN(sizeof(int)))
            {
                std::memcpy(&receivedDescriptor, CMSG_DATA(controlHeader), sizeof(receivedDescriptor));
                break;
            }
        }

        if (static_cast<std::size_t>(receivedLength) < sizeof(header) || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 || header.blobByteCount != 0U)
        {
            // 头没收全、或控制消息被截断，都等于「这份移交不可信」：整体作废，
            // 不要拿半个描述符去 accept——那比失败更难查
            PlatformError::setLastErrorCode(EINVAL);
            if (receivedDescriptor >= 0)
            {
                static_cast<void>(FileDescriptor::close(receivedDescriptor));
            }
            return -1;
        }
        if (receivedDescriptor < 0)
        {
            PlatformError::setLastErrorCode(EBADF);
            return -1;
        }
        // 与 Windows 侧同一道核对：内核重装好的那枚描述符必须与头里写的类型、地址族一致，
        // 不一致就是这条消息不可信，关掉它而不是交给调用方
        if (!handoffHeaderMatchesSocket(receivedDescriptor, header))
        {
            PlatformError::setLastErrorCode(EINVAL);
            static_cast<void>(FileDescriptor::close(receivedDescriptor));
            return -1;
        }
        // 与 Windows 侧同一件事：内核给新描述符时把 FD_CLOEXEC 清掉了（实测 flags 为 0x0），不补就等于
        // 让接手方此后派生的任何子进程都替这个端口留一份持有
        static_cast<void>(FileDescriptor::markNonInheritable(receivedDescriptor));
        return receivedDescriptor;
#endif
    }

    namespace
    {
        /// 把最近一次套接字调用失败包成 expected 的失败值（两侧都按套接字错误码取）
        std::error_code lastSocketError() noexcept
        {
            return std::error_code(PlatformError::lastSocketErrorCode(), std::system_category());
        }
    } // namespace

    std::expected<Socket::HandoffChannelEndpoint, std::error_code> Socket::openHandoffChannel() noexcept
    {
        HandoffChannelEndpoint endpoint;
#if ASYN_PLATFORM_WIN32
        const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (descriptor < 0)
        {
            return std::unexpected(lastSocketError());
        }
        // 交接通道是一次性的，不留 TIME_WAIT 干扰下一次换代；继承位一律取消
        static_cast<void>(setReuseAddress(descriptor));
        static_cast<void>(FileDescriptor::markNonInheritable(descriptor));

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(descriptor, 1) != 0)
        {
            const std::error_code failure = lastSocketError();
            static_cast<void>(FileDescriptor::close(descriptor));
            return std::unexpected(failure);
        }
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
        if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            const std::error_code failure = lastSocketError();
            static_cast<void>(FileDescriptor::close(descriptor));
            return std::unexpected(failure);
        }
        endpoint.listener = descriptor;
        endpoint.address  = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        return endpoint;
#else
        // 同一进程可能开多条通道（换代演练与用例都会），光靠进程号不够，再加一个进程内序号
        static std::atomic<unsigned> sequence{0U};
        std::error_code              pathError;
        const std::filesystem::path  directory = std::filesystem::temp_directory_path(pathError);
        if (pathError)
        {
            return std::unexpected(pathError);
        }
        const std::filesystem::path path = directory / ("asyn-handoff-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                                                        std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)) + ".sock");
        const std::string           text = path.string();
        if (text.empty() || text.size() > sizeof(sockaddr_un::sun_path) - 1U)
        {
            // 路径长度超上限时 bind 会失败在别处，症状是「通道开不出来」而看不出为什么；这里当场说明
            return std::unexpected(std::make_error_code(std::errc::filename_too_long));
        }

        const int descriptor = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (descriptor < 0)
        {
            return std::unexpected(lastSocketError());
        }
        // umask 而不是 bind 之后 chmod：套接字文件一建出来就允许别人连，而它交出去的是监听套接字
        // 的一份引用——那个窗口里任何本机进程连上来都能拿走一份
        const mode_t previousMask = ::umask(S_IRWXG | S_IRWXO);
        sockaddr_un  address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, text.c_str(), sizeof(address.sun_path) - 1U);
        // 上一次运行被强杀时这个文件会留在原地，bind 于是以 EADDRINUSE 失败：先 unlink，
        // 「本来就没有」算成功，别的失败原因照原样交出去
        const bool isPathClear = ::unlink(text.c_str()) == 0 || errno == ENOENT;
        const bool isBound     = isPathClear && ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 && ::listen(descriptor, 1) == 0;
        ::umask(previousMask);
        if (!isBound)
        {
            const std::error_code failure = isPathClear ? lastSocketError() : std::error_code(errno, std::system_category());
            static_cast<void>(FileDescriptor::close(descriptor));
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return std::unexpected(failure);
        }
        endpoint.listener       = descriptor;
        endpoint.address        = text;
        endpoint.socketFilePath = text;
        return endpoint;
#endif
    }

    void Socket::closeHandoffChannel(HandoffChannelEndpoint &endpoint) noexcept
    {
        if (endpoint.listener >= 0)
        {
            static_cast<void>(FileDescriptor::close(endpoint.listener));
            endpoint.listener = -1;
        }
        if (!endpoint.socketFilePath.empty())
        {
            // 删除失败不报：文件不在就是已达目的，路径也不可恢复什么结论
            std::error_code ignored;
            std::filesystem::remove(endpoint.socketFilePath, ignored);
            endpoint.socketFilePath.clear();
        }
        endpoint.address.clear();
    }

    std::expected<bool, std::error_code> Socket::waitForAcceptReady(const int listenerDescriptor, const std::chrono::milliseconds budget) noexcept
    {
        if (listenerDescriptor < 0)
        {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        if (budget < std::chrono::milliseconds::zero())
        {
            // 负预算不是「零」也不是「无限」，它是调用方写错了：当成任何一种都会把一次参数错误
            // 变成一条看不出来的「没人来连」
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        // 按绝对期限自己扣剩余预算：POSIX 的 select 在被信号打断时带着「已经睡掉的那段」返回 EINTR，
        // 直接照原预算再等一次会把等待拉长，而一次都不重试又把「收到一个信号」读成「对端没来」——
        // 编排线程本身就挂着 SIGTERM/SIGINT 的处理函数，这两种错都会传下去
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (true)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining < std::chrono::milliseconds::zero())
            {
                // 预算用尽不是失败：这一条通路的答案本来就是「有没有人来连」。恰好等于 0 时不走这一支，
                // 因为 0 预算的用法就是「只取当前状态」——那需要真去 select 一次（零超时），而不是跳过
                return false;
            }

            fd_set readSet;
            FD_ZERO(&readSet);
#if ASYN_PLATFORM_WIN32
            FD_SET(static_cast<SOCKET>(listenerDescriptor), &readSet);
#else
            FD_SET(listenerDescriptor, &readSet);
#endif
            timeval timeout{};
            timeout.tv_sec  = static_cast<decltype(timeout.tv_sec)>(remaining.count() / 1000);
            timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remaining.count() % 1000 * 1000);

            // Windows 的第一个形参被忽略（它按 fd_set 里的句柄自己找），POSIX 要传「最大描述符 + 1」
#if ASYN_PLATFORM_WIN32
            const int readyCount = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int readyCount = ::select(listenerDescriptor + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (readyCount > 0)
            {
                return true;
            }
            if (readyCount == 0)
            {
                return false;
            }
            if (PlatformError::lastSocketErrorCode() != PlatformError::kInterrupted)
            {
                return std::unexpected(lastSocketError());
            }
        }
    }

    int Socket::acceptHandoffPeer(const int listenerDescriptor) noexcept
    {
        // 只等一个对端：这条通道交出去的是监听套接字的一份引用，交给谁必须确定，
        // 因此不排第二个连接（第二个连接者拿不到任何东西，白占一次 accept）
        return accept(listenerDescriptor, nullptr, nullptr);
    }

    int Socket::connectHandoffChannel(const std::string_view address) noexcept
    {
#if ASYN_PLATFORM_WIN32
        // 地址文本是 "127.0.0.1:端口"：端口分隔符只有一个冒号，按最后一个切即可
        const std::size_t portSeparator = address.rfind(':');
        if (portSeparator == std::string_view::npos || portSeparator + 1U >= address.size())
        {
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }
        std::uint32_t port        = 0U;
        const auto    parseResult = std::from_chars(address.data() + portSeparator + 1U, address.data() + address.size(), port);
        if (parseResult.ec != std::errc{} || parseResult.ptr != address.data() + address.size() || port == 0U || port > 65535U)
        {
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }
        const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (descriptor < 0)
        {
            return -1;
        }
        static_cast<void>(FileDescriptor::markNonInheritable(descriptor));
        sockaddr_in target{};
        target.sin_family      = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port        = htons(static_cast<std::uint16_t>(port));
        if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) != 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            static_cast<void>(FileDescriptor::close(descriptor));
            return -1;
        }
        return descriptor;
#else
        if (address.empty() || address.size() > sizeof(sockaddr_un::sun_path) - 1U)
        {
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }
        const int descriptor = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (descriptor < 0)
        {
            return -1;
        }
        sockaddr_un target{};
        target.sun_family = AF_UNIX;
        std::strncpy(target.sun_path, std::string(address).c_str(), sizeof(target.sun_path) - 1U);
        if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) != 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            static_cast<void>(FileDescriptor::close(descriptor));
            return -1;
        }
        return descriptor;
#endif
    }
} // namespace AsynGyanis::Platform
