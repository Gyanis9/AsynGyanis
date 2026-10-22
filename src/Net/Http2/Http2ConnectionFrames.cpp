/**
 * @file Http2ConnectionFrames.cpp
 * @brief HTTP/2 连接层的入站帧分派与逐类帧处理
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 帧分派、DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PING/GOAWAY/WINDOW_UPDATE/
 *          CONTINUATION 的校验与状态推进都在这里；头块组装、请求头语义校验与响应发送留在
 *          Http2Connection.cpp。两侧只通过 Http2Connection 的成员与 Http2Frame 的解析器交互。
 */

#include "Net/Http2/Http2Connection.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 9113 §6 定义过的帧类型取值上界（CONTINUATION = 0x9）：大于它的按 §4.1 忽略
        constexpr std::uint8_t kLastKnownFrameTypeValue = 0x9;
    } // namespace

    bool Http2Connection::handleFrame(Http2Frame frame)
    {
        const Http2FrameType frameType      = frame.header.type;
        const std::uint8_t   frameTypeValue = static_cast<std::uint8_t>(frameType);

        // 头块拼接期间只允许同流的 CONTINUATION（§6.10）：任何插入都会让拼出的头块与 HPACK 上下文错位
        if (m_isAssemblingHeaderBlock)
        {
            if (frameTypeValue != static_cast<std::uint8_t>(Http2FrameType::Continuation) || frame.header.streamId != m_pendingHeaderStreamId)
            {
                return failProtocol(std::format("流 {} 的头块还没收完（缺 END_HEADERS），此刻收到{}（流 {}）：RFC 9113 §6.10 要求 CONTINUATION "
                                                "不得被任何其它帧打断",
                                                m_pendingHeaderStreamId, http2FrameTypeName(frameType), frame.header.streamId));
            }
            return handleContinuationFrame(frame);
        }

        // 前奏之后的第一个帧必须是客户端自己的 SETTINGS（§3.4）：ACK 不是前奏的一部分
        if (m_state == Http2ConnectionState::AwaitingSettings &&
            (frameTypeValue != static_cast<std::uint8_t>(Http2FrameType::Settings) || (frame.header.flags & kHttp2FlagAcknowledge) != 0))
        {
            return failProtocol(std::format("客户端前奏之后的第一个帧必须是 SETTINGS（RFC 9113 §3.4），收到{}（流 {}）", http2FrameTypeName(frameType), frame.header.streamId));
        }

        // 未定义的类型按 §4.1 忽略：不改变任何状态，也不回帧
        if (frameTypeValue > kLastKnownFrameTypeValue)
        {
            return true;
        }

        switch (frameType)
        {
            case Http2FrameType::Data:
                return handleDataFrame(std::move(frame));
            case Http2FrameType::Headers:
                return handleHeadersFrame(frame);
            case Http2FrameType::Priority:
                return handlePriorityFrame(frame);
            case Http2FrameType::RstStream:
                return handleRstStreamFrame(frame);
            case Http2FrameType::Settings:
                return handleSettingsFrame(frame);
            case Http2FrameType::PushPromise:
                // 本端是服务端：客户端不得发推送承诺（§8.2），收到即协议违规
                fail(Http2ErrorCode::ProtocolError, std::format("收到流 {} 的 PUSH_PROMISE：RFC 9113 §8.4 只允许服务端发送推送承诺，客户端发送即连接错误", frame.header.streamId));
                return false;
            case Http2FrameType::Ping:
                return handlePingFrame(frame);
            case Http2FrameType::GoAway:
                return handleGoAwayFrame(frame);
            case Http2FrameType::WindowUpdate:
                return handleWindowUpdateFrame(frame);
            case Http2FrameType::Continuation:
                return handleContinuationFrame(frame);
        }
        return true;
    }

    bool Http2Connection::handleDataFrame(const Http2Frame &frame)
    {
        return handleDataFrame(Http2Frame(frame));
    }

    bool Http2Connection::handleDataFrame(Http2Frame &&frame)
    {
        const std::uint32_t streamId = frame.header.streamId;
        // 流控按 DATA 帧整段负载记账（RFC 9113 §6.9.1）：含帧层剥掉的 padding，也含随后会被忽略
        // 或判错的那些帧。对端已经按自己的账扣过一次，本端不记就是漏账。这个数要在净负载被移走
        // 之前取：帧头虽然不受移动影响，但放在解析之前读更不易被后来的改动弄错
        const std::int64_t frameByteCount = static_cast<std::int64_t>(frame.header.payloadLength);
        std::string        errorText;
        if (!acceptPeerStreamIdParity(streamId, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, std::move(errorText));
            return false;
        }

        Http2DataPayload payload;
        if (!parseHttp2DataPayload(std::move(frame), payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }

        m_connectionReceiveWindowByteCount -= frameByteCount;
        if (m_connectionReceiveWindowByteCount < 0)
        {
            fail(Http2ErrorCode::FlowControlError, std::format("连接级接收窗口被突破：对端在本端通告的窗口只剩 {} 字节时又发出了 {} 字节的 DATA（RFC 9113 §6.9.1），"
                                                               "请检查对端的窗口记账",
                                                               m_connectionReceiveWindowByteCount + frameByteCount, frameByteCount));
            return false;
        }

        StreamRecord *const stream = findStream(streamId);
        if (stream == nullptr)
        {
            if (streamId <= m_highestPeerStreamId && m_hasEvictedTerminatedStreamRecord)
            {
                // 记录已被挤出（已终止流只保留最近 kTerminatedStreamMemoryCount 条）：对端确实开过这条流，
                // 在终止流上补发的在途 DATA 允许直接忽略——把连接级窗口还回去，不当连接错误
                creditConnectionReceiveWindow(static_cast<std::size_t>(frameByteCount));
                return true;
            }
            // 从未开启的流上出现 DATA：§5.1「idle」段只允许 HEADERS 与 PRIORITY。
            // 一条记录都没被挤掉时「流号不超过已用最大值而账本里又没有」就等于「这条流从未开过」，这种能证伪的违约帧不能替对端咽下去
            fail(Http2ErrorCode::ProtocolError,
                 std::format("流 {} 从未开启（idle），不能在该流上发 DATA（RFC 9113 §5.1）；本端已用过的最大对端流号是 {}", streamId, m_highestPeerStreamId));
            return false;
        }
        if (stream->state != Http2StreamState::Closed)
        {
            // 流级窗口同样按整段负载扣：负数说明对端突破了本端通告的 SETTINGS_INITIAL_WINDOW_SIZE
            stream->receiveWindowByteCount -= frameByteCount;
            if (stream->receiveWindowByteCount < 0)
            {
                fail(Http2ErrorCode::FlowControlError, std::format("流 {} 的接收窗口被突破：对端在窗口只剩 {} 字节时又发出了 {} 字节的 DATA（RFC 9113 §6.9.1）", streamId,
                                                                   stream->receiveWindowByteCount + frameByteCount, frameByteCount));
                return false;
            }
        }
        if (stream->state == Http2StreamState::Closed)
        {
            const TerminatedStreamFrameVerdict verdict = judgeFrameOnTerminatedStream(*stream, Http2FrameType::Data);
            if (verdict != TerminatedStreamFrameVerdict::FailConnection)
            {
                // 这片数据不会再交给上层（流已终止），就当它已被消费：把连接级窗口还回去，
                // 否则对端在终止流上补发的在途 DATA 会永久吃掉连接窗口（§5.1「closed」段末段）
                creditConnectionReceiveWindow(static_cast<std::size_t>(frameByteCount));
            }
            if (verdict == TerminatedStreamFrameVerdict::Ignore)
            {
                return true;
            }
            if (verdict == TerminatedStreamFrameVerdict::ResetStream)
            {
                // 对端自己复位过这条流，其后又发 DATA：TCP 保证这片排在它那枚 RST 之后，按流错误回敬
                failStream(*stream, Http2ErrorCode::StreamClosed,
                           std::format("流 {} 已被对端的 RST_STREAM 终止，其后又收到 DATA：RFC 9113 §5.1「closed」段"
                                       "要求按流错误 STREAM_CLOSED 处理",
                                       streamId));
                return true;
            }
            fail(Http2ErrorCode::StreamClosed,
                 std::format("流 {} 已正常终止（双向 END_STREAM），再收到 DATA：RFC 9113 §5.1「closed」段要求按连接错误 STREAM_CLOSED 处理", streamId));
            return false;
        }
        if (stream->state == Http2StreamState::HalfClosedRemote)
        {
            // 对端自己收过尾了：再发 DATA 是流错误，只关掉这条流（§5.1）
            failStream(*stream, Http2ErrorCode::StreamClosed, std::format("流 {} 的对端已 END_STREAM（half-closed (remote)），不能再发 DATA", streamId));
            return true;
        }

        // 正常正文：收下并交给上层；END_STREAM 落在这一片上时推进流状态
        Http2ReceivedData receivedData;
        receivedData.streamId  = streamId;
        receivedData.data      = std::move(payload.data);
        receivedData.endStream = payload.endStream;
        // 交付时带上整段负载的字节数：上层按它报消费量，窗口账才不会漏掉 padding
        receivedData.flowControlByteCount = static_cast<std::size_t>(frameByteCount);
        m_pendingReceivedData.push_back(std::move(receivedData));
        if (payload.endStream)
        {
            noteRemoteEndStream(*stream);
        }
        return true;
    }

    bool Http2Connection::handleHeadersFrame(const Http2Frame &frame)
    {
        const std::uint32_t streamId = frame.header.streamId;
        std::string         errorText;
        Http2HeadersPayload payload;
        if (!parseHttp2HeadersPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }
        if (!acceptPeerStreamIdParity(streamId, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, std::move(errorText));
            return false;
        }

        StreamRecord *const stream = findStream(streamId);
        if (stream == nullptr)
        {
            // 新流号必须严格大于所有已用过的流号（§5.1.1）：倒退或重用一律判错
            if (streamId <= m_highestPeerStreamId)
            {
                fail(Http2ErrorCode::ProtocolError, std::format("流号 {} 不是新流（本端已用过的最大对端流号是 {}）：RFC 9113 §5.1.1 要求新流的流号严格递增，"
                                                                "请检查对端是否重用了流号",
                                                                streamId, m_highestPeerStreamId));
                return false;
            }
            // 连接进入关闭中之后不再受理新流（§6.8）：明确回 REFUSED_STREAM，让对端知道这条流没有被处理
            if (m_state == Http2ConnectionState::Closing)
            {
                refuseNewStream(streamId, std::format("流 {} 是连接进入关闭中之后新开的流（本端已发收尾 GOAWAY 或已收到对端 GOAWAY，"
                                                      "RFC 9113 §6.8：GOAWAY 之后不得再开新流）",
                                                      streamId));
                return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment, payload.endHeaders);
            }
            // 并发上限（§5.1.2）：回 REFUSED_STREAM，连接继续为其它流服务
            if (m_openStreamCount >= m_configuration.maximumConcurrentStreams)
            {
                refuseNewStream(streamId, std::format("并发流数已达上限 {}（SETTINGS_MAX_CONCURRENT_STREAMS，RFC 9113 §5.1.2）", m_configuration.maximumConcurrentStreams));
                return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment, payload.endHeaders);
            }
            openStream(streamId);
            return beginHeaderBlock(streamId, HeaderBlockPurpose::Request, payload.endStream, payload.headerBlockFragment, payload.endHeaders);
        }

        if (stream->state == Http2StreamState::Closed)
        {
            const TerminatedStreamFrameVerdict verdict = judgeFrameOnTerminatedStream(*stream, Http2FrameType::Headers);
            if (verdict == TerminatedStreamFrameVerdict::FailConnection)
            {
                fail(Http2ErrorCode::StreamClosed,
                     std::format("流 {} 已正常终止（双向 END_STREAM），再收到 HEADERS：RFC 9113 §5.1「closed」段要求按连接错误 STREAM_CLOSED 处理", streamId));
                return false;
            }
            if (verdict == TerminatedStreamFrameVerdict::ResetStream)
            {
                // 对端自己复位过这条流，其后又发头块：同 DATA 那一支，按流错误 STREAM_CLOSED 回敬
                failStream(*stream, Http2ErrorCode::StreamClosed,
                           std::format("流 {} 已被对端的 RST_STREAM 终止，其后又收到 HEADERS：RFC 9113 §5.1「closed」段"
                                       "要求按流错误 STREAM_CLOSED 处理",
                                       streamId));
            }
            // 终止过的流上补发的 HEADERS：字段已经没有归属，但字节必须解码——带增量索引的表示已经改动了
            // 对端编码器的动态表，跳过一次解码会让后续头块的索引整体错位
            return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment, payload.endHeaders);
        }
        if (stream->state == Http2StreamState::HalfClosedRemote)
        {
            // 对端收过尾了：再发头块是流错误（§5.1「half-closed (remote)」），只关掉这条流
            failStream(*stream, Http2ErrorCode::StreamClosed, std::format("流 {} 的对端已 END_STREAM（half-closed (remote)），不能再发 HEADERS", streamId));
            return true;
        }
        // 尾部头块必须带 END_STREAM：RFC 9113 §8.1 说得很直白——「trailer fields comprise a sequence
        // starting with a HEADERS frame, followed by zero or more CONTINUATION frames, where the
        // HEADERS frame bears an END_STREAM flag」，紧接着「An endpoint that receives a HEADERS frame
        // without the END_STREAM flag set after receiving the HEADERS frame that opens a request …
        // MUST treat the corresponding request or response as malformed」，而畸形报文按 §8.1.1 是流错误
        // PROTOCOL_ERROR。不判的话这条流只会一直等一个再也不会来的收尾。
        if (!payload.endStream)
        {
            failStream(*stream, Http2ErrorCode::ProtocolError,
                       std::format("流 {} 的第二个 HEADERS 未带 END_STREAM：尾部头块必须同时终结这条流"
                                   "（RFC 9113 §8.1，判为畸形报文）",
                                   streamId));
            // 字节照样要解码：动态表是连接级状态，跳过这一块会让后续头块的索引整体错位
            return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment, payload.endHeaders);
        }
        // 收尾齐了才是合法的尾部头块（§8.1）：字段随收口信号交给上层，字节同样必须解码完整，否则动态表
        // 与对端编码器错位
        return beginHeaderBlock(streamId, HeaderBlockPurpose::Trailers, payload.endStream, payload.headerBlockFragment, payload.endHeaders);
    }

    bool Http2Connection::handlePriorityFrame(const Http2Frame &frame)
    {
        std::string errorText;
        // 本端不推送：偶数流号属于服务端发起的方向，收到即 §5.1.1 的意外流号
        if (!acceptPeerStreamIdParity(frame.header.streamId, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, std::move(errorText));
            return false;
        }
        // PRIORITY 允许出现在任意状态的流上（含 idle 与 closed，§5.3）：本片不建优先级树，一律忽略且不建流
        return true;
    }

    bool Http2Connection::consumeControlFrameBudget(const std::string_view frameKindName)
    {
        // 回填单位恰好对齐：每秒的帧数 == 每毫秒的毫帧数，整数运算即可，不需要浮点
        const auto now                  = std::chrono::steady_clock::now();
        const auto elapsedMilliseconds  = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_controlFrameBucketRefreshedAt).count();
        m_controlFrameBucketRefreshedAt = now;
        m_controlFrameBucketMilliFrames =
                std::min(kControlFrameBucketCapacityMilliFrames, m_controlFrameBucketMilliFrames + elapsedMilliseconds * kControlFrameBucketRefillPerSecondFrames);

        if (m_controlFrameBucketMilliFrames < 1000)
        {
            fail(Http2ErrorCode::EnhanceYourCalm, std::format("对端持续发送控制帧（{}）越过每连接频次额度：RFC 9113 §10.5 要求跟踪并限制 PING/SETTINGS/RST_STREAM 的使用，"
                                                              "本端按 ENHANCE_YOUR_CALM 收口（额度：容量 {} 帧、每秒回填 {} 帧）",
                                                              frameKindName, kControlFrameBucketCapacityMilliFrames / 1000, kControlFrameBucketRefillPerSecondFrames));
            return false;
        }
        m_controlFrameBucketMilliFrames -= 1000;
        return true;
    }

    bool Http2Connection::handleRstStreamFrame(const Http2Frame &frame)
    {
        // 先记频次再谈内容：洪泛判决与帧本身合不合规无关（RFC 9113 §10.5 的 SHOULD 按用量而不是按对错）
        if (!consumeControlFrameBudget("RST_STREAM"))
        {
            return false;
        }
        const std::uint32_t streamId = frame.header.streamId;
        std::string         errorText;
        if (!acceptPeerStreamIdParity(streamId, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, std::move(errorText));
            return false;
        }
        Http2RstStreamPayload payload;
        if (!parseHttp2RstStreamPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }

        StreamRecord *const stream = findStream(streamId);
        if (stream == nullptr)
        {
            if (streamId <= m_highestPeerStreamId && m_hasEvictedTerminatedStreamRecord)
            {
                // 记录已被挤出：该流开过（终止记录只保留最近一批），§5.1「closed」段要求忽略
                // RST_STREAM——对端可能还没看到本端终止它的那一帧
                return true;
            }
            // idle 流上只允许 HEADERS 与 PRIORITY（§5.1）：对从未开启的流发 RST_STREAM 是连接错误。
            // 账本还完整（没挤掉过记录）时这里判得准，见 handleData 同一处说明
            fail(Http2ErrorCode::ProtocolError, std::format("收到流 {} 的 RST_STREAM，但该流从未开启（idle）：RFC 9113 §5.1 只允许在 idle 流上发 HEADERS 与 PRIORITY", streamId));
            return false;
        }
        if (stream->state == Http2StreamState::Closed)
        {
            // 已终止的流：RST_STREAM 按 §5.1「closed」段必须忽略（对端可能还没看到终止它的那一帧）
            return true;
        }
        // 对端主动取消（§5.4.2）：终止该流，队列里没发出去的正文一并丢掉。终止方式要按方向记：
        // 「本端发过 RST」之后的帧一律忽略，「收到对端 RST」之后的帧则要按流错误回敬（§5.1「closed」段）
        terminateStream(*stream, StreamTermination::ResetByPeer);
        return true;
    }

    bool Http2Connection::handleSettingsFrame(const Http2Frame &frame)
    {
        // 每帧 SETTINGS（含 ACK）都要花本端的处理时间并要求回 ACK：频次同样按用量记（§10.5）
        if (!consumeControlFrameBudget("SETTINGS"))
        {
            return false;
        }
        Http2SettingsPayload payload;
        std::string          errorText;
        if (!parseHttp2SettingsPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }

        if (payload.isAcknowledgement)
        {
            // ACK 只允许匹配一次：没有待确认的 SETTINGS 就是多余 ACK（§6.5.3 的确认语义）
            if (m_outstandingSettingsCount == 0)
            {
                fail(Http2ErrorCode::ProtocolError, "收到多余的 SETTINGS ACK：本端没有待确认的 SETTINGS（RFC 9113 §6.5.3 规定每个 SETTINGS 只回一个 ACK）");
                return false;
            }
            --m_outstandingSettingsCount;
            return true;
        }

        if (!applyPeerSettings(payload))
        {
            return false;
        }
        // 收到非 ACK 的 SETTINGS 必须回一个空负载 ACK（§6.5.3）
        appendOutgoing(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}));
        // 前奏之后的第一个 SETTINGS 收下，连接才算协商完成
        if (m_state == Http2ConnectionState::AwaitingSettings)
        {
            m_state = Http2ConnectionState::Open;
        }
        // 参数落定之后（可能是窗口或分片上限变大）再续发：ACK 排在被唤醒的正文之前
        pumpAllSendQueues();
        return true;
    }

    bool Http2Connection::handlePingFrame(const Http2Frame &frame)
    {
        // 非 ACK 的 PING 强制本端生成回帧，是最便宜的放大器：频次先记再谈应答（§10.5）
        if (!consumeControlFrameBudget("PING"))
        {
            return false;
        }
        Http2PingPayload payload;
        std::string      errorText;
        if (!parseHttp2PingPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }
        // 本片从不发 PING：收到的 ACK 只可能是对从未发出的探测的应答，§6.7 没要求判错，忽略即可
        if (payload.isAcknowledgement)
        {
            return true;
        }
        // 非 ACK 的 PING 必须把 8 字节原样回声（§6.7）
        appendOutgoing(encodeHttp2PingFrame(Http2PingPayload{.isAcknowledgement = true, .opaqueData = payload.opaqueData}));
        return true;
    }

    bool Http2Connection::handleGoAwayFrame(const Http2Frame &frame)
    {
        Http2GoAwayPayload payload;
        std::string        errorText;
        if (!parseHttp2GoAwayPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }
        // 记下对端通告并转入 Closing：不再受理新流，既有流继续做完（§6.8）
        m_hasPeerGoAway = true;
        m_peerGoAway    = payload;
        if (m_state == Http2ConnectionState::Open)
        {
            m_state = Http2ConnectionState::Closing;
        }
        return true;
    }

    bool Http2Connection::handleWindowUpdateFrame(const Http2Frame &frame)
    {
        Http2WindowUpdatePayload payload;
        std::string              errorText;
        if (!parseHttp2WindowUpdatePayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }

        // 连接级窗口只受 WINDOW_UPDATE 影响，与 SETTINGS_INITIAL_WINDOW_SIZE 无关（§6.9.2）
        if (frame.header.streamId == 0)
        {
            if (!increaseConnectionSendWindow(payload.windowSizeIncrement))
            {
                return false;
            }
            // 连接级窗口变大可能解开被它卡住的流：就地把队列里排着的正文发出去
            pumpAllSendQueues();
            return true;
        }

        const std::uint32_t streamId = frame.header.streamId;
        if (!acceptPeerStreamIdParity(streamId, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, std::move(errorText));
            return false;
        }

        StreamRecord *const stream = findStream(streamId);
        if (stream == nullptr)
        {
            if (streamId <= m_highestPeerStreamId && m_hasEvictedTerminatedStreamRecord)
            {
                // 记录已被挤出：与「已终止流」同一处置（§5.1「closed」段要求忽略 WINDOW_UPDATE）——
                // 记成连接错误会把一条合法的连接整条打掉
                return true;
            }
            // 从未开启的流上出现 WINDOW_UPDATE：同样是 §5.1「idle」段的连接错误（与下面「已终止流要忽略」不同）
            fail(Http2ErrorCode::ProtocolError,
                 std::format("收到流 {} 的 WINDOW_UPDATE，但该流从未开启（idle）：RFC 9113 §5.1 只允许在 idle 流上发 HEADERS 与 PRIORITY", streamId));
            return false;
        }
        if (stream->state == Http2StreamState::Closed)
        {
            // 已终止流：§5.1「closed」段明确要求忽略 WINDOW_UPDATE（对端可能还没看到终止它的那一帧）
            return true;
        }
        if (!increaseStreamSendWindow(*stream, payload.windowSizeIncrement))
        {
            // 溢出已按 §6.9.1 在这条流上了结（RST_STREAM FLOW_CONTROL_ERROR）：连接要继续跑，
            // 所以这里返回 true 而不是把整条连接的解码循环打断
            return true;
        }
        pumpSendQueue(*stream);
        return true;
    }

    bool Http2Connection::handleContinuationFrame(const Http2Frame &frame)
    {
        // 走到这里还没在拼头块，说明这条 CONTINUATION 没有前置的 HEADERS（§6.10）
        if (!m_isAssemblingHeaderBlock)
        {
            fail(Http2ErrorCode::ProtocolError,
                 std::format("收到没有前置 HEADERS 的 CONTINUATION（流 {}）：RFC 9113 §6.10 要求它紧跟在同一个流的 HEADERS 之后", frame.header.streamId));
            return false;
        }
        Http2ContinuationPayload payload;
        std::string              errorText;
        if (!parseHttp2ContinuationPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }
        if (!appendHeaderBlockFragment(payload.headerBlockFragment))
        {
            return false;
        }
        if (payload.endHeaders)
        {
            return finishHeaderBlock();
        }
        return true;
    }
} // namespace AsynGyanis::Net
