#include "Core/Socket/InetAddress.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Platform/IO/NetworkInterface.h"
#include "Platform/IO/Socket.h"

#include <charconv>
#include <cstring>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 把 IPv6 文本里「%」之后的作用域标识折成数字作用域号
         * @details 两种写法都收（RFC 4007 §11）：纯数字按字面取值，其余当本机接口名去问索引——
         *          配置文件里写的多半是 eth0 这类人名，而内核的 sockaddr 只认数字，换算只能在这里做完。
         *          空标识、超出 uint32 的数字、查不到的接口名都算不可用。
         * @param zoneText 「%」之后的原文（不含分隔符本身）
         * @param scopeId 输出：数字作用域号
         * @return true 取到了可用的作用域号
         */
        bool resolveZoneIdentifier(const std::string_view zoneText, uint32_t &scopeId) noexcept
        {
            scopeId = 0;
            if (zoneText.empty())
            {
                return false;
            }

            bool isAllDigits = true;
            for (const char character: zoneText)
            {
                if (character < '0' || character > '9')
                {
                    isAllDigits = false;
                    break;
                }
            }
            if (isAllDigits)
            {
                // 全十进制就整段取值；超出 uint32 表示长度属于畸形文本，宁可判失败也不截断成另一个接口
                uint32_t   parsedValue = 0;
                const auto parseResult = std::from_chars(zoneText.data(), zoneText.data() + zoneText.size(), parsedValue);
                if (parseResult.ec != std::errc{} || parseResult.ptr != zoneText.data() + zoneText.size())
                {
                    return false;
                }
                scopeId = parsedValue;
                return true;
            }

            // 接口名写法：换成索引。返回 0 表示本机没有这个接口（或换算调用本身失败）
            const unsigned interfaceIndex = Platform::interfaceIndexOfName(zoneText);
            if (interfaceIndex == 0)
            {
                return false;
            }
            scopeId = static_cast<uint32_t>(interfaceIndex);
            return true;
        }
    } // namespace

    InetAddress::InetAddress()
    {
        // 直接填 sockaddr，不走文本解析那条路：parseStrictLiteral 要在自己内部造出未填好的对象，
        // 而默认构造若回头解析文本就成了无限递归
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = 0;
        address.sin_addr.s_addr = INADDR_ANY;
        std::memcpy(&m_address, &address, sizeof(address));
        m_addressLength = sizeof(address);
    }

    InetAddress::InetAddress(const uint16_t port, const std::string_view ip)
    {
        fromIpPort(ip, port);
    }

    InetAddress::InetAddress(const std::string_view ip, const uint16_t port)
    {
        fromIpPort(ip, port);
    }

    InetAddress::InetAddress(const sockaddr_in &address) : m_addressLength(sizeof(sockaddr_in))
    {
        std::memcpy(&m_address, &address, sizeof(address));
    }

    InetAddress::InetAddress(const sockaddr_in6 &address) : m_addressLength(sizeof(sockaddr_in6))
    {
        std::memcpy(&m_address, &address, sizeof(address));
    }

    InetAddress::InetAddress(const sockaddr_storage &address, const socklen_t length) : m_addressLength(length)
    {
        // 长度由调用方给出，直接按它 memcpy 会越界写（成员只有 sizeof(sockaddr_storage) 字节）：
        // 超出容量的取值当场拒绝，绝不静默截断或照抄
        if (length == 0 || static_cast<std::size_t>(length) > sizeof(sockaddr_storage))
        {
            throw Base::InvalidArgumentException("InetAddress: 地址长度 " + std::to_string(length) + " 非法（必须在 1.." + std::to_string(sizeof(sockaddr_storage)) +
                                                 " 之间）：请传入内核回填的 socklen_t 长度");
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

        // 字面量先由本层解析，不进 getaddrinfo：hints 里的 AI_ADDRCONFIG 会按「本机有没有配到该族的
        // 非回环地址」过滤结果，只有 ::1 可用的容器里连 "::1" 都会解析失败；而「%接口名」要不要接受、
        // 接口名查不到时算不算字面量，两平台的判定也不一致。调用方既然已经把地址写在脸上，就没有
        // 任何要问操作系统的余地
        if (const auto literal = parseLiteral(host, port); literal.has_value())
        {
            return literal;
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

    std::optional<InetAddress> InetAddress::parseLiteral(const std::string_view ipText, const uint16_t port) noexcept
    {
        std::string unusedReason;
        return parseStrictLiteral(ipText, port, unusedReason);
    }

    std::optional<InetAddress> InetAddress::parseStrictLiteral(const std::string_view ipText, const uint16_t port, std::string &failureReason) noexcept
    {
        failureReason.clear();

        // inet_pton 只接受零终止 C 字符串，因此必须先落一份 std::string 副本；
        // 但内嵌 NUL 会让它只解析到第一个 '\0' 为止、静默忽略后面的内容
        // （例如 "1.2.3.4\0evil" 会被当成 1.2.3.4 接受），所以先显式拦下这类输入
        const std::string text(ipText);
        if (text.find('\0') != std::string::npos)
        {
            failureReason = "文本含 NUL 字节：底层按零终止语义解析，内嵌 NUL 会让地址被静默截断成前半段；请在调用方清理掉 NUL";
            return std::nullopt;
        }

        // 作用域后缀：IPv6 的地址本体不含 '%'，所以第一个 '%' 就是分隔符，它后面的整段都当作用域标识
        const std::size_t      zoneSeparator = text.find('%');
        const bool             hasZone       = zoneSeparator != std::string::npos;
        const std::string      bodyText(hasZone ? std::string_view(text).substr(0, zoneSeparator) : std::string_view(text));
        const std::string_view zoneText = hasZone ? std::string_view(text).substr(zoneSeparator + 1) : std::string_view{};

        uint32_t scopeId = 0;
        if (hasZone && !resolveZoneIdentifier(zoneText, scopeId))
        {
            failureReason = "IPv6 作用域标识「" + std::string(zoneText) +
                            "」不可用：它既不是数字接口号，也不是本机存在的接口名（接口名可用 `ip -o link` 或 `ipconfig /all` 查，也可直接写数字接口号）";
            return std::nullopt;
        }

        // 先按 IPv4 试：点分十进制是配置里最常见的写法，命中即返回
        sockaddr_in addressV4{};
        addressV4.sin_family = AF_INET;
        addressV4.sin_port   = htons(port);

        if (inet_pton(AF_INET, bodyText.c_str(), &addressV4.sin_addr) == 1)
        {
            if (hasZone)
            {
                // 本体是合法 IPv4 却带着作用域后缀：作用域只对 IPv6 有意义（RFC 4007 §11），
                // 悄悄丢掉后缀就是把调用方写明的那一层信息抹掉，改判失败让它自己写对
                failureReason = "IPv4 地址不能带「%作用域」后缀：作用域号只对 IPv6 有意义（RFC 4007 §11），请去掉后缀或改用 IPv6 写法";
                return std::nullopt;
            }

            InetAddress parsed;
            std::memcpy(&parsed.m_address, &addressV4, sizeof(addressV4));
            parsed.m_addressLength = sizeof(addressV4);
            return parsed;
        }

        // 再按 IPv6 试：写法与 IPv4 完全不重叠，因此两种都试一遍不会误判
        sockaddr_in6 addressV6{};
        addressV6.sin6_family   = AF_INET6;
        addressV6.sin6_port     = htons(port);
        addressV6.sin6_scope_id = scopeId;

        if (inet_pton(AF_INET6, bodyText.c_str(), &addressV6.sin6_addr) == 1)
        {
            InetAddress parsed;
            std::memcpy(&parsed.m_address, &addressV6, sizeof(addressV6));
            parsed.m_addressLength = sizeof(addressV6);
            return parsed;
        }

        return std::nullopt;
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
            std::string text(buffer);
            // 作用域号要跟着文本走：两块网卡上的同名链路本地地址只在这一个字段上有区别，
            // 漏掉它就让「按地址做键」的消费方（来源限额、审计）把它们并成一个来源。
            // 输出用数字形式，它同样是本层接受的写法（parseLiteral），因此这份文本能原样读回
            if (sin6->sin6_scope_id != 0)
            {
                text += '%';
                text += std::to_string(sin6->sin6_scope_id);
            }
            return text;
        }

        return {};
    }

    uint32_t InetAddress::scopeId() const noexcept
    {
        if (m_address.ss_family != AF_INET6)
        {
            return 0;
        }
        return reinterpret_cast<const sockaddr_in6 *>(&m_address)->sin6_scope_id;
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

    Platform::SocketAddress InetAddress::platformAddress() const noexcept
    {
        Platform::SocketAddress converted;
        // 长度就是本类记着的那一份：构造时已判过它不超过 sockaddr_storage，因此这里不会越界写
        converted.length = m_addressLength;
        std::memcpy(&converted.storage, &m_address, static_cast<std::size_t>(m_addressLength));
        return converted;
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
        // 判据与 parseLiteral() 完全同一份实现：构造函数的拒绝面不得比静态入口更宽或更窄，
        // 否则同一段文本会「换个写法就解析成功」，按地址文本做的放行又被绕过一次
        std::string failureReason;
        if (const auto parsed = parseStrictLiteral(ip, port, failureReason); parsed.has_value())
        {
            *this = *parsed;
            return;
        }

        std::string message = "IP 地址格式非法：'" + std::string(ip) +
                              "'（仅接受 IPv4 点分十进制如 192.168.1.1，"
                              "或 IPv6 冒号十六进制如 ::1、fe80::1%3、fe80::1%eth0；主机名请改用 resolve()）";
        if (!failureReason.empty())
        {
            message += "。具体原因：" + failureReason;
        }
        throw Base::InvalidArgumentException(message);
    }

} // namespace AsynGyanis::Core
