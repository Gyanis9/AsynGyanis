#include "InetAddress.h"
#include "Base/Exception.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>

namespace Core
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

    InetAddress::InetAddress(const sockaddr_in &addr) :
        m_addrLen(sizeof(sockaddr_in))
    {
        std::memcpy(&m_addr, &addr, sizeof(addr));
    }

    InetAddress::InetAddress(const sockaddr_in6 &addr) :
        m_addrLen(sizeof(sockaddr_in6))
    {
        std::memcpy(&m_addr, &addr, sizeof(addr));
    }

    InetAddress::InetAddress(const sockaddr_storage &addr, const socklen_t len) :
        m_addrLen(len)
    {
        std::memcpy(&m_addr, &addr, len);
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
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_ADDRCONFIG;

        const std::string hostStr(host);
        const std::string portStr = std::to_string(port);

        addrinfo *result = nullptr;
        if (getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &result) != 0)
        {
            return std::nullopt;
        }

        std::optional<InetAddress> addr;
        if (result->ai_addr->sa_family == AF_INET)
        {
            addr = InetAddress(*reinterpret_cast<sockaddr_in *>(result->ai_addr));
        } else if (result->ai_addr->sa_family == AF_INET6)
        {
            addr = InetAddress(*reinterpret_cast<sockaddr_in6 *>(result->ai_addr));
        }

        freeaddrinfo(result);
        return addr;
    }

    int InetAddress::family() const noexcept
    {
        return m_addr.ss_family;
    }

    std::string InetAddress::ip() const
    {
        char buf[INET6_ADDRSTRLEN]{};

        if (m_addr.ss_family == AF_INET)
        {
            const auto sin = reinterpret_cast<const sockaddr_in *>(&m_addr);
            inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            return {buf};
        }

        if (m_addr.ss_family == AF_INET6)
        {
            const auto sin6 = reinterpret_cast<const sockaddr_in6 *>(&m_addr);
            inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
            return {buf};
        }

        return {};
    }

    uint16_t InetAddress::port() const
    {
        if (m_addr.ss_family == AF_INET)
        {
            const auto *sin = reinterpret_cast<const sockaddr_in *>(&m_addr);
            return ntohs(sin->sin_port);
        }

        if (m_addr.ss_family == AF_INET6)
        {
            const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(&m_addr);
            return ntohs(sin6->sin6_port);
        }

        return 0;
    }

    const sockaddr *InetAddress::addr() const noexcept
    {
        return reinterpret_cast<const sockaddr *>(&m_addr);
    }

    socklen_t InetAddress::addrLen() const noexcept
    {
        return m_addrLen;
    }

    std::string InetAddress::toString() const
    {
        std::string result;

        if (m_addr.ss_family == AF_INET6)
            result += '[';

        result += ip();

        if (m_addr.ss_family == AF_INET6)
            result += ']';

        result += ':';
        result += std::to_string(port());
        return result;
    }

    bool InetAddress::operator==(const InetAddress &other) const
    {
        if (m_addrLen != other.m_addrLen)
            return false;
        if (m_addr.ss_family != other.m_addr.ss_family)
            return false;
        return std::memcmp(&m_addr, &other.m_addr, m_addrLen) == 0;
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
            std::memcpy(&m_addr, &sin, sizeof(sin));
            m_addrLen = sizeof(sin);
            return;
        }

        // Try IPv6
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = htons(port);

        if (inet_pton(AF_INET6, ipStr.c_str(), &sin6.sin6_addr) == 1)
        {
            std::memcpy(&m_addr, &sin6, sizeof(sin6));
            m_addrLen = sizeof(sin6);
            return;
        }

        throw Base::Exception("InetAddress: invalid IP address '" + ipStr + "'");
    }

}
