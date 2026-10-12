#include "Net/Http/Client/HttpProxyPolicy.h"

#include "Base/Coding/Base64.h"
#include "Platform/System/ProcessInfo.h"

#include <cstddef>

namespace AsynGyanis::Net
{
    namespace
    {
        bool isAsciiDigit(const char character) noexcept
        {
            return character >= '0' && character <= '9';
        }

        char toAsciiLower(const char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        }

        bool equalsIgnoreCase(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (toAsciiLower(left[index]) != toAsciiLower(right[index]))
                {
                    return false;
                }
            }
            return true;
        }

        bool endsWithIgnoreCase(const std::string_view text, const std::string_view suffix) noexcept
        {
            if (suffix.size() > text.size())
            {
                return false;
            }
            return equalsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
        }

        std::string_view trimmed(const std::string_view text) noexcept
        {
            const std::size_t first = text.find_first_not_of(" \t");
            if (first == std::string_view::npos)
            {
                return {};
            }
            const std::size_t last = text.find_last_not_of(" \t");
            return text.substr(first, last - first + 1);
        }

        int hexValue(const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            return -1;
        }

        /// 解码 %XX；出现截断或非十六进制的序列返回空（非法转义不静默按原文用）
        std::optional<std::string> percentDecode(const std::string_view text)
        {
            std::string decoded;
            decoded.reserve(text.size());
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                if (text[index] != '%')
                {
                    decoded.push_back(text[index]);
                    continue;
                }
                if (index + 2U >= text.size())
                {
                    return std::nullopt;
                }
                const int high = hexValue(text[index + 1U]);
                const int low  = hexValue(text[index + 2U]);
                if (high < 0 || low < 0)
                {
                    return std::nullopt;
                }
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2U;
            }
            return decoded;
        }

        /// 读一对同名环境变量（小写优先，空串按未设置）；两条通道与 ProcessInfo 的读写口径一致
        std::optional<std::string> readProxyEnvironmentValue(const char *const lowerName, const char *const upperName)
        {
            if (const std::optional<std::string> lower = Platform::ProcessInfo::environmentVariable(lowerName); lower.has_value() && !lower->empty())
            {
                return lower;
            }
            if (const std::optional<std::string> upper = Platform::ProcessInfo::environmentVariable(upperName); upper.has_value() && !upper->empty())
            {
                return upper;
            }
            return std::nullopt;
        }

        /// 拆 `host[:port]`（host 允许方括号包 IPv6）；端口缺省按 scheme 定（http 80、https 443），非法时写下点名原因
        bool splitHostPort(const std::string_view authority, const std::uint16_t defaultPort, std::string &host, std::uint16_t &port, std::string &failureReason)
        {
            std::string_view hostPart = authority;
            std::string_view portPart;
            bool             hasExplicitPort = false;

            if (!authority.empty() && authority.front() == '[')
            {
                const std::size_t closingBracket = authority.find(']');
                if (closingBracket == std::string_view::npos)
                {
                    // 写法是「以 [ 开头却没闭」：报缺右括号比把整串当主机名去解析地址清楚
                    failureReason = "IPv6 地址缺右方括号（写法是 [地址]:端口）";
                    return false;
                }
                hostPart                    = authority.substr(1U, closingBracket - 1U);
                const std::string_view rest = authority.substr(closingBracket + 1U);
                if (!rest.empty())
                {
                    if (rest.front() != ':')
                    {
                        failureReason = "方括号之后只能跟 :端口";
                        return false;
                    }
                    hasExplicitPort = true;
                    portPart        = rest.substr(1U);
                }
            } else
            {
                const std::size_t colon = authority.rfind(':');
                if (colon != std::string_view::npos)
                {
                    // 冒号不止一个：那是没包方括号的 IPv6，按裸串当「主机:端口」切会把半截地址当主机
                    if (authority.find(':') != colon)
                    {
                        failureReason = "IPv6 地址要写成 [地址]:端口";
                        return false;
                    }
                    hasExplicitPort = true;
                    hostPart        = authority.substr(0U, colon);
                    portPart        = authority.substr(colon + 1U);
                }
            }

            if (hostPart.empty())
            {
                failureReason = "缺主机名";
                return false;
            }
            host = std::string(hostPart);

            port = defaultPort;
            if (!hasExplicitPort)
            {
                return true;
            }
            if (portPart.empty())
            {
                failureReason = "端口是空的（写了冒号就要写端口）";
                return false;
            }
            std::uint32_t parsedPort = 0;
            for (const char digit: portPart)
            {
                if (!isAsciiDigit(digit))
                {
                    failureReason = "端口「" + std::string(portPart) + "」不是十进制数";
                    return false;
                }
                parsedPort = parsedPort * 10U + static_cast<std::uint32_t>(digit - '0');
                if (parsedPort > 65535U)
                {
                    break;
                }
            }
            if (parsedPort == 0U || parsedPort > 65535U)
            {
                failureReason = "端口「" + std::string(portPart) + "」不在 1..65535";
                return false;
            }
            port = static_cast<std::uint16_t>(parsedPort);
            return true;
        }
    } // namespace

    HttpProxyPolicy applyProxyEnvironment(HttpProxyPolicy policy)
    {
        if (!policy.consultEnvironment)
        {
            return policy;
        }
        if (policy.httpProxy.empty())
        {
            if (std::optional<std::string> value = readProxyEnvironmentValue("http_proxy", "HTTP_PROXY"); value.has_value())
            {
                policy.httpProxy = std::move(*value);
            }
        }
        if (policy.httpsProxy.empty())
        {
            if (std::optional<std::string> value = readProxyEnvironmentValue("https_proxy", "HTTPS_PROXY"); value.has_value())
            {
                policy.httpsProxy = std::move(*value);
            }
        }
        if (policy.noProxy.empty())
        {
            if (std::optional<std::string> value = readProxyEnvironmentValue("no_proxy", "NO_PROXY"); value.has_value())
            {
                policy.noProxy = std::move(*value);
            }
        }
        return policy;
    }

    std::expected<HttpProxyRoute, std::string> parseProxyUrl(const std::string_view proxyUrl)
    {
        const std::string_view text = trimmed(proxyUrl);
        if (text.empty())
        {
            return std::unexpected("代理地址是空串（请给出 http:// 或 https:// 主机[:端口]，或清掉这一项走直连）");
        }

        std::string_view authority    = text;
        bool             isTlsToProxy = false;
        if (const std::size_t schemeSeparator = text.find("://"); schemeSeparator != std::string_view::npos)
        {
            const std::string_view scheme = text.substr(0U, schemeSeparator);
            authority                     = text.substr(schemeSeparator + 3U);
            if (equalsIgnoreCase(scheme, "https"))
            {
                // https:// 代理 = TLS 连到代理自身：建立侧据此先做外层 TLS 握手再说话
                isTlsToProxy = true;
            } else if (!equalsIgnoreCase(scheme, "http"))
            {
                return std::unexpected("代理地址的协议「" + std::string(scheme) + "」不支持：只认 http://（明文连代理）与 https://（TLS 连到代理自身）两种");
            }
        }
        // 端口缺省随 scheme 走：http 代理系 80、https 代理系 443（写错的那一侧连不上，不做猜测）
        const std::uint16_t defaultPort = isTlsToProxy ? 443U : 80U;

        // 尾随一个 '/' 是常见写法，去掉；再出现路径/查询/片段就是形状不对
        if (!authority.empty() && authority.back() == '/')
        {
            authority.remove_suffix(1U);
        }
        if (authority.find_first_of("/?#") != std::string_view::npos)
        {
            return std::unexpected("代理地址带了路径或查询：只接受 [http://|https://][用户[:口令]@]主机[:端口]");
        }

        HttpProxyRoute route;
        route.isTls = isTlsToProxy;
        if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos)
        {
            const std::string_view userInfo = authority.substr(0U, at);
            authority                       = authority.substr(at + 1U);
            const std::size_t      colon    = userInfo.find(':');
            const std::string_view userPart = userInfo.substr(0U, colon);
            const std::string_view passPart = colon == std::string_view::npos ? std::string_view{} : userInfo.substr(colon + 1U);

            const std::optional<std::string> user = percentDecode(userPart);
            if (!user.has_value())
            {
                return std::unexpected("代理地址凭据的用户名含非法的 %XX 转义");
            }
            const std::optional<std::string> password = percentDecode(passPart);
            if (!password.has_value())
            {
                return std::unexpected("代理地址凭据的口令含非法的 %XX 转义");
            }
            route.authorizationHeader = "Basic " + Base::base64Encode(*user + ":" + *password);
        }

        std::string failureReason;
        if (!splitHostPort(authority, defaultPort, route.host, route.port, failureReason))
        {
            return std::unexpected("代理地址非法：" + failureReason);
        }
        return route;
    }

    bool matchesNoProxy(const std::string_view host, const std::uint16_t port, const std::string_view noProxyList) noexcept
    {
        std::size_t position = 0;
        while (position <= noProxyList.size())
        {
            const std::size_t comma = noProxyList.find(',', position);
            std::string_view  entry = trimmed(noProxyList.substr(position, comma == std::string_view::npos ? std::string_view::npos : comma - position));
            position                = comma == std::string_view::npos ? noProxyList.size() + 1U : comma + 1U;

            if (entry.empty())
            {
                continue;
            }
            if (entry == "*")
            {
                return true;
            }

            std::string_view entryHost = entry;
            std::string_view entryPort;
            if (entry.front() == '[')
            {
                const std::size_t closingBracket = entry.find(']');
                if (closingBracket == std::string_view::npos)
                {
                    continue; // 畸形条目按「不命中」处理，不牵连整张表
                }
                entryHost                   = entry.substr(1U, closingBracket - 1U);
                const std::string_view rest = entry.substr(closingBracket + 1U);
                if (!rest.empty() && rest.front() == ':')
                {
                    entryPort = rest.substr(1U);
                }
            } else
            {
                const std::size_t colon = entry.rfind(':');
                if (colon != std::string_view::npos && entry.find(':') == colon)
                {
                    entryHost = entry.substr(0U, colon);
                    entryPort = entry.substr(colon + 1U);
                }
            }

            if (!entryPort.empty())
            {
                std::uint32_t entryPortValue = 0;
                for (const char digit: entryPort)
                {
                    if (!isAsciiDigit(digit))
                    {
                        entryPortValue = 0xFFFFFFFFU;
                        break;
                    }
                    entryPortValue = entryPortValue * 10U + static_cast<std::uint32_t>(digit - '0');
                    if (entryPortValue > 65535U)
                    {
                        break;
                    }
                }
                if (entryPortValue != port)
                {
                    continue;
                }
            }

            // 前导点在 curl 语义里等效于裸域名（同样命中域本身与子域），剥掉后走同一条点边界规则
            while (!entryHost.empty() && entryHost.front() == '.')
            {
                entryHost.remove_prefix(1U);
            }
            if (entryHost.empty())
            {
                continue;
            }
            if (equalsIgnoreCase(host, entryHost) || (host.size() > entryHost.size() && endsWithIgnoreCase(host, entryHost) && host[host.size() - entryHost.size() - 1U] == '.'))
            {
                return true;
            }
        }
        return false;
    }

    std::expected<std::optional<HttpProxyRoute>, std::string> resolveOutboundProxy(const HttpProxyPolicy &policy, const std::string_view scheme, const std::string_view host,
                                                                                   const std::uint16_t port)
    {
        // 豁免表先短路：命中即直连，代理地址写没写错都不影响这条目标
        if (matchesNoProxy(host, port, policy.noProxy))
        {
            return std::optional<HttpProxyRoute>{};
        }

        const std::string *proxyText = nullptr;
        if (equalsIgnoreCase(scheme, "http"))
        {
            proxyText = &policy.httpProxy;
        } else if (equalsIgnoreCase(scheme, "https"))
        {
            proxyText = &policy.httpsProxy;
        } else
        {
            return std::optional<HttpProxyRoute>{};
        }

        if (proxyText->empty())
        {
            return std::optional<HttpProxyRoute>{};
        }

        std::expected<HttpProxyRoute, std::string> parsed = parseProxyUrl(*proxyText);
        if (!parsed.has_value())
        {
            // 坏配置拒绝请求而不是直连：静默绕过运维配置的代理是安全面的事（凭据不回显）
            return std::unexpected(parsed.error() + "（地址「" + redactProxyCredentials(*proxyText) + "」来自 HttpProxyPolicy 或 http_proxy/https_proxy 环境值）");
        }
        return std::optional<HttpProxyRoute>{std::move(*parsed)};
    }

    std::string redactProxyCredentials(const std::string_view proxyUrl)
    {
        const std::size_t at = proxyUrl.rfind('@');
        if (at == std::string_view::npos)
        {
            return std::string(proxyUrl);
        }
        std::size_t credentialsStart = 0;
        if (const std::size_t schemeSeparator = proxyUrl.find("://"); schemeSeparator != std::string_view::npos && schemeSeparator < at)
        {
            credentialsStart = schemeSeparator + 3U;
        }
        std::string redacted(proxyUrl.substr(0U, credentialsStart));
        redacted += "***";
        redacted += proxyUrl.substr(at);
        return redacted;
    }
} // namespace AsynGyanis::Net
