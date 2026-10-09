#include "Platform/IO/DatagramSocket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#if ASYN_PLATFORM_WIN32
#include <windows.h>
#include <ws2ipdef.h> // IP_DONTFRAG 与 IPV6_DONTFRAG 长在这里，windows.h 与 winsock2.h 都不带
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
#if !ASYN_PLATFORM_WIN32
        /// 控制报文里那份 ECN 存储的宽度：IPv4 是一字节、IPv6 是 int，按大的那份准备再对齐
        constexpr std::size_t kEcnControlStorageByteLength = CMSG_SPACE(sizeof(int));

        /**
         * @brief 一份对齐好的控制报文存储
         * @details 对齐要求写在**类型**上而不是写在 `std::array` 的模板实参上：后者会让 GCC 报
         *          `ignoring attributes on template argument`（`alignas` 不参与模板实参），而这道门
         *          是 `-Werror`。控制报文的宏（`CMSG_DATA` 等）要求缓冲区按 cmsghdr 对齐，
         *          对不齐在部分平台上是未定义行为，不是「看着能用」
         */
        struct alignas(::cmsghdr) EcnControlStorage
        {
            std::uint8_t bytes[kEcnControlStorageByteLength]; ///< 原始字节，交给 CMSG_* 宏解释
        };

        /**
         * @brief 从一次 recvmsg 的控制报文里读出这条报文的 ECN 字段
         * @details IPv4 认 `IP_TOS`（一字节，低 2 位就是 ECN），IPv6 认 `IPV6_TCLASS`（int，同样低 2 位）。
         *          两个都查而不是按地址族二选一：内核给哪一个由套接字上开过的选项决定，
         *          双栈与迁移场景里同一套接字可能先后见到两种。
         * @param message 已经收完的 msghdr（`CMSG_NXTHDR` 要非 const 的 msg，本函数不改它的内容）
         * @return std::uint8_t 读到的 ECN 取值；没有控制报文（本端没开这个选项）时是 `kEcnCodepointNotCapable`
         */
        std::uint8_t ecnCodepointFromControl(::msghdr &message) noexcept
        {
            for (::cmsghdr *control = CMSG_FIRSTHDR(&message); control != nullptr; control = CMSG_NXTHDR(&message, control))
            {
                if (control->cmsg_level == IPPROTO_IP && control->cmsg_type == IP_TOS && control->cmsg_len >= CMSG_LEN(sizeof(std::uint8_t)))
                {
                    return static_cast<std::uint8_t>(*reinterpret_cast<const std::uint8_t *>(CMSG_DATA(control)) & 0x03U);
                }
                if (control->cmsg_level == IPPROTO_IPV6 && control->cmsg_type == IPV6_TCLASS && control->cmsg_len >= CMSG_LEN(sizeof(int)))
                {
                    return static_cast<std::uint8_t>(*reinterpret_cast<const int *>(CMSG_DATA(control)) & 0x03U);
                }
            }
            return kEcnCodepointNotCapable;
        }

        /**
         * @brief 给一条待发报文准备「按报文设 ECN」的控制报文
         * @param addressFamily 目标地址族（AF_INET / AF_INET6），决定选项层级与取值宽度
         * @param ecnCodepoint 要标的 ECN 取值；`kEcnCodepointNotCapable` 表示这条不标
         * @param storage 调用方准备的对齐存储，至少 `kEcnControlStorageByteLength` 字节
         * @param storageByteLength storage 的字节数，仅用于自证尺寸够
         * @return std::size_t 要交给 `msg_controllen` 的长度；0 表示不带控制报文
         */
        std::size_t prepareEcnSendControl(int addressFamily, std::uint8_t ecnCodepoint, void *storage, std::size_t storageByteLength) noexcept
        {
            if (ecnCodepoint == kEcnCodepointNotCapable || storageByteLength < kEcnControlStorageByteLength)
            {
                return 0;
            }
            auto *control     = reinterpret_cast<::cmsghdr *>(storage);
            control->cmsg_len = CMSG_LEN(sizeof(std::uint8_t));
            if (addressFamily == AF_INET6)
            {
                // IPv6 这一侧 TCLASS 是 int 宽的，多出来的字节按 CMSG_LEN 计，内核只看低 2 位
                control->cmsg_len    = CMSG_LEN(sizeof(int));
                control->cmsg_level  = IPPROTO_IPV6;
                control->cmsg_type   = IPV6_TCLASS;
                const int classValue = static_cast<int>(ecnCodepoint);
                std::memcpy(CMSG_DATA(control), &classValue, sizeof(classValue));
                return CMSG_SPACE(sizeof(int));
            }
            control->cmsg_level                                   = IPPROTO_IP;
            control->cmsg_type                                    = IP_TOS;
            *reinterpret_cast<std::uint8_t *>(CMSG_DATA(control)) = ecnCodepoint;
            return CMSG_SPACE(sizeof(std::uint8_t));
        }

        /**
         * @brief 把批次收包的一个槽位填成一条待收的报文
         * @details 每槽一份独立的控制存储：共用一份会让后一条的 TOS 覆盖前一条，而一个端口上的
         *          多条连接恰恰要靠这些取值各自判拥塞。没开可见性时 `msg_control` 留空，内核于是
         *          完全不交控制报文，读回的取值恒为「非 ECN」——与 `receive()` 那一侧同形。
         * @param message 待填的那条报文
         * @param buffer 与报文配套的 iovec
         * @param control 本槽的控制报文存储
         * @param slot 调用方给的槽位；缓冲与容量已由它准备好，本函数回填读数
         * @param isEcnFieldVisible 本端是否已向内核申请读 ECN 字段
         */
        void prepareReceiveMessage(::mmsghdr &message, ::iovec &buffer, EcnControlStorage &control, DatagramSocket::BatchSlot &slot, const bool isEcnFieldVisible) noexcept
        {
            buffer.iov_base = slot.buffer;
            // 与 receive() 同一条钳制：把交给内核的长度收进单条报文上限里，大缓冲不多拿
            buffer.iov_len = slot.capacity > DatagramSocket::kMaximumDatagramBytes ? DatagramSocket::kMaximumDatagramBytes : slot.capacity;

            slot.peerAddress               = SocketAddress{};
            slot.peerAddress.length        = sizeof(slot.peerAddress.storage);
            slot.receivedByteCount         = 0;
            slot.ecnCodepoint              = kEcnCodepointNotCapable;
            message.msg_hdr.msg_name       = &slot.peerAddress.storage;
            message.msg_hdr.msg_namelen    = static_cast<socklen_t>(slot.peerAddress.length);
            message.msg_hdr.msg_iov        = &buffer;
            message.msg_hdr.msg_iovlen     = 1;
            message.msg_hdr.msg_control    = isEcnFieldVisible ? control.bytes : nullptr;
            message.msg_hdr.msg_controllen = isEcnFieldVisible ? sizeof(control.bytes) : 0;
            message.msg_hdr.msg_flags      = 0;
            message.msg_len                = 0;
        }
#endif

        /// 套接字地址是否已被设置（长度与地址族都得合法）
        bool isAddressSet(const SocketAddress &address) noexcept
        {
            return address.length > 0 && (address.storage.ss_family == AF_INET || address.storage.ss_family == AF_INET6);
        }

        /**
         * @brief 按地址族读一个本地地址上的端口号
         * @param address 内核回填的地址
         * @return std::uint16_t 端口；地址族认不出来时返回 0（调用方按「不合格」处理）
         */
        std::uint16_t portOf(const sockaddr_storage &address) noexcept
        {
            if (address.ss_family == AF_INET)
            {
                return ntohs(reinterpret_cast<const sockaddr_in *>(&address)->sin_port);
            }
            if (address.ss_family == AF_INET6)
            {
                return ntohs(reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_port);
            }
            return 0U;
        }
    } // namespace

    DatagramSocket::~DatagramSocket()
    {
        close();
    }

    DatagramSocket::DatagramSocket(DatagramSocket &&other) noexcept : m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1)), m_ecnFieldVisible(other.m_ecnFieldVisible)
    {
    }

    DatagramSocket &DatagramSocket::operator=(DatagramSocket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_fileDescriptor = std::exchange(other.m_fileDescriptor, -1);
            // 那个选项是挂在描述符上的，接手描述符就得连读数一起搬：只搬描述符会让新主
            // 明明收得到控制报文却报「本端没在读 ECN」，上层据此少报计数
            m_ecnFieldVisible = other.m_ecnFieldVisible;
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

    std::expected<DatagramSocket, std::error_code> DatagramSocket::adopt(const int descriptor) noexcept
    {
        if (!FileDescriptor::isValid(descriptor))
        {
            return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
        }

        // 类型必须是 SOCK_DGRAM。这一步拆成两档报：问不出类型说明这枚句柄根本不是一个套接字（普通文件、
        // 目录、管道都算），调用方要换的是传进来的东西；问得出但不是数据报则是交出方的问题（交了一枚流
        // 套接字过来）。两者合成一个 not_supported 的话，前一种会被当成「本平台不支持」而去换平台——
        // 而数据报的口径（一条报文自带来源、不会被内核切开续读）在流套接字上不成立，接过来才发现的
        // 症状是「收报文永远收不到东西」，比在这里点名难查得多
        int       type       = 0;
        socklen_t typeLength = static_cast<socklen_t>(sizeof(type));
        if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &typeLength) != 0)
        {
            const int platformCode = PlatformError::lastSocketErrorCode();
            if (platformCode == PlatformError::kNotASocket)
            {
                return std::unexpected(std::make_error_code(std::errc::not_a_socket));
            }
            return std::unexpected(std::error_code(platformCode, std::system_category()));
        }
        if (type != SOCK_DGRAM)
        {
            return std::unexpected(std::make_error_code(std::errc::not_supported));
        }

        // 没 bind 过的套接字没有本地端口，也就没有「谁往这个端口发报文」这回事：接手的意义不存在。
        // 端口为 0 就是未绑定——bind(port 0) 之后内核一定给出非 0 端口
        sockaddr_storage localAddress{};
        socklen_t        localAddressLength = static_cast<socklen_t>(sizeof(localAddress));
        if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&localAddress), &localAddressLength) != 0)
        {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        if (portOf(localAddress) == 0U)
        {
            // 地址族认不出来时 portOf 也返回 0，一并走这一支：本层的「数据报服务端」语义只覆盖 IP
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        // 交过来的套接字通常是阻塞态（Windows 按协议信息重建出来的就是阻塞的），而非阻塞是本层
        // 所有收发接口的前提
        if (!FileDescriptor::setNonBlocking(descriptor))
        {
            return std::unexpected(std::error_code(PlatformError::lastSocketErrorCode(), std::system_category()));
        }
        // 与 bindTo 同一条口径：别让这枚句柄随 spawn 漏给下一个进程
        static_cast<void>(FileDescriptor::markNonInheritable(descriptor));

        // 到这里才接管所有权：上面任何一步失败都原样把描述符留在调用方手里，不关也不接管
        DatagramSocket socket;
        socket.m_fileDescriptor = descriptor;
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

    bool DatagramSocket::supportsPerDatagramEcnField() noexcept
    {
        // 判据与上面 setEcnFieldVisible 的 Windows 分支是同一件事：Winsock 既不按报文交回 ECN 字段，
        // 也没有按报文设 TOS 的发送入口，所以读与写在这一格上一起没有
#if ASYN_PLATFORM_WIN32
        return false;
#else
        return true;
#endif
    }

    bool DatagramSocket::setEcnFieldVisible(const bool isVisible) noexcept
    {
        if (!isValid())
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }
#if ASYN_PLATFORM_WIN32
        // Winsock 没有「把收到数据报的 ECN 字段交上来」这一档选项（既没有 IP_RECVTOS 也没有
        // IPV6_RECVTCLASS），所以 Windows 侧永远读不到。错误码直接取平台自己那句「该协议层不支持
        // 这个选项」，调用方拿到的与真实 setsockopt 失败时是同一个值，不是本层编出来的
        static_cast<void>(isVisible);
        PlatformError::setLastErrorCode(WSAENOPROTOOPT);
        return false;
#else
        const SocketAddress boundAddress = localAddress();
        const int           enabled      = isVisible ? 1 : 0;
        if (boundAddress.storage.ss_family == AF_INET6)
        {
            if (::setsockopt(m_fileDescriptor, IPPROTO_IPV6, IPV6_RECVTCLASS, &enabled, sizeof(enabled)) != 0)
            {
                PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
                return false;
            }
        } else if (boundAddress.storage.ss_family == AF_INET)
        {
            if (::setsockopt(m_fileDescriptor, IPPROTO_IP, IP_RECVTOS, &enabled, sizeof(enabled)) != 0)
            {
                PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
                return false;
            }
        } else
        {
            // 还没 bind（或地址族不认识）：不知道该开哪一档选项，与其猜一个不如说清是用法错误
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }
        m_ecnFieldVisible = isVisible;
        return true;
#endif
    }

    bool DatagramSocket::isEcnFieldVisible() const noexcept
    {
        return m_ecnFieldVisible;
    }

    bool DatagramSocket::setDoNotFragment(const bool doNotFragment) noexcept
    {
        if (!isValid())
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }
        // 两侧选项名不同、取值也不同：Linux 那一档是「怎么发现 MTU」的枚举，Windows 才是布尔开关
#if ASYN_PLATFORM_WIN32
        const int optionValue = doNotFragment ? 1 : 0;
        const int ipv4Option  = IP_DONTFRAGMENT; // Winsock 的这个名字带 -MENT，Linux 那个 IP_DONTFRAG 是另一码事
        const int ipv6Option  = IPV6_DONTFRAG;
#else
        const int optionValue = doNotFragment ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        const int ipv4Option  = IP_MTU_DISCOVER;
        const int ipv6Option  = IPV6_MTU_DISCOVER;
#endif
        const SocketAddress boundAddress = localAddress();
        const int           option       = boundAddress.storage.ss_family == AF_INET6 ? ipv6Option : ipv4Option;
        if (boundAddress.storage.ss_family != AF_INET && boundAddress.storage.ss_family != AF_INET6)
        {
            // 还没 bind（或地址族不认识）：不知道该开哪一档选项，与其猜一个不如说清是用法错误
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }
        const int level = boundAddress.storage.ss_family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
        if (::setsockopt(m_fileDescriptor, level, option, reinterpret_cast<const char *>(&optionValue), sizeof(optionValue)) != 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return false;
        }
        m_isDoNotFragmentSet = doNotFragment;
        return true;
    }

    bool DatagramSocket::isDoNotFragmentSet() const noexcept
    {
        return m_isDoNotFragmentSet;
    }

    ssize_t DatagramSocket::receive(void *const buffer, const std::size_t capacity, SocketAddress &peerAddress, std::uint8_t &ecnCodepoint) const noexcept
    {
        // 输出参数先清空：失败或没数据时调用方不该读到上一次的值
        peerAddress  = SocketAddress{};
        ecnCodepoint = kEcnCodepointNotCapable;
        if (!isValid() || buffer == nullptr || capacity == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        // 缓冲区比单条报文上限还大也不会收到更多：把交给系统调用的长度钳进 int 范围，
        // 免得把大 size_t 静默窄化（UDP 语义下超长缓冲既没有额外好处，也没有额外代价）
        const std::size_t receiveCapacity = capacity > kMaximumDatagramBytes ? kMaximumDatagramBytes : capacity;
        peerAddress.length                = sizeof(peerAddress.storage);
#if !ASYN_PLATFORM_WIN32
        // 这一侧一律走 recvmsg：ECN 字段只随控制报文回来，recvfrom 拿不到它。没开
        // setEcnFieldVisible 时控制缓冲区长度留 0，内核因此不复制任何控制信息，
        // 与 recvfrom 的成本差只有一个 msg 结构体的准备
        ::msghdr          message{};
        ::iovec           data{};
        EcnControlStorage controlStorage{};
        data.iov_base                = buffer;
        data.iov_len                 = receiveCapacity;
        message.msg_name             = &peerAddress.storage;
        message.msg_namelen          = static_cast<socklen_t>(peerAddress.length);
        message.msg_iov              = &data;
        message.msg_iovlen           = 1;
        message.msg_control          = m_ecnFieldVisible ? controlStorage.bytes : nullptr;
        message.msg_controllen       = m_ecnFieldVisible ? sizeof(controlStorage.bytes) : 0;
        message.msg_flags            = 0;
        const ssize_t receivedLength = static_cast<ssize_t>(::recvmsg(m_fileDescriptor, &message, MSG_DONTWAIT));
#else
        // Windows 没有「按报文读 ECN 字段」的入口（RFC 9000 §13.4.1 正是允许这种平台不报计数），
        // 这一侧继续用 recvfrom，ecnCodepoint 保持开头置的「非 ECN」值
        const ssize_t receivedLength = static_cast<ssize_t>(::recvfrom(m_fileDescriptor, static_cast<char *>(buffer), static_cast<int>(receiveCapacity), 0,
                                                                       reinterpret_cast<sockaddr *>(&peerAddress.storage), &peerAddress.length));
#endif
        if (receivedLength < 0)
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
#if !ASYN_PLATFORM_WIN32
        if (m_ecnFieldVisible)
        {
            ecnCodepoint = ecnCodepointFromControl(message);
        }
        // 长度要从 `msg_namelen` 读回来：内核把实际地址长度写在那里，而不是写进我们传进去的那个变量
        // （Windows 那一侧的 `recvfrom` 是就地改 `peerAddress.length`，两条路因此形状不同）。漏掉这一步
        // 就把 `sizeof(sockaddr_storage)` 当「这条地址有多长」交出去——按长度搬运地址的上层会多读一截
        // 填充字节，同一条地址与它自己比也就不等了
        peerAddress.length = message.msg_namelen;
#endif
        return receivedLength;
    }

    ssize_t DatagramSocket::receive(void *const buffer, const std::size_t capacity, SocketAddress &peerAddress) const noexcept
    {
        std::uint8_t ignoredEcnCodepoint = kEcnCodepointNotCapable;
        return receive(buffer, capacity, peerAddress, ignoredEcnCodepoint);
    }

    ssize_t DatagramSocket::receiveBatch(BatchSlot *const slots, const std::size_t slotCount) const noexcept
    {
        if (slots == nullptr || slotCount == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        const std::size_t wantedCount = slotCount > kMaximumBatchSlotCount ? kMaximumBatchSlotCount : slotCount;

#if !ASYN_PLATFORM_WIN32
        // 一次 recvmmsg 把已排好的多条报文收完：一条 QUIC 连接上的几个包由此付一次系统调用，
        // 而不是每包一次 recvfrom 加一次就绪等待。MSG_DONTWAIT 显式带上，让「0 条」这条出口
        // 不依赖文件状态位有没有置上（接手来的描述符两侧形状不一）
        ::mmsghdr messages[kMaximumBatchSlotCount]{};
        ::iovec   buffers[kMaximumBatchSlotCount]{};
        // 每槽一份控制报文存储：开了 ECN 可见性时内核把 TOS/TCLASS 写在这里
        std::array<EcnControlStorage, kMaximumBatchSlotCount> controlStorage{};
        for (std::size_t index = 0; index < wantedCount; ++index)
        {
            BatchSlot &slot = slots[index];
            if (slot.buffer == nullptr || slot.capacity == 0)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            prepareReceiveMessage(messages[index], buffers[index], controlStorage[index], slot, m_ecnFieldVisible);
        }

        const int receivedCount = ::recvmmsg(m_fileDescriptor, messages, static_cast<unsigned int>(wantedCount), MSG_DONTWAIT, nullptr);
        if (receivedCount < 0)
        {
            const int errorCode = PlatformError::lastSocketErrorCode();
            if (errorCode == PlatformError::kWouldBlock || errorCode == PlatformError::kInterrupted)
            {
                // 此刻没有可读的报文不算错误：交 0 条，调用方等下一次可读再来
                return 0;
            }
            PlatformError::setLastErrorCode(errorCode);
            for (std::size_t index = 0; index < wantedCount; ++index)
            {
                slots[index].peerAddress = SocketAddress{};
            }
            return -1;
        }
        for (int index = 0; index < receivedCount; ++index)
        {
            // msg_len 已经是内核交付的字节数：缓冲比报文小时按容量截断，与 receive() 的
            // 「多出的字节被丢弃、返回值即容量」同口径，这里不去做 MSG_TRUNC 的纠正
            slots[static_cast<std::size_t>(index)].receivedByteCount = messages[index].msg_len;
            // 来源地址的实际长度同样要读回来（理由见 receive() 里那一格）：内核写的是 `msg_namelen`，
            // 不是我们递进去的那个 sizeof(sockaddr_storage)
            slots[static_cast<std::size_t>(index)].peerAddress.length = messages[index].msg_hdr.msg_namelen;
            if (m_ecnFieldVisible)
            {
                slots[static_cast<std::size_t>(index)].ecnCodepoint = ecnCodepointFromControl(messages[index].msg_hdr);
            }
        }
        return receivedCount;
#else
        // Windows 没有批量入口（WSARecvMsg 一次仍是一条），这里退化为逐条收：接口与语义同形，
        // 但每槽仍付一次系统调用。自述里不把「一次调用」写给两侧共同使用，就是这个原因
        std::size_t collectedCount = 0;
        for (std::size_t index = 0; index < wantedCount; ++index)
        {
            BatchSlot &slot = slots[index];
            if (slot.buffer == nullptr || slot.capacity == 0)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            const ssize_t receivedByteCount = receive(slot.buffer, slot.capacity, slot.peerAddress);
            if (receivedByteCount < 0)
            {
                const int errorCode = PlatformError::lastSocketErrorCode();
                if (errorCode == PlatformError::kWouldBlock)
                {
                    // 已经收到的条数照交：调用方按返回条数遍历，剩下的槽位没被写过
                    return static_cast<ssize_t>(collectedCount);
                }
                PlatformError::setLastErrorCode(errorCode);
                return -1;
            }
            slot.receivedByteCount = static_cast<std::size_t>(receivedByteCount);
            // 这一侧读不到 ECN 字段（见 receive() 的 Windows 分支），槽位要清成「非 ECN」而不是
            // 沿用上一次的值——调用方的槽位数组是复用的，不清就成了拿旧读数当新结论
            slot.ecnCodepoint = kEcnCodepointNotCapable;
            ++collectedCount;
        }
        return static_cast<ssize_t>(collectedCount);
#endif
    }

    ssize_t DatagramSocket::send(const SocketAddress &peerAddress, const void *const buffer, const std::size_t length, const std::uint8_t ecnCodepoint) const noexcept
    {
        // 零长数据报是合法的（RFC 768：最小报文就是 8 字节头、零负载），接收侧一直照收，
        // 这里也照发——用零长报文做保活探测是常见做法，拒掉它会让两种方向的行为不对称。
        // 因此只拒「说有字节却没给缓冲区」这一非法形状：`nullptr` + 0 长度就是空报文的自然写法
        if (!isValid() || (buffer == nullptr && length > 0))
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
        if (ecnCodepoint == kEcnCodepointCe)
        {
            // 与 sendBatch 同一条判据：CE 是网络中间节点的标记，端点不许发出去（RFC 9000 §13.4.1）
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }
#if !ASYN_PLATFORM_WIN32
        if (ecnCodepoint != kEcnCodepointNotCapable)
        {
            // 标了 ECN 的那条必须走 sendmsg：TOS/TCLASS 只在控制报文里给。没标的走下面那条
            // sendto 原路，非 ECN 的使用者（HTTP 之外的 UDP 例子）成本一字未增
            EcnControlStorage controlStorage{};
            const std::size_t controlByteLength = prepareEcnSendControl(peerAddress.storage.ss_family, ecnCodepoint, controlStorage.bytes, sizeof(controlStorage.bytes));
            // 与下面 sendto 那条同口径：长度为 0 时给一个永远读不到的有效指针，别把 nullptr 交给内核
            const void *const markedPayloadPointer = buffer != nullptr ? buffer : static_cast<const void *>("");
            ::iovec           data{const_cast<void *>(markedPayloadPointer), length};
            ::msghdr          message{};
            message.msg_name                  = const_cast<sockaddr *>(reinterpret_cast<const sockaddr *>(&peerAddress.storage));
            message.msg_namelen               = static_cast<socklen_t>(peerAddress.length);
            message.msg_iov                   = &data;
            message.msg_iovlen                = 1;
            message.msg_control               = controlByteLength > 0 ? controlStorage.bytes : nullptr;
            message.msg_controllen            = static_cast<socklen_t>(controlByteLength);
            const ssize_t markedSentByteCount = static_cast<ssize_t>(::sendmsg(m_fileDescriptor, &message, MSG_DONTWAIT));
            if (markedSentByteCount < 0)
            {
                PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
                return -1;
            }
            return markedSentByteCount;
        }
#endif
        // 零长 + NULL 这一组合交给提供者怎么处理，两侧并不一样（有的直接给 WSAEFAULT），而这一层
        // 不想去赌它：长度为 0 时给一个永远读不到的有效指针，让「空报文」在两端都是同一次调用
        const char *const payloadPointer = buffer != nullptr ? static_cast<const char *>(buffer) : "";
        const ssize_t     sentByteCount  = static_cast<ssize_t>(
                ::sendto(m_fileDescriptor, payloadPointer, static_cast<int>(length), 0, reinterpret_cast<const sockaddr *>(&peerAddress.storage), peerAddress.length));
        if (sentByteCount < 0)
        {
            PlatformError::setLastErrorCode(PlatformError::lastSocketErrorCode());
            return -1;
        }
        return sentByteCount;
    }

    ssize_t DatagramSocket::sendBatch(const BatchSendItem *const items, const std::size_t itemCount) const noexcept
    {
        if (items == nullptr || itemCount == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        const std::size_t wantedCount = itemCount > kMaximumBatchSlotCount ? kMaximumBatchSlotCount : itemCount;

        // 整批的用法错误在动手之前判完：发一半才发现第 5 条不合法，交回的是「发了 4 条」这种
        // 谁也说不清的中间态，而调用方没法把那条非法的挑出来重发
        for (std::size_t index = 0; index < wantedCount; ++index)
        {
            const BatchSendItem &item = items[index];
            if (item.buffer == nullptr && item.length > 0)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            if (item.length > kMaximumDatagramBytes)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            if (!isAddressSet(item.peerAddress))
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            if (item.ecnCodepoint == kEcnCodepointCe)
            {
                // ECN-CE 只有网络中间节点能置，端点发出去的是 ECT(*)：整批里出现「要我发 CE」的请求
                // 说明上层把读到的标记和要标的标记混了，当场拒掉而不是发出一条规范禁止的报文
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
        }

#if !ASYN_PLATFORM_WIN32
        std::array<::mmsghdr, kMaximumBatchSlotCount>         messages{};
        std::array<::iovec, kMaximumBatchSlotCount>           buffers{};
        std::array<EcnControlStorage, kMaximumBatchSlotCount> controlStorage{};
        for (std::size_t index = 0; index < wantedCount; ++index)
        {
            const BatchSendItem &item = items[index];
            // 内核只读这份内存，iov_base 与 msg_name 的类型是历史遗留的非 const——按 const_cast 交出，
            // 不改写一个字节（与 recvmsg 那侧处理 msghdr 的手法同一条）
            buffers[index].iov_base             = const_cast<void *>(item.buffer);
            buffers[index].iov_len              = item.length;
            messages[index].msg_hdr.msg_name    = const_cast<sockaddr *>(reinterpret_cast<const sockaddr *>(&item.peerAddress.storage));
            messages[index].msg_hdr.msg_namelen = static_cast<socklen_t>(item.peerAddress.length);
            messages[index].msg_hdr.msg_iov     = &buffers[index];
            messages[index].msg_hdr.msg_iovlen  = 1;
            // 按条目决定带不带控制报文：一批里可以混着标与不标（不同连接的 ECN 状态各自独立）
            const std::size_t controlByteLength =
                    prepareEcnSendControl(item.peerAddress.storage.ss_family, item.ecnCodepoint, controlStorage[index].bytes, sizeof(controlStorage[index].bytes));
            messages[index].msg_hdr.msg_control    = controlByteLength > 0 ? controlStorage[index].bytes : nullptr;
            messages[index].msg_hdr.msg_controllen = static_cast<socklen_t>(controlByteLength);
            messages[index].msg_hdr.msg_flags      = 0;
            messages[index].msg_len                = 0;
        }

        const int sentCount = ::sendmmsg(m_fileDescriptor, messages.data(), static_cast<unsigned int>(wantedCount), MSG_DONTWAIT);
        if (sentCount < 0)
        {
            const int errorCode = PlatformError::lastSocketErrorCode();
            PlatformError::setLastErrorCode(errorCode);
            // 发送缓冲此刻放不下：交出「已发出的条数」（这里是 0），剩下的归调用方续发
            if (errorCode == PlatformError::kWouldBlock)
            {
                return 0;
            }
            return -1;
        }
        return sentCount;
#else
        // Windows 没有批量入口：逐条 send()，但返回形状与 Linux 一致（已交出的条数 + 是否卡在可写上）。
        // 条目里的 ecnCodepoint 在这一侧会被 send() 静默忽略（平台没有按报文设 TOS 的入口），
        // 上层因此先问 supportsPerDatagramEcnField() 再决定要不要请求标记，而不是指望这里报错
        for (std::size_t index = 0; index < wantedCount; ++index)
        {
            const BatchSendItem &item = items[index];
            const ssize_t        sent = send(item.peerAddress, item.buffer, item.length, item.ecnCodepoint);
            if (sent >= 0)
            {
                continue;
            }
            const int errorCode = PlatformError::lastSocketErrorCode();
            PlatformError::setLastErrorCode(errorCode);
            if (errorCode == PlatformError::kWouldBlock)
            {
                return static_cast<ssize_t>(index);
            }
            return -1;
        }
        return static_cast<ssize_t>(wantedCount);
#endif
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
