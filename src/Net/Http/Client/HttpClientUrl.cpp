/**
 * @file HttpClientUrl.cpp
 * @brief 出站 URL 的拆解与下一跳引用解析
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details parseUrl 与 resolveUrlReference 共用同一份「可撕裂请求行」判据与点段折叠规则；
 *          Host/:authority 文本的拼法也只留一处，请求头与 h2/h3 伪头都走它。
 */

#include "Net/Http/Client/HttpClientUrl.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Message/HttpHeaderRules.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace AsynGyanis::Net
{
    namespace HttpClientUrl
    {
        std::string authorityText(const ParsedUrl &url)
        {
            std::string host = url.host.find(':') != std::string::npos ? '[' + url.host + ']' : url.host;
            // 端口只在不等于该协议的默认端口时写：RFC 9110 §7.2 对 Host、RFC 9113 §8.3.1 对
            // :authority 是同一条法。连到哪个端口由 URL 的端口决定，这里漏掉端口的代价是站点选择：
            // 按 Host 分虚拟主机的对端会把 8443 上的服务认成默认那一个，严格的实现直接回 421
            const bool isSchemeDefaultPort = (url.scheme == "https" && url.port == 443U) || (url.scheme != "https" && url.port == 80U);
            if (!isSchemeDefaultPort)
            {
                host += ':' + std::to_string(url.port);
            }
            return host;
        }

        void appendHostHeader(std::string &request, const ParsedUrl &url)
        {
            request += "Host: ";
            request += authorityText(url);
            request += "\r\n";
        }
    } // namespace HttpClientUrl

    namespace
    {
        using HttpClientUrl::authorityText;

        /**
         * @brief 把冒号之后的端口文本读成一个可用的端口
         * @param text 端口文本，必须全为十进制数字
         * @param port 输出端口
         * @return true 取值在 1..65535 内
         */
        [[nodiscard]] bool parsePort(std::string_view text, std::uint32_t &port)
        {
            // 超过 5 位必然大于 65535，先挡掉，省得 from_chars 溢出后还要判符号
            if (text.empty() || text.size() > 5U)
            {
                return false;
            }
            const std::from_chars_result converted = std::from_chars(text.data(), text.data() + text.size(), port);
            return converted.ec == std::errc{} && converted.ptr == text.data() + text.size() && port >= 1U && port <= 65535U;
        }

        /**
         * @brief URL（含下一跳引用）里不许出现的字节：空白与控制字符（≤ 0x20 与 DEL 0x7F）
         * @details 请求目标是从这段文本原样拼进请求行的：留一个 CR/LF 就是让调用方自己结束请求行、
         *          甚至插进新的头部（请求分裂），留一个 NUL 会让后面按 C 字符串处理这段的接口截断目标。
         *          两个入口（`parseUrl()` 与 `resolveUrlReference()`）共用这一份判据：先前各写一条，
         *          一边收全了 ASCII 控制字符、一边只认四个，而注释还写着「同一口径」。
         */
        [[nodiscard]] bool urlHasTearableCharacter(const std::string_view url) noexcept
        {
            for (const char character: url)
            {
                if (static_cast<unsigned char>(character) <= 0x20U || static_cast<unsigned char>(character) == 0x7FU)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 拆解 authority：拒绝 userinfo，识别方括号 IPv6 与端口，写进 ParsedUrl
         * @param authority URL 里 `scheme://` 与 `/?#` 之间那一段
         * @param url 原始 URL，仅用于脱敏后的诊断文本
         * @param parsed 协议已填好、主机与端口待填的输出
         * @throws Base::InvalidArgumentException authority 带 userinfo、方括号/端口畸形或主机为空
         * @note 诊断文本一律走 redactUrlForDiagnostics()：这条路径上的错误最容易被原样写进日志
         */
        void parseAuthorityInto(const std::string_view authority, const std::string_view url, ParsedUrl &parsed)
        {
            // userinfo 必须先认出来：按冒号数量判 IPv6 会把 `user:pass@host:port` 误报成
            // 「IPv6 没加方括号」，而那条文案会把整个 authority 连同口令回显出去。
            // RFC 9110 §4.2.4 已不鼓励在 http(s) URI 里带凭据；这里明确拒绝，并把调用方
            // 指向 Authorization 头。拒绝文案不回显输入，口令因此不会进异常、日志或调用方记录
            if (authority.rfind('@') != std::string_view::npos)
            {
                throw Base::InvalidArgumentException("HttpClient：URL 里不允许带 userinfo（user:pass@ 形式）。HTTP(S) 的凭据请放在 Authorization 头里"
                                                     "（RFC 9110 §4.2.4）；本条 URL 已被拒绝，输入内容不会写入本消息");
            }

            // 方括号里的是 IP 字面量（RFC 3986 §3.2.2）：IPv6 自带冒号，不这样区分就分不清哪段是端口
            std::string_view hostText = authority;
            std::string_view portText;
            bool             hasExplicitPort = false;
            if (!authority.empty() && authority.front() == '[')
            {
                const std::size_t closingBracket = authority.find(']');
                if (closingBracket == std::string_view::npos)
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机部分方括号没闭合（IPv6 字面量要写成 "[::1]:8080" 的形式）：「)" + redactUrlForDiagnostics(url) +
                                                         R"(」)");
                }
                hostText                            = authority.substr(1, closingBracket - 1);
                const std::string_view afterBracket = authority.substr(closingBracket + 1);
                if (!afterBracket.empty())
                {
                    if (afterBracket.front() != ':')
                    {
                        throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机部分在 "]" 之后还跟着多余字符（端口要写成 ":8080"）：「)" + redactUrlForDiagnostics(url) +
                                                             R"(」)");
                    }
                    hasExplicitPort = true;
                    portText        = afterBracket.substr(1);
                }
            } else
            {
                const std::size_t portSeparator = authority.rfind(':');
                if (portSeparator != std::string_view::npos)
                {
                    // 冒号不止一个又没有方括号：那是没按规范包起来的 IPv6，拆出来的「主机」会是半截地址
                    if (authority.find(':') != portSeparator)
                    {
                        throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机是 IPv6 时必须写成方括号形式（"[::1]:8080"）：「)" + redactUrlForDiagnostics(url) + R"(」)");
                    }
                    hasExplicitPort = true;
                    hostText        = authority.substr(0, portSeparator);
                    portText        = authority.substr(portSeparator + 1);
                }
            }

            if (hostText.empty())
            {
                throw Base::InvalidArgumentException(R"(HttpClient：URL 里没有主机部分：「)" + redactUrlForDiagnostics(url) + R"(」)");
            }
            parsed.host = std::string(hostText);

            if (hasExplicitPort)
            {
                std::uint32_t port = 0U;
                if (!parsePort(portText, port))
                {
                    // 原先这里回落到 80：一个写错的 https 端口会静默连到明文端口上，
                    // 宁可当场报错也不替调用方猜一个
                    throw Base::InvalidArgumentException(R"(HttpClient：URL 的端口「)" + std::string(portText) + R"(」不是 1..65535 的十进制数：完整 URL 是「)" +
                                                         redactUrlForDiagnostics(url) + R"(」)");
                }
                parsed.port = static_cast<uint16_t>(port);
            } else
            {
                parsed.port = (parsed.scheme == "https") ? 443 : 80;
            }
        }

        /**
         * @brief 判一段文本是不是合法的协议名（RFC 3986 §3.1：字母打头，其后字母/数字/+/-/.）
         * @details 判据住在 `HttpHeaderRules.h`，与 h3 的 `:scheme` 校验、h2 的伪头校验同一份——
         *          三份各写一遍时，「本端发出去的 URL 能不能被本端收下来」这件事就成了运气。
         * @param text 冒号之前那一段
         * @return true 可以作为协议名
         */
        bool isSchemeName(const std::string_view text)
        {
            return isUriSchemeSyntax(text);
        }

        /**
         * @brief 把输出里的最后一段路径摘掉（RFC 3986 §5.2.4 处理 `..` 时做的那件事）
         * @param output 已确定的输出前缀，就地修改
         */
        void dropLastOutputSegment(std::string &output)
        {
            const std::size_t slash = output.rfind('/');
            output                  = slash == std::string::npos ? std::string{} : output.substr(0U, slash);
        }

        /**
         * @brief 折叠路径里的 `.` 与 `..` 段（RFC 3986 §5.2.4）
         * @param path 以 `/` 打头的路径（不含查询）
         * @return std::string 折叠后的绝对路径
         * @details 照规范那台状态机逐条规则走，不按「切段—过滤—重拼」的省事写法：中段的双斜杠在规范里
         *          是一个空段，必须原样留着（`/a//b` 与 `/a/b` 对按路径分派的路由是两个资源），而按段
         *          过滤的实现会把空段一起滤掉——那种折叠等于替对端改写了下一跳指向哪个资源，且不报任何错。
         *          越根的 `..`（`/../x`）折到根为止：跳出一台主机之外的路径不是合法请求目标。
         */
        std::string collapseDotSegments(const std::string_view path)
        {
            std::string output;
            std::string remaining(path);
            while (!remaining.empty())
            {
                if (remaining.starts_with("../"))
                {
                    remaining.erase(0U, 3U); // 规则 2B：相对形式的 ./ 与 ../ 直接从输入里去掉
                    continue;
                }
                if (remaining.starts_with("./"))
                {
                    remaining.erase(0U, 2U);
                    continue;
                }
                if (remaining == "/./" || remaining.starts_with("/./"))
                {
                    remaining.erase(0U, 2U); // 规则 2C：把 "." 这一段删掉，其后那个 / 本来就是段分隔符，不能补两份
                    continue;
                }
                if (remaining == "/.")
                {
                    remaining.assign(1U, '/'); // 规则 2C 的收尾形态："/." 换成 "/"，末尾那个斜杠要留住
                    continue;
                }
                if (remaining == "/../" || remaining.starts_with("/../"))
                {
                    remaining.erase(0U, 3U); // 规则 2D：把 ".." 这一段删掉并摘掉输出的最后一段；同理不补第二份 /
                    dropLastOutputSegment(output);
                    continue;
                }
                if (remaining == "/..")
                {
                    remaining.assign(1U, '/');
                    dropLastOutputSegment(output);
                    continue;
                }
                if (remaining == "." || remaining == "..")
                {
                    remaining.clear(); // 规则 2E：孤立的一个点什么都不留
                    continue;
                }

                // 规则 2A：把第一个路径段（连同它打头的那个 /，如果有）整段搬到输出里
                const std::size_t nextSlash = remaining.find('/', remaining.front() == '/' ? 1U : 0U);
                if (nextSlash == std::string::npos)
                {
                    output += remaining;
                    remaining.clear();
                } else
                {
                    output += remaining.substr(0U, nextSlash);
                    remaining.erase(0U, nextSlash);
                }
            }
            return output.empty() ? std::string{"/"} : output;
        }

        /**
         * @brief 取基准 URL 的目录部分（最后一个 `/` 及其之前的内容）
         * @param path 基准路径，可能自带查询
         * @return std::string 以 `/` 结尾的目录；路径里没有斜杠时为 "/"
         */
        std::string baseDirectory(std::string_view path)
        {
            if (const std::size_t query = path.find('?'); query != std::string_view::npos)
            {
                path = path.substr(0, query);
            }
            const std::size_t slash = path.rfind('/');
            return slash == std::string_view::npos ? std::string{"/"} : std::string(path.substr(0, slash + 1U));
        }

        /**
         * @brief 把 ParsedUrl 拼回一条绝对 URL
         * @param url 已定下来的协议、主机、端口与路径
         * @return std::string 交回的串可直接喂给 `parseUrl()`
         * @details 端口只在不是该协议默认端口时写出来，主机含冒号时补回方括号——两件事都由
         *          `authorityText()` 一处负责，与拼请求头那一路同一条法
         */
        std::string renderUrl(const ParsedUrl &url)
        {
            return url.scheme + "://" + authorityText(url) + url.path;
        }
    } // namespace

    ParsedUrl parseUrl(const std::string_view url)
    {
        // 请求行是把 path 原样拼出来的：里面若有 CR/LF 或空白，等于让调用方自己结束请求行、
        // 甚至插进新的头部（请求分裂）。这不是「请求失败」，是用法错误，当场报出来
        if (urlHasTearableCharacter(url))
        {
            throw Base::InvalidArgumentException("HttpClient：URL 里不允许出现空白或控制字符（会撕裂请求行）：「" + redactUrlForDiagnostics(url) + "」");
        }

        ParsedUrl         parsed;
        std::string_view  remainder       = url;
        const std::size_t schemeSeparator = remainder.find("://");
        // 协议名必须写出来。缺了就缺了，不能猜：猜 http 等于把一段本应加密的流量静默改成明文外发，
        // 猜 https 又会连到一个只有明文端口的服务——两种猜法的失败都不出声
        if (schemeSeparator == std::string_view::npos)
        {
            throw Base::InvalidArgumentException(R"(HttpClient：URL 少了协议名，要写成 http:// 或 https:// 开头：「)" + redactUrlForDiagnostics(url) +
                                                 R"(」。这里不替调用方补默认协议：补错一次就是把 TLS 整段绕过去)");
        }
        const std::string_view schemeText = remainder.substr(0, schemeSeparator);
        // 协议名大小写无关（RFC 3986 §6.2.3）：HTTPS:// 悄悄当成 http 就是把 TLS 整段降级
        if (equalsIgnoringCase(schemeText, "https"))
        {
            parsed.scheme = "https";
        } else if (equalsIgnoringCase(schemeText, "http"))
        {
            parsed.scheme = "http";
        } else
        {
            throw Base::InvalidArgumentException(R"(HttpClient：只支持 "http" 与 "https" 两种协议，收到的是「)" + std::string(schemeText) + R"(」：换成 http(s):// 开头再试)");
        }
        remainder.remove_prefix(schemeSeparator + 3);

        // 主机的边界按 RFC 3986 §3.2 认 `/`、`?`、`#` 三个：此前只认 `/`，于是
        // `http://host?a=1` 把 `?a=1` 整段当成主机——Host 头随之畸形，而查询被静吞
        const std::size_t      pathSeparator = remainder.find_first_of("/?#");
        const std::string_view authority     = pathSeparator == std::string_view::npos ? remainder : remainder.substr(0U, pathSeparator);
        std::string_view       pathPart      = pathSeparator == std::string_view::npos ? std::string_view{} : remainder.substr(pathSeparator);

        // 片段（`#...`）不进请求：RFC 9110 §7.1 的请求目标里根本没有它。这里按规范剥掉而不是
        // 报错——那不是「替调用方猜意图」，而是明确不该交给服务器的那一段
        if (const std::size_t fragmentOffset = pathPart.find('#'); fragmentOffset != std::string_view::npos)
        {
            pathPart = pathPart.substr(0U, fragmentOffset);
        }

        parseAuthorityInto(authority, url, parsed);
        if (!pathPart.empty())
        {
            // 只有查询（`http://host?a=1`）：authority 之后没有路径段。origin-form 的请求目标不能是空的
            // （RFC 9112 §3.2.1），原样交出去会变成 `GET ?a=1`，这里补成 `/?a=1`
            parsed.path = pathPart.front() == '?' ? "/" + std::string(pathPart) : std::string(pathPart);
        }
        return parsed;
    }

    std::optional<std::string> resolveUrlReference(const ParsedUrl &base, const std::string_view reference)
    {
        if (reference.empty())
        {
            return std::nullopt;
        }
        // 空白与控制字符会撕裂下一跳的请求行，与 parseUrl 共用同一份判据
        if (urlHasTearableCharacter(reference))
        {
            return std::nullopt;
        }

        // 片段（`#...`）整段丢掉：一次 HTTP 跳转用不上它，留着只会让下一跳与它自己的 Origin 对不上
        const std::string_view target = reference.substr(0U, reference.find('#'));
        if (target.empty())
        {
            return std::nullopt; // 「只带片段」的引用去掉片段之后什么都不剩，那不构成一跳
        }

        const std::size_t colonOffset     = target.find(':');
        const std::size_t delimiterOffset = target.find_first_of("/?");
        const bool        isNetworkPath   = target.starts_with("//");
        const bool        isAbsolute = !isNetworkPath && colonOffset != std::string_view::npos && colonOffset < delimiterOffset && isSchemeName(target.substr(0U, colonOffset));

        ParsedUrl   resolved = base; // 协议、主机与端口默认全取基准，只有下面两种引用形式会换掉它们
        std::string path;
        std::string query; // 含前导 `?`；引用里没有查询时留空

        if (isAbsolute || isNetworkPath)
        {
            // 这两种形式的主机由引用说了算，因此要整条重新拆；带认证信息的写法一律拒
            // （RFC 9110 §4.2.4 早已废止 URL 内嵌凭据，而把它抄进下一跳等于把口令发给另一台主机）
            const std::size_t authorityStart = isNetworkPath ? 2U : colonOffset + 3U; // 跳过 "//" 或 "https://"
            const std::size_t authorityEnd   = target.find_first_of("/?", authorityStart);
            if (target.substr(authorityStart, authorityEnd - authorityStart).find('@') != std::string_view::npos)
            {
                return std::nullopt;
            }
            try
            {
                resolved = parseUrl(isAbsolute ? std::string(target) : base.scheme + ":" + std::string(target));
            } catch (const Base::InvalidArgumentException &)
            {
                return std::nullopt; // 对端给的绝对 URL 本身就畸形：那是网络现象，不是本进程的用法错误，别把异常穿过响应处理
            }
            const std::size_t absoluteQueryOffset = resolved.path.find('?');
            path                                  = absoluteQueryOffset == std::string::npos ? resolved.path : std::string(resolved.path.substr(0U, absoluteQueryOffset));
            query                                 = absoluteQueryOffset == std::string::npos ? std::string{} : std::string(resolved.path.substr(absoluteQueryOffset));
        } else
        {
            const std::size_t      queryOffset   = target.find('?');
            const std::string_view referencePath = target.substr(0U, queryOffset);
            if (referencePath.starts_with('/'))
            {
                path = std::string(referencePath);
            } else if (!referencePath.empty())
            {
                path = baseDirectory(base.path) + std::string(referencePath); // 相对路径接在基准的「目录」之后
            } else
            {
                // 只有查询：路径原样留着。把基准自带的查询一起留下就会拼成 `?a=1?b=2` 那种谁也不认的串
                path = std::string(base.path.substr(0U, base.path.find('?')));
            }
            if (path.empty())
            {
                path = "/";
            }
            if (queryOffset != std::string_view::npos)
            {
                // 空查询（`?` 后面什么都没有）与「没有查询」是两回事，按引用给的原样带上
                query = std::string(target.substr(queryOffset));
            }
        }

        // 点段折叠对所有分支都要做：绝对形式的 `Location` 里 `/a/../b` 与相对形式折出来的结果应当是同一个文件
        ParsedUrl result;
        result.scheme = std::move(resolved.scheme);
        result.host   = std::move(resolved.host);
        result.port   = resolved.port;
        result.path   = collapseDotSegments(path) + std::move(query);
        return renderUrl(result);
    }
} // namespace AsynGyanis::Net
