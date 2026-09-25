// HTTP/2 客户端连接层的实现

#include "Net/Http2/Http2ClientConnection.h"

#include "Base/Log/LogMacros.h"
#include "Net/Http/Client/RequestDeadlineGuard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一次从通路上读的字节数：半帧由解码器自己留着，与服务端侧同一档
        constexpr std::size_t kReadChunkByteCount = 8192;

        /// 协议规定的流控窗口初值（RFC 7540 §6.9.2）：SETTINGS 到达前两边都按它算
        constexpr std::int64_t kProtocolInitialWindowByteCount = 65535;

        /// 窗口能表示的上界（31 位）：加上增量越过它就是 §6.9.1 的流控错误
        constexpr std::int64_t kMaximumWindowByteCount = 2147483647;

        /**
         * @brief 从对端 SETTINGS 里取一个参数
         * @param payload 已解析的对端 SETTINGS
         * @param identifier 要取的参数标识
         * @return std::optional<std::uint32_t> 没通告时为空，调用方按协议初值兜底
         */
        std::optional<std::uint32_t> findSetting(const Http2SettingsPayload &payload,
                                                 const Http2SettingIdentifier identifier)
        {
            for (const Http2Setting &setting: payload.parameters)
            {
                if (static_cast<Http2SettingIdentifier>(setting.identifier) == identifier)
                {
                    return setting.value;
                }
            }
            return std::nullopt;
        }
    } // namespace

    Http2ClientConnection::Http2ClientConnection(Core::EventLoop &loop, std::unique_ptr<HttpOutboundConnection> transport,
                                                 const Config config)
        : m_loop(loop)
        , m_transport(std::move(transport))
        , m_config(config)
        , m_decoder(Http2FrameLimits{.maximumFrameSizeByteCount = config.maximumFrameByteSize})
    {
        m_connectionSendWindowByteCount = kProtocolInitialWindowByteCount;
        m_streamSendWindowByteCount = kProtocolInitialWindowByteCount;
    }

    bool Http2ClientConnection::isHealthy() const noexcept
    {
        return m_isHealthy && m_transport->isOpen();
    }

    void Http2ClientConnection::fail(std::string reason)
    {
        m_isHealthy = false;
        m_errorMessage = std::move(reason);
    }

    void Http2ClientConnection::appendOutgoing(std::string frameBytes)
    {
        m_outgoing.append(std::move(frameBytes));
    }

    void Http2ClientConnection::creditWindow(const std::uint32_t streamId, const std::uint32_t increment)
    {
        if (increment == 0U)
        {
            return;
        }
        // 消费即还窗口：流级与连接级各一条。少了连接级那条，对端会卡在一个早已耗尽的连接窗口上
        appendOutgoing(encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = increment}, streamId));
        appendOutgoing(encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = increment}, 0U));
    }

    std::string Http2ClientConnection::encodeLocalSettings() const
    {
        Http2SettingsPayload payload;
        payload.parameters.push_back(
                Http2Setting{.identifier = static_cast<std::uint16_t>(Http2SettingIdentifier::EnablePush), .value = 0U});
        payload.parameters.push_back(Http2Setting{.identifier = static_cast<std::uint16_t>(Http2SettingIdentifier::InitialWindowSize),
                                                  .value = m_config.initialWindowByteCount});
        payload.parameters.push_back(
                Http2Setting{.identifier = static_cast<std::uint16_t>(Http2SettingIdentifier::MaxFrameSize),
                             .value = m_config.maximumFrameByteSize});
        return encodeHttp2SettingsFrame(payload);
    }

    Core::Task<bool> Http2ClientConnection::flushOutgoing()
    {
        if (m_outgoing.empty())
        {
            co_return m_isHealthy;
        }
        std::string pending;
        pending.swap(m_outgoing);
        bool isWritten = false;
        try
        {
            isWritten = co_await m_transport->send(pending);
        } catch (const std::exception &)
        {
            // 通路抛异常（含被本端时限看门狗当场关掉）等于这条连接死了：折成「写失败」，绝不让异常
            // 穿过协程帧逃给调用方——帧就地销毁后调用方那次 co_await 永远等不到恢复，现场只表现为卡死
            isWritten = false;
        }
        if (!isWritten)
        {
            fail("写出 HTTP/2 帧失败：通路已不可用");
            co_return false;
        }
        co_return m_isHealthy;
    }

    Core::Task<bool> Http2ClientConnection::start(const std::chrono::milliseconds waitTimeout)
    {
        // 前奏与本端 SETTINGS 一次写出：前奏不是帧，但顺序上必须在 SETTINGS 之前，攒在一起既少一次
        // 系统调用也让 flushOutgoing 的异常收口只有一处
        appendOutgoing(std::string(kClientPrefaceBytes));
        appendOutgoing(encodeLocalSettings());
        if (!co_await flushOutgoing())
        {
            co_return false;
        }

        // 对端的 SETTINGS 是本端开始提请求的前提：它带着流初始窗口、帧上限与动态表大小，
        // 少一样都会让第一帧就按错的账发出去
        const RequestDeadlineGuard<Http2ClientConnection> deadline(m_loop, *this, waitTimeout, "Http2ClientConnection");
        while (isHealthy() && !m_isPeerSettingsReceived)
        {
            if (!co_await pumpSome())
            {
                break;
            }
        }
        if (!m_isPeerSettingsReceived)
        {
            fail(m_errorMessage.empty() ? "没等到对端的 SETTINGS：连接前奏没走完" : m_errorMessage);
        }
        co_return m_isPeerSettingsReceived && isHealthy();
    }

    Core::Task<bool> Http2ClientConnection::pumpSome()
    {
        std::array<char, kReadChunkByteCount> buffer{};
        ssize_t receivedByteCount = -1;
        try
        {
            receivedByteCount = co_await m_transport->receive(buffer.data(), buffer.size());
        } catch (const std::exception &)
        {
            // 与 flushOutgoing 同理：通路异常（含时限看门狗把套接字关掉）必须折成本端结论，
            // 否则异常穿过协程帧，调用方的 co_await 就再也回不来了
            fail(m_errorMessage.empty() ? "读对端字节异常：通路已不可用" : m_errorMessage);
            co_return false;
        }
        if (receivedByteCount < 0)
        {
            fail(m_errorMessage.empty() ? "读对端字节失败" : m_errorMessage);
            co_return false;
        }
        if (receivedByteCount == 0)
        {
            fail("对端已关闭这条 HTTP/2 连接");
            co_return false;
        }

        std::size_t offset = 0;
        const std::size_t readableByteCount = static_cast<std::size_t>(receivedByteCount);
        while (offset < readableByteCount && isHealthy())
        {
            const Http2FrameDecodeStatus status = m_decoder.parse(buffer.data() + offset, readableByteCount - offset);
            if (status == Http2FrameDecodeStatus::NeedMore)
            {
                // 本段字节已全部被解码器收进半成品帧，一帧也没凑齐：剩下的没有可解的了
                break;
            }
            if (status == Http2FrameDecodeStatus::Error)
            {
                fail("帧解码失败：" + m_decoder.errorMessage());
                co_return false;
            }
            // 帧就绪：consumedByteCount() 是「本帧到哪里结束」的唯一出处，本次调用没吃掉的都属下一帧
            offset += m_decoder.consumedByteCount();
            const Http2Frame frame = m_decoder.takeFrame();
            if (!handleFrame(frame))
            {
                co_return false;
            }
        }

        // 处理阶段只攒帧：到这里一次写出，避免每帧各一次系统调用
        if (!co_await flushOutgoing())
        {
            co_return false;
        }
        co_return isHealthy();
    }

    bool Http2ClientConnection::handleFrame(const Http2Frame &frame)
    {
        switch (frame.header.type)
        {
            case Http2FrameType::Settings:
                return handleSettingsFrame(frame);
            case Http2FrameType::Headers:
                return handleHeadersFrame(frame);
            case Http2FrameType::Continuation:
                return handleContinuationFrame(frame);
            case Http2FrameType::Data:
                return handleDataFrame(frame);
            case Http2FrameType::WindowUpdate:
                return handleWindowUpdateFrame(frame);
            case Http2FrameType::GoAway:
                return handleGoAwayFrame(frame);
            case Http2FrameType::RstStream:
                return handleRstStreamFrame(frame);
            case Http2FrameType::Ping:
                return handlePingFrame(frame);
            case Http2FrameType::Priority:
                // 本端不建优先级树：PRIORITY 收下即忽略（§5.3 允许这样处理）
                return true;
            case Http2FrameType::PushPromise:
                // 本端在 SETTINGS 里已通告 ENABLE_PUSH=0，对端还推就是协议违反（§6.6）
                fail("收到 PUSH_PROMISE，但本端已通告 ENABLE_PUSH=0");
                return false;
        }
        fail("收到了未知的帧类型");
        return false;
    }

    bool Http2ClientConnection::handleSettingsFrame(const Http2Frame &frame)
    {
        Http2SettingsPayload payload;
        std::string errorText;
        if (!parseHttp2SettingsPayload(frame, payload, &errorText))
        {
            fail("SETTINGS 帧不合规：" + errorText);
            return false;
        }
        if (payload.isAcknowledgement)
        {
            // 对端在 ACK 本端的 SETTINGS：没有要改的账
            return true;
        }

        if (const std::optional<std::uint32_t> initialWindow =
                    findSetting(payload, Http2SettingIdentifier::InitialWindowSize);
            initialWindow.has_value())
        {
            // §6.9.2：改了初值要按差值追溯地调整已在途的流的窗口，否则新旧两条流按两套账算
            const std::int64_t deltaByteCount = static_cast<std::int64_t>(*initialWindow)
                                              - static_cast<std::int64_t>(m_peerInitialStreamWindowByteCount);
            m_streamSendWindowByteCount += deltaByteCount;
            m_peerInitialStreamWindowByteCount = *initialWindow;
        }
        if (const std::optional<std::uint32_t> maximumFrame = findSetting(payload, Http2SettingIdentifier::MaxFrameSize);
            maximumFrame.has_value())
        {
            m_peerMaximumFrameByteSize = *maximumFrame;
        }
        if (const std::optional<std::uint32_t> headerTableSize = findSetting(payload, Http2SettingIdentifier::HeaderTableSize);
            headerTableSize.has_value())
        {
            // 对端替它的解码器要一个表大小，本端的编码器照它修剪（RFC 7541 §4.4）
            m_encoder.setMaximumDynamicTableSizeByteCount(*headerTableSize);
        }

        m_isPeerSettingsReceived = true;
        // 每条非 ACK 的 SETTINGS 都要回一个 ACK（§6.5.3），否则对端会一直等在确认上
        appendOutgoing(encodeHttp2Frame(Http2FrameType::Settings, kHttp2FlagAcknowledge, 0U, {}));
        return true;
    }

    bool Http2ClientConnection::finishHeaderBlock(const std::uint32_t streamId)
    {
        std::vector<HpackHeaderField> headerFields;
        std::string errorText;
        const std::string headerBlock = std::move(m_pendingHeaderBlock);
        m_pendingHeaderBlock.clear();
        if (!m_headerDecoder.decode(headerBlock, headerFields, &errorText))
        {
            // HPACK 动态表是连接级状态：解不开就等于两边的表已经错位，留着连接只会让后面的每个
            // 头块都解歪（§7.5.1 要求按 COMPRESSION_ERROR 收尾连接）
            fail("响应头块解码失败：" + errorText);
            return false;
        }

        const auto iterator = m_pendingStreams.find(streamId);
        if (iterator == m_pendingStreams.end())
        {
            // 这条流已收完或被 RST 掉了：字段解完即丢，动态表已随之演进，本端与对端仍同步
            return true;
        }
        PendingStream &pending = iterator->second;
        pending.response.headers.clear();
        for (const HpackHeaderField &field: headerFields)
        {
            if (field.name.empty() || field.name.front() != ':')
            {
                pending.response.headers.emplace_back(field.name, field.value);
                continue;
            }
            if (field.name == ":status")
            {
                pending.response.statusCode = static_cast<int>(std::strtoul(field.value.c_str(), nullptr, 10));
            }
        }
        return true;
    }

    bool Http2ClientConnection::handleHeadersFrame(const Http2Frame &frame)
    {
        Http2HeadersPayload payload;
        std::string errorText;
        if (!parseHttp2HeadersPayload(frame, payload, &errorText))
        {
            fail("HEADERS 帧不合规：" + errorText);
            return false;
        }
        m_continuationStreamId = frame.header.streamId;
        m_pendingHeaderBlock = payload.headerBlockFragment;
        m_isAwaitingContinuation = !payload.endHeaders;
        if (m_isAwaitingContinuation)
        {
            return true;
        }
        if (!finishHeaderBlock(frame.header.streamId))
        {
            return false;
        }
        if (payload.endStream)
        {
            const auto iterator = m_pendingStreams.find(frame.header.streamId);
            if (iterator != m_pendingStreams.end())
            {
                iterator->second.isResponseComplete = true;
            }
        }
        return true;
    }

    bool Http2ClientConnection::handleContinuationFrame(const Http2Frame &frame)
    {
        Http2ContinuationPayload payload;
        std::string errorText;
        if (!parseHttp2ContinuationPayload(frame, payload, &errorText))
        {
            fail("CONTINUATION 帧不合规：" + errorText);
            return false;
        }
        if (!m_isAwaitingContinuation || frame.header.streamId != m_continuationStreamId)
        {
            // §6.10：CONTINUATION 必须紧跟着同一段头块的前序帧，中间夹了别的帧就是连接错误
            fail("收到不该出现的 CONTINUATION");
            return false;
        }
        m_pendingHeaderBlock.append(payload.headerBlockFragment);
        m_isAwaitingContinuation = !payload.endHeaders;
        if (m_isAwaitingContinuation)
        {
            return true;
        }
        return finishHeaderBlock(frame.header.streamId);
    }

    bool Http2ClientConnection::handleDataFrame(const Http2Frame &frame)
    {
        Http2DataPayload payload;
        std::string errorText;
        if (!parseHttp2DataPayload(frame, payload, &errorText))
        {
            fail("DATA 帧不合规：" + errorText);
            return false;
        }
        const std::uint32_t streamId = frame.header.streamId;
        const auto iterator = m_pendingStreams.find(streamId);
        if (iterator == m_pendingStreams.end())
        {
            // 已收完或已被 RST 的流上还在来数据：字节仍占着连接窗口，账要还，内容丢掉
            creditWindow(streamId, static_cast<std::uint32_t>(payload.data.size()));
            return true;
        }
        iterator->second.response.body.append(payload.data);
        creditWindow(streamId, static_cast<std::uint32_t>(payload.data.size()));
        if (payload.endStream)
        {
            iterator->second.isResponseComplete = true;
        }
        return true;
    }

    bool Http2ClientConnection::handleWindowUpdateFrame(const Http2Frame &frame)
    {
        Http2WindowUpdatePayload payload;
        std::string errorText;
        if (!parseHttp2WindowUpdatePayload(frame, payload, &errorText))
        {
            fail("WINDOW_UPDATE 帧不合规：" + errorText);
            return false;
        }
        if (frame.header.streamId == 0U)
        {
            m_connectionSendWindowByteCount += static_cast<std::int64_t>(payload.windowSizeIncrement);
            if (m_connectionSendWindowByteCount > kMaximumWindowByteCount)
            {
                // 连接级窗口越界是**连接**的账坏了：§6.9.1 要求按连接错误 FLOW_CONTROL_ERROR 收口
                fail("连接级流控窗口越过 31 位上界");
                return false;
            }
            return true;
        }
        m_streamSendWindowByteCount += static_cast<std::int64_t>(payload.windowSizeIncrement);
        if (m_streamSendWindowByteCount > kMaximumWindowByteCount)
        {
            // 单流溢出只结这条流（与自家服务端同一条判据），牵连不到别的流
            const auto iterator = m_pendingStreams.find(frame.header.streamId);
            if (iterator != m_pendingStreams.end())
            {
                iterator->second.isReset = true;
                iterator->second.response.errorMessage = "对端的流控增量越过 31 位上界（RFC 7540 §6.9.1）";
            }
            appendOutgoing(encodeHttp2RstStreamFrame(
                    Http2RstStreamPayload{.errorCode = Http2ErrorCode::FlowControlError}, frame.header.streamId));
        }
        return true;
    }

    bool Http2ClientConnection::handleGoAwayFrame(const Http2Frame &frame)
    {
        Http2GoAwayPayload payload;
        std::string errorText;
        if (!parseHttp2GoAwayPayload(frame, payload, &errorText))
        {
            fail("GOAWAY 帧不合规：" + errorText);
            return false;
        }
        m_isPeerGoAway = true;
        for (auto &entry: m_pendingStreams)
        {
            if (entry.first > payload.lastStreamId)
            {
                entry.second.isReset = true;
                entry.second.response.errorMessage = "对端收尾时没把这条流算进已受理范围（GOAWAY）";
            }
        }
        return true;
    }

    bool Http2ClientConnection::handleRstStreamFrame(const Http2Frame &frame)
    {
        Http2RstStreamPayload payload;
        std::string errorText;
        if (!parseHttp2RstStreamPayload(frame, payload, &errorText))
        {
            fail("RST_STREAM 帧不合规：" + errorText);
            return false;
        }
        const auto iterator = m_pendingStreams.find(frame.header.streamId);
        if (iterator == m_pendingStreams.end())
        {
            return true;
        }
        iterator->second.isReset = true;
        iterator->second.response.errorMessage =
                std::string("对端按「") + std::string(http2ErrorCodeName(payload.errorCode)) + "」中止了这条流";
        return true;
    }

    bool Http2ClientConnection::handlePingFrame(const Http2Frame &frame)
    {
        Http2PingPayload payload;
        std::string errorText;
        if (!parseHttp2PingPayload(frame, payload, &errorText))
        {
            fail("PING 帧不合规：" + errorText);
            return false;
        }
        if (payload.isAcknowledgement)
        {
            return true;
        }
        // 未 ACK 的 PING 必须原样回去（§6.7）：对端拿它探活与量往返，不回就是链路被本端弄坏
        appendOutgoing(encodeHttp2Frame(Http2FrameType::Ping, kHttp2FlagAcknowledge, 0U, frame.payload));
        return true;
    }

    Core::Task<bool> Http2ClientConnection::sendBody(const std::uint32_t streamId, const std::string_view body)
    {
        std::size_t offset = 0;
        while (offset < body.size())
        {
            const std::int64_t availableByteCount = std::min(m_connectionSendWindowByteCount, m_streamSendWindowByteCount);
            if (availableByteCount <= 0)
            {
                // 窗口用尽：等对端的 WINDOW_UPDATE。等待期间照常处理它送来的其它帧，否则就成了
                // 「本端不发、对端也不发」的双向死锁
                if (!co_await pumpSome())
                {
                    co_return false;
                }
                continue;
            }
            const std::size_t chunkByteCount =
                    std::min(static_cast<std::size_t>(availableByteCount),
                             std::min(body.size() - offset, static_cast<std::size_t>(m_peerMaximumFrameByteSize)));
            const bool isLastChunk = offset + chunkByteCount >= body.size();
            appendOutgoing(encodeHttp2Frame(Http2FrameType::Data, isLastChunk ? kHttp2FlagEndStream : 0U, streamId,
                                            body.substr(offset, chunkByteCount)));
            if (!co_await flushOutgoing())
            {
                co_return false;
            }
            m_connectionSendWindowByteCount -= static_cast<std::int64_t>(chunkByteCount);
            m_streamSendWindowByteCount -= static_cast<std::int64_t>(chunkByteCount);
            offset += chunkByteCount;
        }
        co_return true;
    }

    Core::Task<Http2ClientResponse> Http2ClientConnection::request(
            const std::string_view scheme, const std::string_view authority, const std::string_view method,
            const std::string_view path, const std::vector<std::pair<std::string, std::string>> &extraHeaders,
            const std::string_view body, const std::chrono::milliseconds waitTimeout)
    {
        Http2ClientResponse response;
        if (!isHealthy())
        {
            response.errorMessage = m_errorMessage.empty() ? "连接已不可用" : m_errorMessage;
            co_return response;
        }
        if (m_isPeerGoAway)
        {
            response.errorMessage = "对端已通告收尾（GOAWAY），本端不再提新流";
            co_return response;
        }

        const std::uint32_t streamId = m_nextStreamId;
        m_nextStreamId += 2U;
        m_streamSendWindowByteCount = static_cast<std::int64_t>(m_peerInitialStreamWindowByteCount);

        std::vector<HpackHeaderField> fields;
        fields.reserve(extraHeaders.size() + 4U);
        // 四个伪头必须排在普通头之前（§8.1.2.1），顺序按 :method :path :scheme :authority
        fields.push_back(HpackHeaderField{":method", std::string(method)});
        fields.push_back(HpackHeaderField{":path", std::string(path)});
        fields.push_back(HpackHeaderField{":scheme", std::string(scheme)});
        fields.push_back(HpackHeaderField{":authority", std::string(authority)});
        for (const auto &header: extraHeaders)
        {
            fields.push_back(HpackHeaderField{header.first, header.second});
        }

        PendingStream pending;
        pending.streamId = streamId;
        m_pendingStreams.emplace(streamId, std::move(pending));

        const std::string headerBlock = m_encoder.encode(fields);
        appendOutgoing(encodeHttp2Frame(Http2FrameType::Headers,
                                        body.empty() ? static_cast<std::uint8_t>(kHttp2FlagEndHeaders | kHttp2FlagEndStream)
                                                     : kHttp2FlagEndHeaders,
                                        streamId, headerBlock));

        const RequestDeadlineGuard<Http2ClientConnection> deadline(m_loop, *this, waitTimeout, "Http2ClientConnection");
        if (!co_await flushOutgoing() || (!body.empty() && !co_await sendBody(streamId, body)))
        {
            m_pendingStreams.erase(streamId);
            response.errorMessage = m_errorMessage.empty() ? "请求没能完整送出" : m_errorMessage;
            co_return response;
        }

        while (isHealthy())
        {
            const auto iterator = m_pendingStreams.find(streamId);
            if (iterator == m_pendingStreams.end() || iterator->second.isResponseComplete || iterator->second.isReset)
            {
                break;
            }
            if (!co_await pumpSome())
            {
                break;
            }
        }

        const auto remainingIterator = m_pendingStreams.find(streamId);
        if (remainingIterator != m_pendingStreams.end())
        {
            response = std::move(remainingIterator->second.response);
            m_pendingStreams.erase(remainingIterator);
        }
        if (response.statusCode == 0 && response.errorMessage.empty())
        {
            response.errorMessage = m_errorMessage.empty() ? "没等到完整的响应" : m_errorMessage;
        }
        co_return response;
    }

    Core::Task<void> Http2ClientConnection::shutdown()
    {
        if (isHealthy())
        {
            Http2GoAwayPayload payload;
            payload.lastStreamId = m_nextStreamId >= 2U ? m_nextStreamId - 2U : 0U;
            payload.errorCode = Http2ErrorCode::NoError;
            appendOutgoing(encodeHttp2GoAwayFrame(payload));
            static_cast<void>(co_await flushOutgoing());
        }
        close();
        co_return;
    }

    void Http2ClientConnection::close() noexcept
    {
        m_isHealthy = false;
        m_pendingStreams.clear();
        m_transport->close();
    }
} // namespace AsynGyanis::Net
