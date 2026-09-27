#include "Net/Http/Client/SseEventParser.h"

#include <cstddef>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 去掉值前面那一个空格：按 §9.2.6，冒号后的第一个空格属于分隔符，不属于取值
        [[nodiscard]] std::string_view stripOneLeadingSpace(std::string_view value)
        {
            if (!value.empty() && value.front() == ' ')
            {
                value.remove_prefix(1);
            }
            return value;
        }

        /**
         * @brief 把 retry: 的取值折成毫秒数
         * @details 规范只认「全十进制数字」的取值，其余一律忽略这一条（不回退到上一个值，也不报错）：
         *          解析成半截数字比忽略一条更糟——那会把重连间隔改成对端没给过的数
         * @param value 冒号后的原文
         * @return std::optional<std::uint64_t> 合法时给毫秒数
         */
        [[nodiscard]] std::optional<std::uint64_t> parseRetryMilliseconds(const std::string_view value)
        {
            if (value.empty() || value.size() > 19)
            {
                return std::nullopt;
            }
            std::uint64_t milliseconds = 0;
            for (const char digit: value)
            {
                if (digit < '0' || digit > '9')
                {
                    return std::nullopt;
                }
                milliseconds = milliseconds * 10U + static_cast<std::uint64_t>(digit - '0');
            }
            return milliseconds;
        }
    } // namespace

    void SseEventParser::feed(const std::string_view chunk)
    {
        m_lineBuffer.append(chunk);

        // 一行一行地结清：分隔符是 CRLF、LF 或单独的 CR，三者都能把攒着的字段变成「这一行到此为止」
        while (true)
        {
            const std::size_t lineEnd = m_lineBuffer.find_first_of("\r\n");
            if (lineEnd == std::string::npos)
            {
                return;
            }
            const char  terminator        = m_lineBuffer[lineEnd];
            std::size_t consumedByteCount = lineEnd + 1;
            if (terminator == '\r' && lineEnd + 1 == m_lineBuffer.size())
            {
                // 结尾一个孤零零的 CR：还判不出它是「单独的 CR」还是 CRLF 的前一半，等下一批再结
                return;
            }
            if (terminator == '\r' && m_lineBuffer[lineEnd + 1] == '\n')
            {
                consumedByteCount = lineEnd + 2;
            }
            handleLine(std::string_view(m_lineBuffer).substr(0, lineEnd));
            m_lineBuffer.erase(0, consumedByteCount);
        }
    }

    void SseEventParser::endOfStream()
    {
        // 对端收线时最后一条事件常常没有那个收尾的空行：把留在缓冲里的那半行当一整行结清，
        // 再补一个空行让攒着的事件真的交出去（否则这条事件就随连接一起丢了）
        if (!m_lineBuffer.empty())
        {
            handleLine(m_lineBuffer);
            m_lineBuffer.clear();
        }
        handleLine({});
    }

    std::optional<SseEvent> SseEventParser::nextEvent()
    {
        if (m_readyEvents.empty())
        {
            return std::nullopt;
        }
        std::optional<SseEvent> event = std::move(m_readyEvents.front());
        m_readyEvents.pop_front();
        return event;
    }

    std::size_t SseEventParser::pendingByteCount() const noexcept
    {
        std::size_t byteCount = m_lineBuffer.size();
        for (const SseEvent &event: m_readyEvents)
        {
            byteCount += event.data.size() + event.type.size() + event.lastEventId.size();
        }
        return byteCount;
    }

    void SseEventParser::handleLine(const std::string_view line)
    {
        if (line.empty())
        {
            // 空行 = 派发点。正文为空时按规范不发事件，只把攒着的字段清掉
            if (m_dataBuffer.empty())
            {
                m_eventType.clear();
                m_hasRetry          = false;
                m_retryMilliseconds = 0;
                return;
            }
            SseEvent event;
            event.type              = m_eventType;
            event.data              = m_dataBuffer;
            event.hasRetry          = m_hasRetry;
            event.retryMilliseconds = m_retryMilliseconds;
            // last event ID 是流内的状态：给过一次之后，后面没写 id 的事件仍带着它（规范的 buffer
            // 独立于单条事件，这里照做，故下面清空攒着的字段时不动它）
            event.lastEventId = m_pendingId;
            // 最后一条 data 后面那个换行是拼接留下的，不属于正文
            event.data.pop_back();
            static_cast<void>(m_readyEvents.emplace_back(std::move(event)));
            m_dataBuffer.clear();
            m_eventType.clear();
            m_hasRetry          = false;
            m_retryMilliseconds = 0;
            return;
        }
        if (line.front() == ':')
        {
            // 注释行（服务端用它做心跳）：按规范整行忽略，不派发也不计数
            return;
        }

        const std::size_t      colonPosition = line.find(':');
        const std::string_view fieldName     = colonPosition == std::string_view::npos ? line : line.substr(0, colonPosition);
        const std::string_view fieldValue    = colonPosition == std::string_view::npos ? std::string_view{} : stripOneLeadingSpace(line.substr(colonPosition + 1));

        if (fieldName == "event")
        {
            m_eventType.assign(fieldValue);
            return;
        }
        if (fieldName == "data")
        {
            m_dataBuffer.append(fieldValue);
            m_dataBuffer.push_back('\n');
            return;
        }
        if (fieldName == "id")
        {
            // 含 NUL 的 id 整条忽略：那值往别处一放就成了截断的字符串，静默收下比拒绝更糟
            if (fieldValue.find('\0') == std::string_view::npos)
            {
                m_pendingId.assign(fieldValue);
            }
            return;
        }
        if (fieldName == "retry")
        {
            if (const auto milliseconds = parseRetryMilliseconds(fieldValue); milliseconds.has_value())
            {
                m_hasRetry          = true;
                m_retryMilliseconds = *milliseconds;
            }
            return;
        }
        // 未知字段：忽略（规范就是这么规定的，未来的字段名不该让这条流报错）
    }

} // namespace AsynGyanis::Net
