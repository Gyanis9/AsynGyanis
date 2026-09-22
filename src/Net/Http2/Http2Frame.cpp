#include "Net/Http2/Http2Frame.h"

#include <algorithm>
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
        /// 流号字段的最高位是保留位 R（RFC 7540 §4.1）：本实现按要求从严，收到即判错
        constexpr std::uint32_t kStreamIdReservedBitMask = 0x80000000U;

        /// SETTINGS 一个参数的线长：16 位标识 + 32 位取值（RFC 7540 §6.5.1）
        constexpr std::size_t kSettingByteCount = 6;

        /// HEADERS 的优先级字段线长：4 字节（E 位 + 31 位父流号）+ 1 字节权重（RFC 7540 §6.2）
        constexpr std::size_t kPriorityFieldByteCount = 5;

        /// PING 的不透明数据长度（RFC 7540 §6.7）
        constexpr std::size_t kPingOpaqueDataByteCount = 8;

        /// GOAWAY 的固定部分长度：4 字节最后流号 + 4 字节错误码（RFC 7540 §6.8）
        constexpr std::size_t kGoAwayFixedByteCount = 8;

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
         * @brief 判断帧类型取值是否落在 RFC 7540 §6 定义过的类型里
         * @param frameType 帧类型取值（可能来自线上的任意字节）
         * @return true 表示是已定义的类型
         */
        bool isKnownFrameType(const Http2FrameType frameType) noexcept
        {
            switch (frameType)
            {
                case Http2FrameType::Data:
                case Http2FrameType::Headers:
                case Http2FrameType::Priority:
                case Http2FrameType::RstStream:
                case Http2FrameType::Settings:
                case Http2FrameType::PushPromise:
                case Http2FrameType::Ping:
                case Http2FrameType::GoAway:
                case Http2FrameType::WindowUpdate:
                case Http2FrameType::Continuation:
                    return true;
            }
            return false;
        }

        /**
         * @brief 按大端读出 16 位无符号数
         * @param bytes 起始指针，调用方保证 2 字节可读
         * @return std::uint16_t 读出的数值
         */
        std::uint16_t readBigEndian16(const char *const bytes) noexcept
        {
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[0])) << 8) |
                                              static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[1])));
        }

        /**
         * @brief 按大端读出 32 位无符号数
         * @param bytes 起始指针，调用方保证 4 字节可读
         * @return std::uint32_t 读出的数值
         */
        std::uint32_t readBigEndian32(const char *const bytes) noexcept
        {
            std::uint32_t value = 0;
            // 线上整数都是大端：先读到的字节是高位，逐字节右移拼接
            for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
            {
                value = (value << 8) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex]));
            }
            return value;
        }

        /**
         * @brief 把 16 位无符号数按大端追加到目标串末尾
         * @param bytes 目标串
         * @param value 待写入的数值
         */
        void appendBigEndian16(std::string &bytes, const std::uint16_t value)
        {
            bytes.push_back(static_cast<char>((value >> 8) & 0xFFU));
            bytes.push_back(static_cast<char>(value & 0xFFU));
        }

        /**
         * @brief 把 32 位无符号数按大端追加到目标串末尾
         * @param bytes 目标串
         * @param value 待写入的数值
         */
        void appendBigEndian32(std::string &bytes, const std::uint32_t value)
        {
            for (int shiftBitCount = 24; shiftBitCount >= 0; shiftBitCount -= 8)
            {
                bytes.push_back(static_cast<char>((value >> shiftBitCount) & 0xFFU));
            }
        }

        /**
         * @brief 把 24 位长度域按大端追加到目标串末尾
         * @param bytes 目标串
         * @param value 待写入的数值，调用方保证不超过 24 位
         */
        void appendBigEndian24(std::string &bytes, const std::uint32_t value)
        {
            bytes.push_back(static_cast<char>((value >> 16) & 0xFFU));
            bytes.push_back(static_cast<char>((value >> 8) & 0xFFU));
            bytes.push_back(static_cast<char>(value & 0xFFU));
        }

        /**
         * @brief 追加优先级字段的 5 字节线格式
         * @param body 目标串
         * @param priority 优先级字段
         */
        void appendPriorityField(std::string &body, const Http2Priority &priority)
        {
            // E 位是最高位，其后是 31 位父流号；权重单独一字节，实际权重为它加一（RFC 7540 §5.3.2）
            const std::uint32_t dependencyField =
                    (priority.streamDependency & kHttp2MaximumStreamId) | (priority.isExclusive ? kStreamIdReservedBitMask : 0U);
            appendBigEndian32(body, dependencyField);
            body.push_back(static_cast<char>(priority.weight));
        }

        /**
         * @brief 校验优先级字段里的父流号不是本帧自身的流号
         * @param streamDependency 优先级字段声明的父流号
         * @param streamId 本帧的流号
         * @throws Base::InvalidArgumentException 父流号等于自身流号（RFC 7540 §5.3.1 禁止流依赖自己）
         */
        void rejectSelfDependency(const std::uint32_t streamDependency, const std::uint32_t streamId)
        {
            if (streamDependency == streamId)
            {
                throw Base::InvalidArgumentException(std::format("优先级字段的父流号等于自身流号 {}（RFC 7540 §5.3.1 禁止流依赖自己）："
                                                                 "请改指另一条流，或去掉优先级字段",
                                                                 streamId));
            }
        }
    } // namespace

    Http2ErrorCode toHttp2ErrorCode(const Http2FrameErrorKind errorKind) noexcept
    {
        switch (errorKind)
        {
            case Http2FrameErrorKind::None:
                return Http2ErrorCode::NoError;
            case Http2FrameErrorKind::ProtocolError:
                return Http2ErrorCode::ProtocolError;
            case Http2FrameErrorKind::FrameSizeError:
                return Http2ErrorCode::FrameSizeError;
            case Http2FrameErrorKind::LimitExceeded:
                // 本端资源上限被突破不是对端违规，按 RFC 7540 §7 对「可能造成过量负载」的建议取值回
                return Http2ErrorCode::EnhanceYourCalm;
        }
        return Http2ErrorCode::InternalError;
    }

    std::string_view http2FrameTypeName(const Http2FrameType frameType) noexcept
    {
        switch (frameType)
        {
            case Http2FrameType::Data:
                return "DATA";
            case Http2FrameType::Headers:
                return "HEADERS";
            case Http2FrameType::Priority:
                return "PRIORITY";
            case Http2FrameType::RstStream:
                return "RST_STREAM";
            case Http2FrameType::Settings:
                return "SETTINGS";
            case Http2FrameType::PushPromise:
                return "PUSH_PROMISE";
            case Http2FrameType::Ping:
                return "PING";
            case Http2FrameType::GoAway:
                return "GOAWAY";
            case Http2FrameType::WindowUpdate:
                return "WINDOW_UPDATE";
            case Http2FrameType::Continuation:
                return "CONTINUATION";
        }
        // 线上类型字段可以是任意字节：未定义取值不属于枚举，但必须能被描述出来
        return "未知类型";
    }

    std::string_view http2ErrorCodeName(const Http2ErrorCode errorCode) noexcept
    {
        switch (errorCode)
        {
            case Http2ErrorCode::NoError:
                return "NO_ERROR";
            case Http2ErrorCode::ProtocolError:
                return "PROTOCOL_ERROR";
            case Http2ErrorCode::InternalError:
                return "INTERNAL_ERROR";
            case Http2ErrorCode::FlowControlError:
                return "FLOW_CONTROL_ERROR";
            case Http2ErrorCode::SettingsTimeout:
                return "SETTINGS_TIMEOUT";
            case Http2ErrorCode::StreamClosed:
                return "STREAM_CLOSED";
            case Http2ErrorCode::FrameSizeError:
                return "FRAME_SIZE_ERROR";
            case Http2ErrorCode::RefusedStream:
                return "REFUSED_STREAM";
            case Http2ErrorCode::Cancel:
                return "CANCEL";
            case Http2ErrorCode::CompressionError:
                return "COMPRESSION_ERROR";
            case Http2ErrorCode::ConnectError:
                return "CONNECT_ERROR";
            case Http2ErrorCode::EnhanceYourCalm:
                return "ENHANCE_YOUR_CALM";
            case Http2ErrorCode::InadequateSecurity:
                return "INADEQUATE_SECURITY";
            case Http2ErrorCode::Http11Required:
                return "HTTP_1_1_REQUIRED";
        }
        return "未定义错误码";
    }

    std::string encodeHttp2FrameHeader(const Http2FrameHeader &header)
    {
        // 长度域只有 24 位：超出的负载写不进帧头，静默截断会让对端按错误的边界切帧
        if (header.payloadLength > kHttp2MaximumFramePayloadByteCount)
        {
            throw Base::InvalidArgumentException(std::format("帧负载长度 {} 字节超出 24 位长度域可表示的范围 {} 字节（RFC 7540 §4.1）："
                                                             "请把数据拆成多条帧",
                                                             header.payloadLength, kHttp2MaximumFramePayloadByteCount));
        }
        if (header.streamId > kHttp2MaximumStreamId)
        {
            throw Base::InvalidArgumentException(std::format("流号 {} 超出 31 位可表示的范围 {}（RFC 7540 §4.1 要求 R 位为 0）："
                                                             "连接级帧请传 0，流级帧请传 1 到 {}",
                                                             header.streamId, kHttp2MaximumStreamId, kHttp2MaximumStreamId));
        }
        if (!isKnownFrameType(header.type))
        {
            throw Base::InvalidArgumentException(
                    std::format("帧类型 0x{:X} 未定义（RFC 7540 §6 只定义了 0x0 到 0x9）：请改用已定义的类型",
                                static_cast<unsigned int>(static_cast<std::uint8_t>(header.type))));
        }

        std::string headerBytes;
        headerBytes.reserve(kHttp2FrameHeaderByteCount);
        appendBigEndian24(headerBytes, header.payloadLength);
        headerBytes.push_back(static_cast<char>(static_cast<std::uint8_t>(header.type)));
        headerBytes.push_back(static_cast<char>(header.flags));
        // 帧头的流号恒为 31 位：R 位在本实现里只能为 0，上面的范围校验已经保证
        appendBigEndian32(headerBytes, header.streamId);
        return headerBytes;
    }

    bool decodeHttp2FrameHeader(const std::string_view bytes, Http2FrameHeader &header, std::string *const errorText)
    {
        clearError(errorText);
        if (bytes.size() < kHttp2FrameHeaderByteCount)
        {
            writeError(errorText, std::format("帧头需要 {} 字节，只收到 {} 字节：请先把帧头读满再解（增量解码请用 Http2FrameDecoder）",
                                              kHttp2FrameHeaderByteCount, bytes.size()));
            return false;
        }

        const std::uint32_t payloadLength = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[0])) << 16) |
                                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[1])) << 8) |
                                            static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[2]));
        const std::uint32_t streamIdField = readBigEndian32(bytes.data() + 5);
        if ((streamIdField & kStreamIdReservedBitMask) != 0)
        {
            // RFC 7540 §4.1 允许接收侧忽略该位，本实现从严：放行等于让「这一帧是什么」取决于对端是否在用
            // 未定义的扩展，而这些帧在本端无法被正确解释
            writeError(errorText, "帧头流号的最高位 R 必须为 0（RFC 7540 §4.1）：请检查对端是否在写未定义的保留位，或先用抓包工具核对这 9 字节");
            return false;
        }

        header.payloadLength = payloadLength;
        header.type = static_cast<Http2FrameType>(static_cast<std::uint8_t>(bytes[3]));
        header.flags = static_cast<std::uint8_t>(bytes[4]);
        header.streamId = streamIdField & kHttp2MaximumStreamId;
        return true;
    }

    std::string encodeHttp2Frame(const Http2FrameType type, const std::uint8_t flags, const std::uint32_t streamId,
                                 const std::string_view payload)
    {
        std::string frame;
        appendHttp2Frame(frame, type, flags, streamId, payload);
        return frame;
    }

    void appendHttp2Frame(std::string &bytes, const Http2FrameType type, const std::uint8_t flags, const std::uint32_t streamId,
                          const std::string_view payload)
    {
        // 先判上限再窄化：直接把 size_t 转成 24 位长度域会在超限时静默回绕
        if (payload.size() > static_cast<std::size_t>(kHttp2MaximumFramePayloadByteCount))
        {
            throw Base::InvalidArgumentException(std::format("帧负载 {} 字节超出 24 位长度域可表示的范围 {} 字节（RFC 7540 §4.1）："
                                                             "请把数据拆成多条帧",
                                                             payload.size(), kHttp2MaximumFramePayloadByteCount));
        }

        Http2FrameHeader header;
        header.payloadLength = static_cast<std::uint32_t>(payload.size());
        header.type = type;
        header.flags = flags;
        header.streamId = streamId;
        // 流号与类型的校验由帧头编码那一份负责：它在写目标缓冲之前就会拒掉非法值，
        // 于是抛错时调用方的缓冲一字节未加，不会留下半帧
        const std::string headerBytes = encodeHttp2FrameHeader(header);

        // 刻意不按整帧精确 reserve：连接的待发缓冲一轮里会连排多条帧，精确预留每次都把容量卡成
        // 「刚好装下」，下一帧又要扩容，累计成 O(帧数²) 的搬移；交给 append 自己的倍增策略才是均摊 O(1)
        bytes.append(headerBytes);
        bytes.append(payload);
    }

    std::string encodeHttp2SettingsFrame(const Http2SettingsPayload &payload)
    {
        // ACK 帧只是确认收到，负载必须为空（RFC 7540 §6.5）；带参数发出去对端按连接错误处理
        if (payload.isAcknowledgement && !payload.parameters.empty())
        {
            throw Base::InvalidArgumentException(std::format("SETTINGS 的 ACK 帧负载必须为空（RFC 7540 §6.5），本次带了 {} 个参数："
                                                             "请把参数放在不带 ACK 的 SETTINGS 帧里发送",
                                                             payload.parameters.size()));
        }

        std::string body;
        if (!payload.isAcknowledgement)
        {
            body.reserve(payload.parameters.size() * kSettingByteCount);
            for (const Http2Setting &setting: payload.parameters)
            {
                appendBigEndian16(body, setting.identifier);
                appendBigEndian32(body, setting.value);
            }
        }
        // SETTINGS 作用于整条连接：流号恒为 0（RFC 7540 §6.5）
        return encodeHttp2Frame(Http2FrameType::Settings, payload.isAcknowledgement ? kHttp2FlagAcknowledge : 0U, 0, body);
    }

    std::string encodeHttp2PingFrame(const Http2PingPayload &payload)
    {
        std::string body;
        body.reserve(kPingOpaqueDataByteCount);
        for (const std::uint8_t byteValue: payload.opaqueData)
        {
            body.push_back(static_cast<char>(byteValue));
        }
        // PING 同样是连接级帧，流号恒为 0（RFC 7540 §6.7）
        return encodeHttp2Frame(Http2FrameType::Ping, payload.isAcknowledgement ? kHttp2FlagAcknowledge : 0U, 0, body);
    }

    std::string encodeHttp2GoAwayFrame(const Http2GoAwayPayload &payload)
    {
        if (payload.lastStreamId > kHttp2MaximumStreamId)
        {
            throw Base::InvalidArgumentException(std::format("GOAWAY 的最后流号 {} 超出 31 位可表示的范围 {}（RFC 7540 §6.8）："
                                                             "没有处理过任何流时请传 0",
                                                             payload.lastStreamId, kHttp2MaximumStreamId));
        }

        std::string body;
        body.reserve(kGoAwayFixedByteCount + payload.debugData.size());
        appendBigEndian32(body, payload.lastStreamId);
        appendBigEndian32(body, static_cast<std::uint32_t>(payload.errorCode));
        body.append(payload.debugData);
        return encodeHttp2Frame(Http2FrameType::GoAway, 0, 0, body);
    }

    std::string encodeHttp2RstStreamFrame(const Http2RstStreamPayload &payload, const std::uint32_t streamId)
    {
        if (streamId == 0)
        {
            throw Base::InvalidArgumentException("RST_STREAM 必须关联到一条流（RFC 7540 §6.4 要求流号非 0）："
                                                 "流号 0 是连接级，终止整条连接请改用 GOAWAY");
        }

        std::string body;
        body.reserve(4);
        appendBigEndian32(body, static_cast<std::uint32_t>(payload.errorCode));
        return encodeHttp2Frame(Http2FrameType::RstStream, 0, streamId, body);
    }

    std::string encodeHttp2WindowUpdateFrame(const Http2WindowUpdatePayload &payload, const std::uint32_t streamId)
    {
        // 增量为 0 的 WINDOW_UPDATE 是规范明文禁止的（RFC 7540 §6.9），发出去对端必然判错
        if (payload.windowSizeIncrement == 0)
        {
            throw Base::InvalidArgumentException("WINDOW_UPDATE 的窗口增量不得为 0（RFC 7540 §6.9）："
                                                 "不需要调整窗口就别发这一帧");
        }
        if (payload.windowSizeIncrement > kHttp2MaximumStreamId)
        {
            throw Base::InvalidArgumentException(std::format("窗口增量 {} 超出 31 位可表示的范围 {}（RFC 7540 §6.9）：请减小增量",
                                                             payload.windowSizeIncrement, kHttp2MaximumStreamId));
        }

        std::string body;
        body.reserve(4);
        appendBigEndian32(body, payload.windowSizeIncrement);
        return encodeHttp2Frame(Http2FrameType::WindowUpdate, 0, streamId, body);
    }

    std::string encodeHttp2DataFrame(const Http2DataPayload &payload, const std::uint32_t streamId)
    {
        return encodeHttp2DataFrame(payload.data, payload.endStream, streamId);
    }

    std::string encodeHttp2DataFrame(const std::string_view data, const bool endStream, const std::uint32_t streamId)
    {
        if (streamId == 0)
        {
            throw Base::InvalidArgumentException("DATA 必须关联到一条流（RFC 7540 §6.1 要求流号非 0）："
                                                 "连接级帧只能是 SETTINGS/PING/GOAWAY 这类不带应用数据的类型");
        }

        // 编码侧不产生 padding：填充只用来打乱长度特征，服务端发出的数据帧没有这个需求
        return encodeHttp2Frame(Http2FrameType::Data, endStream ? kHttp2FlagEndStream : 0U, streamId, data);
    }

    std::string encodeHttp2HeadersFrame(const Http2HeadersPayload &payload, const std::uint32_t streamId)
    {
        if (streamId == 0)
        {
            throw Base::InvalidArgumentException("HEADERS 必须关联到一条流（RFC 7540 §6.2 要求流号非 0）："
                                                 "连接级帧只能是 SETTINGS/PING/GOAWAY 这类不带头块的类型");
        }

        std::uint8_t flags = 0;
        if (payload.endStream)
        {
            flags = static_cast<std::uint8_t>(flags | kHttp2FlagEndStream);
        }
        if (payload.endHeaders)
        {
            flags = static_cast<std::uint8_t>(flags | kHttp2FlagEndHeaders);
        }

        std::string body;
        if (payload.hasPriority)
        {
            rejectSelfDependency(payload.priority.streamDependency, streamId);
            flags = static_cast<std::uint8_t>(flags | kHttp2FlagPriority);
            body.reserve(kPriorityFieldByteCount + payload.headerBlockFragment.size());
            appendPriorityField(body, payload.priority);
        }
        body.append(payload.headerBlockFragment);
        return encodeHttp2Frame(Http2FrameType::Headers, flags, streamId, body);
    }

    std::string encodeHttp2ContinuationFrame(const Http2ContinuationPayload &payload, const std::uint32_t streamId)
    {
        if (streamId == 0)
        {
            throw Base::InvalidArgumentException("CONTINUATION 必须关联到一条流（RFC 7540 §6.10 要求流号非 0）："
                                                 "它只能续在同一条流的 HEADERS 之后");
        }

        return encodeHttp2Frame(Http2FrameType::Continuation, payload.endHeaders ? kHttp2FlagEndHeaders : 0U, streamId,
                                payload.headerBlockFragment);
    }

    std::string encodeHttp2PriorityFrame(const Http2Priority &priority, const std::uint32_t streamId)
    {
        if (streamId == 0)
        {
            throw Base::InvalidArgumentException("PRIORITY 必须关联到一条流（RFC 7540 §6.3 要求流号非 0）："
                                                 "流优先级只对具体的流有意义");
        }
        rejectSelfDependency(priority.streamDependency, streamId);

        std::string body;
        body.reserve(kPriorityFieldByteCount);
        appendPriorityField(body, priority);
        return encodeHttp2Frame(Http2FrameType::Priority, 0, streamId, body);
    }

    bool parseHttp2SettingsPayload(const Http2Frame &frame, Http2SettingsPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::Settings)
        {
            writeError(errorText, std::format("这帧不是 SETTINGS（实际类型 {} 0x{:X}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type),
                                              static_cast<unsigned int>(static_cast<std::uint8_t>(frame.header.type))));
            return false;
        }
        if (frame.payload.size() % kSettingByteCount != 0)
        {
            writeError(errorText, std::format("SETTINGS 负载 {} 字节不是 {} 的整数倍（RFC 7540 §6.5）：参数是 16 位标识加 32 位取值，"
                                              "请检查对端是否截断了帧",
                                              frame.payload.size(), kSettingByteCount));
            return false;
        }

        payload = Http2SettingsPayload{};
        payload.isAcknowledgement = (frame.header.flags & kHttp2FlagAcknowledge) != 0;
        payload.parameters.reserve(frame.payload.size() / kSettingByteCount);
        // 未知标识也原样收下（RFC 7540 §6.5.2 要求忽略而非判错）：不保留它，上层就无从知道对端说了什么
        for (std::size_t offset = 0; offset < frame.payload.size(); offset += kSettingByteCount)
        {
            Http2Setting setting;
            setting.identifier = readBigEndian16(frame.payload.data() + offset);
            setting.value = readBigEndian32(frame.payload.data() + offset + 2);
            payload.parameters.push_back(setting);
        }
        return true;
    }

    bool tryGetHttp2Setting(const Http2SettingsPayload &payload, const Http2SettingIdentifier identifier, std::uint32_t &value) noexcept
    {
        // 同一标识重复出现时取最后一次（RFC 7540 §6.5：参数按顺序处理，值取最后见到的），因此倒着找
        for (auto settingIterator = payload.parameters.rbegin(); settingIterator != payload.parameters.rend(); ++settingIterator)
        {
            if (static_cast<Http2SettingIdentifier>(settingIterator->identifier) == identifier)
            {
                value = settingIterator->value;
                return true;
            }
        }
        return false;
    }

    bool parseHttp2PingPayload(const Http2Frame &frame, Http2PingPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::Ping)
        {
            writeError(errorText, std::format("这帧不是 PING（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }
        if (frame.payload.size() != kPingOpaqueDataByteCount)
        {
            writeError(errorText, std::format("PING 负载必须是 {} 字节（RFC 7540 §6.7），实际 {} 字节：请检查对端是否截断了帧",
                                              kPingOpaqueDataByteCount, frame.payload.size()));
            return false;
        }

        payload = Http2PingPayload{};
        payload.isAcknowledgement = (frame.header.flags & kHttp2FlagAcknowledge) != 0;
        for (std::size_t byteIndex = 0; byteIndex < kPingOpaqueDataByteCount; ++byteIndex)
        {
            payload.opaqueData[byteIndex] = static_cast<std::uint8_t>(frame.payload[byteIndex]);
        }
        return true;
    }

    bool parseHttp2GoAwayPayload(const Http2Frame &frame, Http2GoAwayPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::GoAway)
        {
            writeError(errorText, std::format("这帧不是 GOAWAY（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }
        if (frame.payload.size() < kGoAwayFixedByteCount)
        {
            writeError(errorText, std::format("GOAWAY 负载至少 {} 字节（RFC 7540 §6.8），实际 {} 字节：请检查对端是否截断了帧",
                                              kGoAwayFixedByteCount, frame.payload.size()));
            return false;
        }

        payload = Http2GoAwayPayload{};
        // 最后流号字段的保留位按 RFC 7540 §6.8 掩掉：该位在读取侧没有语义
        payload.lastStreamId = readBigEndian32(frame.payload.data()) & kHttp2MaximumStreamId;
        payload.errorCode = static_cast<Http2ErrorCode>(readBigEndian32(frame.payload.data() + 4));
        payload.debugData = frame.payload.substr(kGoAwayFixedByteCount);
        return true;
    }

    bool parseHttp2RstStreamPayload(const Http2Frame &frame, Http2RstStreamPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::RstStream)
        {
            writeError(errorText, std::format("这帧不是 RST_STREAM（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }
        if (frame.payload.size() != 4)
        {
            writeError(errorText, std::format("RST_STREAM 负载必须是 4 字节错误码（RFC 7540 §6.4），实际 {} 字节", frame.payload.size()));
            return false;
        }

        payload = Http2RstStreamPayload{};
        payload.errorCode = static_cast<Http2ErrorCode>(readBigEndian32(frame.payload.data()));
        return true;
    }

    bool parseHttp2WindowUpdatePayload(const Http2Frame &frame, Http2WindowUpdatePayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::WindowUpdate)
        {
            writeError(errorText, std::format("这帧不是 WINDOW_UPDATE（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }
        if (frame.payload.size() != 4)
        {
            writeError(errorText, std::format("WINDOW_UPDATE 负载必须是 4 字节增量（RFC 7540 §6.9），实际 {} 字节", frame.payload.size()));
            return false;
        }

        payload = Http2WindowUpdatePayload{};
        // 保留位在读取侧没有语义，掩掉；增量 0 是规范禁止的取值，帧层已判错，这里兜住手工构造的帧
        payload.windowSizeIncrement = readBigEndian32(frame.payload.data()) & kHttp2MaximumStreamId;
        if (payload.windowSizeIncrement == 0)
        {
            writeError(errorText, "WINDOW_UPDATE 的窗口增量不得为 0（RFC 7540 §6.9）：该帧只能用于放宽窗口，请检查对端构造");
            return false;
        }
        return true;
    }

    bool parseHttp2DataPayload(const Http2Frame &frame, Http2DataPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::Data)
        {
            writeError(errorText, std::format("这帧不是 DATA（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }

        payload = Http2DataPayload{};
        payload.endStream = (frame.header.flags & kHttp2FlagEndStream) != 0;
        // padding 已由帧层剥掉：这里拿到的就是应用数据，本层不再按标志重算偏移
        payload.data = frame.payload;
        return true;
    }

    bool parseHttp2HeadersPayload(const Http2Frame &frame, Http2HeadersPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::Headers)
        {
            writeError(errorText, std::format("这帧不是 HEADERS（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }

        payload = Http2HeadersPayload{};
        payload.endStream = (frame.header.flags & kHttp2FlagEndStream) != 0;
        payload.endHeaders = (frame.header.flags & kHttp2FlagEndHeaders) != 0;
        payload.hasPriority = frame.hasPriority;
        payload.priority = frame.priority;
        // padding 与优先级字段已由帧层剥掉：这里是**头块片段**，交给 HPACK 层按序拼接与解码
        payload.headerBlockFragment = frame.payload;
        return true;
    }

    bool parseHttp2ContinuationPayload(const Http2Frame &frame, Http2ContinuationPayload &payload, std::string *const errorText)
    {
        clearError(errorText);
        if (frame.header.type != Http2FrameType::Continuation)
        {
            writeError(errorText, std::format("这帧不是 CONTINUATION（实际类型 {}）：请按帧头类型分发后再调用本函数",
                                              http2FrameTypeName(frame.header.type)));
            return false;
        }

        payload = Http2ContinuationPayload{};
        payload.endHeaders = (frame.header.flags & kHttp2FlagEndHeaders) != 0;
        payload.headerBlockFragment = frame.payload;
        return true;
    }

    Http2FrameDecoder::Http2FrameDecoder(Http2FrameLimits limits) : m_limits(limits)
    {
        // 这个上限就是本端要通告出去的 SETTINGS_MAX_FRAME_SIZE，越界取值等于在通告一个非法设置项
        if (m_limits.maximumFrameSizeByteCount < kHttp2DefaultMaximumFrameSize ||
            m_limits.maximumFrameSizeByteCount > kHttp2MaximumMaximumFrameSize)
        {
            throw Base::InvalidArgumentException(
                    std::format("Http2FrameDecoder：单帧负载上限 {} 不在 SETTINGS_MAX_FRAME_SIZE 的合法区间 [{}, {}] 内（RFC 7540 §6.5.2）："
                                "请改用区间内的取值，缺省值 {} 即规范的初始值",
                                m_limits.maximumFrameSizeByteCount, kHttp2DefaultMaximumFrameSize, kHttp2MaximumMaximumFrameSize,
                                kHttp2DefaultMaximumFrameSize));
        }
    }

    Http2FrameDecodeStatus Http2FrameDecoder::parse(const char *const data, const std::size_t length)
    {
        // 已产出的帧还没取走：一字节都不能再吃。这些字节属于下一帧，喂进去会被当成当前帧的开头，
        // 把已经产出的结果改坏
        if (m_hasPendingFrame)
        {
            m_consumedByteCount = 0;
            return Http2FrameDecodeStatus::Frame;
        }

        // 错误粘滞：非 reset() 不能恢复，调用方要么重置要么按错误码收场
        if (m_stage == Stage::Failed)
        {
            m_consumedByteCount = 0;
            return Http2FrameDecodeStatus::Error;
        }

        std::size_t consumed = 0;
        while (consumed < length && m_stage != Stage::Failed && !m_hasPendingFrame)
        {
            // 累计上限是本端策略而不是对端违规：到了就停在当前字节上，不再往下读
            if (m_limits.maximumTotalConsumedByteCount != 0 && m_totalConsumedByteCount >= m_limits.maximumTotalConsumedByteCount)
            {
                recordFailure(Http2FrameErrorKind::LimitExceeded,
                              std::format("累计读取 {} 字节已达上限 {} 字节：请结束这条连接并按 ENHANCE_YOUR_CALM 收场，"
                                          "或调高 Http2FrameLimits::maximumTotalConsumedByteCount",
                                          m_totalConsumedByteCount, m_limits.maximumTotalConsumedByteCount));
                break;
            }

            if (m_stage == Stage::FrameHeader)
            {
                m_headerBytes[m_headerBytesSeen] = static_cast<std::uint8_t>(data[consumed]);
                ++m_headerBytesSeen;
                ++consumed;
                ++m_totalConsumedByteCount;
                if (m_headerBytesSeen < kHttp2FrameHeaderByteCount)
                {
                    continue;
                }

                // 失败原因已由 acceptFrameHeader 记下，循环条件随即退出
                if (!acceptFrameHeader(m_headerBytes))
                {
                    break;
                }
                m_stage = Stage::Payload;

                // 零长度负载在帧头收齐这一刻就算完整：不能留到负载阶段再判，否则调用方得再喂一个字节才拿得到帧
                if (m_currentHeader.payloadLength == 0)
                {
                    completeFrame();
                }
                continue;
            }

            // 走到这里只剩负载阶段，且长度已过上限校验，本次能收多少收多少
            std::size_t chunkLength = std::min(static_cast<std::size_t>(m_currentHeader.payloadLength) - m_payloadBytesSeen,
                                               length - consumed);
            if (m_limits.maximumTotalConsumedByteCount != 0)
            {
                // 累计上限在本次喂入中间也会到：把本段截到上限处，结束位置因此与「逐字节喂」完全一致
                chunkLength = std::min(chunkLength, m_limits.maximumTotalConsumedByteCount - m_totalConsumedByteCount);
            }
            m_payloadBuffer.append(data + consumed, chunkLength);
            m_payloadBytesSeen += chunkLength;
            consumed += chunkLength;
            m_totalConsumedByteCount += chunkLength;
            if (m_payloadBytesSeen == static_cast<std::size_t>(m_currentHeader.payloadLength))
            {
                completeFrame();
            }
        }

        m_consumedByteCount = consumed;
        if (m_stage == Stage::Failed)
        {
            return Http2FrameDecodeStatus::Error;
        }
        return m_hasPendingFrame ? Http2FrameDecodeStatus::Frame : Http2FrameDecodeStatus::NeedMore;
    }

    std::size_t Http2FrameDecoder::consumedByteCount() const
    {
        return m_consumedByteCount;
    }

    Http2Frame Http2FrameDecoder::takeFrame()
    {
        // 没有产出就取属于用法错误：静默返回一个空帧会让上层把「还没有数据」当成一帧真帧处理
        if (!m_hasPendingFrame)
        {
            throw Base::LogicException("HTTP/2 帧解码器：当前没有待取走的帧，请先等 parse() 返回 Frame 再调用 takeFrame()");
        }

        Http2Frame frame = std::move(m_pendingFrame);
        // 取走即清标记：此后 parse() 才能继续消费字节。负载只清内容、保留容量供下一帧复用
        m_pendingFrame.payload.clear();
        m_hasPendingFrame = false;
        return frame;
    }

    void Http2FrameDecoder::reset()
    {
        m_stage = Stage::FrameHeader;
        clearFrameScratch();
        m_payloadBuffer.clear();
        m_pendingFrame = Http2Frame{};
        m_hasPendingFrame = false;
        m_hasError = false;
        m_errorKind = Http2FrameErrorKind::None;
        m_errorMessage.clear();
        m_consumedByteCount = 0;
        m_totalConsumedByteCount = 0;
    }

    bool Http2FrameDecoder::hasError() const
    {
        return m_hasError;
    }

    Http2FrameErrorKind Http2FrameDecoder::errorKind() const
    {
        return m_errorKind;
    }

    std::string Http2FrameDecoder::errorMessage() const
    {
        return m_errorMessage;
    }

    std::size_t Http2FrameDecoder::totalConsumedByteCount() const
    {
        return m_totalConsumedByteCount;
    }

    bool Http2FrameDecoder::acceptFrameHeader(const std::array<std::uint8_t, kHttp2FrameHeaderByteCount> &headerBytes)
    {
        Http2FrameHeader header;
        std::string reason;
        // 帧头字节已收齐，走与独立入口同一套解析：两处规则不会各自漂移
        const std::string_view headerBytesView(reinterpret_cast<const char *>(headerBytes.data()), headerBytes.size());
        if (!decodeHttp2FrameHeader(headerBytesView, header, &reason))
        {
            recordFailure(Http2FrameErrorKind::ProtocolError, std::move(reason));
            return false;
        }

        // 超上限的帧连收都不收：对端声明一个天文数字的长度，本端就会一直等下去，内存与连接都被占着
        if (header.payloadLength > m_limits.maximumFrameSizeByteCount)
        {
            recordFailure(Http2FrameErrorKind::FrameSizeError,
                          std::format("帧负载声明 {} 字节超出本端通告的 SETTINGS_MAX_FRAME_SIZE {} 字节（RFC 7540 §4.2）："
                                      "请让对端把数据拆到多条帧里，或调高 Http2FrameLimits::maximumFrameSizeByteCount",
                                      header.payloadLength, m_limits.maximumFrameSizeByteCount));
            return false;
        }

        if (!acceptFrameShape(header))
        {
            return false;
        }

        m_currentHeader = header;
        m_payloadBuffer.clear();
        m_payloadBytesSeen = 0;
        // 长度已过校验，一次预留免得收的过程中反复扩容
        m_payloadBuffer.reserve(header.payloadLength);

        // 优先级字段的有无由类型与标志共同决定，位置在 padding 长度字节之后，收齐负载时按它切
        m_isCurrentFramePriority = (header.type == Http2FrameType::Priority) ||
                                   (header.type == Http2FrameType::Headers && (header.flags & kHttp2FlagPriority) != 0);
        return true;
    }

    bool Http2FrameDecoder::acceptFrameShape(const Http2FrameHeader &header)
    {
        const bool isOnStream = header.streamId != 0;
        switch (header.type)
        {
            case Http2FrameType::Data:
                if (!isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError,
                                  "DATA 帧的流号必须非 0（RFC 7540 §6.1）：流号 0 只用于连接级帧，请检查对端构造");
                    return false;
                }
                return true;
            case Http2FrameType::Headers:
                if (!isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError,
                                  "HEADERS 帧的流号必须非 0（RFC 7540 §6.2）：流号 0 只用于连接级帧，请检查对端构造");
                    return false;
                }
                return true;
            case Http2FrameType::Priority:
                if (!isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError, "PRIORITY 帧的流号必须非 0（RFC 7540 §6.3）");
                    return false;
                }
                if (header.payloadLength != kPriorityFieldByteCount)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("PRIORITY 帧负载必须是 {} 字节（RFC 7540 §6.3），声明了 {} 字节",
                                              kPriorityFieldByteCount, header.payloadLength));
                    return false;
                }
                return true;
            case Http2FrameType::RstStream:
                if (!isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError, "RST_STREAM 帧的流号必须非 0（RFC 7540 §6.4）");
                    return false;
                }
                if (header.payloadLength != 4)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("RST_STREAM 帧负载必须是 4 字节错误码（RFC 7540 §6.4），声明了 {} 字节", header.payloadLength));
                    return false;
                }
                return true;
            case Http2FrameType::Settings:
                if (isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError,
                                  std::format("SETTINGS 帧是连接级帧，流号必须为 0（RFC 7540 §6.5），收到流号 {}", header.streamId));
                    return false;
                }
                // ACK 帧只是确认收到，负载必须为空（RFC 7540 §6.5）
                if ((header.flags & kHttp2FlagAcknowledge) != 0 && header.payloadLength != 0)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("带 ACK 的 SETTINGS 帧负载必须为空（RFC 7540 §6.5），声明了 {} 字节", header.payloadLength));
                    return false;
                }
                if (header.payloadLength % kSettingByteCount != 0)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("SETTINGS 帧负载长度必须是 {} 的整数倍（RFC 7540 §6.5），声明了 {} 字节",
                                              kSettingByteCount, header.payloadLength));
                    return false;
                }
                return true;
            case Http2FrameType::Ping:
                if (isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError,
                                  std::format("PING 帧是连接级帧，流号必须为 0（RFC 7540 §6.7），收到流号 {}", header.streamId));
                    return false;
                }
                if (header.payloadLength != kPingOpaqueDataByteCount)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("PING 帧负载必须是 {} 字节（RFC 7540 §6.7），声明了 {} 字节",
                                              kPingOpaqueDataByteCount, header.payloadLength));
                    return false;
                }
                return true;
            case Http2FrameType::GoAway:
                if (isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError,
                                  std::format("GOAWAY 帧是连接级帧，流号必须为 0（RFC 7540 §6.8），收到流号 {}", header.streamId));
                    return false;
                }
                if (header.payloadLength < kGoAwayFixedByteCount)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("GOAWAY 帧负载至少 {} 字节（RFC 7540 §6.8），声明了 {} 字节",
                                              kGoAwayFixedByteCount, header.payloadLength));
                    return false;
                }
                return true;
            case Http2FrameType::WindowUpdate:
                if (header.payloadLength != 4)
                {
                    recordFailure(Http2FrameErrorKind::FrameSizeError,
                                  std::format("WINDOW_UPDATE 帧负载必须是 4 字节增量（RFC 7540 §6.9），声明了 {} 字节", header.payloadLength));
                    return false;
                }
                return true;
            case Http2FrameType::Continuation:
                if (!isOnStream)
                {
                    recordFailure(Http2FrameErrorKind::ProtocolError,
                                  "CONTINUATION 帧的流号必须非 0（RFC 7540 §6.10）：它只能续在同一条流的 HEADERS 之后");
                    return false;
                }
                return true;
            default:
                // 未知类型（含 PUSH_PROMISE）按 §4.1 必须忽略：原样交出该帧由上层丢弃或处理，
                // 在这里判错会让「未来新增的帧类型」直接把整条连接打死
                return true;
        }
    }

    bool Http2FrameDecoder::acceptPriorityField(const std::string_view bytes)
    {
        const std::uint32_t dependencyField = readBigEndian32(bytes.data());
        const std::uint32_t streamDependency = dependencyField & kHttp2MaximumStreamId;
        // 依赖自己会让优先级树成环（RFC 7540 §5.3.1），对端构造错误必须当场收口
        if (streamDependency == m_currentHeader.streamId)
        {
            recordFailure(Http2FrameErrorKind::ProtocolError,
                          std::format("优先级字段的父流号等于本帧流号 {}（RFC 7540 §5.3.1 禁止流依赖自己）：请检查对端构造",
                                      m_currentHeader.streamId));
            return false;
        }

        m_currentPriority.isExclusive = (dependencyField & kStreamIdReservedBitMask) != 0;
        m_currentPriority.streamDependency = streamDependency;
        m_currentPriority.weight = static_cast<std::uint8_t>(bytes[4]);
        return true;
    }

    void Http2FrameDecoder::completeFrame()
    {
        const std::uint8_t flags = m_currentHeader.flags;
        const std::string_view rawPayload = m_payloadBuffer;
        const bool isPadded = (m_currentHeader.type == Http2FrameType::Data || m_currentHeader.type == Http2FrameType::Headers) &&
                              (flags & kHttp2FlagPadded) != 0;

        std::size_t bodyBegin = 0;
        std::size_t bodyEnd = rawPayload.size();
        if (isPadded)
        {
            // PADDED 帧的第一个字节是填充长度，填充在负载尾部（RFC 7540 §6.1）
            if (rawPayload.empty())
            {
                recordFailure(Http2FrameErrorKind::ProtocolError,
                              "置了 PADDED 位却没有填充长度字节（RFC 7540 §6.1 要求 PADDED 帧的负载至少 1 字节）：请检查对端构造");
                return;
            }
            const auto paddingLength = static_cast<std::size_t>(static_cast<std::uint8_t>(rawPayload[0]));
            if (paddingLength >= rawPayload.size())
            {
                recordFailure(Http2FrameErrorKind::ProtocolError,
                              std::format("置了 PADDED 位的帧里，填充长度 {} 不小于负载长度 {}（RFC 7540 §6.1 要求填充短于负载）："
                                          "请检查对端构造",
                                          paddingLength, rawPayload.size()));
                return;
            }
            bodyBegin = 1;
            bodyEnd -= paddingLength;
        }

        bool hasPriority = false;
        Http2Priority priority{};
        if (m_isCurrentFramePriority)
        {
            // 优先级字段紧跟填充长度字节、位于头块片段之前（RFC 7540 §6.2、§6.3）
            if (bodyEnd < bodyBegin + kPriorityFieldByteCount)
            {
                recordFailure(Http2FrameErrorKind::FrameSizeError,
                              std::format("{} 帧置了 PRIORITY 位，但剥掉 padding 后不足 {} 字节的优先级字段（RFC 7540 §6.2）："
                                          "请检查对端构造",
                                          http2FrameTypeName(m_currentHeader.type), kPriorityFieldByteCount));
                return;
            }
            if (!acceptPriorityField(std::string_view(rawPayload.data() + bodyBegin, kPriorityFieldByteCount)))
            {
                return;
            }
            bodyBegin += kPriorityFieldByteCount;
            hasPriority = true;
            priority = m_currentPriority;
        }

        // 增量为 0 的 WINDOW_UPDATE 是规范禁止的（RFC 7540 §6.9）：放行会让对端以为窗口动了
        if (m_currentHeader.type == Http2FrameType::WindowUpdate &&
            (readBigEndian32(rawPayload.data()) & kHttp2MaximumStreamId) == 0)
        {
            recordFailure(Http2FrameErrorKind::ProtocolError,
                          std::format("WINDOW_UPDATE 的窗口增量不得为 0（RFC 7540 §6.9）：流号 {} 上的这一帧只能放宽窗口，"
                                      "请检查对端构造",
                                      m_currentHeader.streamId));
            return;
        }

        m_pendingFrame.header = m_currentHeader;
        if (bodyBegin == 0 && bodyEnd == rawPayload.size())
        {
            // 净负载就是整段负载（DATA、无 padding 的 HEADERS 以及其余类型）：整块按移动移交，省一次拷贝
            m_pendingFrame.payload = std::move(m_payloadBuffer);
            m_payloadBuffer.clear();
        }
        else
        {
            m_pendingFrame.payload.assign(rawPayload.data() + bodyBegin, bodyEnd - bodyBegin);
        }
        m_pendingFrame.hasPriority = hasPriority;
        m_pendingFrame.priority = priority;
        m_hasPendingFrame = true;

        clearFrameScratch();
        m_stage = Stage::FrameHeader;
    }

    void Http2FrameDecoder::recordFailure(const Http2FrameErrorKind errorKind, std::string reason)
    {
        m_hasError = true;
        m_errorKind = errorKind;
        // 前缀统一在这里补：调用点只写原因，文案风格不会因为某个分支漏写而不一致
        m_errorMessage = "HTTP/2 帧解码失败：" + std::move(reason);
        m_stage = Stage::Failed;
    }

    void Http2FrameDecoder::clearFrameScratch() noexcept
    {
        m_headerBytes = {};
        m_headerBytesSeen = 0;
        m_currentHeader = Http2FrameHeader{};
        m_payloadBytesSeen = 0;
        m_isCurrentFramePriority = false;
        m_currentPriority = Http2Priority{};
    }
} // namespace AsynGyanis::Net
