#include "Net/Http3/Http3Frame.h"

#include "Net/Quic/Codec/QuicRawBytes.h"

#include <format>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    std::optional<Http3SettingId> http3SettingIdFromValue(const std::uint64_t identifierValue) noexcept
    {
        // 逐个比对本层认识的四个标识：未知与保留取值都落进 default，返回 nullopt 而不是报错（§7.2.4）
        switch (identifierValue)
        {
            case static_cast<std::uint64_t>(Http3SettingId::QpackMaxTableCapacity):
                return Http3SettingId::QpackMaxTableCapacity;
            case static_cast<std::uint64_t>(Http3SettingId::MaxFieldSectionSize):
                return Http3SettingId::MaxFieldSectionSize;
            case static_cast<std::uint64_t>(Http3SettingId::QpackBlockedStreams):
                return Http3SettingId::QpackBlockedStreams;
            case static_cast<std::uint64_t>(Http3SettingId::EnableConnectProtocol):
                return Http3SettingId::EnableConnectProtocol;
            default:
                return std::nullopt;
        }
    }

    std::string_view http3FrameTypeName(const std::uint64_t frameTypeValue) noexcept
    {
        switch (frameTypeValue)
        {
            case static_cast<std::uint64_t>(Http3FrameType::Data):
                return "DATA";
            case static_cast<std::uint64_t>(Http3FrameType::Headers):
                return "HEADERS";
            case static_cast<std::uint64_t>(Http3FrameType::CancelPush):
                return "CANCEL_PUSH";
            case static_cast<std::uint64_t>(Http3FrameType::Settings):
                return "SETTINGS";
            case static_cast<std::uint64_t>(Http3FrameType::PushPromise):
                return "PUSH_PROMISE";
            case static_cast<std::uint64_t>(Http3FrameType::GoAway):
                return "GOAWAY";
            case static_cast<std::uint64_t>(Http3FrameType::MaxPushId):
                return "MAX_PUSH_ID";
            default:
                // 未知/保留帧类型也要有个名字进文案：对端能发任何 62 位内的整数，含糊成「出错」不如照实记下
                return "未知帧类型";
        }
    }

    std::uint64_t http3FrameTypeValue(const Http3Frame &frame) noexcept
    {
        return std::visit(
                [](const auto &concreteFrame) -> std::uint64_t
                {
                    using ConcreteFrame = std::remove_cvref_t<decltype(concreteFrame)>;
                    if constexpr (std::is_same_v<ConcreteFrame, Http3DataFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::Data);
                    }
                    else if constexpr (std::is_same_v<ConcreteFrame, Http3HeadersFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::Headers);
                    }
                    else if constexpr (std::is_same_v<ConcreteFrame, Http3CancelPushFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::CancelPush);
                    }
                    else if constexpr (std::is_same_v<ConcreteFrame, Http3SettingsFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::Settings);
                    }
                    else if constexpr (std::is_same_v<ConcreteFrame, Http3PushPromiseFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::PushPromise);
                    }
                    else if constexpr (std::is_same_v<ConcreteFrame, Http3GoAwayFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::GoAway);
                    }
                    else if constexpr (std::is_same_v<ConcreteFrame, Http3MaxPushIdFrame>)
                    {
                        return static_cast<std::uint64_t>(Http3FrameType::MaxPushId);
                    }
                    else
                    {
                        // 未知帧的类型值本来就带在结构体里，交回去时不许把它换成别的数
                        return concreteFrame.frameType;
                    }
                },
                frame);
    }

    namespace
    {
        /**
         * @brief 一帧载荷内部的读位置游标
         *
         * @details 游标被**声明长度**框住：这里读不到「还没到的字节」，因此越出末尾只有一种解释——
         *          对端把帧写坏了（§7.1），报 Malformed 而不是等更多数据。
         */
        class PayloadCursor
        {
        public:
            /**
             * @brief 构造游标
             * @param frameTypeValue 本帧的类型值，进入所有失败文案
             * @param payload 按声明长度切好的载荷视图
             */
            PayloadCursor(const std::uint64_t frameTypeValue, const std::span<const std::uint8_t> payload)
                : m_frameTypeValue(frameTypeValue), m_payload(payload)
            {
            }

            /**
             * @brief 读一个变长整数字段
             * @param fieldName 字段名（如「Push ID」），只用于文案定位
             * @return std::expected<std::uint64_t, Http3FrameError> 成功返回数值并把游标前移
             * @return 失败返回 `Http3FrameError`，类别恒为 `Malformed`
             */
            [[nodiscard]] std::expected<std::uint64_t, Http3FrameError> readInteger(const std::string_view fieldName)
            {
                const auto decoded = decodeQuicVariableLengthInteger(m_payload.subspan(m_offset));
                if (!decoded.has_value())
                {
                    // 载荷已经收满，此处差字节不是「还没读到」而是帧被写短了：§7.1 要求载荷必须含全部字段
                    return std::unexpected(makeError(std::format("{} 字段越出载荷末尾：声明长度 {} 字节内只剩 {} 字节可读，"
                                                                 "按 RFC 9114 §7.1 判 H3_FRAME_ERROR",
                                                                 fieldName, m_payload.size(), remainingByteCount())));
                }
                m_offset += decoded->byteCount;
                return decoded->value;
            }

            /**
             * @brief 要求「必填字段已读尽、声明长度内没有多余字节」
             * @details §7.1：载荷里在已识别字段之后还有多余字节的，同样按 H3_FRAME_ERROR 处理。
             * @param fieldName 已读到的最后一个字段名，只用于文案定位
             * @return std::expected<void, Http3FrameError> 恰好读尽时成功
             */
            [[nodiscard]] std::expected<void, Http3FrameError> requireFieldsEnd(const std::string_view fieldName)
            {
                if (remainingByteCount() == 0)
                {
                    return {};
                }
                return std::unexpected(makeError(std::format("{} 字段之后还剩 {} 字节没被任何字段占用：§7.2 要求载荷恰好由该帧"
                                                             "定义的字段组成（RFC 9114 §7.1 判 H3_FRAME_ERROR）",
                                                             fieldName, remainingByteCount())));
            }

            /**
             * @brief 取走剩余全部字节并把游标推到末尾
             * @return std::span<const std::uint8_t> 指向载荷的视图，长度可以是 0
             */
            [[nodiscard]] std::span<const std::uint8_t> takeRemaining() noexcept
            {
                const std::span<const std::uint8_t> view = m_payload.subspan(m_offset);
                m_offset = m_payload.size();
                return view;
            }

            /// 声明长度内还剩几字节没用
            [[nodiscard]] std::size_t remainingByteCount() const noexcept
            {
                return m_payload.size() - m_offset;
            }

            /// 声明长度内的字节是否已用尽
            [[nodiscard]] bool atEnd() const noexcept
            {
                return m_offset >= m_payload.size();
            }

            /**
             * @brief 造一条本帧的 Malformed 错误，文案自动带上帧类型与声明长度
             * @param detail 具体差在哪个字段
             * @return Http3FrameError 可直接交给 std::unexpected
             */
            [[nodiscard]] Http3FrameError makeError(std::string detail) const
            {
                return Http3FrameError{Http3FrameErrorKind::Malformed,
                                       std::format("{}帧（声明长度 {} 字节）：{}", http3FrameTypeName(m_frameTypeValue),
                                                   m_payload.size(), std::move(detail))};
            }

        private:
            std::uint64_t m_frameTypeValue{0};       ///< 本帧类型值，只用于文案
            std::span<const std::uint8_t> m_payload; ///< 按声明长度切好的载荷
            std::size_t m_offset{0};                 ///< 已读到的位置
        };

        /**
         * @brief 读「载荷恰好是一个变长整数」的帧字段（CANCEL_PUSH/GOAWAY/MAX_PUSH_ID 共用同一档布局）
         * @param cursor 载荷游标
         * @param fieldName 字段名，只用于文案定位
         * @return std::expected<std::uint64_t, Http3FrameError> 成功返回数值；缺字段或多尾巴均为 `Malformed`
         */
        [[nodiscard]] std::expected<std::uint64_t, Http3FrameError> readSoleInteger(PayloadCursor &cursor,
                                                                                   const std::string_view fieldName)
        {
            const auto value = cursor.readInteger(fieldName);
            if (!value.has_value())
            {
                return std::unexpected(value.error());
            }
            const auto endCheck = cursor.requireFieldsEnd(fieldName);
            if (!endCheck.has_value())
            {
                return std::unexpected(endCheck.error());
            }
            return value;
        }

        /**
         * @brief 按 §7.2 各小节的布局解释一段完整载荷
         * @param frameTypeValue 帧类型线上取值
         * @param payload 已按声明长度收齐的载荷
         * @return std::expected<Http3Frame, Http3FrameError> 类型不认识时返回 Http3UnknownFrame（不是错误）
         */
        [[nodiscard]] std::expected<Http3Frame, Http3FrameError> decodeHttp3FramePayload(const std::uint64_t frameTypeValue,
                                                                                        const std::span<const std::uint8_t> payload)
        {
            PayloadCursor cursor(frameTypeValue, payload);
            switch (frameTypeValue)
            {
                case static_cast<std::uint64_t>(Http3FrameType::Data):
                    // DATA 的 Data 字段是「任意长度到声明末尾」，本层不看内容（§7.2.1）
                    return Http3DataFrame{cursor.takeRemaining()};
                case static_cast<std::uint64_t>(Http3FrameType::Headers):
                    // 头字段段的语义属于 QPACK，本层只按声明长度整段交出（§7.2.2）
                    return Http3HeadersFrame{cursor.takeRemaining()};
                case static_cast<std::uint64_t>(Http3FrameType::CancelPush):
                {
                    const auto pushId = readSoleInteger(cursor, "Push ID");
                    if (!pushId.has_value())
                    {
                        return std::unexpected(pushId.error());
                    }
                    return Http3CancelPushFrame{*pushId};
                }
                case static_cast<std::uint64_t>(Http3FrameType::Settings):
                {
                    Http3SettingsFrame settingsFrame;
                    // 载荷是零或多条「标识 + 值」（§7.2.4 图 7）：一条都不落空地按线上顺序收
                    while (!cursor.atEnd())
                    {
                        const auto identifier = cursor.readInteger("设置项标识");
                        if (!identifier.has_value())
                        {
                            return std::unexpected(identifier.error());
                        }
                        const auto value = cursor.readInteger("设置项的值");
                        if (!value.has_value())
                        {
                            return std::unexpected(value.error());
                        }
                        const auto knownIdentifier = http3SettingIdFromValue(*identifier);
                        if (knownIdentifier.has_value())
                        {
                            settingsFrame.settings.emplace_back(*knownIdentifier, *value);
                        }
                        else
                        {
                            // 未知与保留标识都塞进旁路列表：§7.2.4 要求忽略不等于可以把它抹掉，
                            // 连接层要么照 §7.2.4.1 判错，要么原样再编出去
                            settingsFrame.unknownSettings.push_back(Http3UnknownSetting{*identifier, *value});
                        }
                    }
                    return settingsFrame;
                }
                case static_cast<std::uint64_t>(Http3FrameType::PushPromise):
                {
                    const auto pushId = cursor.readInteger("Push ID");
                    if (!pushId.has_value())
                    {
                        return std::unexpected(pushId.error());
                    }
                    // Push ID 之后的字节整段是被承诺请求的头字段段（§7.2.5 图 8）
                    return Http3PushPromiseFrame{*pushId, cursor.takeRemaining()};
                }
                case static_cast<std::uint64_t>(Http3FrameType::GoAway):
                {
                    const auto streamIdOrPushId = readSoleInteger(cursor, "Stream ID/Push ID");
                    if (!streamIdOrPushId.has_value())
                    {
                        return std::unexpected(streamIdOrPushId.error());
                    }
                    return Http3GoAwayFrame{*streamIdOrPushId};
                }
                case static_cast<std::uint64_t>(Http3FrameType::MaxPushId):
                {
                    const auto pushId = readSoleInteger(cursor, "Push ID");
                    if (!pushId.has_value())
                    {
                        return std::unexpected(pushId.error());
                    }
                    return Http3MaxPushIdFrame{*pushId};
                }
                default:
                    // §9：未知或不受支持的帧类型必须忽略——原样交出载荷，绝不当成解码失败
                    return Http3UnknownFrame{frameTypeValue, cursor.takeRemaining()};
            }
        }

        /**
         * @brief 帧载荷编码器：一种帧一个 operator() 重载
         *
         * @details 用重载而不是 switch：Http3Frame 添了新成员时编译器会直接报「visit 没覆盖该分支」，
         *          比漏写一个 case 拖到运行期强。各重载只写载荷，帧头由 appendHttp3Frame 统一加。
         */
        struct FramePayloadAppender
        {
            std::string &payload; ///< 载荷暂存缓冲，二进制安全

            void operator()(const Http3DataFrame &frame) const
            {
                appendQuicRawBytes(payload, frame.payload);
            }

            void operator()(const Http3HeadersFrame &frame) const
            {
                appendQuicRawBytes(payload, frame.encodedFieldSection);
            }

            void operator()(const Http3CancelPushFrame &frame) const
            {
                appendQuicVariableLengthInteger(payload, frame.pushId);
            }

            void operator()(const Http3SettingsFrame &frame) const
            {
                for (const auto &[identifier, value]: frame.settings)
                {
                    appendQuicVariableLengthInteger(payload, static_cast<std::uint64_t>(identifier));
                    appendQuicVariableLengthInteger(payload, value);
                }
                for (const Http3UnknownSetting &setting: frame.unknownSettings)
                {
                    appendQuicVariableLengthInteger(payload, setting.identifier);
                    appendQuicVariableLengthInteger(payload, setting.value);
                }
            }

            void operator()(const Http3PushPromiseFrame &frame) const
            {
                appendQuicVariableLengthInteger(payload, frame.pushId);
                appendQuicRawBytes(payload, frame.encodedFieldSection);
            }

            void operator()(const Http3GoAwayFrame &frame) const
            {
                appendQuicVariableLengthInteger(payload, frame.streamIdOrPushId);
            }

            void operator()(const Http3MaxPushIdFrame &frame) const
            {
                appendQuicVariableLengthInteger(payload, frame.pushId);
            }

            void operator()(const Http3UnknownFrame &frame) const
            {
                appendQuicRawBytes(payload, frame.payload);
            }
        };
    } // namespace

    void appendHttp3Frame(std::string &bytes, const Http3Frame &frame)
    {
        // 先把载荷拼出来，Length 才能按「实际写出的载荷字节数」填——§7.1 要求长度自洽，
        // 让调用方传长度就等于把写坏帧的口子留出去
        std::string payload;
        std::visit(FramePayloadAppender{payload}, frame);

        appendQuicVariableLengthInteger(bytes, http3FrameTypeValue(frame));
        appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(payload.size()));
        bytes += payload;
    }

    void appendHttp3FrameWithPayload(std::string &bytes, const Http3FrameType frameType, const std::span<const std::uint8_t> payload)
    {
        // 与帧版本同一套头部写法：Length 取载荷视图的实际字节数，二者不可能对不上
        appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(frameType));
        appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(payload.size()));
        appendQuicRawBytes(bytes, payload);
    }

    void appendHttp3StreamTypeHeader(std::string &bytes, const Http3StreamType streamType)
    {
        // §6.2 图 1：单向流开头就是单个变长整数，没有长度域也没有别的字段
        appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(streamType));
    }

    Http3FrameReader::Http3FrameReader(const std::size_t maximumFrameByteCount)
        : m_maximumFrameByteCount(maximumFrameByteCount)
    {
        if (m_maximumFrameByteCount == 0)
        {
            throw Base::InvalidArgumentException("HTTP/3 帧解码器的单帧上限不能是 0：最小的帧（1 字节类型 + 1 字节长度）都容不下");
        }
    }

    std::expected<void, Http3FrameError> Http3FrameReader::feed(const std::span<const std::uint8_t> newBytes)
    {
        if (m_stickyError.has_value())
        {
            // 粘滞：缓冲里的字节已经无法解释，再收只会把坏帧的尾巴当成下一帧的开头
            return std::unexpected(*m_stickyError);
        }
        if (newBytes.size() > m_maximumFrameByteCount)
        {
            // 块长本身就是调用方给的：超过单帧上限说明没按约定切块，收下就等于让缓冲按这个尺度长出去
            return std::unexpected(latchError(Http3FrameErrorKind::LimitExceeded,
                                              std::format("本次喂入 {} 字节，超过单帧上限 {} 字节：请按不超过上限的块切好再喂"
                                                          "（RFC 9114 §10.5 的过量负载防护）",
                                                          newBytes.size(), m_maximumFrameByteCount)));
        }

        // 上一次已交走的帧到此才从缓冲里挪走，好让那一帧的视图在本次调用前一直能读——
        // 也正因此，之前交出的帧的载荷视图从这一刻起失效
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_consumedByteOffset));
        m_consumedByteOffset = 0;
        m_buffer.insert(m_buffer.end(), newBytes.begin(), newBytes.end());
        return {};
    }

    std::expected<std::optional<Http3Frame>, Http3FrameError> Http3FrameReader::nextFrame()
    {
        if (m_stickyError.has_value())
        {
            return std::unexpected(*m_stickyError);
        }
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_consumedByteOffset));
        m_consumedByteOffset = 0;

        const std::span<const std::uint8_t> buffered(m_buffer);
        // 帧头是 Type、Length 两个紧挨着的变长整数，各自都可能横跨两次 feed：
        // 解不出完整的整数字段就是「还没到齐」，交空 optional 而不算错
        const auto typeDecoded = decodeQuicVariableLengthInteger(buffered);
        if (!typeDecoded.has_value())
        {
            return std::optional<Http3Frame>{};
        }
        const auto lengthDecoded = decodeQuicVariableLengthInteger(buffered.subspan(typeDecoded->byteCount));
        if (!lengthDecoded.has_value())
        {
            return std::optional<Http3Frame>{};
        }

        const std::uint64_t headerByteCount = typeDecoded->byteCount + lengthDecoded->byteCount;
        // 变长整数最大 2^62-1，加不超过 8 字节的帧头不会回绕，因此这一步在 64 位里安全
        const std::uint64_t declaredFrameByteCount = headerByteCount + lengthDecoded->value;
        if (declaredFrameByteCount > m_maximumFrameByteCount)
        {
            // 声明长度本身就超上限：等下去也不可能有这一帧，当场判超限而不是让缓冲空长
            return std::unexpected(latchError(Http3FrameErrorKind::LimitExceeded,
                                              std::format("{}帧声明载荷 {} 字节，加上 {} 字节帧头超过单帧上限 {} 字节"
                                                          "（RFC 9114 §10.8 要求长度自洽，本端不接受超上限的帧）",
                                                          http3FrameTypeName(typeDecoded->value), lengthDecoded->value,
                                                          headerByteCount, m_maximumFrameByteCount)));
        }
        if (buffered.size() < declaredFrameByteCount)
        {
            return std::optional<Http3Frame>{};
        }

        const auto payload = buffered.subspan(headerByteCount, static_cast<std::size_t>(lengthDecoded->value));
        auto decodedFrame = decodeHttp3FramePayload(typeDecoded->value, payload);
        if (!decodedFrame.has_value())
        {
            return std::unexpected(latchError(decodedFrame.error().kind, std::move(decodedFrame.error().message)));
        }

        // 只推游标不挪字节：挪动会把刚交出去的视图指向的内存抽掉，留到下一次调用开头再做
        m_consumedByteOffset = static_cast<std::size_t>(declaredFrameByteCount);
        return std::optional<Http3Frame>{std::move(*decodedFrame)};
    }

    void Http3FrameReader::reset() noexcept
    {
        m_buffer.clear();
        m_consumedByteOffset = 0;
        m_stickyError.reset();
    }

    std::size_t Http3FrameReader::pendingByteCount() const noexcept
    {
        return m_buffer.size() - m_consumedByteOffset;
    }

    Http3FrameError Http3FrameReader::latchError(const Http3FrameErrorKind kind, std::string message)
    {
        m_stickyError = Http3FrameError{kind, std::move(message)};
        return *m_stickyError;
    }
} // namespace AsynGyanis::Net
