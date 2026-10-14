#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <iterator>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 判断字节是否属于 RFC 3986 §2.3 的 unreserved（字母 / 数字 与 - . _ ~）
         * @param character 待判断字节
         * @return true 属于 unreserved
         */
        constexpr bool isUnreservedCharacter(const char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' ||
                   character == '.' || character == '_' || character == '~';
        }

        /**
         * @brief 判断字节是否属于 RFC 3986 §2.2 的 sub-delims
         * @param character 待判断字节
         * @return true 属于 sub-delims
         */
        constexpr bool isSubDelimiter(const char character) noexcept
        {
            switch (character)
            {
                case '!':
                case '$':
                case '&':
                case '\'':
                case '(':
                case ')':
                case '*':
                case '+':
                case ',':
                case ';':
                case '=':
                    return true;
                default:
                    return false;
            }
        }

        /**
         * @brief 判断字节是否为十六进制数字
         * @param character 待判断字节
         * @return true 是 0-9 / a-f / A-F
         */
        constexpr bool isHexadecimalCharacter(const char character) noexcept
        {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
        }

        /**
         * @brief 判断方括号内的 IPv6 字面量 / IPvFuture 内容是否只由允许的字节组成
         * @details 按 RFC 3986 §3.2.2 的两支取并集：IPv6address 只用十六进制数字、冒号与点（zone id
         *          以 `%` 引出，RFC 6874 要求写成 %25）；IPvFuture 以 `v` 引出。这里判的是字符集而不是
         *          完整地址语法——本函数要拦的是「能让前端与后端各算一份主机」的字节（空白、斜杠、井号、
         *          裸百分号、非 ASCII），把 IPv6 压缩语法全实现一遍并不增加任何拦截面。
         * @param content 方括号之间的内容（不含两侧的方括号）
         * @return true 字符集合法
         */
        constexpr bool isIpLiteralBody(const std::string_view content) noexcept
        {
            for (const char character: content)
            {
                const bool isAllowed = isHexadecimalCharacter(character) || character == ':' || character == '.' || character == '%' || character == 'v' || character == 'V';
                if (!isAllowed)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::optional<std::string_view> findAuthorityFieldDefect(const std::string_view value) noexcept
    {
        // 空值交回「无缺陷」：h1 允许一个空 Host（RFC 9112 §3.2 给「目标 URI 没有权威段」的客户端就是
        // 这么规定的），h2/h3 各自按「出现就必须非空」判，那一格不在本函数的职责里
        if (value.empty())
        {
            return {};
        }

        // userinfo 单独给一条拒因：三条通道的规范都把它写成 MUST NOT，而「@ 之前算主机、整段算缓存键」
        // 正是前端与后端分诊分裂的入口
        if (value.find('@') != std::string_view::npos)
        {
            return "含 userinfo 段（@ 之前的用户名部分）";
        }

        std::size_t portSeparatorIndex = std::string_view::npos;
        if (value.front() == '[')
        {
            const std::size_t closingBracket = value.find(']');
            if (closingBracket == std::string_view::npos)
            {
                return "IPv6 字面量缺少闭合的方括号";
            }
            if (!isIpLiteralBody(value.substr(1, closingBracket - 1)))
            {
                return "IPv6 字面量内含非十六进制、非冒号、非点的字节";
            }
            // 方括号之后只可能剩下「:端口」，端口分隔符就是右括号之后的那一个字节
            portSeparatorIndex = closingBracket + 1;
        } else
        {
            std::size_t index = 0;
            while (index < value.size() && value[index] != ':')
            {
                if (value[index] == '%')
                {
                    // 百分号必须恰好带两位十六进制：裸 % 或半截转义在 reg-name 里没有对应字节，
                    // 而它下游的解码实现各家不同，收下等于把「这一段代表哪个字符」交给对端猜
                    if (index + 2 >= value.size() || !isHexadecimalCharacter(value[index + 1]) || !isHexadecimalCharacter(value[index + 2]))
                    {
                        return "百分号转义不完整（要写成 % 加两位十六进制数字）";
                    }
                    index += 3;
                    continue;
                }
                if (!isUnreservedCharacter(value[index]) && !isSubDelimiter(value[index]))
                {
                    return "含权威段不允许的字符";
                }
                ++index;
            }
            portSeparatorIndex = index;
        }

        if (portSeparatorIndex == value.size())
        {
            return {};
        }
        if (value[portSeparatorIndex] != ':')
        {
            // 方括号那一支才会走到这里：`]` 之后既不是端口分隔符也不是结尾
            return "IPv6 字面量之后只允许跟「:端口」";
        }

        const std::string_view portText = value.substr(portSeparatorIndex + 1);
        // 端口是 *DIGIT（RFC 3986 §3.2.3）：允许空端口（ABNF 如此），但再出现一个冒号就是两段端口
        // ——`a:b:c` 这类取值各家解析器切法不同，正是权威段被读成两份的形状
        if (portText.find(':') != std::string_view::npos)
        {
            return "出现了多于一个端口分隔符「:」";
        }
        for (const char character: portText)
        {
            if (character < '0' || character > '9')
            {
                return "端口不是十进制数字";
            }
        }
        return {};
    }

    bool isConnectionSpecificHeaderName(const std::string_view name) noexcept
    {
        // 清单按 RFC 9113 §8.2.2 的「连接特定字段」给出；h3（RFC 9114 §4.2）禁止的是同一批
        constexpr std::string_view kConnectionSpecificHeaderNames[] = {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"};
        return std::find(std::begin(kConnectionSpecificHeaderNames), std::end(kConnectionSpecificHeaderNames), name) != std::end(kConnectionSpecificHeaderNames);
    }

    bool parseContentLengthValue(std::string_view text, std::size_t &length) noexcept
    {
        // 字段值允许带首尾 OWS（RFC 9110 §5.5），先裁掉再判
        text = trimOptionalWhitespace(text);

        // 19 位十进制已覆盖现实里可能的长度，再多就是损坏或恶意的取值
        if (text.empty() || text.size() > 19)
            return false;
        std::size_t value = 0;
        for (const char character: text)
        {
            if (character < '0' || character > '9')
                return false;
            value = value * 10 + static_cast<std::size_t>(character - '0');
        }
        length = value;
        return true;
    }
} // namespace AsynGyanis::Net
