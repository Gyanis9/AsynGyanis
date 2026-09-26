#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <iterator>

namespace AsynGyanis::Net
{
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
