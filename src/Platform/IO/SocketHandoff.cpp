#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <cstdlib>
#include <sys/select.h>
#include <sys/un.h>
#endif

namespace AsynGyanis::Platform
{
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
                if (pieceLength == 0)
                {
                    // 成功返回却一个字节没写：这条通道已经不被读了。实测 Winsock 在调用成功时把
                    // last-error 清成 0，照抄槽位就是把「没有错误」当成失败原因交出去，运维据此判方向必错
                    PlatformError::setLastErrorCode(PlatformError::kConnectionReset);
                    return false;
                }
                if (pieceLength < 0)
                {
                    // 被信号打断是「这一段没发出去」，不是通道坏了：重发这一段即可
                    if (PlatformError::lastSocketErrorCode() == PlatformError::kInterrupted)
                    {
                        continue;
                    }
                    PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
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
         * @note 两条失败出口都显式写错误码：`recv` 返回 0（对端已关）是一次成功的调用，实测它会把
         *       last-error 清成 0，照抄槽位就等于把「没有错误」当成失败原因交出去
         */
        bool readAll(int descriptor, char *bytes, std::size_t length)
        {
            std::size_t read = 0;
            while (read < length)
            {
                const int pieceLength = ::recv(descriptor, bytes + read, static_cast<int>(length - read), 0);
                if (pieceLength == 0)
                {
                    PlatformError::setLastErrorCode(PlatformError::kConnectionReset);
                    return false;
                }
                if (pieceLength < 0)
                {
                    // 被信号打断时一个字节都没读走，重读这一段不会丢数据
                    if (PlatformError::lastSocketErrorCode() == PlatformError::kInterrupted)
                    {
                        continue;
                    }
                    PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
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
         * @details 头里的地址族与类型是交出方从同一个套接字问出来的，因此这里对不上只可能是
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

#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 从一次 recvmsg 的控制消息里摘出第一枚描述符，多带的当场关掉
         * @details recvmsg 一成功返回，内核就已经把随消息装填的描述符放进本进程了。这条通道一次只
         *          接手一个监听口：第一枚交给调用方，其余必须当场关闭，否则每轮漏一枚永不回收的
         *          描述符，症状要等长跑撞到上限才看得见。
         * @param message 已完成的 recvmsg 消息
         * @return int 第一枚描述符；消息里没有描述符时返回 -1
         */
        int extractReceivedDescriptor(::msghdr &message) noexcept
        {
            int receivedDescriptor = -1;
            for (const cmsghdr *controlHeader = CMSG_FIRSTHDR(&message); controlHeader != nullptr; controlHeader = CMSG_NXTHDR(&message, const_cast<cmsghdr *>(controlHeader)))
            {
                if (controlHeader->cmsg_level != SOL_SOCKET || controlHeader->cmsg_type != SCM_RIGHTS || controlHeader->cmsg_len < CMSG_LEN(sizeof(int)))
                {
                    continue;
                }
                const std::size_t descriptorCount = (controlHeader->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const auto       *descriptorBytes = static_cast<const char *>(static_cast<const void *>(CMSG_DATA(const_cast<cmsghdr *>(controlHeader))));
                for (std::size_t index = 0; index < descriptorCount; ++index)
                {
                    int descriptor = -1;
                    std::memcpy(&descriptor, descriptorBytes + index * sizeof(int), sizeof(descriptor));
                    if (receivedDescriptor < 0)
                    {
                        receivedDescriptor = descriptor;
                        continue;
                    }
                    static_cast<void>(FileDescriptor::close(descriptor));
                }
            }
            return receivedDescriptor;
        }

        /**
         * @brief 把流式通道上分片到达的移交头补满
         * @details AF_UNIX 流套接字不保留消息边界：控制消息随第一段字节一起到达，头本身却可能只到
         *          这一段。补头途中通道断了，与「一个字节都没收到」同因：对端没交出完整消息。
         * @param channelDescriptor 通道描述符
         * @param header 已收到前 alreadyReadBytes 字节的头，就地补满
         * @param alreadyReadBytes 首次 recvmsg 已收进的字节数
         * @return true 头已补满；false 读失败或对端提前关闭（错误码已置上）
         * @note 失败时不关闭调用方已收下的描述符：那是调用方的清理责任
         */
        bool completeHandoffHeader(const int channelDescriptor, HandoffHeader &header, const std::size_t alreadyReadBytes) noexcept
        {
            std::size_t headerByteCount = alreadyReadBytes;
            while (headerByteCount < sizeof(header))
            {
                const ssize_t pieceLength = ::recv(channelDescriptor, reinterpret_cast<char *>(&header) + headerByteCount, sizeof(header) - headerByteCount, 0);
                if (pieceLength < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
                    return false;
                }
                if (pieceLength == 0)
                {
                    PlatformError::setLastErrorCode(PlatformError::kConnectionReset);
                    return false;
                }
                headerByteCount += static_cast<std::size_t>(pieceLength);
            }
            return true;
        }
#endif
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
        // Winsock 的复制接口按进程号认目标，不需要先把对方 OpenProcess 成句柄；
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

        ssize_t writtenByteCount = -1;
        do
        {
            writtenByteCount = ::sendmsg(channelDescriptor, &message, 0);
        } while (writtenByteCount < 0 && errno == EINTR);
        // 契约是「true 等于完整交出一条消息」：被信号打断的情形已由上面的重试排除，但流上
        // 仍可能发生短写，半条头留在流里，对端只能读成「不像移交消息」；交出方若记成成功
        // 就会白等一次接手。通道本来就是一次性的，整条判失败最诚实
        if (writtenByteCount != static_cast<ssize_t>(sizeof(header)))
        {
            PlatformError::setLastErrorCode(writtenByteCount < 0 ? PlatformError::lastSocketErrorCode() : EPIPE);
            return false;
        }
        return true;
#endif
    }

#if ASYN_PLATFORM_WIN32
    namespace
    {
        /**
         * @brief Windows 侧接手一条移交消息：读协议信息、重建套接字并核对头部
         * @param channelDescriptor 通道描述符
         * @return 接手到的套接字；失败为 -1，原因见 PlatformError
         */
        int receiveWindowsHandoff(const int channelDescriptor) noexcept
        {
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
        }
    } // namespace
#else
    namespace
    {
        /**
         * @brief POSIX 侧接手一条移交消息：收描述符、补齐头部并核对类型与地址族
         * @param channelDescriptor 通道描述符
         * @return 接手到的描述符；失败为 -1，原因见 PlatformError
         */
        int receivePosixHandoff(const int channelDescriptor) noexcept
        {
            HandoffHeader header{};
            char          controlBuffer[CMSG_SPACE(sizeof(int))] = {};
            iovec         dataVector{reinterpret_cast<void *>(&header), sizeof(header)};
            msghdr        message{};
            message.msg_iov        = &dataVector;
            message.msg_iovlen     = 1;
            message.msg_control    = controlBuffer;
            message.msg_controllen = sizeof(controlBuffer);

            ssize_t receivedLength = -1;
            do
            {
                // MSG_CMSG_CLOEXEC：内核把描述符装进本进程的那一刻就带 FD_CLOEXEC，
                // 不再留下「收下之后、fcntl 之前被别的线程 fork+exec 继承」的窗口
                receivedLength = ::recvmsg(channelDescriptor, &message, MSG_CMSG_CLOEXEC);
            } while (receivedLength < 0 && errno == EINTR);
            if (receivedLength < 0)
            {
                PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
                return -1;
            }
            if (receivedLength == 0)
            {
                // 0 是「调用成功、流里一个字节都没有」= 对端已经把这条通道关掉。下面的补头调用
                // 遇到同一个 0 也会退出，但这里直接报出来，不必再绕过一整段判定
                PlatformError::setLastErrorCode(PlatformError::kConnectionReset);
                return -1;
            }

            // 先把描述符从控制消息里摘出来，失败出口才能连它一起关
            const int receivedDescriptor = extractReceivedDescriptor(message);

            if (!completeHandoffHeader(channelDescriptor, header, static_cast<std::size_t>(receivedLength)))
            {
                if (receivedDescriptor >= 0)
                {
                    static_cast<void>(FileDescriptor::close(receivedDescriptor));
                }
                return -1;
            }

            if ((message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 || header.blobByteCount != 0U)
            {
                // 控制消息被截断、或载荷长度不像本平台的载体，都等于「这份移交不可信」：整体作废，
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
            // MSG_CMSG_CLOEXEC 已让内核在装填描述符时置上 FD_CLOEXEC；这里再标一次是为不支持该标志的老内核兜底，
            // 正常路径上是一次幂等的 fcntl
            static_cast<void>(FileDescriptor::markNonInheritable(receivedDescriptor));
            return receivedDescriptor;
        }
    } // namespace
#endif

    int Socket::readListeningSocketHandoff(const int channelDescriptor) noexcept
    {
        if (channelDescriptor < 0)
        {
            PlatformError::setLastErrorCode(EINVAL);
            return -1;
        }
#if ASYN_PLATFORM_WIN32
        return receiveWindowsHandoff(channelDescriptor);
#else
        return receivePosixHandoff(channelDescriptor);
#endif
    }

#if ASYN_PLATFORM_WIN32
    namespace
    {
        /**
         * @brief 开一条回环 TCP 交接通道（Windows 侧）
         * @return 已监听的回环端点；失败为错误码
         */
        std::expected<Socket::HandoffChannelEndpoint, std::error_code> openWindowsHandoffChannel() noexcept
        {
            Socket::HandoffChannelEndpoint endpoint;
            const int                      descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (descriptor < 0)
            {
                return std::unexpected(Socket::lastError());
            }
            // 交接通道是一次性的，不留 TIME_WAIT 干扰下一次换代；继承位一律取消
            static_cast<void>(Socket::setReuseAddress(descriptor));
            static_cast<void>(FileDescriptor::markNonInheritable(descriptor));

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            if (::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(descriptor, 1) != 0)
            {
                const std::error_code failure = Socket::lastError();
                static_cast<void>(FileDescriptor::close(descriptor));
                return std::unexpected(failure);
            }
            socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
            {
                const std::error_code failure = Socket::lastError();
                static_cast<void>(FileDescriptor::close(descriptor));
                return std::unexpected(failure);
            }
            endpoint.listener = descriptor;
            endpoint.address  = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
            return endpoint;
        }
    } // namespace
#else
    namespace
    {
        /**
         * @brief 开一条私有目录下的 AF_UNIX 交接通道（POSIX 侧）
         * @return 已监听的端点；失败为错误码
         */
        std::expected<Socket::HandoffChannelEndpoint, std::error_code> openPosixHandoffChannel() noexcept
        {
            Socket::HandoffChannelEndpoint endpoint;
            std::error_code                pathError;
            const std::filesystem::path    temporaryRoot = std::filesystem::temp_directory_path(pathError);
            if (pathError)
            {
                return std::unexpected(pathError);
            }

            // 套接字文件放在一个刚 `mkdtemp` 出来的私有目录里：目录建出来就是 0700，没有「先建出来再收紧」
            // 的窗口，因此不必再动进程级 umask——那会在同一时刻把别的线程创建的文件（日志、临时文件、
            // 票据密钥）权限一并收紧。别人既进不去这个目录，也就连不上这条通道，而它交出去的是监听套接字
            // 的一份引用
            const std::string templateText = (temporaryRoot / "asyn-handoff-XXXXXX").string();
            // mkdtemp 要一份可写的缓冲，而这份缓冲按路径长度分配——本函数是 noexcept 且已经带着
            // std::expected 这条失败通道，让它抛穿出去就是把「换代通道开不出来」变成 std::terminate。
            // 与 readFileContentsInto 同一处置：按分配不出来落成错误码，文案与分支仍由调用方定
            std::vector<char> templateBytes;
            try
            {
                templateBytes.assign(templateText.begin(), templateText.end());
                templateBytes.push_back('\0');
            } catch (const std::exception &)
            {
                return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
            }
            const char *createdDirectory = ::mkdtemp(templateBytes.data());
            if (createdDirectory == nullptr)
            {
                return std::unexpected(std::error_code(errno, std::system_category()));
            }

            const std::filesystem::path directory(createdDirectory);
            const std::filesystem::path path = directory / "listener.sock";
            const std::string           text = path.string();
            if (text.size() > sizeof(sockaddr_un::sun_path) - 1U)
            {
                // 路径长度超上限时 bind 会失败在别处，症状是「通道开不出来」而看不出为什么；这里当场说明
                std::error_code ignored;
                static_cast<void>(std::filesystem::remove(directory, ignored));
                return std::unexpected(std::make_error_code(std::errc::filename_too_long));
            }

            const int descriptor = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
            if (descriptor < 0)
            {
                const std::error_code failure = Socket::lastError();
                std::error_code       ignored;
                static_cast<void>(std::filesystem::remove(directory, ignored));
                return std::unexpected(failure);
            }
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::strncpy(address.sun_path, text.c_str(), sizeof(address.sun_path) - 1U);
            // 目录是新建的，不会撞上上一次运行留下的同名文件（原先那句「先 unlink 再 bind」随之作废）
            const bool isBound = ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 && ::listen(descriptor, 1) == 0;
            if (!isBound)
            {
                const std::error_code failure = Socket::lastError();
                static_cast<void>(FileDescriptor::close(descriptor));
                std::error_code ignored;
                static_cast<void>(std::filesystem::remove(directory, ignored));
                return std::unexpected(failure);
            }
            endpoint.listener       = descriptor;
            endpoint.address        = text;
            endpoint.socketFilePath = text;
            return endpoint;
        }
    } // namespace
#endif

    std::expected<Socket::HandoffChannelEndpoint, std::error_code> Socket::openHandoffChannel() noexcept
    {
#if ASYN_PLATFORM_WIN32
        return openWindowsHandoffChannel();
#else
        return openPosixHandoffChannel();
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
            // 套接字文件所在的私有目录一并删掉：那个目录是 openHandoffChannel 为这一条通道新建的，
            // 只删文件会把空目录留在临时目录里，一代堆积一个
            static_cast<void>(std::filesystem::remove(std::filesystem::path(endpoint.socketFilePath).parent_path(), ignored));
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
        const auto deadline   = std::chrono::steady_clock::now() + budget;
        bool       hasSampled = false;
        while (true)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            // 「预算用尽」的判定排在至少 poll 过一次之后：0 预算的用法就是「只取当前状态」，那需要
            // 真去 select 一次（零超时）。拿两次 now() 的差当依据时，只要这两句之间被抢占满一毫秒，
            // 截断出来的 remaining 就是负数，于是队列里明明已有一条连接等着 accept 却报「没人来连」
            if (hasSampled && remaining < std::chrono::milliseconds::zero())
            {
                // 预算用尽不是失败：这一条通路的答案本来就是「有没有人来连」
                return false;
            }
            if (remaining < std::chrono::milliseconds::zero())
            {
                remaining = std::chrono::milliseconds::zero();
            }
            hasSampled = true;

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
                return std::unexpected(Socket::lastError());
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
        // 目标是零初始化的，地址长度已在上面判过上限，memcpy 之后自然带结尾 NUL
        std::memcpy(target.sun_path, address.data(), address.size());
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
