#include "Net/WebSocket/WebSocketPeer.h"

#include "Net/Http/HttpServerStats.h"
#include "Net/WebSocket/PerMessageDeflate.h"
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
        // 负载层的失败在本层（会话侧），此刻解码器并没有失败，因此先判它：具体回 1007 还是 1002
        // 由记下这条原因的那一处决定（文本非法 1007、压缩解不开 1002）
        if (!m_payloadErrorMessage.empty())
        {
            return m_payloadErrorCloseCode;
        }

        // 解码器的失败里只有「超限」需要与协议违规分开：超限是体量问题，格式本身合法
        return m_decoder.isLimitExceeded() ? kWebSocketMessageTooBigCode : kWebSocketProtocolErrorCode;
    }

    void WebSocketPeer::setPerMessageDeflateEnabled(const bool enabled) noexcept
    {
        // 收发两侧一起开关：只开一半会让本端按压缩发、按明文收（或反之），线上必然对不上
        m_isPerMessageDeflateEnabled = enabled;
        m_decoder.setPerMessageDeflateEnabled(enabled);
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
        // 本侧已收口就不再发数据帧：RFC 6455 要求发起关闭之后不得再发任何数据。
        // 短路返回不记日志——收口那一刻已记过原因（协议错误、业务异常、传输失败），
        // 这里再记一条只会把同一件事刷成两行，调用方按 false 收手即可
        if (!m_isOpen)
        {
            co_return false;
        }
        co_return co_await sendFrame(WebSocketOpCode::Text, text);
    }

    Core::Task<bool> WebSocketPeer::sendBinary(const std::string_view payload)
    {
        // 短路返回不记日志：理由同 sendText()
        if (!m_isOpen)
        {
            co_return false;
        }
        co_return co_await sendFrame(WebSocketOpCode::Binary, payload);
    }

    Core::Task<bool> WebSocketPeer::sendPing(const std::string_view payload)
    {
        // 短路返回不记日志：理由同 sendText()
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
        // 也绝不把业务给的关闭原因当成一次新的关闭请求发出去。
        // 短路返回不记日志：本侧主动关闭属预期路径，传输失败则早已在收口那一刻记过原因
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
        // 压缩只对数据消息生效（控制帧从不压缩，RFC 7692 §6.1）；压不动就原样发——
        // 压缩是带宽优化，而发一条对端解不开的帧比不压严重得多
        const bool isDataMessage = opCode == WebSocketOpCode::Text || opCode == WebSocketOpCode::Binary;
        std::optional<std::string> compressedPayload;
        if (m_isPerMessageDeflateEnabled && isDataMessage)
        {
            compressedPayload = deflateWebSocketMessage(payload);
        }
        const std::string_view payloadToSend = compressedPayload.has_value() ? std::string_view(*compressedPayload) : payload;

        // 编码结果按值持有：它是本次 co_await 期间发送回调所读字节的唯一来源
        const std::string frameBytes = encodeWebSocketFrame(opCode, payloadToSend, true, compressedPayload.has_value());

        // 置位「有一帧在写」：会话收尾据此避免把自己的 Close 插进这次写里（两条写路径的字节会互相穿插）
        m_isWriteInFlight = true;
        const bool isSucceeded = co_await m_frameSender(frameBytes);
        m_isWriteInFlight = false;

        if (!isSucceeded)
        {
            // 写不出去即连接不可用：本侧随即收口，后续 send*()/close() 一律短路返回 false 且不再记日志
            // （本次失败的原因已由发送回调记下），receive() 返回空
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
        m_payloadErrorCloseCode = kWebSocketInvalidPayloadDataCode;

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

                // 压缩消息先解压再交付（RFC 7692 §7.2.2）：解不开的连接按协议错误收口——
                // 字节流本端解不了，留在连接上只会越走越偏
                if (frame.isCompressed)
                {
                    std::optional<std::string> inflatedPayload =
                            inflateWebSocketMessage(frame.payload, WebSocketFrameDecoder::kMaximumMessagePayloadLength);
                    if (!inflatedPayload.has_value())
                    {
                        m_payloadErrorMessage = std::format("压缩消息解压失败，或解压结果超过上限 {} 字节（RFC 7692 §7.2.2）："
                                                            "请检查对端的压缩实现，或改用未压缩帧发送",
                                                            WebSocketFrameDecoder::kMaximumMessagePayloadLength);
                        m_payloadErrorCloseCode = kWebSocketProtocolErrorCode;
                        return WebSocketFeedStatus::DecodeError;
                    }
                    frame.payload = std::move(*inflatedPayload);
                }

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
                        m_payloadErrorCloseCode = kWebSocketInvalidPayloadDataCode;
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
