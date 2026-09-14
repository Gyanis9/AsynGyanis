#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <iterator>

namespace AsynGyanis::Net
{
    bool isConnectionSpecificHeaderName(const std::string_view name) noexcept
    {
        // 清单按 RFC 9113 §8.2.2 的「连接特定字段」给出；h3（RFC 9114 §4.2）禁止的是同一批
        constexpr std::string_view kConnectionSpecificHeaderNames[] = {"connection", "keep-alive", "proxy-connection",
                                                                      "transfer-encoding", "upgrade"};
        return std::find(std::begin(kConnectionSpecificHeaderNames), std::end(kConnectionSpecificHeaderNames), name) !=
               std::end(kConnectionSpecificHeaderNames);
    }
} // namespace AsynGyanis::Net
