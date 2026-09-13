#include "Net/WebSocket/WebSocketPeer.h"

#include "Net/Http/HttpServerStats.h"
#include "Net/WebSocket/WebSocketUtf8.h"

#include <cstdint>
#include <format>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 关闭帧负载里状态码占用的字节数（RFC 6455 §5.5.1）
        constexpr std::size_t kCloseCodeByteLength = 2;
    } // namespace

    WebSocketPeer::WebSocketPeer(FrameSender frameSender, HttpMetricsCollector *const metrics) :
        m_frameSender(std::move(frameSender)),
        m_metrics(metrics)
    {
        // 发送路径在构造时注入：本对象没有「未装配」状态，省掉每次 send*() 里的判空分支。
        // 采集端可空：空表示只跑协议不上报统计，因此每个上报点都判一次空
    }

    bool WebSocketPeer::isOpen() const noexcept
    {
        return m_isOpen;
    }

    bool WebSocketPeer::isWriteInFlight() const noexcept
    {
        return m_isWriteInFlight;
    }

    std::uint16_t WebSocketPeer::decodeErrorCloseCode() const noexcept
    {
        // 负载非法的原因在本层（会话侧），此刻解码器并没有失败，因此先判它
        if (!m_payloadErrorMessage.empty())
        {
            return kWebSocketInvalidPayloadDataCode;
        }

        // 解码器的失败里只有「超限」需要与协议违规分开：超限是体量问题，格式本身合法
        return m_decoder.isLimitExceeded() ? kWebSocketMessageTooBigCode : kWebSocketProtocolErrorCode;
    }

    std::string WebSocketPeer::decodeErrorText() const
    {
        // 负载非法的原因不在解码器里，两者取其一即为「最近一次失败」的完整描述
        return m_payloadErrorMessage.empty() ? m_decoder.errorMessage() : m_payloadErrorMessage;
    }

    WebSocketPeer::DeliveryAwaiter::DeliveryAwaiter(WebSocketPeer &peer) noexcept :
        m_peer(&peer)
    {
    }

    bool WebSocketPeer::DeliveryAwaiter::await_ready() const noexcept
    {
        // 队列里已有消息、或连接已收口：两种情形都有确定结果，不必挂起
        return !m_peer->m_incomingFrames.empty() || !m_peer->m_isOpen;
    }

    void WebSocketPeer::DeliveryAwaiter::await_suspend(const std::coroutine_handle<> waiter) noexcept
    {
        // 登记等待者：会话交来消息或标记收口时恢复它。业务是顺序执行的，同一时刻只可能有一个等待者
        m_peer->m_deliveryWaiter = waiter;
    }

    std::optional<WebSocketFrame> WebSocketPeer::DeliveryAwaiter::await_resume()
    {
        // 无论走哪条路径都要清空登记：恢复方在恢复之前已经把它取走，这里是幂等的兜底
        m_peer->m_deliveryWaiter = nullptr;

        if (m_peer->m_incomingFrames.empty())
        {
            return std::nullopt;
        }

        WebSocketFrame frame = std::move(m_peer->m_incomingFrames.front());
        m_peer->m_incomingFrames.pop_front();
        return frame;
    }

    Core::Task<std::optional<WebSocketMessage>> WebSocketPeer::receive()
    {
        while (true)
        {
            const std::optional<WebSocketFrame> frame = co_await DeliveryAwaiter(*this);
            if (!frame.has_value())
            {
                // 连接收口：对端关闭、本侧已发起关闭或连接不可用
                co_return std::nullopt;
            }

            if (frame->opCode == WebSocketOpCode::Ping)
            {
                // 心跳应答（RFC 6455 §5.5.2）：负载必须原样回显，业务无需感知这次往返
                [[maybe_unused]] const bool isPongSent = co_await sendFrame(WebSocketOpCode::Pong, frame->payload);
                continue;
            }
            if (frame->opCode == WebSocketOpCode::Pong)
            {
                // 对端对我们心跳的回应，没有业务语义，直接忽略
                continue;
            }
            if (frame->opCode == WebSocketOpCode::Close)
            {
                // 对端发起关闭握手（RFC 6455 §5.5.1）：计入对端一侧，回一条同状态码的 Close 后终止交付
                //（那条回帧不再计入本侧发起，否则一次对端关闭会被算成两笔）
                if (m_metrics != nullptr)
                {
                    m_metrics->countWebSocketPeerClose();
                }
                co_await echoCloseFrame(frame->payload);
                co_return std::nullopt;
            }

            // 解码器只交付 Text 与 Binary：分片消息在解码层已重组，控制帧在上面三条分支里处理完
            co_return WebSocketMessage{.opCode = frame->opCode, .payload = std::move(frame->payload)};
        }
    }

    Core::Task<bool> WebSocketPeer::sendText(const std::string_view text)
    {
        // 本侧已收口就不再发数据帧：RFC 6455 要求发起关闭之后不得再发任何数据
        if (!m_isOpen)
        {
            co_return false;
        }
        co_return co_await sendFrame(WebSocketOpCode::Text, text);
    }

    Core::Task<bool> WebSocketPeer::sendBinary(const std::string_view payload)
    {
        if (!m_isOpen)
        {
            co_return false;
        }
        co_return co_await sendFrame(WebSocketOpCode::Binary, payload);
    }

    Core::Task<bool> WebSocketPeer::sendPing(const std::string_view payload)
    {
        if (!m_isOpen)
        {
            co_return false;
        }
        // 负载超长的用法错误由编码层当场拒绝，不在这里重复判一次
        co_return co_await sendFrame(WebSocketOpCode::Ping, payload);
    }

    Core::Task<bool> WebSocketPeer::close(const std::uint16_t code, const std::string_view reason)
    {
        constexpr std::size_t kMaximumReasonLength = kWebSocketMaximumControlPayloadLength - kCloseCodeByteLength;
        if (reason.size() > kMaximumReasonLength)
        {
            // 超长有意抛异常而不是截断：截断会悄悄改掉业务给出的原因，而这条帧正是对端判断
            // 「为什么被关」的唯一依据
            throw Base::InvalidArgumentException(
                    std::format("WebSocketPeer::close：关闭原因 {} 字节超过上限 {} 字节（控制帧整体不得超过 {} 字节，"
                                "其中状态码占 {} 字节，RFC 6455 §5.5）：请缩短原因，或把长说明改用 sendText() 作为消息发出",
                                reason.size(), kMaximumReasonLength, kWebSocketMaximumControlPayloadLength, kCloseCodeByteLength));
        }

        // 本侧已经发过 Close、或连接已不可用：不再补第二条（§5.5.1 只要求一次关闭握手），
        // 也绝不把业务给的关闭原因当成一次新的关闭请求发出去
        if (!m_isOpen)
        {
            co_return false;
        }

        // 状态先落定再发帧：写出期间协程会挂起，此时业务若再调 receive()/send*() 必须已经看到「已关闭」，
        // 否则它会去等一条永远不会再来的消息
        m_isOpen = false;

        // 本侧先发起关闭才计数：应答对端 Close 的那条回帧归在对端一侧（见 receive() 的 Close 分支）
        if (m_metrics != nullptr && !m_isEchoingPeerClose)
        {
            m_metrics->countWebSocketServerClose();
        }

        std::string payload;
        payload.reserve(kCloseCodeByteLength + reason.size());
        // 负载 = 2 字节大端状态码 + 原因（RFC 6455 §5.7.1）
        payload.push_back(static_cast<char>((code >> 8) & 0xFFU));
        payload.push_back(static_cast<char>(code & 0xFFU));
        payload.append(reason);

        co_return co_await sendFrame(WebSocketOpCode::Close, payload);
    }

    Core::Task<bool> WebSocketPeer::sendFrame(const WebSocketOpCode opCode, const std::string_view payload)
    {
        // 编码结果按值持有：它是本次 co_await 期间发送回调所读字节的唯一来源
        const std::string frameBytes = encodeWebSocketFrame(opCode, payload);

        // 置位「有一帧在写」：会话收尾据此避免把自己的 Close 插进这次写里（两条写路径的字节会互相穿插）
        m_isWriteInFlight = true;
        const bool isSucceeded = co_await m_frameSender(frameBytes);
        m_isWriteInFlight = false;

        if (!isSucceeded)
        {
            // 写不出去即连接不可用：本侧随即收口，后续 send*() 一律返回 false、receive() 返回空
            m_isOpen = false;
        }
        co_return isSucceeded;
    }

    Core::Task<> WebSocketPeer::echoCloseFrame(const std::string_view payload)
    {
        // 状态码 echo（RFC 6455 §5.5.1）：对端给了合法状态码就原样回送；
        // 负载为空或不足两字节表示「不带状态码的关闭」，此时按 1000 正常关闭回送
        std::uint16_t closeCode = kWebSocketNormalClosureCode;
        if (payload.size() >= kCloseCodeByteLength)
        {
            // 线上是大端：第一个字节是高位
            closeCode = static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) << 8) |
                                                   static_cast<std::uint16_t>(static_cast<unsigned char>(payload[1])));
        }

        // 回帧即收口。结果不看：对端往往已经断开，这条回帧写不出去也不影响收尾
        // 先立标记再回帧：本次关闭来自对端，计数归它那一侧，close() 据此不再记一次本侧发起
        m_isEchoingPeerClose = true;
        [[maybe_unused]] const bool isCloseSent = co_await close(closeCode);
        co_return;
    }

    void WebSocketPeer::enqueueFrame(WebSocketFrame frame)
    {
        // 已收口：对端在关闭握手中仍可能补发数据帧，此时没有接收方，直接丢弃而不是交给业务
        if (!m_isOpen)
        {
            return;
        }

        m_incomingFrames.push_back(std::move(frame));

        // 业务正挂在 receive() 上：就地恢复它，把控制权交给它——交付与唤醒都由会话侧驱动，
        // 业务侧的任一挂起点（收消息、发帧）都在本对象的调用链上
        if (m_deliveryWaiter != nullptr)
        {
            const std::coroutine_handle<> waiter = std::exchange(m_deliveryWaiter, nullptr);
            waiter.resume();
        }
    }

    WebSocketFeedStatus WebSocketPeer::feedBytes(const char *const data, const std::size_t length)
    {
        // 已收口就不再解码：Close 之后对端仍可能补帧，解出来也没有接收方，只是白占内存
        if (!m_isOpen)
        {
            return WebSocketFeedStatus::Accepted;
        }

        // 进入本次调用先清空上一次的负载错误：decodeErrorText() 与 decodeErrorCloseCode()
        // 读到的是「最近一次失败」，留着旧原因会把本次结论误导成上一次的
        m_payloadErrorMessage.clear();

        std::size_t offset = 0;
        while (offset < length)
        {
            const WebSocketDecodeStatus status = m_decoder.parse(data + offset, length - offset);
            offset += m_decoder.consumedByteCount();

            if (status == WebSocketDecodeStatus::Error)
            {
                // 原因与「是否超限」都留在解码器里，供会话按 decodeErrorCloseCode() 选状态码
                return WebSocketFeedStatus::DecodeError;
            }
            if (status == WebSocketDecodeStatus::Frame)
            {
                // 取走即清标记，解码器才能继续消费后面的字节（剩下的字节属于下一帧）
                WebSocketFrame frame = m_decoder.takeFrame();

                // 文本负载必须是合法 UTF-8（RFC 6455 §5.6，编码规则见 RFC 3629），校验点是
                // 「交给业务之前」的最后一步。解码层交付的是已重组的完整消息，因此按整条消息
                // 一次校验即可，不需要跨分片或跨帧的增量校验状态
                if (frame.opCode == WebSocketOpCode::Text)
                {
                    const std::size_t invalidByteOffset = findInvalidWebSocketUtf8ByteOffset(frame.payload);
                    if (invalidByteOffset != std::string_view::npos)
                    {
                        m_payloadErrorMessage =
                                std::format("文本帧负载不是合法 UTF-8（RFC 6455 §5.6 要求文本负载为 UTF-8，编码规则见 RFC 3629）："
                                            "第 {} 个字节起违规，常见原因有过长编码、代理区码点（U+D800~U+DFFF）与截断的多字节序列；"
                                            "请按 UTF-8 重新编码这段文本后重发",
                                            invalidByteOffset);
                        // 非法负载不交付业务：返回 DecodeError 让会话发 1007 并收口
                        return WebSocketFeedStatus::DecodeError;
                    }
                }

                // 一条完整的数据消息算一次：解码层已完成分片重组，故这里既是「重组后的那条」。
                // 控制帧（Ping/Pong/Close）同样走到这一步排队，因此按操作码过滤，不计入消息数
                if (m_metrics != nullptr &&
                    (frame.opCode == WebSocketOpCode::Text || frame.opCode == WebSocketOpCode::Binary))
                {
                    m_metrics->countWebSocketMessage();
                }

                enqueueFrame(std::move(frame));
                continue;
            }

            // NeedMore：本段字节已被解码器全部消费，等下一段网络字节
            break;
        }
        return WebSocketFeedStatus::Accepted;
    }

    void WebSocketPeer::markClosed() noexcept
    {
        // 有意不唤醒挂起中的 receive()：会话收尾时业务协程的帧会随之销毁，
        // 让它在别人的栈上接着跑没有意义
        m_isOpen = false;
    }
} // namespace AsynGyanis::Net
