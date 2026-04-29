/**
 * @file InetAddress.h
 * @brief IPv4/IPv6 网络地址封装，支持 DNS 解析
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_INETADDRESS_H
#define CORE_INETADDRESS_H

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Core
{
    class InetAddress
    {
    public:
        InetAddress();

        explicit InetAddress(uint16_t port, std::string_view ip = "0.0.0.0");
        InetAddress(std::string_view ip, uint16_t port);
        explicit InetAddress(const sockaddr_in &addr);
        explicit InetAddress(const sockaddr_in6 &addr);
        InetAddress(const sockaddr_storage &addr, socklen_t len);

        static InetAddress                localhost(uint16_t port);
        static InetAddress                any(uint16_t port);
        static std::optional<InetAddress> resolve(std::string_view host, uint16_t port);

        [[nodiscard]] int             family() const noexcept;
        [[nodiscard]] std::string     ip() const;
        [[nodiscard]] uint16_t        port() const;
        [[nodiscard]] const sockaddr *addr() const noexcept;
        [[nodiscard]] socklen_t       addrLen() const noexcept;
        [[nodiscard]] std::string     toString() const;

        bool operator==(const InetAddress &other) const;
        bool operator!=(const InetAddress &other) const;

    private:
        void fromIpPort(std::string_view ip, uint16_t port);

        sockaddr_storage m_addr{};
        socklen_t        m_addrLen{0};
    };

}

#endif
