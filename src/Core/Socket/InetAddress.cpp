#include "Core/Socket/InetAddress.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Platform/IO/Socket.h"

#include <cstring>

namespace AsynGyanis::Core
{
    InetAddress::InetAddress()
    {
        fromIpPort("0.0.0.0", 0);
    }

    InetAddress::InetAddress(const uint16_t port, const std::string_view ip)
    {
        fromIpPort(ip, port);
    }

    InetAddress::InetAddress(const std::string_view ip, const uint16_t port)
    {
        fromIpPort(ip, port);
    }

    InetAddress::InetAddress(const sockaddr_in &address) :
        m_addressLength(sizeof(sockaddr_in))
    {
        std::memcpy(&m_address, &address, sizeof(address));
    }

    InetAddress::InetAddress(const sockaddr_in6 &address) :
        m_addressLength(sizeof(sockaddr_in6))
    {
        std::memcpy(&m_address, &address, sizeof(address));
    }

    InetAddress::InetAddress(const sockaddr_storage &address, const socklen_t length) :
        m_addressLength(length)
    {
        // 长度由调用方给出，直接按它 memcpy 会越界写（成员只有 sizeof(sockaddr_storage) 字节）：
        // 超出容量的取值当场拒绝，绝不静默截断或照抄
        if (length == 0 || static_cast<std::size_t>(length) > sizeof(sockaddr_storage))
        {
            throw Base::InvalidArgumentException(
                    "InetAddress: 地址长度 " + std::to_string(length) + " 非法（必须在 1.." +
                    std::to_string(sizeof(sockaddr_storage)) + " 之间）：请传入内核回填的 socklen_t 长度");
        }
        std::memcpy(&m_address, &address, length);
    }

    InetAddress InetAddress::localhost(const uint16_t port)
    {
        return InetAddress(port, "127.0.0.1");
    }

    InetAddress InetAddress::any(const uint16_t port)
    {
        return InetAddress(port, "0.0.0.0");
    }

    std::optional<InetAddress> InetAddress::resolve(const std::string_view host, const uint16_t port)
    {
        // 空主机名在 Windows 上会被 getaddrinfo 当作通配地址成功返回，Linux 则报 EAI_NONAME；
        // 解析结果若用于 connect 会得到「0.0.0.0」这类看似合法实则无意义的对端，故统一拒绝。
        // 需要监听地址时请改用 InetAddress::any(port)
        if (host.empty())
        {
            return std::nullopt;
        }

        // Windows 上 getaddrinfo 依赖 Winsock 已初始化，此处自行申请一次初始化引用，
        // 不依赖调用方是否已经启动过 IoContext 等网络宿主
        const Platform::Socket::Initialization winsock;
        if (!winsock.isValid())
        {
            return std::nullopt;
        }

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_ADDRCONFIG;

        const std::string hostString(host);
        const std::string portString = std::to_string(port);

        addrinfo *result = nullptr;
        if (getaddrinfo(hostString.c_str(), portString.c_str(), &hints, &result) != 0)
        {
            return std::nullopt;
        }

        // 优先选择 IPv4（兼容性更好）
        std::optional<InetAddress> address;
        std::optional<InetAddress> ipv6Address;
        for (auto *rp = result; rp != nullptr; rp = rp->ai_next)
        {
            if (rp->ai_addr->sa_family == AF_INET)
            {
                address = InetAddress(*reinterpret_cast<sockaddr_in *>(rp->ai_addr));
                break;
            }
            if (rp->ai_addr->sa_family == AF_INET6 && !ipv6Address.has_value())
            {
                ipv6Address = InetAddress(*reinterpret_cast<sockaddr_in6 *>(rp->ai_addr));
            }
        }

        if (!address.has_value())
            address = std::move(ipv6Address);

        freeaddrinfo(result);
        return address;
    }

    int InetAddress::family() const noexcept
    {
        return m_address.ss_family;
    }

    std::string InetAddress::ip() const
    {
        char buffer[INET6_ADDRSTRLEN]{};

        if (m_address.ss_family == AF_INET)
        {
            const auto sin = reinterpret_cast<const sockaddr_in *>(&m_address);
            inet_ntop(AF_INET, &sin->sin_addr, buffer, sizeof(buffer));
            return {buffer};
        }

        if (m_address.ss_family == AF_INET6)
        {
            const auto sin6 = reinterpret_cast<const sockaddr_in6 *>(&m_address);
            inet_ntop(AF_INET6, &sin6->sin6_addr, buffer, sizeof(buffer));
            return {buffer};
        }

        return {};
    }

    uint16_t InetAddress::port() const
    {
        if (m_address.ss_family == AF_INET)
        {
            const auto *sin = reinterpret_cast<const sockaddr_in *>(&m_address);
            return ntohs(sin->sin_port);
        }

        if (m_address.ss_family == AF_INET6)
        {
            const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(&m_address);
            return ntohs(sin6->sin6_port);
        }

        return 0;
    }

    const sockaddr *InetAddress::nativeAddress() const noexcept
    {
        return reinterpret_cast<const sockaddr *>(&m_address);
    }

    socklen_t InetAddress::nativeAddressLength() const noexcept
    {
        return m_addressLength;
    }

    std::string InetAddress::toString() const
    {
        std::string result;

        if (m_address.ss_family == AF_INET6)
            result += '[';

        result += ip();

        if (m_address.ss_family == AF_INET6)
            result += ']';

        result += ':';
        result += std::to_string(port());
        return result;
    }

    bool InetAddress::operator==(const InetAddress &other) const
    {
        if (m_addressLength != other.m_addressLength)
            return false;
        if (m_address.ss_family != other.m_address.ss_family)
            return false;
        return std::memcmp(&m_address, &other.m_address, m_addressLength) == 0;
    }

    bool InetAddress::operator!=(const InetAddress &other) const
    {
        return !(*this == other);
    }

    void InetAddress::fromIpPort(const std::string_view ip, const uint16_t port)
    {
        // inet_pton 只接受零终止 C 字符串，因此必须先落一份 std::string 副本；
        // 但内嵌 NUL 会让它只解析到第一个 '\0' 为止、静默忽略后面的内容
        //（例如 "1.2.3.4\0evil" 会被当成 1.2.3.4 接受），所以先显式拦下这类输入
        const std::string ipText(ip);
        if (ipText.find('\0') != std::string::npos)
        {
            throw Base::InvalidArgumentException("IP 地址文本含 NUL 字节：" 
                                                 "底层 inet_pton 按零终止语义解析，内嵌 NUL 会让地址被静默截断成前半段。"
                                                 "请在调用方清理掉 NUL，或改用其它方式构造地址");
        }

        // 先按 IPv4 试：点分十进制是配置里最常见的写法，命中即返回
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port   = htons(port);

        if (inet_pton(AF_INET, ipText.c_str(), &sin.sin_addr) == 1)
        {
            std::memcpy(&m_address, &sin, sizeof(sin));
            m_addressLength = sizeof(sin);
            return;
        }

        // 再按 IPv6 试：写法与 IPv4 完全不重叠，因此两种都试一遍不会误判
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = htons(port);

        if (inet_pton(AF_INET6, ipText.c_str(), &sin6.sin6_addr) == 1)
        {
            std::memcpy(&m_address, &sin6, sizeof(sin6));
            m_addressLength = sizeof(sin6);
            return;
        }

        throw Base::InvalidArgumentException("IP 地址格式非法：'" + ipText +
                                             "'（仅接受 IPv4 点分十进制如 192.168.1.1，"
                                             "或 IPv6 冒号十六进制如 ::1；主机名请改用 resolve()）");
    }

}
