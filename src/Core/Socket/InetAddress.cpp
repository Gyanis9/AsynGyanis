#include "Core/Socket/InetAddress.h"
#include "Base/Exception/SystemException.h"
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

        // 遍历 getaddrinfo 链表，优先选择 IPv4（兼容性更好）
        std::optional<InetAddress> address;
        std::optional<InetAddress> ipv6Address;
        for (auto *rp = result; rp != nullptr; rp = rp->ai_next)
        {
            if (rp->ai_addr->sa_family == AF_INET)
            {
                address = InetAddress(*reinterpret_cast<sockaddr_in *>(rp->ai_addr));
                break; // IPv4 优先
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
        // Try IPv4 first
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port   = htons(port);

        const std::string ipStr(ip);
        if (inet_pton(AF_INET, ipStr.c_str(), &sin.sin_addr) == 1)
        {
            std::memcpy(&m_address, &sin, sizeof(sin));
            m_addressLength = sizeof(sin);
            return;
        }

        // Try IPv6
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = htons(port);

        if (inet_pton(AF_INET6, ipStr.c_str(), &sin6.sin6_addr) == 1)
        {
            std::memcpy(&m_address, &sin6, sizeof(sin6));
            m_addressLength = sizeof(sin6);
            return;
        }

        throw Base::Exception("InetAddress: invalid IP address '" + ipStr + "'");
    }

}
