/**
 * @file InetAddress.h
 * @brief IPv4/IPv6 网络地址封装，支持 DNS 解析
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    /**
     * @brief IPv4/IPv6 网络地址封装类
     * @note 内部统一按 sockaddr_storage 存储，屏蔽 sockaddr_in / sockaddr_in6 的差异
     * @note IPv6 文本可带 RFC 4007 §11 的「%作用域」后缀（数字接口号或本机接口名），解析成
     *       sockaddr_in6::sin6_scope_id；链路本地地址（fe80::/10）不带作用域时无法确定是哪块网卡，
     *       绑定或连接会失败，这类地址要写成 `fe80::1%eth0` 的形状
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
         * @param ip   IP 地址字符串（如 "192.168.1.1"、"::1"、"fe80::1%3"、"fe80::1%eth0"），
         *             默认为 "0.0.0.0"
         * @throws Base::InvalidArgumentException IP 文本既不是合法的 IPv4 也不是合法的 IPv6，
         *         或其中含 NUL 字节，或 IPv6 带了「%作用域」后缀但作用域既不是数字也不是本机存在的
         *         接口名；**本构造函数不解析主机名**，需要主机名请改用 resolve()
         */
        explicit InetAddress(uint16_t port, std::string_view ip = "0.0.0.0");

        /**
         * @brief 使用 IP 地址和端口构造（参数顺序与上一个相反）
         * @param ip   IP 地址字符串，IPv6 可带「%作用域」后缀
         * @param port 端口号（主机字节序）
         * @throws Base::InvalidArgumentException 与上面那个构造函数同一套拒绝条件
         */
        InetAddress(std::string_view ip, uint16_t port);

        /**
         * @brief 从 IPv4 的 sockaddr_in 构造
         * @param address IPv4 地址结构
         */
        explicit InetAddress(const sockaddr_in &address);

        /**
         * @brief 从 IPv6 的 sockaddr_in6 构造
         * @param address IPv6 地址结构
         */
        explicit InetAddress(const sockaddr_in6 &address);

        /**
         * @brief 从通用 sockaddr_storage 及长度构造
         * @param address 套接字地址存储结构
         * @param length  地址结构实际长度（必须与 address 指定的协议族匹配）
         * @throws Base::InvalidArgumentException length 为 0 或超过 sockaddr_storage 的容量
         */
        InetAddress(const sockaddr_storage &address, socklen_t length);

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
         * @param host 主机名（如 "localhost"）或 IP 地址字符串（IPv6 可带「%作用域」后缀）
         * @param port 端口号（主机字节序）
         * @return 成功返回 InetAddress 对象，失败返回 std::nullopt
         * @note IP 字面量走 parseLiteral() 自己解析，不进 getaddrinfo：后者的 AI_ADDRCONFIG 会按
         *       「本机有没有配到该族的非回环地址」过滤结果，只有 ::1 可用的容器里连 "::1" 都解析不出
         *       地址；两平台对「%接口名 要不要接受」的判定也不一致，故本层自己按严格文法解析
         */
        static std::optional<InetAddress> resolve(std::string_view host, uint16_t port);

        /**
         * @brief 把 IP 字面量文本解析成地址；不是字面量时返回空，不查 DNS、不抛异常
         * @param ipText IP 文本：IPv4 点分十进制、IPv6 冒号十六进制（可带「%作用域」后缀）
         * @param port   端口号（主机字节序）
         * @return std::optional<InetAddress> 解析成功时的地址；文本不是严格字面量时为空
         * @details 严格文法：内嵌 NUL、IPv4 的宽松变体（前导零/八进制/尾随空格）一律返回空；
         *          IPv6 的「%作用域」接受数字接口号与本机接口名两种写法，后者由平台层换算成索引，
         *          换算不到即整体解析失败。方括号不在本层职责内（URL 里的主机由上层剥括号）。
         * @note 作用域号只保证「文本里写了什么就取什么」，不校验该接口号在本机存在——校验要枚举
         *       全部网卡，而绑定或连接本来就会以 EADDRNOTAVAIL/ENODEV 明确报出来，不在这里重复花钱
         */
        [[nodiscard]] static std::optional<InetAddress> parseLiteral(std::string_view ipText, uint16_t port) noexcept;

        /**
         * @brief 获取地址族（AF_INET 或 AF_INET6）
         * @return 协议族常量
         */
        [[nodiscard]] int family() const noexcept;

        /**
         * @brief 获取 IP 地址的点分十进制（IPv4）或十六进制字符串（IPv6）
         * @return IP 地址字符串，若地址无效则返回空字符串；IPv6 带非零作用域号时输出「%接口号」后缀
         *         （如 "fe80::1%3"），该文本能被 parseLiteral() 原样读回
         * @warning 双栈监听器上的 IPv4 对端会以 "::ffff:a.b.c.d" 形状返回。作用域号参与本类的相等
         *          比较（比的是原始字节），也出现在这份文本里，因此两块网卡上的同名链路本地地址
         *          无论按对象还是按文本都不会撞成同一个来源
         * @see parseLiteral scopeId
         */
        [[nodiscard]] std::string ip() const;

        /**
         * @brief 获取 IPv6 作用域号（RFC 4007 的 zone id，即 sockaddr_in6::sin6_scope_id）
         * @return 作用域号；IPv4 地址与未带作用域的 IPv6 地址都是 0
         * @note 内核在链路本地对端上会填这个字段，故按地址做键的消费方要把它算进区分度里
         */
        [[nodiscard]] uint32_t scopeId() const noexcept;

        /**
         * @brief 获取端口号（主机字节序）
         * @return 端口号
         */
        [[nodiscard]] uint16_t port() const;

        /**
         * @brief 获取指向底层 sockaddr 结构的指针，可用于系统调用
         * @return const sockaddr* 指针，指向内部存储的 sockaddr_in 或 sockaddr_in6
         */
        [[nodiscard]] const sockaddr *nativeAddress() const noexcept;

        /**
         * @brief 获取底层 sockaddr 结构的实际长度
         * @return 地址结构的字节长度，若地址未初始化则返回 0
         */
        [[nodiscard]] socklen_t nativeAddressLength() const noexcept;

        /**
         * @brief 取可直接交给平台层套接字收发的地址
         * @details 本类内部就是 `sockaddr_storage`，而平台层的收发入口收的是「存储 + 长度」成对的
         *          `Platform::SocketAddress`。这层换算只有本类知道长度该取多少，调用方各自 memcpy
         *          出来的版本一旦漏了长度就会把半个结构交给内核。
         * @return Platform::SocketAddress 地址与长度成对；未初始化的地址给出 length 为 0 的空地址
         */
        [[nodiscard]] Platform::SocketAddress platformAddress() const noexcept;

        /**
         * @brief 将地址转换为可读字符串（"IP:Port" 格式）
         * @return 格式化的字符串，例如 "192.168.1.1:8080"、"[fe80::1%3]:8080"
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
         * @brief 从 IP 字符串和端口初始化内部地址（支持 IPv4/IPv6 与 IPv6 的「%作用域」后缀）
         * @param ip   IP 地址字符串
         * @param port 端口号（主机字节序）
         * @throws Base::InvalidArgumentException 文本不是严格字面量，或 IPv6 的作用域写法不可用；
         *         文案按 parseStrictLiteral 给失败原因，与 parseLiteral() 同一套判据
         */
        void fromIpPort(std::string_view ip, uint16_t port);

        /**
         * @brief 严格字面量解析，顺带把失败原因写成一句可直接进异常文案的话
         * @param ipText IP 文本
         * @param port 端口号（主机字节序）
         * @param failureReason 输出：失败原因；三种判据下都会被填上一句中文说明，其余情况留空
         * @return std::optional<InetAddress> 解析成功时的地址，失败时为空
         * @see parseLiteral fromIpPort
         */
        [[nodiscard]] static std::optional<InetAddress> parseStrictLiteral(std::string_view ipText, uint16_t port, std::string &failureReason) noexcept;

        sockaddr_storage m_address;       ///< 内部存储的地址结构，足够容纳 IPv4 或 IPv6
        socklen_t        m_addressLength; ///< 实际使用的地址结构长度
    };
} // namespace AsynGyanis::Core
