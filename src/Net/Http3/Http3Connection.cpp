#include "Net/Http3/Http3Connection.h"

#include "Base/Log/LogMacros.h"

#include <cstddef>
#include <variant>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 把字符串按字节交给只认「指针 + 长度」的接口，不留零终止的假设
        [[nodiscard]] std::span<const std::uint8_t> asBytes(const std::string &text) noexcept
        {
            return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        }

        /// 客户端发起的双向流号之间的跨度（RFC 9000 §2.1：这类流号恒 ≡ 0 mod 4）
        constexpr std::int64_t kClientBidirectionalStreamIdStep = 4;

        /// 对端发起的双向流：请求只跑在这类流上（RFC 9000 §2.1 的低位编码）
        [[nodiscard]] constexpr bool isPeerInitiatedBidirectionalStream(const std::int64_t streamId) noexcept
        {
            return (streamId & 3) == 0;
        }

        /// 对端发起的单向流：控制流与两条 QPACK 流（RFC 9114 §6.2）
        [[nodiscard]] constexpr bool isPeerInitiatedUnidirectionalStream(const std::int64_t streamId) noexcept
        {
            return (streamId & 3) == 2;
        }

        /// 帧层的错误类别映射成线上码：布局不合是帧错误，撑破缓冲是过量负载（RFC 9114 §8.1）
        [[nodiscard]] Http3ErrorCode toHttp3ErrorCode(const Http3FrameErrorKind errorKind) noexcept
        {
            return errorKind == Http3FrameErrorKind::LimitExceeded ? Http3ErrorCode::ExcessiveLoad : Http3ErrorCode::FrameError;
        }
    } // namespace

    Http3Connection::Http3Connection(StreamOpener opener, StreamWriter writer, StreamCrediter crediter, Callbacks callbacks,
                                     const LocalSettings settings) :
        m_streamOpener(std::move(opener)), m_streamWriter(std::move(writer)), m_streamCrediter(std::move(crediter)),
        m_callbacks(std::move(callbacks)), m_localSettings(settings)
    {
        if (!m_streamOpener || !m_streamWriter)
        {
            LOG_ERROR("Http3Connection: 缺少单向流的开流口或流数据出口，HTTP/3 协议层不可用");
            return;
        }

        // 三条本端发起的单向流：控制流、QPACK 编码器流、QPACK 解码器流（RFC 9114 §6.2.1、RFC 9204 §4.2/§4.3）
        m_localControlStreamId = m_streamOpener();
        m_localEncoderStreamId = m_streamOpener();
        m_localDecoderStreamId = m_streamOpener();
        if (m_localControlStreamId < 0 || m_localEncoderStreamId < 0 || m_localDecoderStreamId < 0)
        {
            LOG_WARN("Http3Connection: 开本端单向流失败，HTTP/3 协议层不可用");
            return;
        }

        // 解码器按本端公布的能力建：它决定本端能接受多大的动态表与多少条同时阻塞的头块
        m_qpackDecoder.emplace(QpackDecoderSettings{
                .maximumTableCapacityByteCount = m_localSettings.qpackMaximumTableCapacityByteCount,
                .maximumBlockedStreamCount = m_localSettings.qpackMaximumBlockedStreamCount,
                .maximumFieldSectionSizeByteCount = m_localSettings.maximumFieldSectionSizeByteCount,
        });

        // 编码器此刻还不知道能让对端建多大的表：先按全 0 建，即只用静态表编码。对端 SETTINGS 到了
        // 再抬起来（applyPeerSettings 里），那条容量指令会占住编码器流的第一个位置（RFC 9204 §4.2.6）
        m_qpackEncoder.emplace(std::size_t{0}, std::size_t{0}, std::size_t{0});

        // 每条单向流的第一个变长整数说明它是哪种流（RFC 9114 §6.2）
        appendHttp3StreamTypeHeader(m_outbound[m_localControlStreamId].bytes, Http3StreamType::Control);
        appendHttp3StreamTypeHeader(m_outbound[m_localEncoderStreamId].bytes, Http3StreamType::QpackEncoder);
        appendHttp3StreamTypeHeader(m_outbound[m_localDecoderStreamId].bytes, Http3StreamType::QpackDecoder);
        for (const std::int64_t streamId: {m_localControlStreamId, m_localEncoderStreamId, m_localDecoderStreamId})
        {
            m_outboundOrder.push_back(streamId);
        }

        // SETTINGS 必须排在控制流所有帧之前（RFC 9114 §7.2.4.2）
        Http3SettingsFrame settingsFrame;
        settingsFrame.settings.emplace_back(Http3SettingId::QpackMaxTableCapacity, m_localSettings.qpackMaximumTableCapacityByteCount);
        settingsFrame.settings.emplace_back(Http3SettingId::QpackBlockedStreams, m_localSettings.qpackMaximumBlockedStreamCount);
        settingsFrame.settings.emplace_back(Http3SettingId::MaxFieldSectionSize, m_localSettings.maximumFieldSectionSizeByteCount);
        if (m_localSettings.isExtendedConnectEnabled)
        {
            // 不声明这一项，对端就不该在这条连接上发带 :protocol 的 CONNECT（RFC 9220 §3.2.1）
            settingsFrame.settings.emplace_back(Http3SettingId::EnableConnectProtocol, 1);
        }
        appendHttp3Frame(m_outbound[m_localControlStreamId].bytes, settingsFrame);

        m_isUsable = true;
        flush();
        LOG_DEBUG_FMT("Http3Connection: HTTP/3 协议层已建立（控制流 {}、编码器流 {}、解码器流 {}）", m_localControlStreamId,
                      m_localEncoderStreamId, m_localDecoderStreamId);
    }

    bool Http3Connection::isUsable() const noexcept
    {
        return m_isUsable && !m_isBroken;
    }

    bool Http3Connection::isBroken() const noexcept
    {
        return m_isBroken;
    }

    Http3ErrorCode Http3Connection::connectionErrorCode() const noexcept
    {
        return m_connectionErrorCode;
    }

    const std::string &Http3Connection::connectionErrorReason() const noexcept
    {
        return m_connectionErrorReason;
    }

    std::size_t Http3Connection::peerTableCapacityByteCount() const noexcept
    {
        return m_qpackEncoder ? m_qpackEncoder->tableCapacityByteCount() : std::size_t{0};
    }

    void Http3Connection::consumeStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data,
                                            const bool isEndStream)
    {
        if (!isUsable())
        {
            return;
        }

        if (isPeerInitiatedUnidirectionalStream(streamId))
        {
            if (const auto kindEntry = m_peerStreamKinds.find(streamId); kindEntry == m_peerStreamKinds.end())
            {
                // 类型前缀可能跨多次交付：先攒够一个变长整数再归类，归完类的余字节交给对应角色
                if (!m_isBroken)
                {
                    consumePeerUnidirectionalTypePrefix(streamId, data, isEndStream);
                }
                return;
            }
            if (m_isBroken)
            {
                return;
            }
            switch (m_peerStreamKinds[streamId])
            {
                case PeerStreamKind::Control:
                    consumeControlStreamBytes(streamId, data, isEndStream);
                    break;
                case PeerStreamKind::QpackEncoder:
                    consumeQpackEncoderStreamBytes(data);
                    break;
                case PeerStreamKind::QpackDecoder:
                {
                    // 对端解码器流上的指令只影响本端编码器能不能继续用动态表，额度到达即还
                    const auto fed = m_qpackEncoder->feedDecoderStream(data);
                    if (!fed)
                    {
                        breakConnection(toHttp3ErrorCode(fed.error().kind), fed.error().message);
                        return;
                    }
                    if (m_streamCrediter && !data.empty())
                    {
                        m_streamCrediter(streamId, data.size());
                    }
                    break;
                }
                case PeerStreamKind::Ignored:
                case PeerStreamKind::Unknown:
                    // 未知类型的流：整条丢弃，但额度照还——不还就会把对端卡死在自己耗尽的窗口上
                    if (m_streamCrediter && !data.empty())
                    {
                        m_streamCrediter(streamId, data.size());
                    }
                    break;
            }
            return;
        }

        if (isPeerInitiatedBidirectionalStream(streamId))
        {
            consumeRequestStream(streamId, data, isEndStream);
            return;
        }

        // 本端发起的流不会带回字节；真带回来说明承载层接线错了，照实记一条并还额度而不是静默吞掉
        LOG_WARN_FMT("Http3Connection: 收到本端发起的流 {} 上的数据，已按忽略处理", streamId);
        if (m_streamCrediter && !data.empty())
        {
            m_streamCrediter(streamId, data.size());
        }
    }

    void Http3Connection::consumePeerUnidirectionalTypePrefix(const std::int64_t streamId, const std::span<const std::uint8_t> data,
                                                             const bool isEndStream)
    {
        std::string &buffer = m_peerStreamTypeBuffers[streamId];
        buffer.append(reinterpret_cast<const char *>(data.data()), data.size());

        const auto decoded = decodeQuicVariableLengthInteger(asBytes(buffer));
        if (!decoded.has_value())
        {
            if (decoded.error().kind == QuicDecodeErrorKind::Truncated)
            {
                // 还不够一个完整的前缀：整段留在本类缓冲里等下趟，不还额度（还没被任何一方消费）
                return;
            }
            m_peerStreamTypeBuffers.erase(streamId);
            breakConnection(Http3ErrorCode::FrameError, "对端单向流的类型前缀解不开：" + decoded.error().message);
            return;
        }

        const std::size_t prefixByteCount = decoded->byteCount;
        const std::string remaining = buffer.substr(prefixByteCount);
        m_peerStreamTypeBuffers.erase(streamId);

        PeerStreamKind kind = PeerStreamKind::Ignored;
        switch (static_cast<Http3StreamType>(decoded->value))
        {
            case Http3StreamType::Control:
                kind = PeerStreamKind::Control;
                break;
            case Http3StreamType::QpackEncoder:
                kind = PeerStreamKind::QpackEncoder;
                break;
            case Http3StreamType::QpackDecoder:
                kind = PeerStreamKind::QpackDecoder;
                break;
            case Http3StreamType::Push:
                // 推送流只能由服务端发起：客户端发来即流类型用错（RFC 9114 §6.2.2）
                breakConnection(Http3ErrorCode::StreamCreationError, "收到客户端发起的推送流，本服务端不支持推送");
                return;
        }

        // 每种角色只允许一条，第二条就是重复开通关键流（RFC 9114 §6.2.1）
        const bool isDuplicated = (kind == PeerStreamKind::Control && m_peerControlStreamId >= 0)
                                  || (kind == PeerStreamKind::QpackEncoder && m_peerEncoderStreamId >= 0)
                                  || (kind == PeerStreamKind::QpackDecoder && m_peerDecoderStreamId >= 0);
        if (isDuplicated)
        {
            breakConnection(Http3ErrorCode::StreamCreationError, "对端重复开通了关键单向流");
            return;
        }
        if (kind == PeerStreamKind::Control)
        {
            m_peerControlStreamId = streamId;
        }
        else if (kind == PeerStreamKind::QpackEncoder)
        {
            m_peerEncoderStreamId = streamId;
        }
        else if (kind == PeerStreamKind::QpackDecoder)
        {
            m_peerDecoderStreamId = streamId;
        }
        m_peerStreamKinds[streamId] = kind;

        // 前缀本身已被消费，可以还额度；同一趟里跟在后面的字节交给对应角色继续处理
        if (m_streamCrediter && prefixByteCount != 0)
        {
            m_streamCrediter(streamId, prefixByteCount);
        }
        if (!remaining.empty() && !m_isBroken)
        {
            const auto remainingBytes = asBytes(remaining);
            switch (kind)
            {
                case PeerStreamKind::Control:
                    consumeControlStreamBytes(streamId, remainingBytes, isEndStream);
                    break;
                case PeerStreamKind::QpackEncoder:
                    consumeQpackEncoderStreamBytes(remainingBytes);
                    break;
                case PeerStreamKind::QpackDecoder:
                {
                    const auto fed = m_qpackEncoder->feedDecoderStream(remainingBytes);
                    if (!fed)
                    {
                        breakConnection(toHttp3ErrorCode(fed.error().kind), fed.error().message);
                    }
                    break;
                }
                case PeerStreamKind::Ignored:
                case PeerStreamKind::Unknown:
                    if (m_streamCrediter && !remainingBytes.empty())
                    {
                        m_streamCrediter(streamId, remainingBytes.size());
                    }
                    break;
            }
        }
    }

    void Http3Connection::consumeControlStreamBytes(const std::int64_t streamId, const std::span<const std::uint8_t> data,
                                                   const bool isEndStream)
    {
        StreamState &state = streamStateFor(streamId);
        if (!state.reader)
        {
            // 只在第一次建：读取器里可能还留着上一趟没凑齐的半截帧
            state.reader.emplace(m_localSettings.maximumFrameByteCount);
        }
        state.fedByteCount += data.size();

        if (const auto fed = state.reader->feed(data); !fed)
        {
            // 控制流上的布局错误没法按流回收：帧边界已经不可信（RFC 9114 §7.2.1）
            breakConnection(toHttp3ErrorCode(fed.error().kind), fed.error().message);
            return;
        }

        while (!m_isBroken)
        {
            const auto nextFrame = state.reader->nextFrame();
            if (!nextFrame)
            {
                breakConnection(toHttp3ErrorCode(nextFrame.error().kind), nextFrame.error().message);
                return;
            }
            if (!nextFrame->has_value())
            {
                break; // 字节还没凑出一个完整帧，等下一趟
            }
            static_cast<void>(handleControlFrame(**nextFrame));
        }

        creditConsumedBytes(streamId, state, 0);
        if (isEndStream)
        {
            // 关键流在对端侧提前关闭即连接级故障（RFC 9114 §6.2.1）
            breakConnection(Http3ErrorCode::ClosedCriticalStream, "对端控制流已关闭");
        }
    }

    void Http3Connection::consumeQpackEncoderStreamBytes(const std::span<const std::uint8_t> data)
    {
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string decoderStreamBytes;
        const auto fed = m_qpackDecoder->feedEncoderStream(data, unblockedStreamIds, decoderStreamBytes);
        if (!fed)
        {
            breakConnection(toHttp3ErrorCode(fed.error().kind), fed.error().message);
            return;
        }
        queueQpackInstructions({}, decoderStreamBytes);
        for (const std::uint64_t unblockedStreamId: unblockedStreamIds)
        {
            deliverResumedFieldSection(static_cast<std::int64_t>(unblockedStreamId));
        }
        // 指令字节已被解码器收下（或被它按上限拒收），到达即还额度
        if (m_streamCrediter && !data.empty())
        {
            m_streamCrediter(m_peerEncoderStreamId, data.size());
        }
    }

    void Http3Connection::consumeRequestStream(const std::int64_t streamId, const std::span<const std::uint8_t> data,
                                               const bool isEndStream)
    {
        // 这条流是不是刚见到：决定要不要按排空通告拒绝，也决定 GOAWAY 该报哪个标识
        const bool isNewRequestStream = !m_streams.contains(streamId);
        // 排空通告之后才到的新流一律不处理（RFC 9114 §5.2：等于或高于通告标识的请求被拒绝），
        // 对端会把这条请求换一条连接重发；这里不回应，但要照还额度并放弃这条流
        if (isNewRequestStream && m_isDraining && streamId >= m_rejectedFromStreamId)
        {
            rejectStreamAfterDrain(streamId, data);
            return;
        }

        StreamState &state = streamStateFor(streamId);
        if (isNewRequestStream && streamId > m_lastProcessedRequestStreamId)
        {
            m_lastProcessedRequestStreamId = streamId;
        }
        if (state.isAbandoned)
        {
            // 已经作废的流：剩下的字节全部直接还额度，不再产生任何事件
            if (m_streamCrediter && !data.empty())
            {
                m_streamCrediter(streamId, data.size());
            }
            return;
        }
        if (!state.reader)
        {
            state.reader.emplace(m_localSettings.maximumFrameByteCount);
        }
        state.fedByteCount += data.size();

        if (const auto fed = state.reader->feed(data); !fed)
        {
            failStream(streamId, toHttp3ErrorCode(fed.error().kind), fed.error().message);
            return;
        }

        std::size_t dataPayloadByteCount = 0;
        while (!m_isBroken && !state.isAbandoned && !state.isHeadRejected)
        {
            const auto nextFrame = state.reader->nextFrame();
            if (!nextFrame)
            {
                failStream(streamId, toHttp3ErrorCode(nextFrame.error().kind), nextFrame.error().message);
                return;
            }
            if (!nextFrame->has_value())
            {
                break;
            }
            const Http3Frame &frame = **nextFrame;
            if (const auto *bodyFrame = std::get_if<Http3DataFrame>(&frame); bodyFrame != nullptr)
            {
                dataPayloadByteCount += bodyFrame->payload.size();
            }
            if (const auto handled = handleRequestFrame(streamId, state, frame); !handled)
            {
                if (!state.isAbandoned && !m_isBroken)
                {
                    breakConnection(toHttp3ErrorCode(handled.error().kind), handled.error().message);
                }
                return;
            }
        }

        creditConsumedBytes(streamId, state, dataPayloadByteCount);

        if (isEndStream && !state.isPeerFinished && !state.isAbandoned)
        {
            state.isPeerFinished = true;
            finishRequestStreamIfEnded(streamId, state);
        }
        // 此刻不再使用该流的引用：把处理中途被判定放弃的流在这里回收
        pruneAbandonedStream(streamId);
    }

    std::expected<void, QpackError> Http3Connection::handleRequestFrame(const std::int64_t streamId, StreamState &state,
                                                                        const Http3Frame &frame)
    {
        if (std::holds_alternative<Http3HeadersFrame>(frame))
        {
            const auto &headersFrame = std::get<Http3HeadersFrame>(frame);
            // 头段之后再来一个头段就是尾段（RFC 9114 §4.1）；正文开始之前只允许一个头段
            const bool isTrailers = state.isHeaderSectionSeen;
            if (isTrailers && state.isTrailersSeen)
            {
                failStream(streamId, Http3ErrorCode::MessageError, "请求里出现了第三个头段：只允许头段加一个尾段");
                return {};
            }
            if (isTrailers)
            {
                state.isTrailersSeen = true;
            }

            std::vector<QpackHeaderField> fields;
            std::string decoderStreamBytes;
            const auto decoded = m_qpackDecoder->decodeFieldSection(static_cast<std::uint64_t>(streamId), headersFrame.encodedFieldSection,
                                                                    fields, decoderStreamBytes);
            queueQpackInstructions({}, decoderStreamBytes);
            if (!decoded)
            {
                // 头块解不开属对端编码器的错：按 §4.5 以流级处置，不牵连整条连接
                failStream(streamId, toHttp3ErrorCode(decoded.error().kind), decoded.error().message);
                return {};
            }
            if (!state.isHeaderSectionSeen)
            {
                state.isHeaderSectionSeen = true;
            }
            if (*decoded == QpackFieldSectionDecodeStatus::Blocked)
            {
                // 声明的动态表内容还没到：整段由解码器代管，等编码器流补齐后再交（RFC 9204 §2.2.1）
                return {};
            }
            static_cast<void>(deliverFieldSection(streamId, state, fields, isTrailers));
            return {};
        }

        if (const auto *bodyFrame = std::get_if<Http3DataFrame>(&frame); bodyFrame != nullptr)
        {
            if (!state.isHeaderSectionSeen)
            {
                failStream(streamId, Http3ErrorCode::FrameUnexpected, "请求正文出现在头段之前（RFC 9114 §7.2.1）");
                return {};
            }
            if (state.isTrailersSeen)
            {
                failStream(streamId, Http3ErrorCode::MessageError, "尾段之后又出现正文：消息顺序不合法（RFC 9114 §4.1.2）");
                return {};
            }
            state.isBodyStarted = true;
            state.receivedBodyByteCount += bodyFrame->payload.size();
            if (m_callbacks.onBodyBytes && !bodyFrame->payload.empty())
            {
                m_callbacks.onBodyBytes(streamId, bodyFrame->payload);
            }
            return {};
        }

        if (std::holds_alternative<Http3UnknownFrame>(frame))
        {
            // 未知帧类型按 §7.2.1 忽略：那是将来扩展用的，不该因为不认识就打死一条连接
            return {};
        }
        if (isHttp3ReservedExtensionIdentifier(http3FrameTypeValue(frame)))
        {
            return {};
        }

        // 剩下的都是「出现在错误位置的已知帧」：SETTINGS/PUSH_PROMISE/GOAWAY/MAX_PUSH_ID/CANCEL_PUSH
        // 都只许出现在控制流上（RFC 9114 §7.2.4/§7.2.5/§7.2.6/§7.2.7/§7.2.3）
        failStream(streamId, Http3ErrorCode::FrameUnexpected,
                   std::string("帧类型 ") + std::string(http3FrameTypeName(http3FrameTypeValue(frame))) + " 不得出现在请求流上");
        return {};
    }

    std::expected<void, QpackError> Http3Connection::handleControlFrame(const Http3Frame &frame)
    {
        if (const auto *settingsFrame = std::get_if<Http3SettingsFrame>(&frame); settingsFrame != nullptr)
        {
            if (m_isPeerSettingsReceived)
            {
                // 一条连接上只能有一个 SETTINGS，且必须是控制流的第一个帧（RFC 9114 §7.2.4.1）
                breakConnection(Http3ErrorCode::FrameUnexpected, "对端在控制流上发了第二个 SETTINGS");
            }
            else
            {
                m_isPeerSettingsReceived = true;
                applyPeerSettings(*settingsFrame);
            }
            return {};
        }

        if (!m_isPeerSettingsReceived)
        {
            breakConnection(Http3ErrorCode::MissingSettings, "对端控制流的第一个帧不是 SETTINGS（RFC 9114 §7.2.4.2）");
            return {};
        }

        if (const auto *goAwayFrame = std::get_if<Http3GoAwayFrame>(&frame); goAwayFrame != nullptr)
        {
            LOG_DEBUG_FMT("Http3Connection: 收到对端 GOAWAY，标识 {} 及以上的流对端不再受理", goAwayFrame->streamIdOrPushId);
            return {};
        }
        if (const auto *maxPushIdFrame = std::get_if<Http3MaxPushIdFrame>(&frame); maxPushIdFrame != nullptr)
        {
            m_maximumPushId = maxPushIdFrame->pushId;
            return {};
        }
        if (std::holds_alternative<Http3CancelPushFrame>(frame) || std::holds_alternative<Http3UnknownFrame>(frame))
        {
            // 本服务端从不承诺推送，撤销一个不存在的推送按 §7.2.3 直接忽略；未知帧同理
            return {};
        }
        if (isHttp3ReservedExtensionIdentifier(http3FrameTypeValue(frame)))
        {
            return {};
        }

        breakConnection(Http3ErrorCode::FrameUnexpected,
                        std::string("帧类型 ") + std::string(http3FrameTypeName(http3FrameTypeValue(frame))) + " 不得出现在控制流上");
        return {};
    }

    bool Http3Connection::deliverFieldSection(const std::int64_t streamId, StreamState &state,
                                              const std::vector<QpackHeaderField> &fields, const bool isTrailers)
    {
        // 判定器按流持有：尾段的合法性（不得有伪头、必须在头段之后）依赖头段已经收过这个事实，
        // 每个头段新建一份就会把合法尾段判成非法序列
        if (!state.validator)
        {
            state.validator = std::make_unique<Http3HeaderValidator>(Http3MessageKind::Request, m_localSettings.isExtendedConnectEnabled);
        }
        Http3HeaderValidator &validator = *state.validator;
        if (const auto began = validator.beginHeaderBlock(isTrailers); !began)
        {
            rejectRequestHead(streamId, state, began.error().message);
            return false;
        }
        for (const auto &field: fields)
        {
            if (const auto accepted = validator.onHeaderField(field.name, field.value); !accepted)
            {
                rejectRequestHead(streamId, state, accepted.error().message);
                return false;
            }
        }
        if (const auto ended = validator.endHeaderBlock(); !ended)
        {
            rejectRequestHead(streamId, state, ended.error().message);
            return false;
        }

        if (!isTrailers)
        {
            // 正文对账要用的声明长度在这里落表：DATA 到达时只累加，收尾时比对
            state.hasContentLengthDeclaration = validator.contentLengthByteCount().has_value();
            state.declaredContentLengthByteCount = validator.contentLengthByteCount().value_or(0);
        }

        // 判定过了才交给上层：伪头的归位与业务映射是会话的事，本类不重复那份逻辑
        if (m_callbacks.onHeaderField)
        {
            for (const auto &field: fields)
            {
                m_callbacks.onHeaderField(streamId, field.name, field.value);
            }
        }

        std::string decoderStreamBytes;
        // 「已交付」之后才允许发 Section Ack（RFC 9204 §4.4.1）：此刻字段已逐个交给上层
        static_cast<void>(m_qpackDecoder->noteFieldSectionDelivered(static_cast<std::uint64_t>(streamId), decoderStreamBytes));
        queueQpackInstructions({}, decoderStreamBytes);

        if (m_callbacks.onHeaderBlockReceived)
        {
            m_callbacks.onHeaderBlockReceived(streamId, isTrailers);
        }
        return true;
    }

    void Http3Connection::deliverResumedFieldSection(const std::int64_t streamId)
    {
        const auto entry = m_streams.find(streamId);
        if (entry == m_streams.end() || entry->second.isAbandoned)
        {
            // 阻塞期间该流已被对端取消：解码器那边由 noteStreamCancelledByPeer 收过尾，这里无事可做
            return;
        }
        StreamState &state = entry->second;

        std::vector<QpackHeaderField> fields;
        std::string decoderStreamBytes;
        const auto resumed = m_qpackDecoder->resumeBlockedFieldSection(static_cast<std::uint64_t>(streamId), fields, decoderStreamBytes);
        queueQpackInstructions({}, decoderStreamBytes);
        if (!resumed)
        {
            failStream(streamId, toHttp3ErrorCode(resumed.error().kind), resumed.error().message);
            return;
        }
        if (*resumed == QpackFieldSectionDecodeStatus::Blocked)
        {
            return; // 还没补齐，继续等
        }
        // 阻塞过的头段一定是请求的头段：正文不可能排在它之前，所以 isTrailers 恒假
        static_cast<void>(deliverFieldSection(streamId, state, fields, state.isBodyStarted));
    }

    void Http3Connection::finishRequestStreamIfEnded(const std::int64_t streamId, StreamState &state)
    {
        if (!state.isHeaderSectionSeen)
        {
            // 一条什么都没收到的流：按非法消息序列处置，不派发业务
            failStream(streamId, Http3ErrorCode::MessageError, "请求流上没有头段就结束了（RFC 9114 §4.1.2）");
            return;
        }
        if (state.hasContentLengthDeclaration && state.declaredContentLengthByteCount != state.receivedBodyByteCount)
        {
            // §4.1.2 明写：声明长度与 DATA 总长不等即畸形
            failStream(streamId, Http3ErrorCode::MessageError,
                       "content-length 声明 " + std::to_string(state.declaredContentLengthByteCount) + " 字节，实收 "
                           + std::to_string(state.receivedBodyByteCount) + " 字节（RFC 9114 §4.1.2）");
            return;
        }

        if (m_callbacks.onRequestEnded)
        {
            m_callbacks.onRequestEnded(streamId);
        }
        closeStreamIfDone(streamId);
    }

    void Http3Connection::creditConsumedBytes(const std::int64_t streamId, StreamState &state, const std::size_t dataPayloadByteCount)
    {
        if (!state.reader || !m_streamCrediter)
        {
            return;
        }
        // 读取器已消化的字节 = 喂进去的 - 还留在缓冲里的；DATA 载荷归上层还，别还两次
        const std::size_t consumedTotal = state.fedByteCount > state.reader->pendingByteCount()
                                              ? static_cast<std::size_t>(state.fedByteCount - state.reader->pendingByteCount())
                                              : 0;
        const std::size_t increment = consumedTotal > state.creditedByteCount ? static_cast<std::size_t>(consumedTotal - state.creditedByteCount)
                                                                              : 0;
        state.creditedByteCount += increment;
        if (increment <= dataPayloadByteCount)
        {
            return;
        }
        m_streamCrediter(streamId, increment - dataPayloadByteCount);
    }

    std::expected<void, QpackError> Http3Connection::submitResponseHead(const std::int64_t streamId,
                                                                        const std::vector<QpackHeaderField> &fieldLines,
                                                                        const bool isEndOfStream)
    {
        if (m_isBroken)
        {
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState, .message = "HTTP/3 协议层已作废，无法提交响应"});
        }
        const auto entry = m_streams.find(streamId);
        if (entry == m_streams.end() || entry->second.isAbandoned || entry->second.isLocalFinished)
        {
            // 流不在了或已收尾：只作废这一条响应，不牵连连接（对端多半已重置该流）
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState,
                                              .message = "流 " + std::to_string(streamId) + " 已不存在或本端已收尾，响应作废"});
        }

        // 响应侧用一次性判定器：本端是唯一写出方，不必像请求流那样跨头段与尾段累积顺序
        Http3HeaderValidator validator(Http3MessageKind::Response, m_localSettings.isExtendedConnectEnabled);
        if (const auto began = validator.beginHeaderBlock(false); !began)
        {
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState, .message = began.error().message});
        }
        for (const auto &field: fieldLines)
        {
            if (const auto accepted = validator.onHeaderField(field.name, field.value); !accepted)
            {
                return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState, .message = accepted.error().message});
            }
        }
        if (const auto ended = validator.endHeaderBlock(); !ended)
        {
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState, .message = ended.error().message});
        }

        std::string headerBlock;
        std::string encoderStreamBytes;
        if (const auto encoded = m_qpackEncoder->encodeFieldSection(static_cast<std::uint64_t>(streamId),
                                                                    std::span<const QpackHeaderField>(fieldLines.data(), fieldLines.size()),
                                                                    headerBlock, encoderStreamBytes);
            !encoded)
        {
            return std::unexpected(encoded.error());
        }
        // 指令排编码器流、头块排响应流：两条流之间 QUIC 不保证到达顺序，对端靠头块前缀里的
        // Required Insert Count 自己判断要不要挂起，因此这里不必（也做不到）跨流定序
        queueQpackInstructions(encoderStreamBytes, {});

        queueOutboundFrame(streamId, Http3FrameType::Headers, asBytes(headerBlock), isEndOfStream);

        if (isEndOfStream)
        {
            // 只记收尾标记，不在这里摘状态：尾字节还在待发队列里，等 flush 把它交给传输层之后
            // 才走 noteLocallyFinishedStream 发关闭通知
            entry->second.isLocalFinished = true;
        }
        return {};
    }

    std::expected<std::size_t, QpackError> Http3Connection::appendResponseBody(const std::int64_t streamId,
                                                                                const std::span<const std::uint8_t> bytes,
                                                                                const bool isEndStream)
    {
        const auto entry = m_streams.find(streamId);
        if (entry == m_streams.end() || entry->second.isAbandoned || entry->second.isLocalFinished)
        {
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState,
                                              .message = "流 " + std::to_string(streamId) + " 已不存在或本端已收尾，正文作废"});
        }

        if (!bytes.empty())
        {
            queueOutboundFrame(streamId, Http3FrameType::Data, bytes, isEndStream);
        }
        else if (isEndStream)
        {
            // 没有正文也要把收尾传下去：空字节段加 endStream 是本端结束这条流的唯一写法
            queueOutboundBytes(streamId, {}, true);
        }

        if (isEndStream)
        {
            entry->second.isLocalFinished = true;
            closeStreamIfDone(streamId);
        }
        return bytes.size();
    }

    std::size_t Http3Connection::pendingOutputByteCount(const std::int64_t streamId) const noexcept
    {
        const auto entry = m_outbound.find(streamId);
        return entry == m_outbound.end() ? 0 : entry->second.bytes.size();
    }

    bool Http3Connection::isLocalStreamFinished(const std::int64_t streamId) const noexcept
    {
        const auto entry = m_streams.find(streamId);
        return entry == m_streams.end() || entry->second.isLocalFinished || entry->second.isAbandoned;
    }

    void Http3Connection::noteStreamCancelledByPeer(const std::int64_t streamId)
    {
        if (!m_isUsable)
        {
            return;
        }
        // 承载层报来的取消：本端的待发字节直接丢掉，阻塞中的头段与动态表引用一并收尾
        m_outbound.erase(streamId);
        std::string decoderStreamBytes;
        if (m_qpackDecoder)
        {
            m_qpackDecoder->noteStreamAbandoned(static_cast<std::uint64_t>(streamId), decoderStreamBytes);
        }
        if (m_qpackEncoder)
        {
            m_qpackEncoder->noteStreamAbandoned(static_cast<std::uint64_t>(streamId), decoderStreamBytes);
        }
        queueQpackInstructions({}, decoderStreamBytes);

        const auto entry = m_streams.find(streamId);
        if (entry == m_streams.end())
        {
            return;
        }
        entry->second.isPeerFinished = true;
        entry->second.isAbandoned = true;
        if (m_callbacks.onStreamReset)
        {
            m_callbacks.onStreamReset(streamId, Http3ErrorCode::RequestCancelled);
        }
        m_streams.erase(entry);
    }

    void Http3Connection::flush()
    {
        if (!m_isUsable || !m_streamWriter)
        {
            return;
        }

        // 先补上欠对端的插入数告知：它和这一趟要发的头块/指令属于同一批 QPACK 状态推进
        emitDecoderStreamIncrements();

        for (std::size_t round = 0; round < kMaximumFlushRounds && !m_outboundOrder.empty(); ++round)
        {
            const std::int64_t streamId = m_outboundOrder.front();
            m_outboundOrder.pop_front();

            const auto entry = m_outbound.find(streamId);
            if (entry == m_outbound.end())
            {
                continue; // 排队之后这条流被重置或已摘掉：跳过即可
            }
            OutboundStream outbound = std::move(entry->second);
            m_outbound.erase(entry);

            if (outbound.bytes.empty())
            {
                if (outbound.isEndStream)
                {
                    m_streamWriter(streamId, std::span<const std::uint8_t>{}, true);
                    noteLocallyFinishedStream(streamId);
                }
                continue;
            }
            // 一次把这流的待发字节全交出去：传输层自己留重传副本并按流控发包，本类不必分批
            m_streamWriter(streamId, asBytes(outbound.bytes), outbound.isEndStream);
            if (outbound.isEndStream)
            {
                noteLocallyFinishedStream(streamId);
            }
        }
    }

    std::expected<void, QpackError> Http3Connection::beginGracefulDrain()
    {
        if (!m_isUsable)
        {
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState,
                                              .message = "HTTP/3 协议层没建起来，控制流还不存在，GOAWAY 无处可发"});
        }
        if (m_isBroken)
        {
            return std::unexpected(QpackError{.kind = QpackErrorKind::InvalidLocalState,
                                              .message = "HTTP/3 协议层已作废，不再往任何流写字节"});
        }
        // 通告只发一次：后发的 GOAWAY 标识不得比先发的大（§7.2.6），重复发也不带来新信息
        if (m_isDraining)
        {
            return {};
        }

        m_isDraining = true;
        // 语义是「等于或高于该标识都被拒绝」，因此要留住的最后一条流本身不能进通告值，
        // 报它之后的下一条客户端双向流号；一条请求都没收过时按 §5.2 报 0
        m_rejectedFromStreamId =
                m_lastProcessedRequestStreamId < 0 ? 0 : m_lastProcessedRequestStreamId + kClientBidirectionalStreamIdStep;

        Http3GoAwayFrame goAwayFrame;
        goAwayFrame.streamIdOrPushId = static_cast<std::uint64_t>(m_rejectedFromStreamId);
        std::string frameBytes;
        appendHttp3Frame(frameBytes, goAwayFrame);
        // GOAWAY 走控制流且不收尾：这条流上随后可能还要发别的（本端不主动结束控制流）
        queueOutboundBytes(m_localControlStreamId, frameBytes, false);
        LOG_INFO_FMT("Http3Connection: 已发出 GOAWAY，流 {} 及以上的请求不再受理（已受理的照常处理完）", m_rejectedFromStreamId);
        return {};
    }

    bool Http3Connection::isDraining() const noexcept
    {
        return m_isDraining;
    }

    void Http3Connection::rejectStreamAfterDrain(const std::int64_t streamId, const std::span<const std::uint8_t> data)
    {
        // 先建状态再立刻放弃：不建的话对端后续字节会被当成「一条全新的流」重新判一遍，
        // 而「已受理」与「已拒绝」的边界必须稳定
        StreamState &state = streamStateFor(streamId);
        state.isAbandoned = true;
        if (m_streamCrediter && !data.empty())
        {
            m_streamCrediter(streamId, data.size());
        }
        // §5.2 的 SHOULD：通告之后到达的新流要显式取消，让对端立刻知道这条流不会被处理，而不是
        // 等到连接关闭。走 failStream 是为了让会话在同一处收到「该流已放弃」并回收它的记账
        failStream(streamId, Http3ErrorCode::RequestRejected, "GOAWAY 已通告，新请求不再受理");
    }

    void Http3Connection::noteLocallyFinishedStream(const std::int64_t streamId)
    {
        const auto entry = m_streams.find(streamId);
        if (entry == m_streams.end())
        {
            return; // 本端单向流：没有流状态记录
        }
        entry->second.isLocalFinished = true;
        closeStreamIfDone(streamId);
    }

    void Http3Connection::emitDecoderStreamIncrements()
    {
        if (!m_qpackDecoder || m_localDecoderStreamId < 0)
        {
            return;
        }
        // 只在真有欠账时才排字节：没有增量时本方法不写任何东西，也就不会凭空多一趟 flush
        std::string decoderStreamBytes;
        if (m_qpackDecoder->emitInsertCountIncrement(decoderStreamBytes) == 0)
        {
            return;
        }
        queueOutboundBytes(m_localDecoderStreamId, decoderStreamBytes, false);
    }

    void Http3Connection::queueQpackInstructions(const std::string_view encoderBytes, const std::string_view decoderBytes)
    {
        if (!encoderBytes.empty() && m_localEncoderStreamId >= 0)
        {
            queueOutboundBytes(m_localEncoderStreamId, encoderBytes, false);
        }
        if (!decoderBytes.empty() && m_localDecoderStreamId >= 0)
        {
            queueOutboundBytes(m_localDecoderStreamId, decoderBytes, false);
        }
    }

    Http3Connection::OutboundStream &Http3Connection::outboundFor(const std::int64_t streamId)
    {
        const auto entry = m_outbound.find(streamId);
        if (entry != m_outbound.end())
        {
            return entry->second;
        }
        // 新建才排进轮转队列：一次 flush 会把这条流的字节全交出去，重复排队等于让同一条流走两遍
        OutboundStream &outbound = m_outbound[streamId];
        m_outboundOrder.push_back(streamId);
        return outbound;
    }

    void Http3Connection::queueOutboundBytes(const std::int64_t streamId, const std::string_view bytes, const bool isEndStream)
    {
        if (m_isBroken)
        {
            return; // 作废之后不再排任何字节，等销毁
        }
        OutboundStream &outbound = outboundFor(streamId);
        outbound.bytes.append(bytes);
        outbound.isEndStream = outbound.isEndStream || isEndStream;
    }

    void Http3Connection::queueOutboundFrame(const std::int64_t streamId, const Http3FrameType frameType,
                                             const std::span<const std::uint8_t> payload, const bool isEndStream)
    {
        if (m_isBroken)
        {
            return;
        }
        // 帧头与载荷直接排进本流缓冲：先拼一份临时 string 会把载荷整段多搬一遍，
        // 而 DATA 段可以很大——静态文件正文因此每响应白付一次 memcpy
        OutboundStream &outbound = outboundFor(streamId);
        appendHttp3FrameWithPayload(outbound.bytes, frameType, payload);
        outbound.isEndStream = outbound.isEndStream || isEndStream;
    }

    void Http3Connection::applyPeerSettings(const Http3SettingsFrame &settingsFrame)
    {
        std::size_t peerTableCapacityByteCount = 0;
        std::size_t peerMaximumBlockedStreamCount = 0;
        for (const auto &[settingId, value]: settingsFrame.settings)
        {
            switch (settingId)
            {
                case Http3SettingId::QpackMaxTableCapacity:
                    peerTableCapacityByteCount = static_cast<std::size_t>(value);
                    break;
                case Http3SettingId::QpackBlockedStreams:
                    peerMaximumBlockedStreamCount = static_cast<std::size_t>(value);
                    break;
                case Http3SettingId::MaxFieldSectionSize:
                case Http3SettingId::EnableConnectProtocol:
                    // 前者只约束本端发出去的头段大小（响应头都由本类生成，远低于上限），
                    // 后者对服务端而言只是「对端是否接受本端把它当隧道」的声明，不参与判定
                    break;
            }
        }
        for (const auto &unknown: settingsFrame.unknownSettings)
        {
            LOG_DEBUG_FMT("Http3Connection: 忽略对端 settings 里本端不认识的一项（标识 {}、取值 {}）", unknown.identifier, unknown.value);
        }

        m_peerMaximumBlockedStreamCount = peerMaximumBlockedStreamCount;
        const std::size_t effectiveCapacityByteCount = std::min(peerTableCapacityByteCount, m_localSettings.qpackMaximumTableCapacityByteCount);
        if (effectiveCapacityByteCount == 0)
        {
            return; // 对端不接动态表：编码器保持只用静态表
        }
        // 构造时对端能力还不知道，编码器是按「对端上限 0」建的，直接调容量必然被拒。
        // 此刻还没编过任何引用动态表的头块（上限 0 时插不进去），重建实例不会让对端索引错位（§2.1.3）
        m_qpackEncoder.emplace(peerTableCapacityByteCount, peerMaximumBlockedStreamCount, std::size_t{0});
        std::string encoderStreamBytes;
        if (const auto applied = m_qpackEncoder->setMaximumTableCapacityByteCount(effectiveCapacityByteCount, encoderStreamBytes); !applied)
        {
            LOG_WARN_FMT("Http3Connection: 按对端容量启用动态表失败（{}），本端响应继续只用静态表", applied.error().message);
            return;
        }
        queueQpackInstructions(encoderStreamBytes, {});
    }

    Http3Connection::StreamState &Http3Connection::streamStateFor(const std::int64_t streamId)
    {
        const auto entry = m_streams.find(streamId);
        if (entry != m_streams.end())
        {
            return entry->second;
        }
        return m_streams[streamId];
    }

    void Http3Connection::closeStreamIfDone(const std::int64_t streamId)
    {
        const auto entry = m_streams.find(streamId);
        if (entry == m_streams.end())
        {
            return;
        }
        const StreamState &state = entry->second;
        if (!state.isPeerFinished || !state.isLocalFinished || state.isAbandoned)
        {
            return;
        }
        // 两侧都收尾了才摘状态并发通知：早一步摘掉就会让还在路上的正文找不到归属
        if (m_callbacks.onStreamClosed)
        {
            m_callbacks.onStreamClosed(streamId);
        }
        m_streams.erase(entry);
    }

    void Http3Connection::breakConnection(const Http3ErrorCode errorCode, const std::string_view reason)
    {
        if (m_isBroken)
        {
            return; // 只记第一个原因：后面的都是同一个故障的连带表现
        }
        m_isBroken = true;
        m_connectionErrorCode = errorCode;
        m_connectionErrorReason.assign(reason);
        LOG_WARN_FMT("Http3Connection: {}（{}），HTTP/3 协议层作废", reason, http3ErrorCodeName(errorCode));
        if (m_callbacks.onConnectionClosed)
        {
            m_callbacks.onConnectionClosed(errorCode, reason);
        }
    }

    void Http3Connection::rejectRequestHead(const std::int64_t streamId, StreamState &state, const std::string_view reason)
    {
        // 不在这里重置：RFC 9114 §4.1.2 允许服务端先回一个错误响应再收流，而回哪个状态码属业务层
        // （400/431/414 的区分只有会话与 Router 知道）。会话据此作答，作答完照常结束这条流
        LOG_WARN_FMT("Http3Connection: 流 {} 的请求头部畸形（{}），已交回会话按错误响应处置", streamId, reason);
        state.isHeadRejected = true;
        if (m_callbacks.onMalformedRequest)
        {
            m_callbacks.onMalformedRequest(streamId, reason);
            return;
        }
        // 没有作答口可接时只能自己收场：按 §4.1.2 以 H3_MESSAGE_ERROR 放弃该流
        failStream(streamId, Http3ErrorCode::MessageError, reason);
    }

    void Http3Connection::failStream(const std::int64_t streamId, const Http3ErrorCode errorCode, const std::string_view reason)
    {
        LOG_WARN_FMT("Http3Connection: 流 {} 被判定为 {}（{}），该流作废；连接与其它流不受影响", streamId, reason,
                     http3ErrorCodeName(errorCode));
        m_outbound.erase(streamId);

        const auto entry = m_streams.find(streamId);
        if (entry != m_streams.end())
        {
            entry->second.isAbandoned = true;
        }
        if (m_callbacks.onStreamReset)
        {
            m_callbacks.onStreamReset(streamId, errorCode);
        }
        // 这里不摘状态：调用链上到处握着 StreamState 的引用（帧循环、额度归还、收尾判定都在用），
        // 当场 erase 就是让那些引用悬空。标记已放弃后交给 pruneAbandonedStream 在安全点回收
    }

    void Http3Connection::pruneAbandonedStream(const std::int64_t streamId)
    {
        const auto entry = m_streams.find(streamId);
        if (entry != m_streams.end() && entry->second.isAbandoned)
        {
            m_streams.erase(entry);
        }
    }
} // namespace AsynGyanis::Net
