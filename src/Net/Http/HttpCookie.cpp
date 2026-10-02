#include "Net/Http/HttpCookie.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 判断字符能否作为 Cookie 取值的一部分
         * @details 按 RFC 6265 §4.1.1 的 cookie-octet：0x21、0x23..0x2B、0x2D..0x3A、0x3C..0x7E。
         *          挡在外面的是会破坏头部结构的几个：空格、'"'、';'、','、控制符与 DEL；'=' 与 '/'
         *          这类是允许的（两条解析入口只按**第一个** '=' 切分，取值里再出现的 '=' 原样保留）。
         *          0x5C（反斜杠）是规范不让而本层放行的唯一一格：它不动任何头部结构，而真实取值里确实
         *          会出现它，挡下来只会把一条能用的 Cookie 变成存不进来。
         * @param character 待判字符
         * @return true 允许
         */
        constexpr bool isValueCharacter(const char character) noexcept
        {
            const auto code = static_cast<unsigned char>(character);
            return code == 0x21 || (code >= 0x23 && code <= 0x2B) || (code >= 0x2D && code <= 0x3A) || (code >= 0x3C && code <= 0x7E);
        }

        /**
         * @brief 把 quoted-string 包起来的取值还原成裸值（RFC 6265 §5.1.2 与 §5.2）
         * @param rawValue 按第一个 '=' 切出来、两侧空白已去的原样文本
         * @return std::string 还原后的取值；不是 quoted-string 时就是原样文本
         * @details 只认「首尾各一枚引号」这一形，内层的 '\' 按 §5.1.2 去掉反斜杠、后一个字符照字面收。
         *          单侧引号不当这里是 quoted-string：那时引号就是普通字符。内层没转义的引号不在这里
         *          特判——它解开之后仍然含一枚裸写法容不下的 '"'，调用方那道字符闸门自然会把整条拒掉。
         */
        [[nodiscard]] std::string unwrapQuotedValue(const std::string_view rawValue)
        {
            if (rawValue.size() < 2U || rawValue.front() != '"' || rawValue.back() != '"')
            {
                return std::string(rawValue);
            }

            std::string unwrapped;
            unwrapped.reserve(rawValue.size() - 2U);
            for (std::size_t index = 1; index + 1 < rawValue.size(); ++index)
            {
                const char character = rawValue[index];
                // 反斜杠只在后面确实还有一个「非收尾引号」的字符时才算转义，孤零零的一个 '\' 按字面留
                if (character == '\\' && index + 2 < rawValue.size())
                {
                    unwrapped.push_back(rawValue[++index]);
                    continue;
                }
                unwrapped.push_back(character);
            }
            return unwrapped;
        }

        /// 去掉两侧的空白
        [[nodiscard]] std::string_view trimSpaces(const std::string_view text) noexcept
        {
            std::size_t begin = 0;
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
            {
                ++begin;
            }
            std::size_t end = text.size();
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        /**
         * @brief 大小写不敏感地比较属性名
         * @details 服务端写出的 Cookie 属性名大小写五花八门（'PATH'、'samesite'），
         *          RFC 6265 §5.2 要求收端不区分大小写。只用 ASCII 折叠，不走 locale。
         */
        [[nodiscard]] bool attributeNamesMatch(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const char lowerLeft  = (left[index] >= 'A' && left[index] <= 'Z') ? static_cast<char>(left[index] - 'A' + 'a') : left[index];
                const char lowerRight = (right[index] >= 'A' && right[index] <= 'Z') ? static_cast<char>(right[index] - 'A' + 'a') : right[index];
                if (lowerLeft != lowerRight)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 解析整段十进制数字（允许一个前导 '+'），必须整段消费
         * @param text 待解析文本
         * @param[out] value 解析结果
         * @return true 解析成功
         */
        bool parseSignedInteger(const std::string_view text, std::int64_t &value) noexcept
        {
            const std::string_view digits = (text.size() > 1 && text.front() == '+') ? text.substr(1) : text;
            if (digits.empty())
            {
                return false;
            }
            std::int64_t parsed                = 0;
            const auto [endPointer, errorCode] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
            if (errorCode != std::errc() || endPointer != digits.data() + digits.size())
            {
                return false;
            }
            value = parsed;
            return true;
        }
    } // namespace

    HttpCookie::HttpCookie(const std::string_view cookieName, const std::string_view cookieValue)
    {
        if (!isValidName(cookieName))
        {
            throw Base::InvalidArgumentException("HttpCookie: Cookie 名字必须是 RFC 7230 的 token，收到的是「" + std::string(cookieName) +
                                                 "」（不能含空格、控制符与 \" ; , 等分隔符）");
        }
        if (!isValidValue(cookieValue))
        {
            throw Base::InvalidArgumentException("HttpCookie: Cookie 取值含非法字符，名字「" + std::string(cookieName) + "」的取值是「" + std::string(cookieValue) +
                                                 "」（不能含空格、控制符与 \" ; , ；'=' 是允许的）");
        }
        m_name.assign(cookieName);
        m_value.assign(cookieValue);
    }

    bool HttpCookie::isValidName(const std::string_view text) noexcept
    {
        // 字符集判据只留一份：Cookie 名字用的就是 RFC 9110 §5.1 的 tchar，与头部字段名、方法名
        // 同一张表（本函数此前自带一份逐字符比较的同集合实现，两份只会在改表那天分叉）
        return !text.empty() && containsOnlyTokenCharacters(text);
    }

    bool HttpCookie::isValidValue(const std::string_view text) noexcept
    {
        return std::ranges::all_of(text, isValueCharacter);
    }

    void HttpCookie::setValue(const std::string_view cookieValue)
    {
        if (!isValidValue(cookieValue))
        {
            throw Base::InvalidArgumentException("HttpCookie: Cookie 取值含非法字符，名字「" + m_name + "」的取值是「" + std::string(cookieValue) + "」");
        }
        m_value.assign(cookieValue);
    }

    void HttpCookie::setPath(const std::string_view path)
    {
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpCookie: Path 必须以 '/' 开头，收到的是「" + std::string(path) +
                                                 "」（非 '/' 开头的值会被浏览器整段丢掉，这里不替调用方补斜杠）");
        }
        if (!std::ranges::all_of(path, isValueCharacter))
        {
            throw Base::InvalidArgumentException("HttpCookie: Path 含非法字符：「" + std::string(path) + "」");
        }
        m_path = std::string(path);
    }

    bool HttpCookie::isValidDomain(const std::string_view text) noexcept
    {
        // 空格与控制符（含 0x7F）之外的这些字符都不属于主机名：`;` 与 `"` 会破坏属性切分，
        // `=` 会让对端把整段重新解析成另一条属性，`/` 则会把路径写进域名里
        constexpr std::string_view forbiddenCharacters{" ;\"=/"};
        return !text.empty() && text.find_first_of(forbiddenCharacters) == std::string_view::npos &&
               std::ranges::none_of(text, [](const char character) { return static_cast<unsigned char>(character) < 0x21 || static_cast<unsigned char>(character) == 0x7F; });
    }

    void HttpCookie::setDomain(const std::string_view domain)
    {
        if (!isValidDomain(domain))
        {
            throw Base::InvalidArgumentException("HttpCookie: Domain 不能为空或含空格、控制符与 \" ; = /：「" + std::string(domain) + "」");
        }
        m_domain = std::string(domain);
    }

    std::string HttpCookie::renderAsSetCookie() const
    {
        std::string text = m_name + "=" + m_value;

        if (m_path.has_value())
        {
            text += "; Path=" + *m_path;
        }
        if (m_domain.has_value())
        {
            text += "; Domain=" + *m_domain;
        }
        if (m_maxAgeSeconds.has_value())
        {
            text += "; Max-Age=" + std::to_string(*m_maxAgeSeconds);
        }
        if (m_expiresAt.has_value())
        {
            text += "; Expires=" + formatHttpDate(*m_expiresAt);
        }
        if (m_isSecure)
        {
            text += "; Secure";
        }
        if (m_isHttpOnly)
        {
            text += "; HttpOnly";
        }
        if (m_sameSite.has_value())
        {
            switch (*m_sameSite)
            {
                case CookieSameSitePolicy::Strict:
                    text += "; SameSite=Strict";
                    break;
                case CookieSameSitePolicy::Lax:
                    text += "; SameSite=Lax";
                    break;
                case CookieSameSitePolicy::None:
                    text += "; SameSite=None";
                    break;
            }
        }
        return text;
    }

    std::optional<HttpCookie> HttpCookie::parseSetCookie(const std::string_view headerValue)
    {
        const std::size_t      nameValueEnd  = headerValue.find(';');
        const std::string_view nameValuePair = trimSpaces(headerValue.substr(0, nameValueEnd));

        const std::size_t equalsPosition = nameValuePair.find('=');
        if (equalsPosition == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::string_view name = trimSpaces(nameValuePair.substr(0, equalsPosition));
        // 先解 quoted-string 再验字符：服务端包引号正是为了让值里的字符能发出来，反过来先验字符
        // 会把「带引号的一条合法 Cookie」整条判死（实测：sid="abc123" 过去直接返回空）
        const std::string value = unwrapQuotedValue(trimSpaces(nameValuePair.substr(equalsPosition + 1)));
        if (!isValidName(name) || !isValidValue(value))
        {
            return std::nullopt;
        }

        HttpCookie cookie;
        cookie.m_name.assign(name);
        cookie.m_value = std::move(value);

        std::size_t cursor = nameValueEnd;
        while (cursor != std::string_view::npos && cursor < headerValue.size())
        {
            const std::size_t      nextSeparator = headerValue.find(';', cursor + 1);
            const std::string_view attribute =
                    trimSpaces(headerValue.substr(cursor + 1, nextSeparator == std::string_view::npos ? std::string_view::npos : nextSeparator - cursor - 1));
            cursor = nextSeparator;
            if (attribute.empty())
            {
                continue;
            }

            const std::size_t      attributeEquals = attribute.find('=');
            const std::string_view attributeName   = trimSpaces(attributeEquals == std::string_view::npos ? attribute : attribute.substr(0, attributeEquals));
            const std::string_view attributeValue  = attributeEquals == std::string_view::npos ? std::string_view{} : trimSpaces(attribute.substr(attributeEquals + 1));

            if (attributeNamesMatch(attributeName, "path"))
            {
                if (!attributeValue.empty() && attributeValue.front() == '/' && std::ranges::all_of(attributeValue, isValueCharacter))
                {
                    cookie.m_path = std::string(attributeValue);
                }
            } else if (attributeNamesMatch(attributeName, "domain"))
            {
                // 与写侧同一判据：不合规定的 Domain 当「这一条属性没给」处理（RFC 6265 §5.2.3 对
                // 无效域名就是忽略该属性），而不是放宽收下——放宽收下等于让罐子存下一条
                // 本框架永远写不出来、却参与匹配的条目
                if (isValidDomain(attributeValue))
                {
                    cookie.m_domain = std::string(attributeValue);
                }
            } else if (attributeNamesMatch(attributeName, "max-age"))
            {
                std::int64_t seconds = 0;
                if (parseSignedInteger(attributeValue, seconds))
                {
                    cookie.m_maxAgeSeconds = seconds;
                }
            } else if (attributeNamesMatch(attributeName, "expires"))
            {
                if (const auto expiresAt = parseHttpDate(attributeValue); expiresAt.has_value())
                {
                    cookie.m_expiresAt = *expiresAt;
                }
            } else if (attributeNamesMatch(attributeName, "secure"))
            {
                cookie.m_isSecure = true;
            } else if (attributeNamesMatch(attributeName, "httponly"))
            {
                cookie.m_isHttpOnly = true;
            } else if (attributeNamesMatch(attributeName, "samesite"))
            {
                if (attributeNamesMatch(attributeValue, "strict"))
                {
                    cookie.m_sameSite = CookieSameSitePolicy::Strict;
                } else if (attributeNamesMatch(attributeValue, "lax"))
                {
                    cookie.m_sameSite = CookieSameSitePolicy::Lax;
                } else if (attributeNamesMatch(attributeValue, "none"))
                {
                    cookie.m_sameSite = CookieSameSitePolicy::None;
                }
            }
            // 其余属性名一概忽略：RFC 6265 §5.2 明确要求收端不认的属性跳过而不判整条失败
        }

        return cookie;
    }

    std::vector<HttpCookie> HttpCookie::parseCookieHeader(const std::string_view headerValue)
    {
        std::vector<HttpCookie> cookies;

        std::size_t       cursor = 0;
        const std::size_t size   = headerValue.size();
        while (cursor < size)
        {
            const std::size_t      separator = headerValue.find(';', cursor);
            const std::string_view pair      = headerValue.substr(cursor, separator == std::string_view::npos ? std::string_view::npos : separator - cursor);
            cursor                           = (separator == std::string_view::npos) ? size : separator + 1;

            const std::size_t equalsPosition = pair.find('=');
            if (equalsPosition == std::string_view::npos)
            {
                continue;
            }
            const std::string_view name = trimSpaces(pair.substr(0, equalsPosition));
            // 与 parseSetCookie 同一套：quoted-string 先解开再验字符，两条入口对同一种线上形状要给同一个答案
            const std::string value = unwrapQuotedValue(trimSpaces(pair.substr(equalsPosition + 1)));
            if (!isValidName(name) || !isValidValue(value))
            {
                // 单条畸形不判整条头失败：浏览器与代理拼出的 Cookie 头里混一个怪项不该让其余读不到
                continue;
            }

            HttpCookie cookie;
            cookie.m_name.assign(name);
            cookie.m_value = std::move(value);
            cookies.push_back(std::move(cookie));
        }

        return cookies;
    }
} // namespace AsynGyanis::Net
