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
    /**
     * @brief IPv4/IPv6 网络地址封装类
     *
     * 该类统一封装 sockaddr_in / sockaddr_in6 结构，提供便捷的地址构造、IP字符串转换、
     * 端口访问、DNS 解析等功能。内部使用 sockaddr_storage 存储，支持 IPv4 和 IPv6 协议族。
     */
    class InetAddress
    {
    public:
        /**
         * @brief 默认构造一个空地址（未初始化）
         */
        InetAddress();

        /**
         * @brief 使用端口和 IP 地址构造（IP 默认为 0.0.0.0）
         * @param port 端口号（主机字节序）
         * @param ip   IP 地址字符串（如 "192.168.1.1" 或 "::1"），默认为 "0.0.0.0"
         */
        explicit InetAddress(uint16_t port, std::string_view ip = "0.0.0.0");

        /**
         * @brief 使用 IP 地址和端口构造（参数顺序与上一个相反）
         * @param ip   IP 地址字符串
         * @param port 端口号（主机字节序）
         */
        InetAddress(std::string_view ip, uint16_t port);

        /**
         * @brief 从 IPv4 的 sockaddr_in 构造
         * @param addr IPv4 地址结构
         */
        explicit InetAddress(const sockaddr_in &addr);

        /**
         * @brief 从 IPv6 的 sockaddr_in6 构造
         * @param addr IPv6 地址结构
         */
        explicit InetAddress(const sockaddr_in6 &addr);

        /**
         * @brief 从通用 sockaddr_storage 及长度构造
         * @param addr 套接字地址存储结构
         * @param len  地址结构实际长度（必须与 addr 指定的协议族匹配）
         */
        InetAddress(const sockaddr_storage &addr, socklen_t len);

        /**
         * @brief 创建指向本地回环地址（127.0.0.1 或 ::1）的地址对象
         * @param port 端口号（主机字节序）
         * @return InetAddress 本地回环地址对象
         */
        static InetAddress localhost(uint16_t port);

        /**
         * @brief 创建任意地址（INADDR_ANY 或 in6addr_any）的地址对象
         * @param port 端口号（主机字节序）
         * @return InetAddress 任意地址对象
         */
        static InetAddress any(uint16_t port);

        /**
         * @brief 解析主机名（或 IP 字符串）和端口，返回可用的地址对象
         * @param host 主机名（如 "localhost"）或 IP 地址字符串
         * @param port 端口号（主机字节序）
         * @return 成功返回 InetAddress 对象，失败返回 std::nullopt
         */
        static std::optional<InetAddress> resolve(std::string_view host, uint16_t port);

        /**
         * @brief 获取地址族（AF_INET 或 AF_INET6）
         * @return 协议族常量
         */
        [[nodiscard]] int family() const noexcept;

        /**
         * @brief 获取 IP 地址的点分十进制（IPv4）或十六进制字符串（IPv6）
         * @return IP 地址字符串，若地址无效则返回空字符串
         */
        [[nodiscard]] std::string ip() const;

        /**
         * @brief 获取端口号（主机字节序）
         * @return 端口号
         */
        [[nodiscard]] uint16_t port() const;

        /**
         * @brief 获取指向底层 sockaddr 结构的指针，可用于系统调用
         * @return const sockaddr* 指针，指向内部存储的 sockaddr_in 或 sockaddr_in6
         */
        [[nodiscard]] const sockaddr *addr() const noexcept;

        /**
         * @brief 获取底层 sockaddr 结构的实际长度
         * @return 地址结构的字节长度，若地址未初始化则返回 0
         */
        [[nodiscard]] socklen_t addrLen() const noexcept;

        /**
         * @brief 将地址转换为可读字符串（"IP:Port" 格式）
         * @return 格式化的字符串，例如 "192.168.1.1:8080"
         */
        [[nodiscard]] std::string toString() const;

        /**
         * @brief 比较两个地址是否相等（比较 IP 和端口）
         * @param other 另一个地址对象
         * @return true 如果两个地址协议族、IP 和端口完全相同
         */
        bool operator==(const InetAddress &other) const;

        /**
         * @brief 比较两个地址是否不等
         * @param other 另一个地址对象
         * @return true 如果不相等
         */
        bool operator!=(const InetAddress &other) const;

    private:
        /**
         * @brief 从 IP 字符串和端口初始化内部地址（支持 IPv4/IPv6）
         * @param ip   IP 地址字符串
         * @param port 端口号（主机字节序）
         */
        void fromIpPort(std::string_view ip, uint16_t port);

        sockaddr_storage m_addr;    ///< 内部存储的地址结构，足够容纳 IPv4 或 IPv6
        socklen_t        m_addrLen; ///< 实际使用的地址结构长度
    };
}

#endif
