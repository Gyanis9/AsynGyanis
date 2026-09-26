// PROXY 协议读侧解析：v1 文本行与 v2 二进制块都只认「最前面那一条头」，多余字节留给调用方

#include "Net/Proxy/ProxyProtocol.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Platform/IO/Socket.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// v2 的 12 字节签名：连续四个 CR/LF、一个 NUL、CR LF、"QUIT"、换行
        constexpr std::string_view kV2Signature{"\r\n\r\n\0\r\nQUIT\n", 12};

        /// v1 的行前缀
        constexpr std::string_view kV1Prefix{"PROXY "};

        /// v2 头里「版本号与命令」那一字节所在的位置
        constexpr std::size_t kV2VersionCommandOffset = 12U;

        /// v2 头里「地址族与传输协议」那一字节所在的位置
        constexpr std::size_t kV2FamilyOffset = 13U;

        /// v2 头里地址块长度字段的起点（大端 16 位）
        constexpr std::size_t kV2LengthOffset = 14U;

        /// v2 定长段的字节数（签名 12 + 版本命令 1 + 地址族 1 + 长度 2）
        constexpr std::size_t kV2FixedPartBytes = 16U;

        /// v2 的地址族编码
        constexpr std::uint8_t kV2FamilyUnspec = 0U;
        constexpr std::uint8_t kV2FamilyIpv4   = 1U;
        constexpr std::uint8_t kV2FamilyIpv6   = 2U;
        constexpr std::uint8_t kV2FamilyUnix   = 3U;

        /**
         * @brief 读出一个十进制端口（v1 用），越界与非数字都算失败
         * @param text 数字文本
         * @param port 输出参数：写回端口
         * @return true 读出了一个合法的 0..65535
         */
        bool parsePortText(std::string_view text, std::uint16_t &port) noexcept
        {
            if (text.empty() || text.size() > 5U)
            {
                return false;
            }
            unsigned int      value  = 0U;
            const char *const end    = text.data() + text.size();
            const auto        result = std::from_chars(text.data(), end, value);
            if (result.ec != std::errc{} || result.ptr != end || value > 65535U)
            {
                return false;
            }
            port = static_cast<std::uint16_t>(value);
            return true;
        }

        /**
         * @brief 把 IP 文本与端口拼成一个地址；文本不合规范时返回空
         * @param text IP 文本（点分十进制或 IPv6 冒分十六进制，不含百分号作用域）
         * @param port 端口
         * @return std::optional<Core::InetAddress> 解析结果
         * @details 走 InetAddress 的文本构造函数：它自己会抛 `InvalidArgumentException`，这里就地
         *          接住翻成空值——报文格式问题不该把异常穿过接受循环
         */
        [[nodiscard]] std::optional<Core::InetAddress> makeAddress(std::string_view text, const std::uint16_t port) noexcept
        {
            try
            {
                return Core::InetAddress{text, port};
            } catch (const Base::InvalidArgumentException &)
            {
                return std::nullopt;
            } catch (const Base::Exception &)
            {
                return std::nullopt;
            }
        }

        /**
         * @brief 按空白切出 v1 的字段（连续空格算一个分隔符，行尾 CRLF 不计入）
         * @param line 去掉前缀后的正文
         * @param fields 输出容器：按顺序塞进字段
         * @return std::size_t 切出的字段数
         */
        std::size_t splitV1Fields(std::string_view line, std::string_view *fields, const std::size_t capacity) noexcept
        {
            std::size_t count  = 0U;
            std::size_t cursor = 0U;
            while (cursor < line.size())
            {
                if (line[cursor] == ' ')
                {
                    ++cursor;
                    continue;
                }
                const std::size_t start = cursor;
                while (cursor < line.size() && line[cursor] != ' ')
                {
                    ++cursor;
                }
                if (count < capacity)
                {
                    fields[count] = line.substr(start, cursor - start);
                }
                ++count;
            }
            return count;
        }

        /**
         * @brief 解析 v2 的地址块
         * @param block 定长段之后的正文（含 TLV）
         * @param family 地址族编码
         * @param endpoint 输出：交出的身份
         * @return true 地址块够用且解析成功（`hasAddresses` 由本函数置位）
         */
        bool parseV2AddressBlock(std::string_view block, const std::uint8_t family, ProxyEndpoint &endpoint) noexcept
        {
            endpoint.hasAddresses = false;
            if (family == kV2FamilyIpv4)
            {
                // 4 + 4 + 2 + 2：源地址、目的地址、源端口、目的端口
                if (block.size() < 12U)
                {
                    return false;
                }
                const auto *const bytes = reinterpret_cast<const std::uint8_t *>(block.data());
                char              sourceText[64]{};
                std::snprintf(sourceText, sizeof(sourceText), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
                char destinationText[64]{};
                std::snprintf(destinationText, sizeof(destinationText), "%u.%u.%u.%u", bytes[4], bytes[5], bytes[6], bytes[7]);
                const std::uint16_t sourcePort      = static_cast<std::uint16_t>((bytes[8] << 8) | bytes[9]);
                const std::uint16_t destinationPort = static_cast<std::uint16_t>((bytes[10] << 8) | bytes[11]);
                const auto          source          = makeAddress(sourceText, sourcePort);
                const auto          destination     = makeAddress(destinationText, destinationPort);
                if (!source.has_value() || !destination.has_value())
                {
                    return false;
                }
                endpoint.source       = *source;
                endpoint.destination  = *destination;
                endpoint.hasAddresses = true;
                return true;
            }
            if (family == kV2FamilyIpv6)
            {
                // 16 + 16 + 2 + 2
                if (block.size() < 36U)
                {
                    return false;
                }
                const auto *const bytes = reinterpret_cast<const std::uint8_t *>(block.data());
                // 逐段拼成冒分十六进制；不做零压缩，也不返回视图——文本就长在返回的字符串里
                auto render = [bytes](const std::size_t offset)
                {
                    char text[64]{};
                    std::snprintf(text, sizeof(text), "%x:%x:%x:%x:%x:%x:%x:%x", (bytes[offset] << 8) | bytes[offset + 1U], (bytes[offset + 2U] << 8) | bytes[offset + 3U],
                                  (bytes[offset + 4U] << 8) | bytes[offset + 5U], (bytes[offset + 6U] << 8) | bytes[offset + 7U], (bytes[offset + 8U] << 8) | bytes[offset + 9U],
                                  (bytes[offset + 10U] << 8) | bytes[offset + 11U], (bytes[offset + 12U] << 8) | bytes[offset + 13U],
                                  (bytes[offset + 14U] << 8) | bytes[offset + 15U]);
                    return std::string{text};
                };
                const std::string   sourceText      = render(0U);
                const std::string   destinationText = render(16U);
                const std::size_t   portOffset      = 32U;
                const std::uint16_t sourcePort      = static_cast<std::uint16_t>((bytes[portOffset] << 8) | bytes[portOffset + 1U]);
                const std::uint16_t destinationPort = static_cast<std::uint16_t>((bytes[portOffset + 2U] << 8) | bytes[portOffset + 3U]);
                const auto          source          = makeAddress(sourceText, sourcePort);
                const auto          destination     = makeAddress(destinationText, destinationPort);
                if (!source.has_value() || !destination.has_value())
                {
                    return false;
                }
                endpoint.source       = *source;
                endpoint.destination  = *destination;
                endpoint.hasAddresses = true;
                return true;
            }
            // UNSPEC（v2 的 UNKNOWN）与 AF_UNIX 都没有可当客户端的 IP 身份：认出「前面有个代理」就够了，
            // 来源记账得回落到套接字自己的对端
            return family == kV2FamilyUnspec || family == kV2FamilyUnix;
        }

        /**
         * @brief 解析一条完整的 v1 文本行
         * @param line 去掉 `PROXY ` 前缀、也去掉结尾 CRLF 的正文
         * @param endpoint 输出：交出的身份
         * @return true 解析成功
         */
        bool parseV1Line(std::string_view line, ProxyEndpoint &endpoint) noexcept
        {
            constexpr std::size_t kFieldCapacity = 6U;
            std::string_view      fields[kFieldCapacity]{};
            const std::size_t     count = splitV1Fields(line, fields, kFieldCapacity);

            endpoint.hasAddresses = false;
            if (count == 1U && fields[0] == "UNKNOWN")
            {
                // 「PROXY UNKNOWN」：只声明有代理，不带地址
                return true;
            }
            if (count != 5U)
            {
                return false;
            }

            std::uint16_t sourcePort      = 0U;
            std::uint16_t destinationPort = 0U;
            if (!parsePortText(fields[3], sourcePort) || !parsePortText(fields[4], destinationPort))
            {
                return false;
            }
            const auto source      = makeAddress(fields[1], sourcePort);
            const auto destination = makeAddress(fields[2], destinationPort);
            if (!source.has_value() || !destination.has_value())
            {
                return false;
            }

            // 声明的地址族与实际文本必须对得上：TCP4 里塞一个 IPv6、或两族互换，都算报文不合规范。
            // v1 只定义 TCP4/TCP6/UNKNOWN 三种，别的形式（TCP、UDP）不在规范里，认了就等于给
            // 「什么样的字节算合法头」开一个规范之外的口子
            const bool sourceIsIpv6      = source->family() == AF_INET6;
            const bool destinationIsIpv6 = destination->family() == AF_INET6;
            if (fields[0] == "TCP4" && (sourceIsIpv6 || destinationIsIpv6))
            {
                return false;
            }
            if (fields[0] == "TCP6" && (!sourceIsIpv6 || !destinationIsIpv6))
            {
                return false;
            }
            if (fields[0] != "TCP4" && fields[0] != "TCP6")
            {
                return false;
            }

            endpoint.source       = *source;
            endpoint.destination  = *destination;
            endpoint.hasAddresses = true;
            return true;
        }
    } // namespace

    ProxyHeaderFraming frameProxyHeader(const std::string_view buffered) noexcept
    {
        ProxyHeaderFraming framing;
        if (buffered.empty())
        {
            return framing;
        }

        // 先问「已读到的这段还可能是什么」：两个版本的前缀都只对得上自己那一支，全对不上就不是头。
        // 这一步不能跳：v2 的签名以 CR 开头，读满 12 字节之前和「不是头」长得一模一样
        const std::size_t v2Compared = std::min(buffered.size(), kV2Signature.size());
        const bool        couldBeV2  = kV2Signature.substr(0U, v2Compared) == buffered.substr(0U, v2Compared);
        const std::size_t v1Compared = std::min(buffered.size(), kV1Prefix.size());
        const bool        couldBeV1  = kV1Prefix.substr(0U, v1Compared) == buffered.substr(0U, v1Compared);
        if (!couldBeV2 && !couldBeV1)
        {
            framing.isStillPlausible = false;
            return framing;
        }

        if (couldBeV2)
        {
            if (buffered.size() < kV2FixedPartBytes)
            {
                return framing; // 定长段还没读满，长度字段读不出来
            }
            const auto *const bytes    = reinterpret_cast<const std::uint8_t *>(buffered.data());
            const std::size_t declared = (bytes[kV2LengthOffset] << 8) | bytes[kV2LengthOffset + 1U];
            const std::size_t total    = kV2FixedPartBytes + declared;
            if (total > kMaximumProxyHeaderV2Bytes)
            {
                // 长度字段谎报到上界之外：这条头不可能是合法代理发出来的，判死比继续读便宜
                framing.isStillPlausible = false;
                return framing;
            }
            framing.totalLength = total;
            return framing;
        }

        const std::size_t lineEnd = buffered.find("\r\n");
        if (lineEnd == std::string_view::npos)
        {
            framing.isStillPlausible = buffered.size() <= kMaximumProxyHeaderV1Bytes;
            return framing;
        }
        const std::size_t total = lineEnd + 2U;
        if (total > kMaximumProxyHeaderV1Bytes)
        {
            framing.isStillPlausible = false;
            return framing;
        }
        framing.totalLength = total;
        return framing;
    }

    std::optional<ProxyEndpoint> parseProxyHeader(const std::string_view header, std::size_t &consumedBytes) noexcept
    {
        consumedBytes = 0U;
        ProxyEndpoint endpoint;

        if (header.size() >= kV2FixedPartBytes && header.substr(0U, kV2Signature.size()) == kV2Signature)
        {
            const auto *const  bytes    = reinterpret_cast<const std::uint8_t *>(header.data());
            const std::uint8_t version  = static_cast<std::uint8_t>(bytes[kV2VersionCommandOffset] >> 4U);
            const std::uint8_t command  = static_cast<std::uint8_t>(bytes[kV2VersionCommandOffset] & 0x0FU);
            const std::uint8_t family   = static_cast<std::uint8_t>(bytes[kV2FamilyOffset] >> 4U);
            const std::size_t  declared = (bytes[kV2LengthOffset] << 8) | bytes[kV2LengthOffset + 1U];
            if (version != 2U || declared + kV2FixedPartBytes > header.size())
            {
                return std::nullopt;
            }
            // LOCAL 与 UNKNOWN 命令按规范不带可信地址（0.0.0.0 那一类填充值），只算「有代理」的声明；
            // 只有 PROXY 命令的地址才拿来当客户端身份
            const std::string_view block = header.substr(kV2FixedPartBytes, declared);
            if (!parseV2AddressBlock(block, family, endpoint))
            {
                return std::nullopt;
            }
            if (command != 1U)
            {
                endpoint.hasAddresses = false;
            }
            consumedBytes = kV2FixedPartBytes + declared;
            return endpoint;
        }

        const std::size_t lineEnd = header.find("\r\n");
        if (lineEnd == std::string_view::npos || header.substr(0U, kV1Prefix.size()) != kV1Prefix)
        {
            return std::nullopt;
        }
        if (!parseV1Line(header.substr(kV1Prefix.size(), lineEnd - kV1Prefix.size()), endpoint))
        {
            return std::nullopt;
        }
        consumedBytes = lineEnd + 2U;
        return endpoint;
    }
} // namespace AsynGyanis::Net
