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
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一次从通路上读的字节数：半帧由解码器自己留着，与服务端侧同一档
        constexpr std::size_t kReadChunkByteCount = 8192;

        // 窗口初值与上限两条常量在 Http2Frame.h：那是角色中立的协议数值，两条方向共用一份
    } // namespace

    Http2ClientConnection::Http2ClientConnection(Core::EventLoop &loop, std::unique_ptr<HttpOutboundConnection> transport,
                                                 const Config config)
        : m_loop(loop)
        , m_transport(std::move(transport))
        , m_config(config)
        , m_decoder(Http2FrameLimits{.maximumFrameSizeByteCount = config.maximumFrameByteSize})
    {
        m_connectionSendWindowByteCount = kHttp2InitialWindowSizeByteCount;
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
        // 前奏期间把驱动权拿在手上：契约是「前奏走完才把对象交给别的协程」，但真有请求挤进来，
        // 它会挂在等待体上等我叫醒，而不是自己跑去读同一条通路
        static_cast<void>(tryTakePumpLease());
        const PumpLease startLease(*this);
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
                failConnection(Http2ErrorCode::ProtocolError, "帧解码失败：" + m_decoder.errorMessage());
                break; // 不在这里 co_return：收尾那次写出要把本端攒下的 GOAWAY 送出去
            }
            // 帧就绪：consumedByteCount() 是「本帧到哪里结束」的唯一出处，本次调用没吃掉的都属下一帧
            offset += m_decoder.consumedByteCount();
            const Http2Frame frame = m_decoder.takeFrame();
            if (!handleFrame(frame))
            {
                break; // 同上：handleFrame 判死前攒下的 GOAWAY 也得有机会上线
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
                failConnection(Http2ErrorCode::ProtocolError, "收到 PUSH_PROMISE，但本端已通告 ENABLE_PUSH=0");
                return false;
        }
        // 帧层对不认识的类型照收（§5.5：未知帧类型 MUST 忽略，其空间等效于连接级），
        // 判死反而是违规——扩展帧本来就该能被旧实现安全略过
        return true;
    }

    bool Http2ClientConnection::handleSettingsFrame(const Http2Frame &frame)
    {
        Http2SettingsPayload payload;
        std::string errorText;
        if (!parseHttp2SettingsPayload(frame, payload, &errorText))
        {
            failConnection(Http2ErrorCode::ProtocolError, "SETTINGS 帧不合规：" + errorText);
            return false;
        }
        if (payload.isAcknowledgement)
        {
            // 本端只在 start() 里发过一次 SETTINGS，因此 ACK 也只能有一次（§6.5.3）：多出来的一条
            // 说明对端把账记错了，留着它等于承认后面每条 SETTINGS 都可以各回一次确认
            if (m_isOwnSettingsAcknowledged)
            {
                failConnection(Http2ErrorCode::ProtocolError,
                               "收到多余的 SETTINGS ACK：本端只发过一次 SETTINGS（RFC 7540 §6.5.3）");
                return false;
            }
            m_isOwnSettingsAcknowledged = true;
            return true;
        }
        return applyPeerSettings(payload);
    }

    bool Http2ClientConnection::applyPeerSettings(const Http2SettingsPayload &payload)
    {
        for (const Http2Setting &setting: payload.parameters)
        {
            switch (static_cast<Http2SettingIdentifier>(setting.identifier))
            {
                case Http2SettingIdentifier::HeaderTableSize:
                    // 对端通告的是它解码侧的表上限：本端编码器的动态表不得超过它（RFC 7541 §4.2）
                    m_encoder.setMaximumDynamicTableSizeByteCount(setting.value);
                    break;
                case Http2SettingIdentifier::EnablePush:
                case Http2SettingIdentifier::EnableConnectProtocol:
                    if (setting.value > 1U)
                    {
                        // 两条布尔型参数同理：0/1 之外的取值没有第三种语义可推（§6.5.2 与 RFC 8441 §3）
                        failConnection(Http2ErrorCode::ProtocolError,
                                       std::format("对端 SETTINGS 的参数 {} 取值 {} 非法：布尔型只允许 0 或 1（RFC 7540 §6.5.2）",
                                                   setting.identifier, setting.value));
                        return false;
                    }
                    break;
                case Http2SettingIdentifier::InitialWindowSize:
                {
                    if (setting.value > kHttp2MaximumWindowSizeByteCount)
                    {
                        failConnection(Http2ErrorCode::FlowControlError,
                                       std::format("对端 SETTINGS 的 INITIAL_WINDOW_SIZE 取值 {} 超过上限 2^31-1（RFC 7540 §6.5.2）",
                                                   setting.value));
                        return false;
                    }
                    // §6.9.2：改了初值要按差值追溯地调整**每一条在途流**的窗口，否则新旧几条流按两套账
                    // 算；平移之后越过上限同样是流控错误（对端把窗口"改大到装不下"也是它挑的）
                    const std::int64_t deltaByteCount = static_cast<std::int64_t>(setting.value)
                                                      - static_cast<std::int64_t>(m_peerInitialStreamWindowByteCount);
                    for (auto &entry: m_pendingStreams)
                    {
                        PendingStream &stream = entry.second;
                        stream.sendWindowByteCount += deltaByteCount;
                        if (stream.sendWindowByteCount > static_cast<std::int64_t>(kHttp2MaximumWindowSizeByteCount))
                        {
                            failConnection(Http2ErrorCode::FlowControlError,
                                           std::format("对端把 INITIAL_WINDOW_SIZE 改成 {} 之后，流 {} 的发送窗口超过上限 2^31-1（RFC 7540 §6.9.2）",
                                                       setting.value, stream.streamId));
                            return false;
                        }
                    }
                    m_peerInitialStreamWindowByteCount = setting.value;
                    break;
                }
                case Http2SettingIdentifier::MaxFrameSize:
                    if (setting.value < kHttp2DefaultMaximumFrameSize || setting.value > kHttp2MaximumMaximumFrameSize)
                    {
                        // 这一条不只是合法性问题：本端按它切正文，收到 0 就是每帧 0 字节的死循环
                        failConnection(Http2ErrorCode::ProtocolError,
                                       std::format("对端 SETTINGS 的 MAX_FRAME_SIZE 取值 {} 越界：合法区间是 [{}, {}]（RFC 7540 §6.5.2）",
                                                   setting.value, kHttp2DefaultMaximumFrameSize, kHttp2MaximumMaximumFrameSize));
                        return false;
                    }
                    m_peerMaximumFrameByteSize = setting.value;
                    break;
                case Http2SettingIdentifier::MaxConcurrentStreams:
                case Http2SettingIdentifier::MaxHeaderListSize:
                    // 对端限制的是「本端能同时提几条流 / 能发多大的头列表」：本层不代调用方节流，也不做
                    // 发送侧的头列表预算（要节流的是调用方：它才知道排队与快速失败哪个更合适）。收下即
                    // 无事可做，也就不留没有消费方的记账
                    break;
                default:
                    // 未知标识必须忽略（§6.5.2）——扩展参数的含义不在本层
                    break;
            }
        }

        m_isPeerSettingsReceived = true;
        // 每条非 ACK 的 SETTINGS 都要回一个 ACK（§6.5.3），否则对端会一直等在确认上
        appendOutgoing(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}));
        return true;
    }

    std::uint32_t Http2ClientConnection::lastOpenedStreamId() const noexcept
    {
        // m_nextStreamId 是「下一条要用的流号」，本端开过的最后一条是它减 2；一条都没开过时给 0
        return m_nextStreamId >= 2U ? m_nextStreamId - 2U : 0U;
    }

    void Http2ClientConnection::failConnection(const Http2ErrorCode errorCode, std::string reason)
    {
        if (m_isHealthy)
        {
            // 连接级出错也要按 §6.8 交代一句：GOAWAY 带错误码，对端才知道是哪条法被本端判死了。
            // 只攒不写——写由 pumpSome 的收尾统一做，这里再早退就不会把这条 GOAWAY 送出去
            Http2GoAwayPayload payload;
            payload.lastStreamId = lastOpenedStreamId();
            payload.errorCode = errorCode;
            appendOutgoing(encodeHttp2GoAwayFrame(payload));
        }
        fail(std::move(reason));
    }

    bool Http2ClientConnection::tryTakePumpLease() noexcept
    {
        if (m_isPumpLeaseTaken)
        {
            return false;
        }
        m_isPumpLeaseTaken = true;
        return true;
    }

    void Http2ClientConnection::releasePumpLease() noexcept
    {
        m_isPumpLeaseTaken = false;
    }

    void Http2ClientConnection::wakeWaitingStreams() noexcept
    {
        if (m_waitingStreamCount == 0U)
        {
            return;
        }
        m_waitingStreamCount = 0U;
        for (auto &entry: m_pendingStreams)
        {
            if (const std::coroutine_handle<> waiter = std::exchange(entry.second.waiter, nullptr); waiter != nullptr)
            {
                // 排到本层循环的调度器上，而不是在驱动者的栈上直接恢复：驱动者与等待者可能为同一批
                // 帧来回互叫好几趟，内联恢复会把调用栈按往返次数叠起来
                m_loop.scheduler().schedule(waiter);
            }
        }
    }

    bool Http2ClientConnection::StreamAwaiter::await_ready() const noexcept
    {
        const auto iterator = m_connection->m_pendingStreams.find(m_streamId);
        if (iterator == m_connection->m_pendingStreams.end())
        {
            return true; // 记录都不在了，没有可等的东西
        }
        const PendingStream &stream = iterator->second;
        // 「此刻没人驱动连接」这一条不能省：等着的人里必须有一个去当驱动者，否则全体挂起、
        // 连上再没人读字节，连接就地僵在这里
        return stream.isResponseComplete || stream.isReset || !m_connection->isHealthy()
               || !m_connection->m_isPumpLeaseTaken;
    }

    bool Http2ClientConnection::StreamAwaiter::await_suspend(const std::coroutine_handle<> waiter) noexcept
    {
        auto &connection = *m_connection;
        const auto iterator = connection.m_pendingStreams.find(m_streamId);
        if (iterator == connection.m_pendingStreams.end())
        {
            return false; // 挂不上就不挂：让协程继续往下走，它自己的收尾逻辑会处理
        }
        iterator->second.waiter = waiter;
        ++connection.m_waitingStreamCount;
        return true;
    }

    bool Http2ClientConnection::appendHeaderBlockFragment(PendingStream &stream, const std::string_view fragment)
    {
        // 拼接上限是本端策略：CONTINUATION 可以无限续，不设闸门等于让对端用一个头块撑爆内存。
        // 越界只能终止连接而不能只结这条流（服务端侧是同一条判据）：本端不肯存下的片段交不给 HPACK
        // 解码器，两边的动态表就此错位，留着连接只会让后面每条响应都解歪
        if (stream.pendingHeaderBlock.size() + fragment.size() > m_config.maximumHeaderBlockByteCount)
        {
            failConnection(Http2ErrorCode::EnhanceYourCalm,
                           std::format("流 {} 的头块压缩后已超过本端上限 {} 字节（HEADERS 与 CONTINUATION 片段之和）："
                                       "本端无法在撑爆内存的前提下继续同步 HPACK 动态表，只能终止连接，请对端减小头列表",
                                       stream.streamId, m_config.maximumHeaderBlockByteCount));
            return false;
        }
        stream.pendingHeaderBlock.append(fragment);
        return true;
    }

    bool Http2ClientConnection::finishHeaderBlock(PendingStream &stream)
    {
        std::vector<HpackHeaderField> headerFields;
        std::string errorText;
        const std::string headerBlock = std::move(stream.pendingHeaderBlock);
        stream.pendingHeaderBlock.clear();
        if (!m_headerDecoder.decode(headerBlock, headerFields, &errorText))
        {
            // HPACK 动态表是连接级状态：解不开就等于两边的表已经错位，留着连接只会让后面的每个
            // 头块都解歪（§7.5.1 要求按 COMPRESSION_ERROR 收尾连接）
            failConnection(Http2ErrorCode::CompressionError, "响应头块解码失败：" + errorText);
            return false;
        }

        // 收到这条流上的头块就证明对端已经接手了请求：连接复用时的「能不能重来一次」按这位判
        stream.response.isAnyByteReceived = true;
        stream.response.headers.clear();
        for (const HpackHeaderField &field: headerFields)
        {
            if (field.name.empty() || field.name.front() != ':')
            {
                stream.response.headers.emplace_back(field.name, field.value);
                continue;
            }
            if (field.name == ":status")
            {
                stream.response.statusCode = static_cast<int>(std::strtoul(field.value.c_str(), nullptr, 10));
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
            failConnection(Http2ErrorCode::ProtocolError, "HEADERS 帧不合规：" + errorText);
            return false;
        }
        const auto iterator = m_pendingStreams.find(frame.header.streamId);
        if (iterator == m_pendingStreams.end())
        {
            // 本端没开过这条流：这不是给谁的响应（§8.1.1 的畸形响应）。头块仍必须解完，动态表才能与
            // 对端同步——但那只做得到「整段头块一次到位」：多段头块要有人替它攒片段，而为一条本端不认
            // 的流留一份连接级暂存，等于把交错的两段头块混成一团
            if (!payload.endHeaders)
            {
                failConnection(Http2ErrorCode::ProtocolError,
                               std::format("流 {} 的头块没结束却没人替它攒片段：本端不认识这条流，多段头块无法继续同步 HPACK 动态表",
                                           frame.header.streamId));
                return false;
            }
            PendingStream orphan;
            orphan.streamId = frame.header.streamId;
            return appendHeaderBlockFragment(orphan, payload.headerBlockFragment)
                   && finishHeaderBlock(orphan);
        }
        PendingStream &stream = iterator->second;
        stream.isAwaitingContinuation = !payload.endHeaders;
        if (!appendHeaderBlockFragment(stream, payload.headerBlockFragment))
        {
            return false;
        }
        if (stream.isAwaitingContinuation)
        {
            return true;
        }
        if (!finishHeaderBlock(stream))
        {
            return false;
        }
        if (payload.endStream)
        {
            stream.isResponseComplete = true;
        }
        return true;
    }

    bool Http2ClientConnection::handleContinuationFrame(const Http2Frame &frame)
    {
        Http2ContinuationPayload payload;
        std::string errorText;
        if (!parseHttp2ContinuationPayload(frame, payload, &errorText))
        {
            failConnection(Http2ErrorCode::ProtocolError, "CONTINUATION 帧不合规：" + errorText);
            return false;
        }
        const auto iterator = m_pendingStreams.find(frame.header.streamId);
        if (iterator == m_pendingStreams.end() || !iterator->second.isAwaitingContinuation)
        {
            // §6.10：CONTINUATION 必须紧跟着同一段头块的前序帧，中间夹了别的帧就是连接错误
            failConnection(Http2ErrorCode::ProtocolError, "收到不该出现的 CONTINUATION");
            return false;
        }
        PendingStream &stream = iterator->second;
        stream.isAwaitingContinuation = !payload.endHeaders;
        if (!appendHeaderBlockFragment(stream, payload.headerBlockFragment))
        {
            return false;
        }
        if (stream.isAwaitingContinuation)
        {
            return true;
        }
        return finishHeaderBlock(stream);
    }

    bool Http2ClientConnection::handleDataFrame(const Http2Frame &frame)
    {
        Http2DataPayload payload;
        std::string errorText;
        if (!parseHttp2DataPayload(frame, payload, &errorText))
        {
            failConnection(Http2ErrorCode::ProtocolError, "DATA 帧不合规：" + errorText);
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
        iterator->second.response.isAnyByteReceived = true;
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
            failConnection(Http2ErrorCode::ProtocolError, "WINDOW_UPDATE 帧不合规：" + errorText);
            return false;
        }
        if (frame.header.streamId == 0U)
        {
            m_connectionSendWindowByteCount += static_cast<std::int64_t>(payload.windowSizeIncrement);
            if (m_connectionSendWindowByteCount > static_cast<std::int64_t>(kHttp2MaximumWindowSizeByteCount))
            {
                // 连接级窗口越界是**连接**的账坏了：§6.9.1 要求按连接错误 FLOW_CONTROL_ERROR 收口
                failConnection(Http2ErrorCode::FlowControlError, "连接级流控窗口越过 31 位上界（RFC 7540 §6.9.1）");
                return false;
            }
            return true;
        }
        const auto iterator = m_pendingStreams.find(frame.header.streamId);
        if (iterator == m_pendingStreams.end())
        {
            // 这条流本端已经不认了（收完或被结掉）：它的窗口没人记账，这条增量既无处累加也不影响别人
            return true;
        }
        PendingStream &stream = iterator->second;
        stream.sendWindowByteCount += static_cast<std::int64_t>(payload.windowSizeIncrement);
        if (stream.sendWindowByteCount > static_cast<std::int64_t>(kHttp2MaximumWindowSizeByteCount))
        {
            // 单流溢出只结这条流（与自家服务端同一条判据），牵连不到别的流
            stream.isReset = true;
            stream.response.errorMessage = "对端的流控增量越过 31 位上界（RFC 7540 §6.9.1）";
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
            failConnection(Http2ErrorCode::ProtocolError, "GOAWAY 帧不合规：" + errorText);
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
            failConnection(Http2ErrorCode::ProtocolError, "RST_STREAM 帧不合规：" + errorText);
            return false;
        }
        const auto iterator = m_pendingStreams.find(frame.header.streamId);
        if (iterator == m_pendingStreams.end())
        {
            return true;
        }
        // 对端亲手中止这条流，说明请求已被接手：这一位为真，复用侧就不该再重来一次
        iterator->second.response.isAnyByteReceived = true;
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
            failConnection(Http2ErrorCode::ProtocolError, "PING 帧不合规：" + errorText);
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

    Core::Task<bool> Http2ClientConnection::sendBody(PendingStream &stream, const std::string_view body)
    {
        std::size_t offset = 0;
        while (offset < body.size())
        {
            const std::int64_t availableByteCount = std::min(m_connectionSendWindowByteCount, stream.sendWindowByteCount);
            if (availableByteCount <= 0)
            {
                // 窗口用尽：等对端的 WINDOW_UPDATE。等待期间照常处理它送来的其它帧，否则就成了
                // 「本端不发、对端也不发」的双向死锁；驱动权在别人手上时先让开，等叫醒再看窗口
                if (!tryTakePumpLease())
                {
                    co_await StreamAwaiter(*this, stream.streamId);
                    continue;
                }
                const PumpLease lease(*this);
                if (!co_await pumpSome())
                {
                    co_return false;
                }
                continue;
            }
            if (stream.isReset)
            {
                // 流已被对端中止：再切正文就是送出不可能被读到的字节
                co_return false;
            }
            const std::size_t chunkByteCount =
                    std::min(static_cast<std::size_t>(availableByteCount),
                             std::min(body.size() - offset, static_cast<std::size_t>(m_peerMaximumFrameByteSize)));
            const bool isLastChunk = offset + chunkByteCount >= body.size();
            appendOutgoing(encodeHttp2Frame(Http2FrameType::Data, isLastChunk ? kHttp2FlagEndStream : 0U, stream.streamId,
                                            body.substr(offset, chunkByteCount)));
            if (!co_await flushOutgoing())
            {
                co_return false;
            }
            m_connectionSendWindowByteCount -= static_cast<std::int64_t>(chunkByteCount);
            stream.sendWindowByteCount -= static_cast<std::int64_t>(chunkByteCount);
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
        // 新流的发送窗口从对端通告的初值起算（§6.9.2）；本端 SETTINGS 里那条改的是自己收侧的账
        pending.sendWindowByteCount = static_cast<std::int64_t>(m_peerInitialStreamWindowByteCount);
        PendingStream &stream = m_pendingStreams.emplace(streamId, std::move(pending)).first->second;

        const std::string headerBlock = m_encoder.encode(fields);
        appendOutgoing(encodeHttp2Frame(Http2FrameType::Headers,
                                        body.empty() ? static_cast<std::uint8_t>(kHttp2FlagEndHeaders | kHttp2FlagEndStream)
                                                     : kHttp2FlagEndHeaders,
                                        streamId, headerBlock));

        const RequestDeadlineGuard<Http2ClientConnection> deadline(m_loop, *this, waitTimeout, "Http2ClientConnection");
        if (!co_await flushOutgoing() || (!body.empty() && !co_await sendBody(stream, body)))
        {
            const auto failedIterator = m_pendingStreams.find(streamId);
            if (failedIterator != m_pendingStreams.end())
            {
                // 流自己带回来的原因（被对端 RST 等）比连接级那句「请求没能完整送出」更准，优先用它的
                response = std::move(failedIterator->second.response);
                m_pendingStreams.erase(failedIterator);
            }
            if (response.statusCode == 0 && response.errorMessage.empty())
            {
                response.errorMessage = m_errorMessage.empty() ? "请求没能完整送出" : m_errorMessage;
            }
            co_return response;
        }

        while (isHealthy())
        {
            if (stream.isResponseComplete || stream.isReset)
            {
                break;
            }
            if (!tryTakePumpLease())
            {
                // 别人在驱动这条通路：他会顺手把我们的流往前推，推完这一轮把挂着的人叫醒
                co_await StreamAwaiter(*this, streamId);
                continue;
            }
            const PumpLease lease(*this);
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
            payload.lastStreamId = lastOpenedStreamId();
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
        // 这里**不清**在途流的记录：每条流的记录由提起它的那个请求协程自己收走（它可能正拿着这条记录的
        // 引用跨 co_await，就地清掉等于把引用悬空）。本端判死之后所有等待方都会因 isHealthy() 转假而收尾
        m_transport->close();
        // 挂在这条连接上的请求协程不需要在这里叫：它们都是挂在「驱动者」身上等的，而通路一关，正在
        // 读通路的驱动者必然醒来（读到异常），它在放开租约时统一叫醒大家
    }
} // namespace AsynGyanis::Net
