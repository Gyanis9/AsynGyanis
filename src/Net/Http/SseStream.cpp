#include "Net/Http/SseStream.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        // SSE 行的固定片段：前缀写成具名常量，避免在拼帧处散落字面量
        constexpr std::string_view kEventFieldPrefix = "event: ";   ///< event 字段行前缀
        constexpr std::string_view kIdFieldPrefix = "id: ";         ///< id 字段行前缀
        constexpr std::string_view kRetryFieldPrefix = "retry: ";   ///< retry 字段行前缀
        constexpr std::string_view kDataLinePrefix = "data: ";      ///< data 行的行前缀
        constexpr std::string_view kCommentLinePrefix = ": ";       ///< 注释行的行前缀

        /// SSE 的行分隔符固定为 LF（不是 HTTP 报文的 CRLF）：线格式按 LF 定界，收端也按 LF 解析
        constexpr char kLineFeed = '\n';

        /**
         * @brief 判断单行字段值里是否出现 CR、LF 或 NUL
         * @details 这三个字符会让字段行提前结束或让收端截断，单行字段必须整行可写。
         * @param value 待检查的字段值
         * @return true 含禁止字符
         */
        bool containsLineBreakOrNul(const std::string_view value) noexcept
        {
            // 显式给出长度 3：NUL 无法靠零终止表达，必须按「指针 + 长度」比较
            constexpr std::string_view kForbiddenCharacters("\r\n\0", 3);
            return value.find_first_of(kForbiddenCharacters) != std::string_view::npos;
        }
    } // namespace

    SseStream::SseStream(HttpResponse &response) :
        m_response(response)
    {
        // 先进入流式模式再设头部：startChunkedResponse 会删掉调用方先设的 content-length，
        // 顺序反过来则连 SSE 需要的头部一起删掉。头部随首段正文上线，因此必须在这里定稿
        m_response.startChunkedResponse(200);

        // 只有这两条是 SSE 的协议要求。content-length 与 connection 一律不设：前者与 chunked
        // 互斥，后者由会话按 keep-alive 判定补齐。名与值都是本类给出的合法字面量，不会拒收
        m_response.setHeader("content-type", "text/event-stream");
        m_response.setHeader("cache-control", "no-cache");
    }

    bool SseStream::isOpen() const noexcept
    {
        return m_isOpen;
    }

    void SseStream::appendTextLines(std::string &target, const std::string_view linePrefix, const std::string_view value)
    {
        std::size_t lineBegin = 0;
        while (true)
        {
            std::size_t lineEnd = lineBegin;
            while (lineEnd < value.size() && value[lineEnd] != '\n' && value[lineEnd] != '\r')
            {
                ++lineEnd;
            }

            // 前缀 + 本段 + LF：行内因此不会残留 \r
            target.append(linePrefix);
            target.append(value.substr(lineBegin, lineEnd - lineBegin));
            target.push_back(kLineFeed);

            // 没有更多换行：最后一段已写出，收工
            if (lineEnd >= value.size())
            {
                break;
            }

            // 跳过换行本身；\r\n 只算一个换行，裸 \r 同理
            lineBegin = lineEnd + 1;
            if (value[lineEnd] == '\r' && lineBegin < value.size() && value[lineBegin] == '\n')
            {
                ++lineBegin;
            }
        }
    }

    Core::Task<bool> SseStream::sendComment(const std::string_view comment)
    {
        // 连接已不可用：直接短路，此后连校验都不做 —— 对端收不到，报错只会误导调用方。
        // 短路不记日志：原因已在 writeChunk 首次返回 false 那一刻记过
        if (!m_isOpen)
        {
            co_return false;
        }

        // 超长输入先按长度拦一道：整帧必然超限，没必要为一个注定被拒的帧先分配一大块内存
        if (comment.size() > kMaximumFrameLength)
        {
            throw Base::LogicException("SseStream::sendComment：注释文本共 " + std::to_string(comment.size()) +
                                       " 字节，已超过单帧上限 " + std::to_string(kMaximumFrameLength) +
                                       " 字节；注释帧只用于心跳与调试，请缩短这条注释，"
                                       "或把长文本改用 sendEvent() 分批发送");
        }

        std::string frame;
        appendTextLines(frame, kCommentLinePrefix, comment);

        // 空行是帧结束的边界：注释虽无后续字段，收不到它这一帧就还没有派发
        frame.push_back(kLineFeed);

        if (frame.size() > kMaximumFrameLength)
        {
            throw Base::LogicException("SseStream::sendComment：本次注释帧共 " + std::to_string(frame.size()) +
                                       " 字节，超过单帧上限 " + std::to_string(kMaximumFrameLength) +
                                       " 字节；请缩短这条注释，或把长文本改用 sendEvent() 分批发送");
        }

        const bool isSent = co_await m_response.writeChunk(frame);
        if (!isSent)
        {
            // 粘滞关闭：此后所有发送接口直接短路，不再尝试写这条连接
            m_isOpen = false;
        }
        co_return isSent;
    }

    Core::Task<bool> SseStream::sendEvent(const std::string_view data, const std::string_view eventName,
                                          const std::string_view eventId,
                                          const std::optional<std::chrono::milliseconds> retry)
    {
        // 连接已不可用：直接短路，不写也不校验，也不记日志（理由同 sendComment()）
        if (!m_isOpen)
        {
            co_return false;
        }

        // event 与 id 是单行字段：CR/LF 会让这一行提前结束、NUL 会让收端截断，两者都会把后面的
        // data 行挤成另一个字段，宁可拒绝也不产出一帧收端读不懂的报文
        if (containsLineBreakOrNul(eventName))
        {
            throw Base::LogicException("SseStream::sendEvent：事件名（eventName）含有 CR、LF 或 NUL，"
                                       "写进 event: 行会让该行提前结束、后续 data 行被解析成别的字段；"
                                       "请改用不含换行与控制字符的事件名（如 progress）");
        }
        if (containsLineBreakOrNul(eventId))
        {
            throw Base::LogicException("SseStream::sendEvent：事件标识（eventId）含有 CR、LF 或 NUL，"
                                       "写进 id: 行会让该行提前结束并污染后续字段；"
                                       "请改用不含换行与控制字符的事件标识（如 42）");
        }

        // retry 是给客户端的重连建议值，负数属调用方取值错误，因此归入 logic_error 分支而非运行期故障
        if (retry.has_value() && retry->count() < 0)
        {
            throw Base::InvalidArgumentException("SseStream::sendEvent：重连间隔 retry 不能为负数，收到 " +
                                                 std::to_string(retry->count()) +
                                                 " 毫秒；retry 是给客户端的重连建议毫秒数，请传入非负值，"
                                                 "或传 std::nullopt 表示本次不带该字段");
        }

        // data 单独超限时整帧必然超限：先按长度拦一道，避免为一个注定被拒的帧先分配一大块内存
        if (data.size() > kMaximumFrameLength)
        {
            throw Base::LogicException("SseStream::sendEvent：事件数据共 " + std::to_string(data.size()) +
                                       " 字节，已超过单帧上限 " + std::to_string(kMaximumFrameLength) +
                                       " 字节；请把数据拆成多条小事件分别发送（客户端会按到达顺序逐条投递），"
                                       "或改用一次性普通响应下发这份内容");
        }

        // 字段顺序固定为 event → id → retry → data：顺序不影响语义，但固定下来帧内容才可逐字节复现；
        // 空值一律表示本次不带该字段，客户端不会看到空行
        std::string frame;
        if (!eventName.empty())
        {
            frame.append(kEventFieldPrefix);
            frame.append(eventName);
            frame.push_back(kLineFeed);
        }
        if (!eventId.empty())
        {
            frame.append(kIdFieldPrefix);
            frame.append(eventId);
            frame.push_back(kLineFeed);
        }
        if (retry.has_value())
        {
            frame.append(kRetryFieldPrefix);
            frame.append(std::to_string(retry->count()));
            frame.push_back(kLineFeed);
        }
        appendTextLines(frame, kDataLinePrefix, data);

        // 空行 = 帧结束：收端见到空行才把累积的字段派发成一个事件
        frame.push_back(kLineFeed);

        if (frame.size() > kMaximumFrameLength)
        {
            throw Base::LogicException("SseStream::sendEvent：本次事件帧共 " + std::to_string(frame.size()) +
                                       " 字节，超过单帧上限 " + std::to_string(kMaximumFrameLength) +
                                       " 字节；请把数据拆成多条小事件分别发送，或改用一次性普通响应下发这份内容");
        }

        const bool isSent = co_await m_response.writeChunk(frame);
        if (!isSent)
        {
            m_isOpen = false;
        }
        co_return isSent;
    }

} // namespace AsynGyanis::Net
