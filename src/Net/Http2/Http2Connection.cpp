#include "Net/Http2/Http2Connection.h"

#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 待发缓冲的压缩阈值：前缀攒到这么多就整段搬一次，均摊到每帧是 O(1)
        constexpr std::size_t kPendingDataCompactThresholdByteCount = 64 * 1024;
    } // namespace

    namespace
    {
        /// RFC 7540 §6 定义过的帧类型取值上界（CONTINUATION = 0x9）：大于它的按 §4.1 忽略
        constexpr std::uint8_t kLastKnownFrameTypeValue = 0x9;

        /// 已终止流的记录保留条数：记录只为区分「忽略」与「判错」而留，超上限就挤掉最旧的，账本不随连接时长增长
        constexpr std::size_t kTerminatedStreamMemoryCount = 256;

        /// 连接特定头（RFC 7540 §8.1.2.2）：HTTP/2 里一律不得出现，收到即判该流不合规
        constexpr std::string_view kConnectionSpecificHeaderNames[] = {"connection", "keep-alive", "proxy-connection",
                                                                      "transfer-encoding", "upgrade"};

        /**
         * @brief 在对外方法入口清空可选出参
         * @details 调用方常复用同一个字符串跨多次调用，若只在失败时写入，成功返回的调用会把上一次的
         *          失败原因留在出参里，「这次是不是失败」的判据随即失效。
         * @param errorText 出参指针，为空时什么都不做
         */
        void clearError(std::string *errorText) noexcept
        {
            if (errorText != nullptr)
            {
                errorText->clear();
            }
        }

        /**
         * @brief 把失败原因写入可选出参
         * @param errorText 出参指针，为空时什么都不做
         * @param reason 中文失败原因，须写清原因与替代做法
         */
        void writeError(std::string *errorText, std::string reason)
        {
            if (errorText != nullptr)
            {
                *errorText = std::move(reason);
            }
        }

        /**
         * @brief 判断字符是否含 ASCII 大写字母
         * @param text 待检查的文本
         * @return true 表示含 A-Z
         */
        bool containsUppercaseAscii(const std::string_view text) noexcept
        {
            for (const char character: text)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 判断头名是否只由 RFC 9110 的 token 字符组成
         * @details 与 HttpParser 判头部行的规则同一套：HTTP/2 的头一旦交给上层业务，就必须和 HTTP/1.1
         *          路径交出的字节满足同样的可用性约束，否则同一段转发代码在两条协议上行为不同。
         * @param name 头名
         * @return true 表示是合法的 token
         */
        bool isTokenName(const std::string_view name) noexcept
        {
            return containsOnlyTokenCharacters(name);
        }

        /**
         * @brief 判断头值是否只由 RFC 9110 允许的字节组成
         * @details 按 HttpParser 的同一套判定：HTAB、可见 ASCII 与 obs-text（0x80 以上）可接收，其余控制
         *          字符（含 CR、LF、NUL、DEL）一律拒绝，它们既不出现在合法报文里，又是注入的经典入口。
         * @param value 头值
         * @return true 表示合法
         */
        bool isHeaderValueBytes(const std::string_view value) noexcept
        {
            return containsOnlyFieldValueCharacters(value);
        }

        /**
         * @brief 取连接状态的中文名，用于错误文案与日志
         * @param connectionState 连接状态
         * @return std::string_view 中文名
         */
        std::string_view http2ConnectionStateName(const Http2ConnectionState connectionState) noexcept
        {
            switch (connectionState)
            {
                case Http2ConnectionState::AwaitingPreface:
                    return "等待前奏";
                case Http2ConnectionState::AwaitingSettings:
                    return "等待对端 SETTINGS";
                case Http2ConnectionState::Open:
                    return "正常收发";
                case Http2ConnectionState::Closing:
                    return "关闭中（已发或已收 GOAWAY）";
                case Http2ConnectionState::Failed:
                    return "已失败";
            }
            return "未定义状态";
        }
    } // namespace

    const std::string *Http2Request::findHeaderValue(const std::string_view name) const noexcept
    {
        // 按到达顺序取第一条命中：本对象已保证头名小写，比较不必再做归一化
        for (const HpackHeaderField &field: headerFields)
        {
            if (field.name == name)
            {
                return &field.value;
            }
        }
        return nullptr;
    }

    Http2Connection::Http2Connection(Http2ConnectionConfiguration configuration)
        : m_configuration(std::move(configuration)),
          m_frameDecoder(Http2FrameLimits{.maximumFrameSizeByteCount = m_configuration.maximumFrameSize,
                                          .maximumTotalConsumedByteCount = m_configuration.maximumTotalConsumedByteCount}),
          m_hpackDecoder(HpackDecoderLimits{.maximumDynamicTableSizeByteCount = m_configuration.headerTableSize,
                                            .maximumHeaderListByteCount = m_configuration.maximumHeaderListSize})
    {
        // 配置取值在本端通告出去之前就校验：非法值一旦发出去，对端只能按连接错误收场（§6.5.2）
        if (m_configuration.enablePush > 1)
        {
            throw Base::InvalidArgumentException(std::format("HTTP/2 连接配置的 ENABLE_PUSH 只能是 0 或 1（RFC 7540 §6.5.2），"
                                                             "收到 {}：请改为 0（本端不推送）或 1（允许对端期待推送）",
                                                             m_configuration.enablePush));
        }
        if (m_configuration.enableConnectProtocol > 1)
        {
            throw Base::InvalidArgumentException(std::format("HTTP/2 连接配置的 ENABLE_CONNECT_PROTOCOL 只能是 0 或 1（RFC 8441 §3），"
                                                             "收到 {}：请改为 1（接受扩展 CONNECT）或 0（不接受）",
                                                             m_configuration.enableConnectProtocol));
        }
    }

    Http2ConnectionFeedStatus Http2Connection::feedBytes(const char *const data, const std::size_t length)
    {
        // 失败态粘滞：剩余字节已经无从解释，继续消费只会把真正的根因埋掉
        if (m_state == Http2ConnectionState::Failed)
        {
            return Http2ConnectionFeedStatus::Failed;
        }

        std::size_t consumedByteCount = 0;
        while (consumedByteCount < length)
        {
            // 前奏阶段只按字节比对：它是固定的 24 字节串，任何一位不符都不必再等后续字节
            if (m_state == Http2ConnectionState::AwaitingPreface)
            {
                const std::size_t remainingByteCount = kHttp2ConnectionPreface.size() - m_prefaceByteCount;
                const std::size_t takenByteCount = std::min(remainingByteCount, length - consumedByteCount);
                for (std::size_t byteIndex = 0; byteIndex < takenByteCount; ++byteIndex)
                {
                    const char actualByte = data[consumedByteCount + byteIndex];
                    const char expectedByte = kHttp2ConnectionPreface[m_prefaceByteCount + byteIndex];
                    if (actualByte != expectedByte)
                    {
                        const std::size_t position = m_prefaceByteCount + byteIndex + 1;
                        fail(Http2ErrorCode::ProtocolError,
                             std::format("客户端前奏第 {} 个字节不匹配（RFC 7540 §3.5）：应当是 0x{:02X}，实际是 0x{:02X}；"
                                         "前奏必须是 \"PRI * HTTP/2.0\\r\\n\\r\\nSM\\r\\n\\r\\n\"（共 24 字节）",
                                         position, static_cast<unsigned int>(static_cast<unsigned char>(expectedByte)),
                                         static_cast<unsigned int>(static_cast<unsigned char>(actualByte))));
                        return Http2ConnectionFeedStatus::Failed;
                    }
                }
                m_prefaceByteCount += takenByteCount;
                consumedByteCount += takenByteCount;
                // 前奏收齐：本端立刻发初始 SETTINGS，并转入「等对端 SETTINGS」
                if (m_prefaceByteCount == kHttp2ConnectionPreface.size())
                {
                    sendInitialSettings();
                    m_state = Http2ConnectionState::AwaitingSettings;
                }
                continue;
            }

            const Http2FrameDecodeStatus decodeStatus = m_frameDecoder.parse(data + consumedByteCount, length - consumedByteCount);
            if (decodeStatus == Http2FrameDecodeStatus::NeedMore)
            {
                // 帧还没凑齐：本段字节已全部交给解码器缓冲，等上层再喂
                break;
            }
            if (decodeStatus == Http2FrameDecodeStatus::Error)
            {
                // 帧层判错：错误码直接取它的映射（协议错、尺寸错、本端上限各有对应）
                fail(toHttp2ErrorCode(m_frameDecoder.errorKind()), "帧层解码失败：" + m_frameDecoder.errorMessage());
                return Http2ConnectionFeedStatus::Failed;
            }

            consumedByteCount += m_frameDecoder.consumedByteCount();
            Http2Frame frame = m_frameDecoder.takeFrame();
            if (!handleFrame(std::move(frame)))
            {
                return Http2ConnectionFeedStatus::Failed;
            }
        }
        return Http2ConnectionFeedStatus::NeedMore;
    }

    std::string Http2Connection::takeOutgoingBytes()
    {
        // 按移动交出并清空：调用方拿到的是一批完整字节，内部缓冲随即可以复用
        return std::exchange(m_outgoingBytes, std::string{});
    }

    void Http2Connection::recycleOutgoingBytes(std::string &&buffer) noexcept
    {
        // 只在内部缓冲为空时回收：非空说明取走之后又产生了新字节（例如写挂起期间排队的帧），
        // 换过去就是覆盖在途数据
        if (!m_outgoingBytes.empty())
        {
            return;
        }
        buffer.clear();
        m_outgoingBytes = std::move(buffer);
    }

    std::vector<Http2Request> Http2Connection::takeRequests()
    {
        return std::exchange(m_pendingRequests, std::vector<Http2Request>{});
    }

    std::vector<Http2ReceivedData> Http2Connection::takeReceivedData()
    {
        return std::exchange(m_pendingReceivedData, std::vector<Http2ReceivedData>{});
    }

    bool Http2Connection::creditReceivedData(const std::uint32_t streamId, const std::size_t byteCount, std::string *const errorText)
    {
        clearError(errorText);
        // 零长 DATA 帧不占窗口，没什么可还
        if (byteCount == 0)
        {
            return true;
        }
        if (m_state == Http2ConnectionState::Failed)
        {
            writeError(errorText, std::format("连接已失败（{}）：不再补发 WINDOW_UPDATE，请按 errorCode() 终止连接", m_errorMessage));
            return false;
        }
        // 窗口增量的线上字段是 31 位（§6.9.1）：超过它只能是调用方报错了数量，宁可拒绝也不静默截断
        if (byteCount > kHttp2MaximumWindowSizeByteCount)
        {
            writeError(errorText, std::format("本次消费字节数 {} 超过窗口上限 2^31-1：请按 Http2ReceivedData::flowControlByteCount 逐片报量",
                                             byteCount));
            return false;
        }

        creditConnectionReceiveWindow(byteCount);
        // 流已终止时只还连接级窗口：对已终止流的流级 WINDOW_UPDATE 会被对端按 §5.1 忽略
        StreamRecord *const stream = findStream(streamId);
        creditStreamReceiveWindow(stream, byteCount);
        return true;
    }

    bool Http2Connection::sendGoAway(const std::string_view reason, std::string *const errorText)
    {
        clearError(errorText);
        // 失败态的 GOAWAY 已经带过真正的错误码：再补一条 NO_ERROR 会让对端把故障当正常收尾
        if (m_state == Http2ConnectionState::Failed)
        {
            writeError(errorText, "连接已进入失败态：本端已经按 errorCode() 发过 GOAWAY，收尾通告不再发；请直接收口连接");
            return false;
        }
        // 协商完成前对端还没有解释 GOAWAY 的前提（前奏与 SETTINGS 都没到），此刻发没有意义
        if (m_state == Http2ConnectionState::AwaitingPreface || m_state == Http2ConnectionState::AwaitingSettings)
        {
            writeError(errorText, std::format("连接尚未完成 HTTP/2 协商（当前状态是「{}」）：请等收到对端 SETTINGS 之后再发收尾通告",
                                              http2ConnectionStateName(m_state)));
            return false;
        }
        // 已经在关闭中：重复通告不会改写已经告知对端的 last-stream-id，只会让对端多收一帧
        if (m_state == Http2ConnectionState::Closing)
        {
            writeError(errorText, "连接已经在关闭中（本端已发过收尾通告，或已收到对端 GOAWAY）："
                                  "重复调用不会改写已通告的 last-stream-id，请直接收口连接");
            return false;
        }

        // 收尾通告带上已处理的最大流号与 NO_ERROR（§6.8）：对端据此知道该号之前的流仍会被处理完，
        // 之后的新流没有生效，可以放心在新连接上重试
        Http2GoAwayPayload payload;
        payload.lastStreamId = m_highestPeerStreamId;
        payload.errorCode = Http2ErrorCode::NoError;
        payload.debugData = std::string(reason);
        appendOutgoing(encodeHttp2GoAwayFrame(payload));
        // 转 Closing：此后新流一律回 REFUSED_STREAM，既有流照旧收发（与收到对端 GOAWAY 同一状态）
        m_state = Http2ConnectionState::Closing;
        return true;
    }

    bool Http2Connection::abortStream(const std::uint32_t streamId, const std::string_view reason, std::string *const errorText)
    {
        clearError(errorText);
        if (m_state != Http2ConnectionState::Open && m_state != Http2ConnectionState::Closing)
        {
            writeError(errorText, std::format("连接当前状态是「{}」，不接受流操作：必须先收齐客户端前奏与对端 SETTINGS，且连接没有失败",
                                              http2ConnectionStateName(m_state)));
            return false;
        }

        StreamRecord *const stream = findActiveStream(streamId);
        if (stream == nullptr)
        {
            writeError(errorText, std::format("流 {} 不在账本里或已经终止：只有对端开过、还没收尾的流才谈得上中止", streamId));
            return false;
        }

        // NO_ERROR：RFC 9113 §8.1 允许服务端在发完完整响应后这样请对端中止请求正文（不是「出错」，
        // 而是「这条流不再需要了」），因此走同一个「终止单流」的收口路径
        failStream(*stream, Http2ErrorCode::NoError, std::string(reason));
        return true;
    }

    bool Http2Connection::failConnection(const Http2ErrorCode errorCode, const std::string_view reason, std::string *const errorText)
    {
        clearError(errorText);
        // NO_ERROR 是收尾通告的码：走 sendGoAway()，它通告 last-stream-id 并转入关闭中，语义完全不同
        if (errorCode == Http2ErrorCode::NoError)
        {
            writeError(errorText, "连接错误码不能是 NO_ERROR：正常收尾请改用 sendGoAway()（它通告 last-stream-id 并转入关闭中）");
            return false;
        }
        // 失败态已经带过一次真正的错误码：再补一条只会让对端把故障当成别的故障
        if (m_state == Http2ConnectionState::Failed)
        {
            writeError(errorText, std::format("连接已进入失败态（{}）：GOAWAY 已经按 errorCode() 发过一次，不再重复发；请直接收口连接",
                                              m_errorMessage));
            return false;
        }

        // 复用连接级失败的统一记账：置粘滞失败态并把带该错误码的 GOAWAY 排进待发字节（§6.8）
        fail(errorCode, std::string(reason));
        return true;
    }

    bool Http2Connection::hasSettingsAwaitingAcknowledgement() const noexcept
    {
        return m_outstandingSettingsCount > 0;
    }

    std::chrono::steady_clock::time_point Http2Connection::lastSettingsSentTime() const noexcept
    {
        return m_lastSettingsSentTime;
    }

    Http2ResponseSendStatus Http2Connection::sendResponseHeaders(const std::uint32_t streamId, const std::uint32_t statusCode,
                                                                 const std::vector<HpackHeaderField> &headerFields, const bool endStream,
                                                                 std::string *const errorText)
    {
        clearError(errorText);
        if (m_state != Http2ConnectionState::Open && m_state != Http2ConnectionState::Closing)
        {
            writeError(errorText, std::format("连接当前状态是「{}」，不接受响应：必须先收齐客户端前奏与对端 SETTINGS，且连接没有失败",
                                             http2ConnectionStateName(m_state)));
            return Http2ResponseSendStatus::ConnectionUnavailable;
        }

        StreamRecord *const stream = findActiveStream(streamId);
        if (stream == nullptr)
        {
            writeError(errorText, std::format("流 {} 不在账本里或已经终止：只有对端开过、还没收尾的流才能回响应，"
                                             "请用 takeRequests() 交出的 streamId，或另开新流",
                                             streamId));
            return Http2ResponseSendStatus::StreamNotWritable;
        }
        if (stream->state == Http2StreamState::HalfClosedLocal)
        {
            writeError(errorText, std::format("流 {} 上本端已经发过 END_STREAM：一条流只能有一个响应，请另开新流", streamId));
            return Http2ResponseSendStatus::Rejected;
        }
        if (statusCode < 100 || statusCode > 999)
        {
            writeError(errorText, std::format("响应状态码 {} 越界：HTTP 状态码是 100..999 的三位数字，请给出合法取值", statusCode));
            return Http2ResponseSendStatus::Rejected;
        }
        for (const HpackHeaderField &field: headerFields)
        {
            if (!acceptResponseHeaderField(field.name, field.value, errorText))
            {
                return Http2ResponseSendStatus::Rejected;
            }
        }

        // :status 必须排在最前（§8.1.2.1：伪头先于普通头部），其余按调用方给的顺序编码。
        // 这里只另起一张视图表，不再把每个头的名与值各拷两份字符串：整张 owning vector 拷一遍
        // 是每条响应第二次的无谓往返，编码器读到的字节完全一样
        std::string statusCodeText = std::to_string(statusCode);
        std::vector<HpackHeaderFieldView> fields;
        fields.reserve(headerFields.size() + 1U);
        fields.push_back(HpackHeaderFieldView{.name = std::string_view(":status"), .value = statusCodeText});
        for (const HpackHeaderField &field: headerFields)
        {
            fields.push_back(HpackHeaderFieldView{.name = field.name, .value = field.value});
        }

        emitHeaderBlock(streamId, m_encoder.encode(fields), endStream);
        if (endStream)
        {
            // 响应头就带 END_STREAM：本端方向到此为止（无正文）
            noteLocalEndStream(*stream);
        }
        return Http2ResponseSendStatus::Sent;
    }

    Http2ResponseSendStatus Http2Connection::sendResponseData(const std::uint32_t streamId, const std::string_view data,
                                                              const bool endStream, std::string *const errorText)
    {
        clearError(errorText);
        if (m_state != Http2ConnectionState::Open && m_state != Http2ConnectionState::Closing)
        {
            writeError(errorText, std::format("连接当前状态是「{}」，不接受响应：必须先收齐客户端前奏与对端 SETTINGS，且连接没有失败",
                                             http2ConnectionStateName(m_state)));
            return Http2ResponseSendStatus::ConnectionUnavailable;
        }

        StreamRecord *const stream = findActiveStream(streamId);
        if (stream == nullptr)
        {
            writeError(errorText, std::format("流 {} 不在账本里或已经终止：只有对端开过、还没收尾的流才能发正文", streamId));
            return Http2ResponseSendStatus::StreamNotWritable;
        }
        if (stream->state == Http2StreamState::HalfClosedLocal)
        {
            writeError(errorText, std::format("流 {} 上本端已经发过 END_STREAM：不能再发正文，请另开新流", streamId));
            return Http2ResponseSendStatus::Rejected;
        }
        if (stream->isEndStreamPending)
        {
            writeError(errorText, std::format("流 {} 上已经安排了 END_STREAM（可能还在等窗口）：本片之后只能由对端收尾，"
                                             "请不要再追加正文",
                                             streamId));
            return Http2ResponseSendStatus::Rejected;
        }

        // 正文先进队列再按窗口尽量出帧：窗口不足的部分留在这里，等对端 WINDOW_UPDATE 进来后由 feedBytes() 续发
        stream->pendingData.append(data);
        if (endStream)
        {
            stream->isEndStreamPending = true;
        }
        pumpSendQueue(*stream);
        return Http2ResponseSendStatus::Sent;
    }

    Http2ConnectionState Http2Connection::state() const noexcept
    {
        return m_state;
    }

    bool Http2Connection::hasFailed() const noexcept
    {
        return m_state == Http2ConnectionState::Failed;
    }

    Http2ErrorCode Http2Connection::errorCode() const noexcept
    {
        return m_errorCode;
    }

    std::string Http2Connection::errorMessage() const
    {
        return m_errorMessage;
    }

    std::string Http2Connection::lastStreamErrorMessage() const
    {
        return m_lastStreamErrorMessage;
    }

    bool Http2Connection::tryGetStreamState(const std::uint32_t streamId, Http2StreamState &streamState) const noexcept
    {
        const auto streamIterator = m_streams.find(streamId);
        if (streamIterator == m_streams.end())
        {
            return false;
        }
        streamState = streamIterator->second.state;
        return true;
    }

    std::size_t Http2Connection::openStreamCount() const noexcept
    {
        return m_openStreamCount;
    }

    std::size_t Http2Connection::pendingResponseByteCount(const std::uint32_t streamId) const noexcept
    {
        const auto streamIterator = m_streams.find(streamId);
        if (streamIterator == m_streams.end())
        {
            return 0;
        }
        // 整段缓冲都算：游标之前的前缀虽然已经出过帧，但在压缩掉之前仍是本端占住的内存
        return streamIterator->second.pendingData.size();
    }

    std::size_t Http2Connection::totalPendingResponseByteCount() const noexcept
    {
        // 逐条流相加而不另记总量：账本里也含刚终止的流（它们的队列应为空），另记一本账就要在每个
        // 增删点跟上，漏一处就把连接级闸门算歪
        std::size_t totalByteCount = 0;
        for (const auto &[streamId, stream]: m_streams)
        {
            static_cast<void>(streamId);
            totalByteCount += stream.pendingData.size();
        }
        return totalByteCount;
    }

    bool Http2Connection::tryGetPeerSetting(const Http2SettingIdentifier identifier, std::uint32_t &value) const noexcept
    {
        const auto settingIterator = m_peerSettings.find(static_cast<std::uint16_t>(identifier));
        if (settingIterator == m_peerSettings.end())
        {
            return false;
        }
        value = settingIterator->second;
        return true;
    }

    bool Http2Connection::tryGetPeerGoAway(Http2GoAwayPayload &payload) const
    {
        if (!m_hasPeerGoAway)
        {
            return false;
        }
        payload = m_peerGoAway;
        return true;
    }

    void Http2Connection::sendInitialSettings()
    {
        // 参数按 §6.5.2 的标识顺序写出：对端不必按序处理，但有序列出的字节便于逐字段比对与排查
        Http2SettingsPayload payload;
        payload.parameters = {
            {static_cast<std::uint16_t>(Http2SettingIdentifier::HeaderTableSize), m_configuration.headerTableSize},
            {static_cast<std::uint16_t>(Http2SettingIdentifier::EnablePush), m_configuration.enablePush},
            {static_cast<std::uint16_t>(Http2SettingIdentifier::MaxConcurrentStreams), m_configuration.maximumConcurrentStreams},
            {static_cast<std::uint16_t>(Http2SettingIdentifier::InitialWindowSize), m_configuration.initialWindowSize},
            {static_cast<std::uint16_t>(Http2SettingIdentifier::MaxFrameSize), m_configuration.maximumFrameSize},
            {static_cast<std::uint16_t>(Http2SettingIdentifier::MaxHeaderListSize), m_configuration.maximumHeaderListSize},
            {static_cast<std::uint16_t>(Http2SettingIdentifier::EnableConnectProtocol), m_configuration.enableConnectProtocol}};
        appendOutgoing(encodeHttp2SettingsFrame(payload));
        // 记下「有 1 个 SETTINGS 待确认」：对端的第一个 ACK 只能匹配它，第二个就是多余的。
        // 同时记下发帧时刻：上层按「该时刻 + 握手期限额」算 SETTINGS_TIMEOUT 的空闲截止时间
        m_outstandingSettingsCount = 1;
        m_lastSettingsSentTime = std::chrono::steady_clock::now();
    }

    bool Http2Connection::handleFrame(Http2Frame frame)
    {
        const Http2FrameType frameType = frame.header.type;
        const std::uint8_t frameTypeValue = static_cast<std::uint8_t>(frameType);

        // 头块拼接期间只允许同流的 CONTINUATION（§6.10）：任何插入都会让拼出的头块与 HPACK 上下文错位
        if (m_isAssemblingHeaderBlock)
        {
            if (frameTypeValue != static_cast<std::uint8_t>(Http2FrameType::Continuation) ||
                frame.header.streamId != m_pendingHeaderStreamId)
            {
                fail(Http2ErrorCode::ProtocolError,
                     std::format("流 {} 的头块还没收完（缺 END_HEADERS），此刻收到{}（流 {}）：RFC 7540 §6.10 要求 CONTINUATION "
                                 "不得被任何其它帧打断",
                                 m_pendingHeaderStreamId, http2FrameTypeName(frameType), frame.header.streamId));
                return false;
            }
            return handleContinuationFrame(frame);
        }

        // 前奏之后的第一个帧必须是客户端自己的 SETTINGS（§3.5）：ACK 不是前奏的一部分
        if (m_state == Http2ConnectionState::AwaitingSettings &&
            (frameTypeValue != static_cast<std::uint8_t>(Http2FrameType::Settings) || (frame.header.flags & kHttp2FlagAcknowledge) != 0))
        {
            fail(Http2ErrorCode::ProtocolError,
                 std::format("客户端前奏之后的第一个帧必须是 SETTINGS（RFC 7540 §3.5），收到{}（流 {}）",
                             http2FrameTypeName(frameType), frame.header.streamId));
            return false;
        }

        // 未定义的类型按 §4.1 忽略：不改变任何状态，也不回帧
        if (frameTypeValue > kLastKnownFrameTypeValue)
        {
            return true;
        }

        switch (frameType)
        {
            case Http2FrameType::Data:
                return handleDataFrame(frame);
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
                fail(Http2ErrorCode::ProtocolError,
                     std::format("收到流 {} 的 PUSH_PROMISE：RFC 7540 §8.2 只允许服务端发送推送承诺，客户端发送即连接错误",
                                 frame.header.streamId));
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
        const std::uint32_t streamId = frame.header.streamId;
        std::string errorText;
        if (!acceptPeerStreamIdParity(streamId, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, std::move(errorText));
            return false;
        }

        Http2DataPayload payload;
        if (!parseHttp2DataPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }

        // 流控记账（RFC 7540 §6.9.1）：DATA 帧的**整段负载**都占窗口——含帧层剥掉的 padding，
        // 也含随后会被忽略或判错的那些帧。对端已经按自己的账扣过一次，本端不记就是漏账
        const std::int64_t frameByteCount = static_cast<std::int64_t>(frame.header.payloadLength);
        m_connectionReceiveWindowByteCount -= frameByteCount;
        if (m_connectionReceiveWindowByteCount < 0)
        {
            fail(Http2ErrorCode::FlowControlError,
                 std::format("连接级接收窗口被突破：对端在本端通告的窗口只剩 {} 字节时又发出了 {} 字节的 DATA（RFC 7540 §6.9.1），"
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
            // 一条记录都没被挤掉时「流号不超过已用最大值而账本里又没有」就等于「这条流从未开过」，
            // 这种能证伪的违约帧不能替对端咽下去
            fail(Http2ErrorCode::ProtocolError,
                 std::format("流 {} 从未开启（idle），不能在该流上发 DATA（RFC 7540 §5.1）；本端已用过的最大对端流号是 {}",
                             streamId, m_highestPeerStreamId));
            return false;
        }
        if (stream->state != Http2StreamState::Closed)
        {
            // 流级窗口同样按整段负载扣：负数说明对端突破了本端通告的 SETTINGS_INITIAL_WINDOW_SIZE
            stream->receiveWindowByteCount -= frameByteCount;
            if (stream->receiveWindowByteCount < 0)
            {
                fail(Http2ErrorCode::FlowControlError,
                     std::format("流 {} 的接收窗口被突破：对端在窗口只剩 {} 字节时又发出了 {} 字节的 DATA（RFC 7540 §6.9.1）",
                                 streamId, stream->receiveWindowByteCount + frameByteCount, frameByteCount));
                return false;
            }
        }
        if (stream->state == Http2StreamState::Closed)
        {
            if (isIgnorableFrameOnTerminatedStream(*stream, Http2FrameType::Data))
            {
                // 这片数据不会再交给上层（流已终止），就当它已被消费：把连接级窗口还回去，
                // 否则对端在终止流上补发的在途 DATA 会永久吃掉连接窗口
                creditConnectionReceiveWindow(static_cast<std::size_t>(frameByteCount));
                return true;
            }
            fail(Http2ErrorCode::StreamClosed,
                 std::format("流 {} 已正常终止（双向 END_STREAM），再收到 DATA：RFC 7540 §5.1「closed」段要求按连接错误 STREAM_CLOSED 处理",
                             streamId));
            return false;
        }
        if (stream->state == Http2StreamState::HalfClosedRemote)
        {
            // 对端自己收过尾了：再发 DATA 是流错误，只关掉这条流（§5.1）
            failStream(*stream, Http2ErrorCode::StreamClosed,
                       std::format("流 {} 的对端已 END_STREAM（half-closed (remote)），不能再发 DATA", streamId));
            return true;
        }

        // 正常正文：收下并交给上层；END_STREAM 落在这一片上时推进流状态
        Http2ReceivedData receivedData;
        receivedData.streamId = streamId;
        receivedData.data = std::move(payload.data);
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
        std::string errorText;
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
                fail(Http2ErrorCode::ProtocolError,
                     std::format("流号 {} 不是新流（本端已用过的最大对端流号是 {}）：RFC 7540 §5.1.1 要求新流的流号严格递增，"
                                 "请检查对端是否重用了流号",
                                 streamId, m_highestPeerStreamId));
                return false;
            }
            // 连接进入关闭中之后不再受理新流（§6.8）：明确回 REFUSED_STREAM，让对端知道这条流没有被处理
            if (m_state == Http2ConnectionState::Closing)
            {
                refuseNewStream(streamId, std::format("流 {} 是连接进入关闭中之后新开的流（本端已发收尾 GOAWAY 或已收到对端 GOAWAY，"
                                                      "RFC 7540 §6.8：GOAWAY 之后不得再开新流）",
                                                      streamId));
                return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment,
                                        payload.endHeaders);
            }
            // 并发上限（§5.1.2）：回 REFUSED_STREAM，连接继续为其它流服务
            if (m_openStreamCount >= m_configuration.maximumConcurrentStreams)
            {
                refuseNewStream(streamId, std::format("并发流数已达上限 {}（SETTINGS_MAX_CONCURRENT_STREAMS，RFC 7540 §5.1.2）",
                                                      m_configuration.maximumConcurrentStreams));
                return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment,
                                        payload.endHeaders);
            }
            openStream(streamId);
            return beginHeaderBlock(streamId, HeaderBlockPurpose::Request, payload.endStream, payload.headerBlockFragment,
                                    payload.endHeaders);
        }

        if (stream->state == Http2StreamState::Closed)
        {
            if (!isIgnorableFrameOnTerminatedStream(*stream, Http2FrameType::Headers))
            {
                fail(Http2ErrorCode::StreamClosed,
                     std::format("流 {} 已正常终止（双向 END_STREAM），再收到 HEADERS：RFC 7540 §5.1「closed」段要求按连接错误 STREAM_CLOSED 处理",
                                 streamId));
                return false;
            }
            // 终止过的流上补发的 HEADERS：字段已经没有归属，但字节必须解码——带增量索引的表示已经改动了
            // 对端编码器的动态表，跳过一次解码会让后续头块的索引整体错位
            return beginHeaderBlock(streamId, HeaderBlockPurpose::Discard, payload.endStream, payload.headerBlockFragment,
                                    payload.endHeaders);
        }
        if (stream->state == Http2StreamState::HalfClosedRemote)
        {
            // 对端收过尾了：再发头块是流错误（§5.1「half-closed (remote)」），只关掉这条流
            failStream(*stream, Http2ErrorCode::StreamClosed,
                       std::format("流 {} 的对端已 END_STREAM（half-closed (remote)），不能再发 HEADERS", streamId));
            return true;
        }
        // 剩余情形是尾部头块（§8.1）：字段有意丢弃，但字节必须解码，否则动态表与对端编码器错位
        return beginHeaderBlock(streamId, HeaderBlockPurpose::Trailers, payload.endStream, payload.headerBlockFragment,
                                payload.endHeaders);
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

    bool Http2Connection::handleRstStreamFrame(const Http2Frame &frame)
    {
        const std::uint32_t streamId = frame.header.streamId;
        std::string errorText;
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
            fail(Http2ErrorCode::ProtocolError,
                 std::format("收到流 {} 的 RST_STREAM，但该流从未开启（idle）：RFC 7540 §5.1 只允许在 idle 流上发 HEADERS 与 PRIORITY",
                             streamId));
            return false;
        }
        if (stream->state == Http2StreamState::Closed)
        {
            // 已终止的流：RST_STREAM 按 §5.1「closed」段必须忽略（对端可能还没看到终止它的那一帧）
            return true;
        }
        // 对端主动取消（§5.4.2）：终止该流，队列里没发出去的正文一并丢掉
        terminateStream(*stream, true);
        return true;
    }

    bool Http2Connection::handleSettingsFrame(const Http2Frame &frame)
    {
        Http2SettingsPayload payload;
        std::string errorText;
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
                fail(Http2ErrorCode::ProtocolError,
                     "收到多余的 SETTINGS ACK：本端没有待确认的 SETTINGS（RFC 7540 §6.5.3 规定每个 SETTINGS 只回一个 ACK）");
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
        Http2PingPayload payload;
        std::string errorText;
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
        std::string errorText;
        if (!parseHttp2GoAwayPayload(frame, payload, &errorText))
        {
            fail(Http2ErrorCode::ProtocolError, errorText);
            return false;
        }
        // 记下对端通告并转入 Closing：不再受理新流，既有流继续做完（§6.8）
        m_hasPeerGoAway = true;
        m_peerGoAway = payload;
        if (m_state == Http2ConnectionState::Open)
        {
            m_state = Http2ConnectionState::Closing;
        }
        return true;
    }

    bool Http2Connection::handleWindowUpdateFrame(const Http2Frame &frame)
    {
        Http2WindowUpdatePayload payload;
        std::string errorText;
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
                 std::format("收到流 {} 的 WINDOW_UPDATE，但该流从未开启（idle）：RFC 7540 §5.1 只允许在 idle 流上发 HEADERS 与 PRIORITY",
                             streamId));
            return false;
        }
        if (stream->state == Http2StreamState::Closed)
        {
            // 已终止流：§5.1「closed」段明确要求忽略 WINDOW_UPDATE（对端可能还没看到终止它的那一帧）
            return true;
        }
        if (!increaseStreamSendWindow(*stream, payload.windowSizeIncrement))
        {
            return false;
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
                 std::format("收到没有前置 HEADERS 的 CONTINUATION（流 {}）：RFC 7540 §6.10 要求它紧跟在同一个流的 HEADERS 之后",
                             frame.header.streamId));
            return false;
        }
        Http2ContinuationPayload payload;
        std::string errorText;
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

    bool Http2Connection::beginHeaderBlock(const std::uint32_t streamId, const HeaderBlockPurpose purpose, const bool endStream,
                                           const std::string_view firstFragment, const bool endHeaders)
    {
        m_isAssemblingHeaderBlock = true;
        m_pendingHeaderStreamId = streamId;
        m_pendingHeaderPurpose = purpose;
        m_pendingHeaderEndStream = endStream;
        m_pendingHeaderBlock.clear();
        if (!appendHeaderBlockFragment(firstFragment))
        {
            return false;
        }
        // 整个头块就在 HEADERS 里：当场收尾，不必等 CONTINUATION
        if (endHeaders)
        {
            return finishHeaderBlock();
        }
        return true;
    }

    bool Http2Connection::appendHeaderBlockFragment(const std::string_view fragment)
    {
        // 拼接上限是本端策略：CONTINUATION 可以无限续，不设闸门等于让对端用一个头块撑爆内存
        if (m_pendingHeaderBlock.size() + fragment.size() > m_configuration.maximumHeaderBlockByteCount)
        {
            fail(Http2ErrorCode::EnhanceYourCalm,
                 std::format("流 {} 的头块压缩后已超过本端上限 {} 字节（HEADERS 与 CONTINUATION 片段之和）：本端无法在撑爆内存的前提下"
                             "继续同步 HPACK 动态表，只能终止连接，请对端减小头列表",
                             m_pendingHeaderStreamId, m_configuration.maximumHeaderBlockByteCount));
            return false;
        }
        m_pendingHeaderBlock.append(fragment);
        return true;
    }

    bool Http2Connection::finishHeaderBlock()
    {
        const std::uint32_t streamId = m_pendingHeaderStreamId;
        const HeaderBlockPurpose purpose = m_pendingHeaderPurpose;
        const bool endStream = m_pendingHeaderEndStream;
        std::string headerBlock = std::move(m_pendingHeaderBlock);
        // 拼接态先清掉：解码路径上可能判错收场，留着半成品会让后续帧被误判成「中途插帧」
        m_isAssemblingHeaderBlock = false;
        m_pendingHeaderBlock.clear();

        std::vector<HpackHeaderField> headerFields;
        if (!m_hpackDecoder.decode(headerBlock, headerFields))
        {
            if (!m_hpackDecoder.isLimitExceeded())
            {
                // 压缩上下文出错必须终止连接（§4.3）：两端的动态表已经错开，
                // 之后每个头块都会解成别的错误，边拒绝这条流边服务整条连接是不可能的
                fail(toHttp2ErrorCode(m_hpackDecoder.errorKind()), std::format("流 {} 的头块解码失败：{}", streamId, m_hpackDecoder.errorMessage()));
                return false;
            }
            // 越过头部上限只是「本端不收这一条请求」：解码器已把整块处理完，动态表仍与对端同步
            // （RFC 9113 §10.5.1 明说头块必须处理完以保证连接状态一致），因此按 431 作废这一条流
            // 就够了，不必把整条连接上其它在跑的流一起带走
            if (purpose == HeaderBlockPurpose::Discard)
            {
                // 这条流早先已被拒绝或已终止：解完即弃，动态表同步就是它唯一的作用
                return true;
            }
            if (purpose == HeaderBlockPurpose::Trailers)
            {
                // 尾部头块越限：响应已经发出，没有 431 可回，按流错误收掉这一条流
                if (StreamRecord *const trailerStream = findStream(streamId); trailerStream != nullptr)
                {
                    failStream(*trailerStream, Http2ErrorCode::EnhanceYourCalm,
                               std::format("流 {} 的尾部头块超出本端上限：{}", streamId, m_hpackDecoder.errorMessage()));
                }
                return true;
            }
            Http2Request oversizedRequest;
            oversizedRequest.streamId = streamId;
            oversizedRequest.hasBody = !endStream;
            oversizedRequest.isHeaderListTooLarge = true;
            m_pendingRequests.push_back(std::move(oversizedRequest));
            if (endStream)
            {
                if (StreamRecord *const closedStream = findStream(streamId); closedStream != nullptr)
                {
                    noteRemoteEndStream(*closedStream);
                }
            }
            return true;
        }

        StreamRecord *const stream = findStream(streamId);
        if (purpose == HeaderBlockPurpose::Discard)
        {
            // 已拒绝或已终止的流：字段丢弃，但解码本身必须做（带增量索引的表示已经改动了动态表）
            return true;
        }
        if (stream == nullptr || stream->state == Http2StreamState::Closed)
        {
            // 正常路径走不到这里：拼接期间这条流不可能被终止（帧插队已由 §6.10 挡住），兜底按忽略处理
            return true;
        }

        if (purpose == HeaderBlockPurpose::Trailers)
        {
            std::string errorText;
            if (!acceptTrailerHeaderFields(headerFields, &errorText))
            {
                failStream(*stream, Http2ErrorCode::ProtocolError,
                           std::format("流 {} 的尾部头块不合规：{}", streamId, errorText));
                return true;
            }
            // 尾部头块的字段有意不交给任何人：与 HttpParser 对分块 trailer 的既有处置一致（只校验语法与上限）。
            // 但它可能携带 END_STREAM（§8.1：服务端要接受以尾部头块收尾的请求），此时必须让上层看到
            // 「这条流的正文收齐了」——上层只按 Http2ReceivedData::endStream 判定收齐，不读流状态，
            // 少这一条零长片段，这条请求会一直等不到收齐、永远不路由
            if (endStream)
            {
                noteRemoteEndStream(*stream);
                // 零长片段：不占流控窗口（creditReceivedData() 对 0 直接放过），只用来传达收尾
                m_pendingReceivedData.push_back(Http2ReceivedData{.streamId = streamId,
                                                                  .data = {},
                                                                  .endStream = true,
                                                                  .flowControlByteCount = 0});
            }
            return true;
        }

        Http2Request request;
        std::string errorText;
        if (!acceptRequestHeaderFields(headerFields, request, &errorText))
        {
            // 请求语义不合规是流错误（§8.1.2.6）：RST_STREAM 这条流，连接继续服务其它流
            failStream(*stream, Http2ErrorCode::ProtocolError, std::format("流 {} 的请求头不合规：{}", streamId, errorText));
            return true;
        }
        request.streamId = streamId;
        request.hasBody = !endStream;
        m_pendingRequests.push_back(std::move(request));
        if (endStream)
        {
            noteRemoteEndStream(*stream);
        }
        return true;
    }

    bool Http2Connection::acceptRequestHeaderFields(const std::vector<HpackHeaderField> &headerFields, Http2Request &request,
                                                    std::string *const errorText)
    {
        clearError(errorText);
        request = Http2Request{};

        bool hasSeenRegularHeader = false;
        bool hasMethodField = false;
        bool hasSchemeField = false;
        bool hasPathField = false;
        bool hasAuthorityField = false;
        bool hasProtocolField = false;
        bool hasContentLengthField = false;
        std::size_t contentLengthValue = 0;
        for (const HpackHeaderField &field: headerFields)
        {
            // 头名必须全小写（§8.1.2）：HTTP/2 不允许大小写折叠，大写会让同一个头部出现两种写法
            if (field.name.empty())
            {
                writeError(errorText, "请求头里出现空头名：RFC 7540 §8.1.2 要求头部名必须是非空的小写字段名");
                return false;
            }
            if (containsUppercaseAscii(field.name))
            {
                writeError(errorText, std::format("请求头名 \"{}\" 含大写字母：RFC 7540 §8.1.2 要求 HTTP/2 的头部名必须全小写，"
                                                 "请让对端改成小写",
                                                 field.name));
                return false;
            }

            if (field.name.front() == ':')
            {
                // 伪头必须全部出现在普通头部之前（§8.1.2.1）
                if (hasSeenRegularHeader)
                {
                    writeError(errorText, std::format("伪头 \"{}\" 出现在普通头部之后：RFC 7540 §8.1.2.1 要求所有伪头必须排在普通头部之前",
                                                      field.name));
                    return false;
                }
                if (field.name == ":method")
                {
                    if (hasMethodField)
                    {
                        writeError(errorText, ":method 伪头出现了两次：RFC 7540 §8.1.2.3 要求每个请求带且只带一个");
                        return false;
                    }
                    hasMethodField = true;
                    request.method = field.value;
                }
                else if (field.name == ":scheme")
                {
                    if (hasSchemeField)
                    {
                        writeError(errorText, ":scheme 伪头出现了两次：RFC 7540 §8.1.2.3 要求每个请求带且只带一个");
                        return false;
                    }
                    hasSchemeField = true;
                    request.scheme = field.value;
                }
                else if (field.name == ":path")
                {
                    if (hasPathField)
                    {
                        writeError(errorText, ":path 伪头出现了两次：RFC 7540 §8.1.2.3 要求每个请求带且只带一个");
                        return false;
                    }
                    hasPathField = true;
                    request.path = field.value;
                }
                else if (field.name == ":authority")
                {
                    if (hasAuthorityField)
                    {
                        writeError(errorText, ":authority 伪头出现了两次：RFC 7540 §8.1.2.3 要求每个请求最多带一个");
                        return false;
                    }
                    hasAuthorityField = true;
                    request.authority = field.value;
                }
                else if (field.name == ":protocol")
                {
                    if (hasProtocolField)
                    {
                        writeError(errorText, ":protocol 伪头出现了两次：RFC 8441 §4 要求它只出现一次");
                        return false;
                    }
                    hasProtocolField = true;
                    request.protocol = field.value;
                }
                else
                {
                    writeError(errorText, std::format("出现未知伪头 \"{}\"：RFC 7540 §8.1.2.1 只定义了 :method/:scheme/:path/:authority 四个"
                                                     "（:protocol 见 RFC 8441）",
                                                      field.name));
                    return false;
                }
                if (!isHeaderValueBytes(field.value))
                {
                    writeError(errorText, std::format("伪头 \"{}\" 的值含 CR/LF/NUL 之类的控制字符：这类字节既不是合法头值，"
                                                     "又是注入的经典入口，请让对端改用百分号编码",
                                                     field.name));
                    return false;
                }
                continue;
            }

            hasSeenRegularHeader = true;
            // 连接特定头在 HTTP/2 里一律不存在（§8.1.2.2）：要让中间设备改写，只能靠扩展机制
            if (isConnectionSpecificHeaderName(field.name))
            {
                writeError(errorText, std::format("请求头里出现连接特定头 \"{}\"：RFC 7540 §8.1.2.2 禁止 connection/keep-alive/"
                                                 "proxy-connection/transfer-encoding/upgrade 出现在 HTTP/2 报文里",
                                                 field.name));
                return false;
            }
            // te 是唯一的例外：只允许取值 trailers（§8.1.2.2）
            if (field.name == "te" && field.value != "trailers")
            {
                writeError(errorText, std::format("请求头 te 的取值是 \"{}\"：RFC 7540 §8.1.2.2 只允许 te: trailers，请让对端改掉", field.value));
                return false;
            }
            if (!isTokenName(field.name))
            {
                writeError(errorText, std::format("请求头名 \"{}\" 含 token 之外的字符（空白、冒号前空白等）：RFC 7540 §8.1.2 要求头部名"
                                                 "符合字段名语法，请让对端按 token 字符集拼头名",
                                                 field.name));
                return false;
            }
            if (!isHeaderValueBytes(field.value))
            {
                writeError(errorText, std::format("请求头 \"{}\" 的值含 CR/LF/NUL 之类的控制字符：这类字节既不是合法头值，"
                                                 "又是注入的经典入口，请让对端改用百分号编码",
                                                 field.name));
                return false;
            }
            // content-length 与 h1 侧同一口径：取值必须是单个十进制数字，重复出现必须完全一致。
            // 长度有歧义时中间设备与业务可能各按一种读法理解正文边界，正是请求走私的形态
            if (field.name == "content-length")
            {
                std::size_t declaredLength = 0;
                if (!parseContentLengthValue(field.value, declaredLength) ||
                    (hasContentLengthField && declaredLength != contentLengthValue))
                {
                    writeError(errorText, std::format("请求头 content-length 的取值 \"{}\" 非法或前后冲突：RFC 9110 §8.6 要求它是"
                                                     "单个十进制数字，重复出现时必须完全一致",
                                                     field.value));
                    return false;
                }
                hasContentLengthField = true;
                contentLengthValue    = declaredLength;
            }
            request.headerFields.push_back(field);
        }

        if (!hasMethodField)
        {
            writeError(errorText, "请求缺少 :method 伪头：RFC 7540 §8.1.2.3 要求每个请求都必须带 :method");
            return false;
        }
        if (request.method.empty())
        {
            writeError(errorText, ":method 伪头的值为空：RFC 7540 §8.1.2.3 要求给出非空的方法名");
            return false;
        }
        if (!isTokenName(request.method))
        {
            writeError(errorText, std::format(":method 取值 \"{}\" 含 token 之外的字符：RFC 7540 §8.1.2.3 要求方法名符合字段名语法",
                                              request.method));
            return false;
        }

        // 带 :protocol 的请求只可能是 RFC 8441 的扩展 CONNECT：其它方法带它就是报文不合法
        if (hasProtocolField)
        {
            if (request.method != "CONNECT")
            {
                writeError(errorText, std::format(":protocol 伪头出现在 {} 请求里：RFC 8441 §4 只把它定义给 CONNECT，"
                                                 "请改用 CONNECT 或去掉该伪头",
                                                 request.method));
                return false;
            }
            if (!isTokenName(request.protocol))
            {
                writeError(errorText, std::format(":protocol 取值 \"{}\" 非法：RFC 8441 §4 要求它是一个协议名 token（本端支持 websocket）",
                                                 request.protocol));
                return false;
            }
        }

        // CONNECT 的伪头规则与其它方法不同（§8.3），而带 :protocol 的扩展 CONNECT 又把它反过来（RFC 8441 §4）
        if (request.method == "CONNECT")
        {
            if (hasProtocolField)
            {
                // 扩展 CONNECT：:scheme、:path、:authority 一个都不能少——它要的就是「这个 :path 上的隧道」
                if (!hasSchemeField || !hasPathField)
                {
                    writeError(errorText, "带 :protocol 的 CONNECT 缺少 :scheme 或 :path：RFC 8441 §4 要求扩展 CONNECT 同时给出"
                                         "这两个伪头（普通 CONNECT 恰好相反，要求省略它们）");
                    return false;
                }
                if (!hasAuthorityField || request.authority.empty())
                {
                    writeError(errorText, "带 :protocol 的 CONNECT 缺少非空的 :authority：RFC 8441 §4 要求给出目标主机与端口");
                    return false;
                }
                return true;
            }
            if (hasSchemeField || hasPathField)
            {
                writeError(errorText, "CONNECT 请求带了 :scheme 或 :path：RFC 7540 §8.3 要求 CONNECT 请求省略这两个伪头，"
                                     "只保留 :method 与 :authority");
                return false;
            }
            if (!hasAuthorityField || request.authority.empty())
            {
                writeError(errorText, "CONNECT 请求缺少非空的 :authority：RFC 7540 §8.3 要求 CONNECT 必须给出目标主机与端口");
                return false;
            }
            return true;
        }
        if (!hasSchemeField)
        {
            writeError(errorText, "请求缺少 :scheme 伪头：RFC 7540 §8.1.2.3 要求非 CONNECT 请求必须带 :scheme");
            return false;
        }
        if (!hasPathField)
        {
            writeError(errorText, "请求缺少 :path 伪头：RFC 7540 §8.1.2.3 要求非 CONNECT 请求必须带 :path");
            return false;
        }
        if (request.path.empty())
        {
            writeError(errorText, ":path 伪头的值为空：RFC 7540 §8.1.2.3 要求 http/https URI 的 :path 不能为空（要表示「整个服务」请用 \"*\"）");
            return false;
        }
        return true;
    }

    bool Http2Connection::acceptTrailerHeaderFields(const std::vector<HpackHeaderField> &headerFields, std::string *const errorText)
    {
        clearError(errorText);
        for (const HpackHeaderField &field: headerFields)
        {
            if (field.name.empty())
            {
                writeError(errorText, "尾部头块里出现空头名：RFC 7540 §8.1.2 要求头部名必须是非空的小写字段名");
                return false;
            }
            if (containsUppercaseAscii(field.name))
            {
                writeError(errorText, std::format("尾部头块的头名 \"{}\" 含大写字母：RFC 7540 §8.1.2 要求 HTTP/2 的头部名必须全小写", field.name));
                return false;
            }
            if (field.name.front() == ':')
            {
                writeError(errorText, std::format("尾部头块里出现伪头 \"{}\"：RFC 7540 §8.1.2.1 要求尾部头块不得包含伪头", field.name));
                return false;
            }
            if (isConnectionSpecificHeaderName(field.name))
            {
                writeError(errorText, std::format("尾部头块里出现连接特定头 \"{}\"：RFC 7540 §8.1.2.2 禁止这类头部出现在 HTTP/2 报文里",
                                                  field.name));
                return false;
            }
            if (field.name == "te" && field.value != "trailers")
            {
                writeError(errorText, std::format("尾部头块里 te 的取值是 \"{}\"：RFC 7540 §8.1.2.2 只允许 te: trailers", field.value));
                return false;
            }
            if (!isTokenName(field.name) || !isHeaderValueBytes(field.value))
            {
                writeError(errorText, std::format("尾部头块的头 \"{}\" 含 token 之外的头名或 CR/LF/NUL 之类的头值控制字符，"
                                                 "请让对端按字段语法重新拼",
                                                 field.name));
                return false;
            }
        }
        return true;
    }

    bool Http2Connection::acceptResponseHeaderField(const std::string_view name, const std::string_view value,
                                                    std::string *const errorText)
    {
        clearError(errorText);
        if (name.empty())
        {
            writeError(errorText, "响应头名不能为空：请给出非空的小写头名，或删掉这一项");
            return false;
        }
        if (name.front() == ':')
        {
            writeError(errorText, std::format("响应头名 \"{}\" 以 ':' 开头：伪头由本层自行拼出（:status 恒在最前，§8.1.2.1），"
                                             "调用方只能给普通头部",
                                             name));
            return false;
        }
        if (containsUppercaseAscii(name))
        {
            writeError(errorText, std::format("响应头名 \"{}\" 含大写字母：RFC 7540 §8.1.2 要求头名全小写，请改成小写后再发", name));
            return false;
        }
        if (isConnectionSpecificHeaderName(name))
        {
            writeError(errorText, std::format("响应头名 \"{}\" 是 HTTP/1.1 的连接特定头：RFC 7540 §8.1.2.2 禁止它出现在 HTTP/2 里，"
                                             "请删掉这一项",
                                             name));
            return false;
        }
        if (!isTokenName(name))
        {
            writeError(errorText, std::format("响应头名 \"{}\" 含 token 之外的字符：RFC 7540 §8.1.2 要求头名符合字段名语法", name));
            return false;
        }
        if (!isHeaderValueBytes(value))
        {
            writeError(errorText, std::format("响应头 \"{}\" 的值含 CR/LF/NUL 之类的控制字符：发出去会被对端按非法报文处理，"
                                             "请先净化或改用百分号编码",
                                             name));
            return false;
        }
        return true;
    }

    bool Http2Connection::applyPeerSettings(const Http2SettingsPayload &payload)
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
                    if (setting.value > 1)
                    {
                        fail(Http2ErrorCode::ProtocolError,
                             std::format("对端 SETTINGS 的 ENABLE_PUSH 取值 {} 非法：RFC 7540 §6.5.2 只允许 0 或 1，请检查对端实现",
                                         setting.value));
                        return false;
                    }
                    break;
                case Http2SettingIdentifier::MaxConcurrentStreams:
                    // 对端限制的是本端发起的流：本片不发流，只记账备查
                    break;
                case Http2SettingIdentifier::InitialWindowSize:
                {
                    if (setting.value > kHttp2MaximumWindowSizeByteCount)
                    {
                        fail(Http2ErrorCode::FlowControlError,
                             std::format("对端 SETTINGS 的 INITIAL_WINDOW_SIZE 取值 {} 超过上限 2^31-1（RFC 7540 §6.5.2）", setting.value));
                        return false;
                    }
                    // 新值同时改写所有活动流的窗口：按增量平移（§6.9.2），平移后超过上限即流控错误
                    const std::int64_t windowDelta = static_cast<std::int64_t>(setting.value) -
                                                     static_cast<std::int64_t>(peerInitialWindowSize());
                    for (auto &streamEntry: m_streams)
                    {
                        StreamRecord &stream = streamEntry.second;
                        if (stream.state == Http2StreamState::Closed)
                        {
                            continue;
                        }
                        stream.sendWindowByteCount += windowDelta;
                        if (stream.sendWindowByteCount > static_cast<std::int64_t>(kHttp2MaximumWindowSizeByteCount))
                        {
                            fail(Http2ErrorCode::FlowControlError,
                                 std::format("对端把 INITIAL_WINDOW_SIZE 改成 {} 后，流 {} 的发送窗口超过上限 2^31-1（RFC 7540 §6.9.2）",
                                             setting.value, stream.streamId));
                            return false;
                        }
                    }
                    break;
                }
                case Http2SettingIdentifier::MaxFrameSize:
                    if (setting.value < kHttp2DefaultMaximumFrameSize || setting.value > kHttp2MaximumMaximumFrameSize)
                    {
                        fail(Http2ErrorCode::ProtocolError,
                             std::format("对端 SETTINGS 的 MAX_FRAME_SIZE 取值 {} 越界：RFC 7540 §6.5.2 的合法区间是 [16384, 16777215]",
                                         setting.value));
                        return false;
                    }
                    break;
                case Http2SettingIdentifier::MaxHeaderListSize:
                    // 对端限制的是本端发出的头列表：本片不做发送侧的头列表预算，只记账备查
                    break;
                case Http2SettingIdentifier::EnableConnectProtocol:
                    // RFC 8441 §3：取值只能是 0 或 1，其它取值是连接错误
                    if (setting.value > 1)
                    {
                        fail(Http2ErrorCode::ProtocolError,
                             std::format("对端 SETTINGS 的 ENABLE_CONNECT_PROTOCOL 取值 {} 非法：RFC 8441 §3 只允许 0 或 1",
                                         setting.value));
                        return false;
                    }
                    // 对端限制的是本端发起的扩展 CONNECT：本端不发起，只记账备查
                    break;
                default:
                    // 未知标识必须忽略（§6.5.2），但仍然记进账本，便于排查对端用了哪些扩展
                    break;
            }
            m_peerSettings.insert_or_assign(setting.identifier, setting.value);
        }
        return true;
    }

    void Http2Connection::openStream(const std::uint32_t streamId)
    {
        StreamRecord stream;
        stream.streamId = streamId;
        stream.state = Http2StreamState::Open;
        // 流级发送窗口的初值取对端通告的 SETTINGS_INITIAL_WINDOW_SIZE（§6.9.2），不是本端通告的那个
        stream.sendWindowByteCount = peerInitialWindowSize();
        // 流级接收窗口的初值取**本端**通告的 SETTINGS_INITIAL_WINDOW_SIZE：对端按它扣，本端按它判超发
        stream.receiveWindowByteCount = static_cast<std::int64_t>(m_configuration.initialWindowSize);
        m_streams[streamId] = std::move(stream);
        ++m_openStreamCount;
        // 记下已用过的最大流号：后续新流必须严格大于它（§5.1.1）
        m_highestPeerStreamId = streamId;
    }

    void Http2Connection::noteRemoteEndStream(StreamRecord &stream)
    {
        // 对端收尾：本端还没收尾就是半关（remote），本端已收尾则这条流整条终止
        if (stream.state == Http2StreamState::Open)
        {
            stream.state = Http2StreamState::HalfClosedRemote;
            return;
        }
        if (stream.state == Http2StreamState::HalfClosedLocal)
        {
            terminateStream(stream, false);
        }
    }

    void Http2Connection::noteLocalEndStream(StreamRecord &stream)
    {
        if (stream.state == Http2StreamState::Open)
        {
            stream.state = Http2StreamState::HalfClosedLocal;
            return;
        }
        if (stream.state == Http2StreamState::HalfClosedRemote)
        {
            terminateStream(stream, false);
        }
    }

    void Http2Connection::terminateStream(StreamRecord &stream, const bool wasTerminatedByReset)
    {
        if (stream.state == Http2StreamState::Closed)
        {
            // 幂等：重复终止不再记账，也不再挪动终止窗口的顺序
            return;
        }
        stream.state = Http2StreamState::Closed;
        stream.wasTerminatedByReset = wasTerminatedByReset;
        // 终止之后本端不再发正文：队列里没出去的数据就此丢掉（对端已经或将要按 RST/END_STREAM 看待它）
        stream.pendingData.clear();
        stream.pendingDataOffset = 0;
        stream.isEndStreamPending = false;
        --m_openStreamCount;
        rememberTerminatedStream(stream.streamId);
    }

    void Http2Connection::rememberTerminatedStream(const std::uint32_t streamId)
    {
        m_terminatedStreamIds.push_back(streamId);
        // 终止记录只为区分「忽略」与「判错」而留：超过上限就把最旧的整条删掉，账本不随连接时长无限增长
        std::size_t evictedCount = 0;
        while (m_terminatedStreamIds.size() - evictedCount > kTerminatedStreamMemoryCount)
        {
            m_streams.erase(m_terminatedStreamIds[evictedCount]);
            ++evictedCount;
            // 账本自此不完整：再也无法证明某个流号「从未被开过」，对 idle 流上的违约帧只能退回宽容忽略
            m_hasEvictedTerminatedStreamRecord = true;
        }
        // 攒成一段一起摘，而不是逐条 erase(begin())：稳态下每条流终止都要挤掉一条，逐条摘等于每回都把
        // 整张表往前搬一遍
        if (evictedCount != 0)
        {
            m_terminatedStreamIds.erase(m_terminatedStreamIds.begin(),
                                        m_terminatedStreamIds.begin()
                                            + static_cast<std::vector<std::uint32_t>::difference_type>(evictedCount));
        }
    }

    void Http2Connection::refuseNewStream(const std::uint32_t streamId, std::string reason)
    {
        // 流号已经被对端用掉：后续新流必须更大（§5.1.1）
        m_highestPeerStreamId = streamId;
        appendOutgoing(encodeHttp2RstStreamFrame(Http2RstStreamPayload{.errorCode = Http2ErrorCode::RefusedStream}, streamId));
        StreamRecord stream;
        stream.streamId = streamId;
        stream.state = Http2StreamState::Closed;
        // 本端 RST 掉的流：其上的在途帧一律忽略，而不是再回一次 RST_STREAM
        stream.wasTerminatedByReset = true;
        m_streams[streamId] = std::move(stream);
        rememberTerminatedStream(streamId);
        m_lastStreamErrorMessage = std::move(reason);
    }

    bool Http2Connection::isIgnorableFrameOnTerminatedStream(const StreamRecord &stream, const Http2FrameType frameType)
    {
        // RST_STREAM 终止的流：对端可能还没看到那一帧，其上的在途帧一律忽略（回敬 RST 只会变成风暴）
        if (stream.wasTerminatedByReset)
        {
            return true;
        }
        // 正常终止（双向 END_STREAM）的流：§5.1「closed」段明确要求忽略 WINDOW_UPDATE / RST_STREAM / PRIORITY
        return frameType == Http2FrameType::WindowUpdate || frameType == Http2FrameType::RstStream ||
               frameType == Http2FrameType::Priority;
    }

    bool Http2Connection::acceptPeerStreamIdParity(const std::uint32_t streamId, std::string *const errorText)
    {
        clearError(errorText);
        if (streamId % 2U == 0)
        {
            writeError(errorText, std::format("流号 {} 是偶数：RFC 7540 §5.1.1 规定客户端发起的流号必须是奇数（偶数留给服务端推送，"
                                             "本端不实现推送）",
                                             streamId));
            return false;
        }
        return true;
    }

    bool Http2Connection::increaseConnectionSendWindow(const std::uint32_t increment)
    {
        if (m_connectionSendWindowByteCount + static_cast<std::int64_t>(increment) >
            static_cast<std::int64_t>(kHttp2MaximumWindowSizeByteCount))
        {
            fail(Http2ErrorCode::FlowControlError,
                 std::format("连接级发送窗口加上 WINDOW_UPDATE 的增量 {} 会超过上限 2^31-1（RFC 7540 §6.9.1）：本端无法为这么大的窗口记账，"
                             "请检查对端的窗口记账",
                             increment));
            return false;
        }
        m_connectionSendWindowByteCount += static_cast<std::int64_t>(increment);
        return true;
    }

    bool Http2Connection::increaseStreamSendWindow(StreamRecord &stream, const std::uint32_t increment)
    {
        if (stream.sendWindowByteCount + static_cast<std::int64_t>(increment) >
            static_cast<std::int64_t>(kHttp2MaximumWindowSizeByteCount))
        {
            fail(Http2ErrorCode::FlowControlError,
                 std::format("流 {} 的发送窗口加上 WINDOW_UPDATE 的增量 {} 会超过上限 2^31-1（RFC 7540 §6.9.1）：本端无法为这么大的窗口记账",
                             stream.streamId, increment));
            return false;
        }
        stream.sendWindowByteCount += static_cast<std::int64_t>(increment);
        return true;
    }

    void Http2Connection::creditConnectionReceiveWindow(const std::size_t byteCount)
    {
        m_pendingConnectionReceiveCreditByteCount += byteCount;
        // 不够阈值就先攒着：逐帧回敬 WINDOW_UPDATE 会让小分片的请求多出一倍控制帧
        if (!isReceiveCreditWorthFlushing(m_pendingConnectionReceiveCreditByteCount, kHttp2InitialWindowSizeByteCount))
        {
            return;
        }

        const std::size_t creditByteCount = std::exchange(m_pendingConnectionReceiveCreditByteCount, std::size_t{0});
        // 连接级窗口的初值恒为 65535，与 SETTINGS_INITIAL_WINDOW_SIZE 无关（§6.9.2）
        m_connectionReceiveWindowByteCount += static_cast<std::int64_t>(creditByteCount);
        appendOutgoing(encodeHttp2WindowUpdateFrame(
                Http2WindowUpdatePayload{.windowSizeIncrement = static_cast<std::uint32_t>(creditByteCount)}, 0U));
    }

    void Http2Connection::creditStreamReceiveWindow(StreamRecord *const stream, const std::size_t byteCount)
    {
        // 流已经终止或压根不在账本里：流级窗口连同记录一起作废，只当没有这条窗口
        if (stream == nullptr || stream->state == Http2StreamState::Closed)
        {
            return;
        }

        stream->pendingReceiveCreditByteCount += byteCount;
        if (!isReceiveCreditWorthFlushing(stream->pendingReceiveCreditByteCount, m_configuration.initialWindowSize))
        {
            return;
        }

        const std::size_t creditByteCount = std::exchange(stream->pendingReceiveCreditByteCount, std::size_t{0});
        stream->receiveWindowByteCount += static_cast<std::int64_t>(creditByteCount);
        appendOutgoing(encodeHttp2WindowUpdateFrame(
                Http2WindowUpdatePayload{.windowSizeIncrement = static_cast<std::uint32_t>(creditByteCount)}, stream->streamId));
    }

    bool Http2Connection::isReceiveCreditWorthFlushing(const std::size_t pendingByteCount,
                                                       const std::uint32_t advertisedWindowByteCount) noexcept
    {
        // 阈值取该窗口初始值的一半：攒够半个窗口发一次，既不会逐帧回敬，也不会让对端停在半途
        const std::size_t thresholdByteCount = std::max<std::size_t>(1U, advertisedWindowByteCount / 2U);
        return pendingByteCount >= thresholdByteCount;
    }

    void Http2Connection::pumpSendQueue(StreamRecord &stream)
    {
        const std::uint32_t maximumFrameSize = peerMaximumFrameSize();
        // 两个窗口都为正才允许出帧（§5.2.2）：负窗口是 SETTINGS 缩减留下的，必须等 WINDOW_UPDATE 救回来
        while (stream.hasPendingData() && stream.sendWindowByteCount > 0 && m_connectionSendWindowByteCount > 0)
        {
            const std::int64_t allowedByteCount = std::min({stream.sendWindowByteCount, m_connectionSendWindowByteCount,
                                                            static_cast<std::int64_t>(maximumFrameSize)});
            const std::size_t remainingByteCount = stream.pendingData.size() - stream.pendingDataOffset;
            const std::size_t byteCount          = std::min<std::size_t>(remainingByteCount, static_cast<std::size_t>(allowedByteCount));
            // END_STREAM 只落在把队列排空的那一帧上：窗口不足时提前收尾会把没发出去的正文丢掉
            const bool isEndStreamSegment = byteCount == remainingByteCount && stream.isEndStreamPending;
            // 载荷按视图直接拼进待发缓冲：先前的写法是「视图 → 临时帧串 → 待发缓冲」，
            // 中间那趟整段拷贝随载荷线性放大（一帧最大可到对端通告的 SETTINGS_MAX_FRAME_SIZE）
            appendOutgoingFrame(Http2FrameType::Data, isEndStreamSegment ? kHttp2FlagEndStream : 0U, stream.streamId,
                                std::string_view(stream.pendingData).substr(stream.pendingDataOffset, byteCount));
            // 只推进游标：原来每帧都 erase(0, n) 搬移整个剩余缓冲，1 MiB 响应会白搬几十 MB。
            // 前缀攒够阈值再整段压缩一次，均摊到每帧是 O(1)
            stream.pendingDataOffset += byteCount;
            if (stream.pendingDataOffset == stream.pendingData.size())
            {
                stream.pendingData.clear();
                stream.pendingDataOffset = 0;
            } else if (stream.pendingDataOffset >= kPendingDataCompactThresholdByteCount)
            {
                stream.pendingData.erase(0, stream.pendingDataOffset);
                stream.pendingDataOffset = 0;
            }
            stream.sendWindowByteCount -= static_cast<std::int64_t>(byteCount);
            m_connectionSendWindowByteCount -= static_cast<std::int64_t>(byteCount);
            if (isEndStreamSegment)
            {
                stream.isEndStreamPending = false;
                noteLocalEndStream(stream);
                return;
            }
        }

        // 队列空了还想收尾（例如正文恰好用完整窗口）：补一个零长 DATA 帧，它的长度是 0、不占用窗口（§6.1）
        const bool hasWindowSpace = stream.sendWindowByteCount >= 0 && m_connectionSendWindowByteCount >= 0;
        if (!stream.hasPendingData() && stream.isEndStreamPending && hasWindowSpace && stream.state != Http2StreamState::Closed)
        {
            stream.isEndStreamPending = false;
            appendOutgoing(encodeHttp2DataFrame(Http2DataPayload{.endStream = true}, stream.streamId));
            noteLocalEndStream(stream);
        }
    }

    void Http2Connection::pumpAllSendQueues()
    {
        // 只跑还排着队的流：窗口变大不会凭空产生数据
        for (auto &streamEntry: m_streams)
        {
            if (streamEntry.second.hasPendingData() || streamEntry.second.isEndStreamPending)
            {
                pumpSendQueue(streamEntry.second);
            }
        }
    }

    void Http2Connection::emitHeaderBlock(const std::uint32_t streamId, const std::string &headerBlock, const bool endStream)
    {
        const std::size_t maximumFragmentByteCount = peerMaximumFrameSize();
        std::size_t offsetByteCount = 0;
        bool isFirstFrame = true;
        do
        {
            const std::size_t fragmentByteCount = std::min(maximumFragmentByteCount, headerBlock.size() - offsetByteCount);
            const bool isLastFrame = offsetByteCount + fragmentByteCount >= headerBlock.size();
            // 分片按视图交出、帧直接拼进待发缓冲：整块头块本就在调用方手里，先 substr 成片段
            // 再攒进临时帧串、最后搬进待发缓冲等于整块白拷三遍
            const std::string_view fragment = std::string_view(headerBlock).substr(offsetByteCount, fragmentByteCount);
            if (isFirstFrame)
            {
                // END_STREAM 只允许出现在头块的第一帧（§6.2、§6.10）
                appendHttp2HeadersFrame(m_outgoingBytes, fragment, endStream, isLastFrame, streamId);
                isFirstFrame = false;
            }
            else
            {
                appendOutgoingFrame(Http2FrameType::Continuation, isLastFrame ? kHttp2FlagEndHeaders : 0U, streamId, fragment);
            }
            offsetByteCount += fragmentByteCount;
        } while (offsetByteCount < headerBlock.size());
    }

    void Http2Connection::appendOutgoing(std::string frameBytes)
    {
        m_outgoingBytes += frameBytes;
    }

    void Http2Connection::appendOutgoingFrame(const Http2FrameType type, const std::uint8_t flags, const std::uint32_t streamId,
                                              const std::string_view payload)
    {
        appendHttp2Frame(m_outgoingBytes, type, flags, streamId, payload);
    }

    std::uint32_t Http2Connection::peerMaximumFrameSize() const noexcept
    {
        std::uint32_t maximumFrameSize = 0;
        // 对端还没通告就按 §6.5.2 的初始值发：任何帧都不许超过它，哪怕对端随后会把它调大
        return tryGetPeerSetting(Http2SettingIdentifier::MaxFrameSize, maximumFrameSize) ? maximumFrameSize : kHttp2DefaultMaximumFrameSize;
    }

    std::uint32_t Http2Connection::peerInitialWindowSize() const noexcept
    {
        std::uint32_t initialWindowSize = 0;
        return tryGetPeerSetting(Http2SettingIdentifier::InitialWindowSize, initialWindowSize) ? initialWindowSize
                                                                                              : kHttp2InitialWindowSizeByteCount;
    }

    Http2Connection::StreamRecord *Http2Connection::findStream(const std::uint32_t streamId) noexcept
    {
        const auto streamIterator = m_streams.find(streamId);
        return streamIterator == m_streams.end() ? nullptr : &streamIterator->second;
    }

    Http2Connection::StreamRecord *Http2Connection::findActiveStream(const std::uint32_t streamId) noexcept
    {
        StreamRecord *const stream = findStream(streamId);
        if (stream == nullptr || stream->state == Http2StreamState::Closed)
        {
            return nullptr;
        }
        return stream;
    }

    void Http2Connection::fail(const Http2ErrorCode errorCode, std::string reason)
    {
        if (m_state == Http2ConnectionState::Failed)
        {
            // 第一个原因才是根因：后续判错不再改写结论（例如解码器在失败态里反复报同一批字节）
            return;
        }
        m_state = Http2ConnectionState::Failed;
        m_errorCode = errorCode;
        m_errorMessage = std::move(reason);
        // 连接错误必须让对端看见：GOAWAY 带上本端处理过的最大流号与错误码（§5.4.1、§6.8）
        Http2GoAwayPayload payload;
        payload.lastStreamId = m_highestPeerStreamId;
        payload.errorCode = errorCode;
        payload.debugData = m_errorMessage;
        appendOutgoing(encodeHttp2GoAwayFrame(payload));
    }

    void Http2Connection::failStream(StreamRecord &stream, const Http2ErrorCode errorCode, std::string reason)
    {
        // 流错误只终止这一条流：连接继续，对端从 RST_STREAM 的错误码看出原因（§5.4.2）
        appendOutgoing(encodeHttp2RstStreamFrame(Http2RstStreamPayload{.errorCode = errorCode}, stream.streamId));
        terminateStream(stream, true);
        m_lastStreamErrorMessage = std::move(reason);
    }
} // namespace AsynGyanis::Net
