// TestHttp2Connection.cpp —— HTTP/2 连接层状态机（RFC 7540 §3.5/§4/§5/§6/§8.1）的单元测试
//
// 覆盖六块：
//   1) 前奏与 SETTINGS 协商：24 字节前奏的分段喂入与不匹配判错、初始 SETTINGS 的六个字段逐项对照、
//      首个帧必须是 SETTINGS、ACK 只能匹配一次、非法参数取值（§6.5.2）；
//   2) 头块拼接与请求语义：CONTINUATION 续帧、中间插帧判错、伪头齐全/顺序/重复/未知、连接特定头、
//      头名大写与非 token 字符、CONNECT 的例外规则（§8.1.2.1–§8.1.2.3、§8.3）；
//   3) 流状态与并发：奇数且严格递增的流号、并发上限回 REFUSED_STREAM、RST 与双向 END_STREAM 两种终止
//      各自的「忽略 / 判错」处置、GOAWAY 之后拒收新流（§5.1、§5.1.1、§5.1.2、§6.8）；
//   4) 响应发送：:status 排在最前（编码字节里对应静态表索引 8）、DATA 末片带 END_STREAM、
//      按对端 MAX_FRAME_SIZE 分片；
//   5) 发送方向流控：窗口不足不出帧、WINDOW_UPDATE 与 SETTINGS_INITIAL_WINDOW_SIZE 续发、
//      连接级与流级窗口取小、窗口溢出判 FLOW_CONTROL_ERROR（§5.2、§6.9）；
//   6) 契约面：失败入口不写字节、错误出参可操作、逐字节喂入与一次性喂入结果一致。
// 请求方向的头部字节有一部分直接取自规范：RFC 7541 C.3.1/C.4.1 的两个请求头块 dump 用作「黄金字节」，
// 其余请求头块由用例按静态表索引手工拼出（片段与取值都标了出处）。用例不起网络、不依赖外部服务。
//
// 逐项对照：帧类型与标志常量、静态表索引均取自 RFC 7540 §6 与 RFC 7541 Appendix A。

#include "Net/Http2/Http2Connection.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 由字节序列拼出二进制文本
         * @details 不能直接用字符串字面量：帧头与负载里常含 0x00，按 const char* 构造会被零终止截断，
         *          那样断言就测不到完整字节了。
         * @param byteValues 字节序列
         * @return std::string 逐字节写入的结果
         */
        std::string makeBytes(const std::initializer_list<unsigned char> byteValues)
        {
            std::string bytes;
            bytes.reserve(byteValues.size());
            for (const unsigned char byteValue: byteValues)
            {
                bytes.push_back(static_cast<char>(byteValue));
            }
            return bytes;
        }

        /**
         * @brief 把 32 位无符号数按大端写成 4 字节
         * @details WINDOW_UPDATE 的增量与 RST_STREAM 的错误码都是 4 字节大端，且常含 0x00：
         *          不能写成字符串字面量，按 const char* 构造会在第一个 NUL 处截断。
         * @param value 待写的数值
         * @return std::string 4 字节
         */
        std::string makeBigEndian32(const std::uint32_t value)
        {
            std::string bytes;
            for (int shiftBitCount = 24; shiftBitCount >= 0; shiftBitCount -= 8)
            {
                bytes.push_back(static_cast<char>((value >> shiftBitCount) & 0xFFU));
            }
            return bytes;
        }

        /**
         * @brief 手拼 9 字节帧头
         * @details 未定义帧类型（不属于 RFC 7540 §6）的帧只能这样构造：编码器按契约拒绝产出它们。
         * @param payloadLength 负载长度
         * @param typeValue 帧类型取值
         * @param flags 标志位
         * @param streamId 流号字段
         * @return std::string 9 字节帧头
         */
        std::string makeFrameHeaderBytes(const std::uint32_t payloadLength, const unsigned char typeValue,
                                         const unsigned char flags, const std::uint32_t streamId)
        {
            std::string header;
            header.push_back(static_cast<char>((payloadLength >> 16) & 0xFFU));
            header.push_back(static_cast<char>((payloadLength >> 8) & 0xFFU));
            header.push_back(static_cast<char>(payloadLength & 0xFFU));
            header.push_back(static_cast<char>(typeValue));
            header.push_back(static_cast<char>(flags));
            for (int shiftBitCount = 24; shiftBitCount >= 0; shiftBitCount -= 8)
            {
                header.push_back(static_cast<char>((streamId >> shiftBitCount) & 0xFFU));
            }
            return header;
        }

        /**
         * @brief 用编码器拼出一帧（帧头长度按负载实际大小落定）
         * @param type 帧类型
         * @param flags 标志位
         * @param streamId 流号
         * @param payload 负载字节
         * @return std::string 完整帧字节
         */
        std::string makeFrame(const Http2FrameType type, const unsigned char flags, const std::uint32_t streamId,
                              const std::string_view payload)
        {
            return encodeHttp2Frame(type, flags, streamId, payload);
        }

        /**
         * @brief 取一个具名 SETTINGS 参数
         * @param identifier 参数标识
         * @param value 参数取值
         * @return Http2Setting 参数项
         */
        Http2Setting namedSetting(const Http2SettingIdentifier identifier, const std::uint32_t value)
        {
            return Http2Setting{.identifier = static_cast<std::uint16_t>(identifier), .value = value};
        }

        /**
         * @brief 拼一个 SETTINGS 帧（不带 ACK）
         * @param parameters 参数列表，按给定顺序写出
         * @return std::string 完整帧字节
         */
        std::string makeSettingsFrame(const std::vector<Http2Setting> &parameters)
        {
            Http2SettingsPayload payload;
            payload.parameters = parameters;
            return encodeHttp2SettingsFrame(payload);
        }

        /**
         * @brief 拼一个空的 SETTINGS ACK 帧（§6.5 要求 ACK 负载为空）
         * @return std::string 完整帧字节
         */
        std::string makeSettingsAckFrame()
        {
            return encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true});
        }

        /**
         * @brief 编一个「索引表示」（RFC 7541 §6.1）
         * @param index 索引空间的索引（1..61 是静态表）
         * @return std::string 编码结果
         */
        std::string hpackIndexedField(const std::size_t index)
        {
            return encodeHpackInteger(index, 7, 0x80);
        }

        /**
         * @brief 编一个「带增量索引的字面量」，名字走静态表索引（RFC 7541 §6.2.1）
         * @param staticNameIndex 静态表里的名字索引
         * @param value 头值
         * @return std::string 编码结果
         */
        std::string hpackLiteralField(const std::size_t staticNameIndex, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(staticNameIndex, 6, 0x40);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 编一个「带增量索引的字面量」，名字与值都是字面量（RFC 7541 §6.2.1 的名字索引 0）
         * @param name 头名（原样写出，用例靠它构造非法名）
         * @param value 头值
         * @return std::string 编码结果
         */
        std::string hpackLiteralField(const std::string_view name, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(0, 6, 0x40);
            appendHpackString(bytes, name);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 拼一个合法的最小 GET 请求头块
         * @details 索引取自 RFC 7541 Appendix A：2 是 :method: GET、6 是 :scheme: http、4 是 :path: /、
         *          1 是只带名字的 :authority（值只能由字面量给出）。
         * @return std::string 头块字节
         */
        std::string makeMinimalGetRequestBlock()
        {
            std::string headerBlock;
            headerBlock += hpackIndexedField(2);
            headerBlock += hpackIndexedField(6);
            headerBlock += hpackIndexedField(4);
            headerBlock += hpackLiteralField(1, "example.com");
            return headerBlock;
        }

        /**
         * @brief 拼一个不带 END_STREAM 的 POST 请求头块（用于造出还在等正文的流）
         * @return std::string 头块字节
         */
        std::string makePostRequestBlock()
        {
            std::string headerBlock;
            headerBlock += hpackLiteralField(2, "POST");
            headerBlock += hpackIndexedField(6);
            headerBlock += hpackLiteralField(4, "/upload");
            headerBlock += hpackLiteralField(1, "example.com");
            return headerBlock;
        }

        /**
         * @brief RFC 7541 C.3.1「First Request」的头块字节（未启用 Huffman）
         * @details 规范原文的 hex dump 是 8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d，
         *          解出的头列表是 :method: GET、:scheme: http、:path: /、:authority: www.example.com。
         * @return std::string 头块字节
         */
        std::string makeRfc7541FirstRequestBlock()
        {
            return makeBytes({0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65, 0x78,
                              0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d});
        }

        /**
         * @brief RFC 7541 C.4.1「First Request」的头块字节（:authority 的值用 Huffman 编码）
         * @details 规范原文的 hex dump 是 8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff：头列表与 C.3.1 相同，
         *          只有值 "www.example.com" 换成了 12 字节 Huffman 码。
         * @return std::string 头块字节
         */
        std::string makeRfc7541HuffmanFirstRequestBlock()
        {
            return makeBytes({0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff});
        }

        /**
         * @brief 把一段字节喂给连接状态机
         * @param connection 连接状态机
         * @param bytes 本次喂入的字节
         * @return Http2ConnectionFeedStatus 本次结论
         */
        Http2ConnectionFeedStatus feed(Http2Connection &connection, const std::string_view bytes)
        {
            return connection.feedBytes(bytes.data(), bytes.size());
        }

        /**
         * @brief 完成前奏与首个 SETTINGS，并取走服务端吐出的初始 SETTINGS 与 ACK
         * @details 握手是所有后续用例的前置。本函数只做「喂进去、取出来」并钉住「两个控制帧」，
         *          逐字段对照见 EmitsInitialSettingsAndAcknowledgementAfterThePreface。
         * @param connection 连接状态机
         * @param peerSettings 对端首个 SETTINGS 带的参数
         */
        void completeHandshake(Http2Connection &connection, const std::vector<Http2Setting> &peerSettings = {})
        {
            const std::string inputBytes = std::string(kHttp2ConnectionPreface) + makeSettingsFrame(peerSettings);
            EXPECT_EQ(feed(connection, inputBytes), Http2ConnectionFeedStatus::NeedMore);
            EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
            EXPECT_EQ(connection.state(), Http2ConnectionState::Open) << connection.errorMessage();
            static_cast<void>(connection.takeOutgoingBytes());
        }

        /**
         * @brief 用帧解码器把一整串字节解成帧序列
         * @details 复用帧层解码器：服务端吐出的字节必须能被上一片的解码器原样解回来，这本身就是一条对照。
         * @param bytes 完整帧字节序列
         * @return std::vector<Http2Frame> 按顺序解出的帧
         */
        std::vector<Http2Frame> parseFrames(const std::string_view bytes)
        {
            Http2FrameDecoder decoder;
            std::vector<Http2Frame> frames;
            std::size_t offsetByteCount = 0;
            while (offsetByteCount < bytes.size())
            {
                const Http2FrameDecodeStatus status = decoder.parse(bytes.data() + offsetByteCount, bytes.size() - offsetByteCount);
                EXPECT_EQ(status, Http2FrameDecodeStatus::Frame) << "服务端吐出的字节解不出帧：" << decoder.errorMessage();
                if (status != Http2FrameDecodeStatus::Frame)
                {
                    break;
                }
                offsetByteCount += decoder.consumedByteCount();
                frames.push_back(decoder.takeFrame());
            }
            return frames;
        }

        /**
         * @brief 用一个新解码器解一段响应头块
         * @details 本端的 HpackEncoder 与对端解码器成对演进，用例里的「对端」就是这里的解码器实例。
         * @param headerBlock 头块字节
         * @return std::vector<HpackHeaderField> 解出的头列表
         */
        std::vector<HpackHeaderField> decodeResponseHeaderBlock(const std::string_view headerBlock)
        {
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            EXPECT_TRUE(decoder.decode(headerBlock, headerFields)) << "响应头块解不开：" << decoder.errorMessage();
            return headerFields;
        }

        /**
         * @brief 在解出的头列表里找一条头的值
         * @param headerFields 头列表
         * @param name 头名
         * @return std::string 头值；没有该头时为空串
         */
        std::string findHeaderValue(const std::vector<HpackHeaderField> &headerFields, const std::string_view name)
        {
            for (const HpackHeaderField &field: headerFields)
            {
                if (field.name == name)
                {
                    return field.value;
                }
            }
            return {};
        }

        /**
         * @brief 取一段字节里出现的全部 RST_STREAM 帧
         * @param connection 连接状态机
         * @return std::vector<Http2Frame> RST_STREAM 帧列表
         */
        std::vector<Http2Frame> takeRstStreamFrames(Http2Connection &connection)
        {
            std::vector<Http2Frame> resetFrames;
            for (Http2Frame &frame: parseFrames(connection.takeOutgoingBytes()))
            {
                if (frame.header.type == Http2FrameType::RstStream)
                {
                    resetFrames.push_back(std::move(frame));
                }
            }
            return resetFrames;
        }

        /**
         * @brief 断言一段字节以 GOAWAY 收尾，并交出其中的错误码
         * @param bytes 服务端吐出的字节
         * @return Http2ErrorCode GOAWAY 里带的错误码；没有 GOAWAY 时用例已被记为失败
         */
        Http2ErrorCode takeGoAwayErrorCode(const std::string_view bytes)
        {
            const std::vector<Http2Frame> frames = parseFrames(bytes);
            EXPECT_FALSE(frames.empty()) << "连接级失败必须让对端看见（至少一个 GOAWAY）";
            if (frames.empty())
            {
                return Http2ErrorCode::NoError;
            }
            EXPECT_EQ(frames.back().header.type, Http2FrameType::GoAway) << "最后一个控制帧应当是 GOAWAY";
            Http2GoAwayPayload payload;
            std::string errorText;
            EXPECT_TRUE(parseHttp2GoAwayPayload(frames.back(), payload, &errorText)) << errorText;
            return payload.errorCode;
        }

        /**
         * @brief 断言一次请求被流错误拒绝：连接还在、该流被 RST_STREAM、且没有交出请求
         * @param connection 连接状态机
         * @param streamId 被拒绝的流号
         * @return std::string 流级错误的中文原因
         */
        std::string expectStreamRejected(Http2Connection &connection, const std::uint32_t streamId)
        {
            EXPECT_FALSE(connection.hasFailed()) << "请求头不合规是流错误，连接应当继续：" << connection.errorMessage();
            EXPECT_EQ(connection.state(), Http2ConnectionState::Open);
            const std::vector<Http2Frame> resetFrames = takeRstStreamFrames(connection);
            EXPECT_EQ(resetFrames.size(), 1U) << "被拒绝的流应当收到一个 RST_STREAM";
            if (!resetFrames.empty())
            {
                EXPECT_EQ(resetFrames.front().header.streamId, streamId);
                Http2RstStreamPayload payload;
                std::string errorText;
                EXPECT_TRUE(parseHttp2RstStreamPayload(resetFrames.front(), payload, &errorText)) << errorText;
                EXPECT_EQ(payload.errorCode, Http2ErrorCode::ProtocolError);
            }
            Http2StreamState streamState{};
            EXPECT_TRUE(connection.tryGetStreamState(streamId, streamState));
            EXPECT_EQ(streamState, Http2StreamState::Closed);
            EXPECT_FALSE(connection.lastStreamErrorMessage().empty()) << "流级错误必须留下可排查的中文原因";
            return connection.lastStreamErrorMessage();
        }
    } // namespace

    /**
     * @brief 钉住：前奏收齐后本端发出的初始 SETTINGS 逐字段等于配置默认值，对端 SETTINGS 被回 ACK
     */
    TEST(Http2Connection, EmitsInitialSettingsAndAcknowledgementAfterThePreface)
    {
        Http2Connection connection;
        const std::string inputBytes = std::string(kHttp2ConnectionPreface) +
                                       makeSettingsFrame({namedSetting(Http2SettingIdentifier::HeaderTableSize, 8192)});
        EXPECT_EQ(feed(connection, inputBytes), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_EQ(connection.state(), Http2ConnectionState::Open);
        EXPECT_EQ(connection.openStreamCount(), 0U);

        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 2U) << "前奏之后本端应当只发初始 SETTINGS 与 ACK";
        EXPECT_EQ(frames[0].header.type, Http2FrameType::Settings);
        EXPECT_EQ(frames[0].header.flags, 0U);
        EXPECT_EQ(frames[0].header.streamId, 0U);

        Http2SettingsPayload initialSettings;
        std::string errorText;
        ASSERT_TRUE(parseHttp2SettingsPayload(frames[0], initialSettings, &errorText)) << errorText;
        // 六项本端参数按 §6.5.2 的参数标识顺序逐项对照，取值来源是 Http2ConnectionConfiguration 的默认值
        const std::vector<std::pair<Http2SettingIdentifier, std::uint32_t>> expectedParameters = {
            {Http2SettingIdentifier::HeaderTableSize, static_cast<std::uint32_t>(kHpackDefaultDynamicTableSizeByteCount)},
            {Http2SettingIdentifier::EnablePush, 0U},
            {Http2SettingIdentifier::MaxConcurrentStreams, 100U},
            {Http2SettingIdentifier::InitialWindowSize, kHttp2InitialWindowSizeByteCount},
            {Http2SettingIdentifier::MaxFrameSize, kHttp2DefaultMaximumFrameSize},
            {Http2SettingIdentifier::MaxHeaderListSize, 16U * 1024U}};
        ASSERT_EQ(initialSettings.parameters.size(), expectedParameters.size());
        for (std::size_t index = 0; index < expectedParameters.size(); ++index)
        {
            EXPECT_EQ(initialSettings.parameters[index].identifier, static_cast<std::uint16_t>(expectedParameters[index].first))
                << "初始 SETTINGS 第 " << index << " 项的标识不符";
            EXPECT_EQ(initialSettings.parameters[index].value, expectedParameters[index].second)
                << "初始 SETTINGS 第 " << index << " 项的取值不符";
        }

        // 对端 SETTINGS 的应答：ACK 置位、负载为空、流号为 0（§6.5.3）
        EXPECT_EQ(frames[1].header.type, Http2FrameType::Settings);
        EXPECT_EQ(frames[1].header.flags, kHttp2FlagAcknowledge);
        EXPECT_EQ(frames[1].header.streamId, 0U);
        EXPECT_TRUE(frames[1].payload.empty()) << "ACK 的负载必须为空（§6.5）";

        // 对端参数记账：HEADER_TABLE_SIZE 被记下来（它决定本端编码器动态表的上限）
        std::uint32_t peerSettingValue = 0;
        ASSERT_TRUE(connection.tryGetPeerSetting(Http2SettingIdentifier::HeaderTableSize, peerSettingValue));
        EXPECT_EQ(peerSettingValue, 8192U);
        EXPECT_FALSE(connection.tryGetPeerSetting(Http2SettingIdentifier::MaxFrameSize, peerSettingValue));
    }

    /**
     * @brief 钉住：前奏不满 24 字节时状态机只等待，一个字节都不发出
     */
    TEST(Http2Connection, WaitsForTheCompletePrefaceBeforeReplying)
    {
        Http2Connection connection;
        const std::string preface(kHttp2ConnectionPreface);
        EXPECT_EQ(feed(connection, preface.substr(0, 23)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(connection.state(), Http2ConnectionState::AwaitingPreface);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "前奏没收齐之前不得发出任何字节";

        EXPECT_EQ(feed(connection, preface.substr(23)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(connection.state(), Http2ConnectionState::AwaitingSettings);
        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U);
        EXPECT_EQ(frames[0].header.type, Http2FrameType::Settings);
    }

    /**
     * @brief 钉住：前奏字节不匹配即判 PROTOCOL_ERROR，并以带同一错误码的 GOAWAY 收场
     */
    TEST(Http2Connection, RejectsPrefaceThatDivergesFromTheFixedBytes)
    {
        Http2Connection connection;
        EXPECT_EQ(feed(connection, "GET / HTTP/1.1\r\nHost: example.com\r\n"), Http2ConnectionFeedStatus::Failed);
        EXPECT_TRUE(connection.hasFailed());
        EXPECT_EQ(connection.state(), Http2ConnectionState::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(connection.errorMessage().find("前奏"), std::string::npos) << connection.errorMessage();
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::ProtocolError);

        // 失败态粘滞：随后的合法字节不再被解释，也不会把连接救回来
        EXPECT_EQ(feed(connection, std::string(kHttp2ConnectionPreface)), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
    }

    /**
     * @brief 钉住：前奏之后的第一个帧必须是 SETTINGS（§3.5），其它类型判 PROTOCOL_ERROR
     */
    TEST(Http2Connection, RejectsFirstFrameThatIsNotSettings)
    {
        Http2Connection connection;
        const std::string inputBytes = std::string(kHttp2ConnectionPreface) +
                                       makeFrame(Http2FrameType::Ping, 0, 0, std::string(8, 'P'));
        EXPECT_EQ(feed(connection, inputBytes), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(connection.errorMessage().find("第一个帧"), std::string::npos) << connection.errorMessage();
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::ProtocolError);

        // SETTINGS ACK 同样不算「客户端自己的 SETTINGS」：前奏里必须是它自己的那份参数
        Http2Connection ackFirst;
        EXPECT_EQ(feed(ackFirst, std::string(kHttp2ConnectionPreface) + makeSettingsAckFrame()), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(ackFirst.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(ackFirst.errorMessage().find("SETTINGS"), std::string::npos) << ackFirst.errorMessage();
    }

    /**
     * @brief 钉住：对端的 SETTINGS ACK 只能匹配一次，多余的 ACK 判 PROTOCOL_ERROR
     */
    TEST(Http2Connection, RejectsSettingsAcknowledgementWithoutOutstandingSettings)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // 首个 ACK 匹配本端刚发的初始 SETTINGS（§6.5.3）
        EXPECT_EQ(feed(connection, makeSettingsAckFrame()), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "ACK 的应答应当是静默的";

        // 已经没有待确认的 SETTINGS：再来一个 ACK 就是多余的
        EXPECT_EQ(feed(connection, makeSettingsAckFrame()), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(connection.errorMessage().find("ACK"), std::string::npos) << connection.errorMessage();
    }

    /**
     * @brief 钉住：SETTINGS 的非法取值按 §6.5.2 各自判错（ENABLE_PUSH/INITIAL_WINDOW_SIZE/MAX_FRAME_SIZE）
     */
    TEST(Http2Connection, RejectsIllegalSettingValues)
    {
        struct IllegalSettingSample
        {
            Http2Setting setting;              ///< 对端发来的非法参数
            Http2ErrorCode expectedErrorCode;  ///< 期望的连接错误码
            std::string_view expectedText;     ///< 错误文案里必须出现的关键词
        };

        const std::vector<IllegalSettingSample> samples = {
            {namedSetting(Http2SettingIdentifier::EnablePush, 2U), Http2ErrorCode::ProtocolError, "ENABLE_PUSH"},
            {namedSetting(Http2SettingIdentifier::EnablePush, 0xFFFFFFFFU), Http2ErrorCode::ProtocolError, "ENABLE_PUSH"},
            {namedSetting(Http2SettingIdentifier::InitialWindowSize, 0x80000000U), Http2ErrorCode::FlowControlError, "INITIAL_WINDOW_SIZE"},
            {namedSetting(Http2SettingIdentifier::MaxFrameSize, 16383U), Http2ErrorCode::ProtocolError, "MAX_FRAME_SIZE"},
            {namedSetting(Http2SettingIdentifier::MaxFrameSize, 16777216U), Http2ErrorCode::ProtocolError, "MAX_FRAME_SIZE"}};

        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            Http2Connection connection;
            const std::string inputBytes = std::string(kHttp2ConnectionPreface) + makeSettingsFrame({samples[index].setting});
            EXPECT_EQ(feed(connection, inputBytes), Http2ConnectionFeedStatus::Failed) << "第 " << index << " 个样本本应判错";
            EXPECT_EQ(connection.errorCode(), samples[index].expectedErrorCode) << "第 " << index << " 个样本的错误码不符";
            EXPECT_NE(connection.errorMessage().find(samples[index].expectedText), std::string::npos)
                << "第 " << index << " 个样本的文案：" << connection.errorMessage();
            EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), samples[index].expectedErrorCode)
                << "第 " << index << " 个样本的 GOAWAY 错误码与 errorCode() 不一致";
        }
    }

    /**
     * @brief 钉住：未知的 SETTINGS 参数标识按 §6.5.2 忽略，但要记进账本，且不影响 ACK
     */
    TEST(Http2Connection, IgnoresUnknownSettingIdentifiers)
    {
        Http2Connection connection;
        const std::string inputBytes = std::string(kHttp2ConnectionPreface) +
                                       makeSettingsFrame({Http2Setting{.identifier = 0x99U, .value = 7U},
                                                          namedSetting(Http2SettingIdentifier::MaxFrameSize, 32768U)});
        EXPECT_EQ(feed(connection, inputBytes), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 2U) << "仍然只发初始 SETTINGS 与 ACK";
        EXPECT_EQ(frames[1].header.flags, kHttp2FlagAcknowledge);

        std::uint32_t peerSettingValue = 0;
        // 未定义标识也要记账：将来排查「对端用了哪些扩展」时只有账本这一处可看
        ASSERT_TRUE(connection.tryGetPeerSetting(static_cast<Http2SettingIdentifier>(0x99U), peerSettingValue));
        EXPECT_EQ(peerSettingValue, 7U);
        ASSERT_TRUE(connection.tryGetPeerSetting(Http2SettingIdentifier::MaxFrameSize, peerSettingValue));
        EXPECT_EQ(peerSettingValue, 32768U);
    }

    /**
     * @brief 钉住：RFC 7541 C.3.1 的请求头块（黄金字节）能解出四个伪头，END_STREAM 落在 HEADERS 上
     */
    TEST(Http2Connection, DeliversRequestFromRfc7541NonHuffmanSample)
    {
        Http2Connection connection;
        completeHandshake(connection);

        const std::string headersFrame = makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                                   makeRfc7541FirstRequestBlock());
        EXPECT_EQ(feed(connection, headersFrame), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U);
        EXPECT_EQ(requests[0].streamId, 1U);
        EXPECT_EQ(requests[0].method, "GET");
        EXPECT_EQ(requests[0].scheme, "http");
        EXPECT_EQ(requests[0].path, "/");
        EXPECT_EQ(requests[0].authority, "www.example.com");
        EXPECT_FALSE(requests[0].hasBody) << "END_STREAM 在 HEADERS 上：这是一个没有正文的请求";
        EXPECT_TRUE(requests[0].headerFields.empty()) << "这份样本里没有普通头部";
        EXPECT_EQ(connection.openStreamCount(), 1U);

        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::HalfClosedRemote);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "合规请求不该引出任何控制帧";
    }

    /**
     * @brief 钉住：RFC 7541 C.4.1 的 Huffman 版请求头块（黄金字节）解出同一个 :authority
     */
    TEST(Http2Connection, DeliversRequestFromRfc7541HuffmanSample)
    {
        Http2Connection connection;
        completeHandshake(connection);

        const std::string headersFrame = makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                                   makeRfc7541HuffmanFirstRequestBlock());
        EXPECT_EQ(feed(connection, headersFrame), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U);
        EXPECT_EQ(requests[0].method, "GET");
        EXPECT_EQ(requests[0].path, "/");
        EXPECT_EQ(requests[0].authority, "www.example.com") << "Huffman 编码的 :authority 应当被解回原文";
    }

    /**
     * @brief 钉住：一个头块可以跨 HEADERS 与多个 CONTINUATION，END_HEADERS 之前不交出请求
     */
    TEST(Http2Connection, JoinsHeaderBlockFragmentsAcrossContinuationFrames)
    {
        Http2Connection connection;
        completeHandshake(connection);

        const std::string headerBlock = makeMinimalGetRequestBlock();
        ASSERT_GT(headerBlock.size(), 4U);
        const std::size_t firstLength = headerBlock.size() / 3U;
        const std::size_t secondLength = headerBlock.size() / 3U;
        const std::string firstPart = headerBlock.substr(0, firstLength);
        const std::string secondPart = headerBlock.substr(firstLength, secondLength);
        const std::string thirdPart = headerBlock.substr(firstLength + secondLength);

        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream, 1U, firstPart)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeRequests().empty()) << "END_HEADERS 之前不得交出请求";
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Continuation, 0, 1U, secondPart)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeRequests().empty());
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Continuation, kHttp2FlagEndHeaders, 1U, thirdPart)), Http2ConnectionFeedStatus::NeedMore);

        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U);
        EXPECT_EQ(requests[0].path, "/");
        EXPECT_EQ(requests[0].authority, "example.com");
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
    }

    /**
     * @brief 钉住：头块未收完时插入任何其它帧都判 PROTOCOL_ERROR（§6.10）
     */
    TEST(Http2Connection, RejectsFrameInterleavedInsideHeaderBlock)
    {
        Http2Connection connection;
        completeHandshake(connection);
        const std::string headerBlock = makeMinimalGetRequestBlock();
        ASSERT_GT(headerBlock.size(), 2U);

        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, 0, 1U, headerBlock.substr(0, 1U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Ping, 0, 0, std::string(8, 'Q'))), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(connection.errorMessage().find("CONTINUATION"), std::string::npos) << connection.errorMessage();
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::ProtocolError);

        // 没有前置 HEADERS 的 CONTINUATION 同样判错（§6.10）
        Http2Connection orphan;
        completeHandshake(orphan);
        EXPECT_EQ(feed(orphan, makeFrame(Http2FrameType::Continuation, kHttp2FlagEndHeaders, 1U, "ab")), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(orphan.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(orphan.errorMessage().find("前置"), std::string::npos) << orphan.errorMessage();
    }

    /**
     * @brief 钉住：伪头与头名的各类不合规都按流错误 PROTOCOL_ERROR 拒绝，连接继续服务其它流
     */
    TEST(Http2Connection, RejectsMalformedRequestHeaderFields)
    {
        struct MalformedRequestSample
        {
            std::string description;       ///< 样本说明（诊断输出用）
            std::string headerBlock;       ///< 请求头块字节
            std::string_view expectedText; ///< 错误文案里必须出现的关键词
        };

        const std::string validPseudoFields = hpackIndexedField(2) + hpackIndexedField(6) + hpackIndexedField(4);
        std::vector<MalformedRequestSample> samples;
        samples.push_back({"缺少 :path", hpackIndexedField(2) + hpackIndexedField(6) + hpackLiteralField(1, "example.com"), "缺少 :path"});
        samples.push_back({"缺少 :scheme", hpackIndexedField(2) + hpackIndexedField(4) + hpackLiteralField(1, "example.com"), "缺少 :scheme"});
        samples.push_back({"缺少 :method", hpackIndexedField(6) + hpackIndexedField(4) + hpackLiteralField(1, "example.com"), "缺少 :method"});
        samples.push_back({":method 重复", hpackIndexedField(2) + hpackLiteralField(2, "POST") + hpackIndexedField(6) + hpackIndexedField(4), "出现了两次"});
        samples.push_back({"未知伪头", validPseudoFields + hpackLiteralField(":foo", "1"), "未知伪头"});
        samples.push_back({"伪头排在普通头部之后", hpackLiteralField("x-test", "1") + validPseudoFields, "普通头部之后"});
        samples.push_back({"头名含大写字母", validPseudoFields + hpackLiteralField("X-Test", "1"), "含大写字母"});
        samples.push_back({"头名含非 token 字符", validPseudoFields + hpackLiteralField("x test", "1"), "token"});
        samples.push_back({"头值含 CR", validPseudoFields + hpackLiteralField("x-test", "a\rb"), "控制字符"});
        samples.push_back({"连接特定头 connection", validPseudoFields + hpackLiteralField("connection", "keep-alive"), "连接特定头"});
        samples.push_back({"连接特定头 keep-alive", validPseudoFields + hpackLiteralField("keep-alive", "timeout=5"), "连接特定头"});
        samples.push_back({"连接特定头 proxy-connection", validPseudoFields + hpackLiteralField("proxy-connection", "keep-alive"), "连接特定头"});
        samples.push_back({"连接特定头 transfer-encoding", validPseudoFields + hpackLiteralField(57, "chunked"), "连接特定头"});
        samples.push_back({"连接特定头 upgrade", validPseudoFields + hpackLiteralField("upgrade", "h2c"), "连接特定头"});
        samples.push_back({"te 取值不是 trailers", validPseudoFields + hpackLiteralField("te", "gzip"), "te"});
        samples.push_back({":path 为空", hpackIndexedField(2) + hpackIndexedField(6) + hpackLiteralField(4, ""), ":path"});

        for (const MalformedRequestSample &sample: samples)
        {
            Http2Connection connection;
            completeHandshake(connection);
            EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, sample.headerBlock)),
                      Http2ConnectionFeedStatus::NeedMore)
                << "样本「" << sample.description << "」不该判连接错误";
            EXPECT_TRUE(connection.takeRequests().empty()) << "样本「" << sample.description << "」不该交出请求";
            const std::string streamErrorMessage = expectStreamRejected(connection, 1U);
            EXPECT_NE(streamErrorMessage.find(sample.expectedText), std::string::npos)
                << "样本「" << sample.description << "」的流级原因：" << streamErrorMessage;

            // 连接必须还能服务其它流：换一个流号再发一个合法请求
            EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3U,
                                                makeMinimalGetRequestBlock())),
                      Http2ConnectionFeedStatus::NeedMore);
            const std::vector<Http2Request> requests = connection.takeRequests();
            ASSERT_EQ(requests.size(), 1U) << "样本「" << sample.description << "」之后其它流仍应当被服务";
            EXPECT_EQ(requests[0].streamId, 3U);
        }
    }

    /**
     * @brief 钉住：CONNECT 的例外规则（§8.3）——:scheme/:path 必须缺席、:authority 必须存在
     */
    TEST(Http2Connection, AcceptsConnectWithoutSchemeAndPath)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // 合法的 CONNECT：只有 :method 与 :authority
        std::string connectBlock = hpackLiteralField(2, "CONNECT") + hpackLiteralField(1, "example.com:443");
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, connectBlock)),
                  Http2ConnectionFeedStatus::NeedMore);
        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U);
        EXPECT_EQ(requests[0].method, "CONNECT");
        EXPECT_EQ(requests[0].authority, "example.com:443");
        EXPECT_TRUE(requests[0].scheme.empty()) << "CONNECT 请求没有 :scheme";
        EXPECT_TRUE(requests[0].path.empty()) << "CONNECT 请求没有 :path";

        // 带了 :path 的 CONNECT 不合规
        std::string illegalConnectBlock = hpackLiteralField(2, "CONNECT") + hpackLiteralField(1, "example.com:443") + hpackIndexedField(4);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3U, illegalConnectBlock)),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeRequests().empty());
        EXPECT_NE(expectStreamRejected(connection, 3U).find("CONNECT"), std::string::npos);

        // 缺 :authority 的 CONNECT 同样不合规
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 5U,
                                            hpackLiteralField(2, "CONNECT"))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeRequests().empty());
        EXPECT_NE(expectStreamRejected(connection, 5U).find("CONNECT"), std::string::npos);
    }

    /**
     * @brief 钉住：头列表超出本端通告的 SETTINGS_MAX_HEADER_LIST_SIZE 时按 ENHANCE_YOUR_CALM 收场
     * @details 依据：§7 把 ENHANCE_YOUR_CALM 列为「对端可能造成过量负载」的建议取值，而 HpackDecoder 在
     *          超限后处于粘滞错误态、动态表已与对端编码器不同步，因此只能作为连接级故障终止。
     */
    TEST(Http2Connection, RejectsHeaderListBeyondTheAdvertisedLimit)
    {
        Http2ConnectionConfiguration configuration;
        configuration.maximumHeaderListSize = 64U; // 只够放一两条头（§6.5.2 算式：名长 + 值长 + 32）
        Http2Connection connection(configuration);
        completeHandshake(connection);

        // 一条 100 字节的头值就足以撑爆 64 字节的预算
        const std::string headerBlock = makeMinimalGetRequestBlock() + hpackLiteralField("x-big", std::string(100, 'a'));
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, headerBlock)),
                  Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::EnhanceYourCalm);
        EXPECT_NE(connection.errorMessage().find("头块解码失败"), std::string::npos) << connection.errorMessage();
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::EnhanceYourCalm);

        // 上限是配置项：同一个请求块在默认配置下必须通过
        Http2Connection defaults;
        completeHandshake(defaults);
        EXPECT_EQ(feed(defaults, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, headerBlock)),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(defaults.hasFailed()) << defaults.errorMessage();
        EXPECT_EQ(defaults.takeRequests().size(), 1U);
    }

    /**
     * @brief 钉住：单个头块（HEADERS 与 CONTINUATION 之和）超过本端字节上限即判 ENHANCE_YOUR_CALM
     */
    TEST(Http2Connection, RejectsHeaderBlockOverTheLocalAssemblyLimit)
    {
        Http2ConnectionConfiguration configuration;
        configuration.maximumHeaderBlockByteCount = 8U;
        Http2Connection connection(configuration);
        completeHandshake(connection);

        const std::string headerBlock = makeMinimalGetRequestBlock();
        ASSERT_GT(headerBlock.size(), 8U);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream, 1U, headerBlock.substr(0, 8U))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Continuation, kHttp2FlagEndHeaders, 1U, headerBlock.substr(8U))),
                  Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::EnhanceYourCalm);
        EXPECT_NE(connection.errorMessage().find("头块压缩后"), std::string::npos) << connection.errorMessage();
    }

    /**
     * @brief 钉住：正文按 DATA 帧逐片交出，末片的 END_STREAM 把对端方向半关
     */
    TEST(Http2Connection, DeliversReceivedBodyDataAsSeparateEvents)
    {
        Http2Connection connection;
        completeHandshake(connection);

        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U);
        EXPECT_EQ(requests[0].method, "POST");
        EXPECT_EQ(requests[0].path, "/upload");
        EXPECT_TRUE(requests[0].hasBody) << "HEADERS 没带 END_STREAM：正文随后会到";

        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, "abc")), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1U, "def")), Http2ConnectionFeedStatus::NeedMore);
        const std::vector<Http2ReceivedData> bodyParts = connection.takeReceivedData();
        ASSERT_EQ(bodyParts.size(), 2U);
        EXPECT_EQ(bodyParts[0].streamId, 1U);
        EXPECT_EQ(bodyParts[0].data, "abc");
        EXPECT_FALSE(bodyParts[0].endStream);
        EXPECT_EQ(bodyParts[1].data, "def");
        EXPECT_TRUE(bodyParts[1].endStream);

        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::HalfClosedRemote);

        // 空 DATA 帧也要交出（它可能只是为了带 END_STREAM）
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1U, "")), Http2ConnectionFeedStatus::NeedMore);
        const std::vector<Http2Frame> resetFrames = takeRstStreamFrames(connection);
        EXPECT_EQ(resetFrames.size(), 1U) << "对端已经在流 1 上 END_STREAM，再发 DATA 是流错误 STREAM_CLOSED";
        Http2RstStreamPayload payload;
        std::string errorText;
        ASSERT_TRUE(parseHttp2RstStreamPayload(resetFrames.front(), payload, &errorText)) << errorText;
        EXPECT_EQ(payload.errorCode, Http2ErrorCode::StreamClosed);
        EXPECT_FALSE(connection.hasFailed()) << "流错误不该终止连接：" << connection.errorMessage();
    }

    /**
     * @brief 钉住：响应头与两片正文的出帧顺序、:status 的编码字节、末片 DATA 带 END_STREAM
     */
    TEST(Http2Connection, SendsResponseHeadersAndDataFramesInOrder)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeRfc7541FirstRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        ASSERT_EQ(connection.takeRequests().size(), 1U);

        std::string errorText;
        ASSERT_TRUE(connection.sendResponseHeaders(1U, 200U, {{"content-type", "text/plain"}}, false, &errorText)) << errorText;
        ASSERT_TRUE(connection.sendResponseData(1U, "Hello, ", false, &errorText)) << errorText;
        ASSERT_TRUE(connection.sendResponseData(1U, "world!", true, &errorText)) << errorText;

        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 3U) << "响应头 + 两片正文";
        EXPECT_EQ(frames[0].header.type, Http2FrameType::Headers);
        EXPECT_EQ(frames[0].header.streamId, 1U);
        EXPECT_EQ(frames[0].header.flags, kHttp2FlagEndHeaders) << "响应有正文，END_STREAM 不该落在 HEADERS 上";
        // :status: 200 命中静态表索引 8（RFC 7541 Appendix A），编码成单字节 0x88
        ASSERT_FALSE(frames[0].payload.empty());
        EXPECT_EQ(frames[0].payload.front(), static_cast<char>(0x88));
        const std::vector<HpackHeaderField> responseFields = decodeResponseHeaderBlock(frames[0].payload);
        ASSERT_GE(responseFields.size(), 2U);
        EXPECT_EQ(responseFields[0].name, ":status") << "伪头必须排在最前（§8.1.2.1）";
        EXPECT_EQ(responseFields[0].value, "200");
        EXPECT_EQ(findHeaderValue(responseFields, "content-type"), "text/plain");

        EXPECT_EQ(frames[1].header.type, Http2FrameType::Data);
        EXPECT_EQ(frames[1].payload, "Hello, ");
        EXPECT_EQ(frames[1].header.flags, 0U);
        EXPECT_EQ(frames[2].header.type, Http2FrameType::Data);
        EXPECT_EQ(frames[2].payload, "world!");
        EXPECT_EQ(frames[2].header.flags, kHttp2FlagEndStream) << "末片 DATA 必须带 END_STREAM";

        // 请求与响应都 END_STREAM：这条流整条终止
        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::Closed);
        EXPECT_EQ(connection.openStreamCount(), 0U);
    }

    /**
     * @brief 钉住：正文按对端通告的 SETTINGS_MAX_FRAME_SIZE 分片
     */
    TEST(Http2Connection, SplitsResponseDataByPeerMaximumFrameSize)
    {
        Http2Connection connection;
        completeHandshake(connection, {namedSetting(Http2SettingIdentifier::MaxFrameSize, kHttp2DefaultMaximumFrameSize)});
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 24000 字节 > 16384：必须切成两帧，末片带 END_STREAM
        const std::string body(24000U, 'x');
        std::string errorText;
        ASSERT_TRUE(connection.sendResponseData(1U, body, true, &errorText)) << errorText;
        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 2U);
        EXPECT_EQ(frames[0].payload.size(), static_cast<std::size_t>(kHttp2DefaultMaximumFrameSize));
        EXPECT_EQ(frames[1].payload.size(), body.size() - static_cast<std::size_t>(kHttp2DefaultMaximumFrameSize));
        EXPECT_EQ(frames[1].header.flags, kHttp2FlagEndStream);
        EXPECT_EQ(frames[0].payload + frames[1].payload, body) << "分片必须无损还原";
    }

    /**
     * @brief 钉住：响应头块超过对端 MAX_FRAME_SIZE 时拆成 HEADERS + CONTINUATION（§4.3、§6.10）
     */
    TEST(Http2Connection, SplitsResponseHeaderBlockAcrossContinuationFrames)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 一条 20000 字节的响应头值：编码后必然超过 16384 的单帧上限
        const std::string bigValue(20000U, 'v');
        std::string errorText;
        ASSERT_TRUE(connection.sendResponseHeaders(1U, 200U, {{"x-big", bigValue}}, false, &errorText)) << errorText;
        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 2U) << "头块放不进一帧时必须续 CONTINUATION";
        EXPECT_EQ(frames[0].header.type, Http2FrameType::Headers);
        EXPECT_EQ(frames[0].header.streamId, 1U);
        EXPECT_EQ(frames[0].header.flags, 0U) << "END_HEADERS 不在第一帧上";
        EXPECT_EQ(frames[0].payload.size(), static_cast<std::size_t>(kHttp2DefaultMaximumFrameSize));
        EXPECT_EQ(frames[1].header.type, Http2FrameType::Continuation);
        EXPECT_EQ(frames[1].header.flags, kHttp2FlagEndHeaders);
        EXPECT_EQ(frames[1].header.streamId, 1U);

        // 拼接回来的头块必须原样解出 :status 与那条大头部。对端的解码器按它自己的字段长度上限建：
        // 本用例要验的是「头块跨帧拼接」，因此把上限放大到装得下这 20000 字节的值。
        HpackDecoder peerDecoder(HpackDecoderLimits{.maximumDynamicTableSizeByteCount = kHpackDefaultDynamicTableSizeByteCount,
                                                    .maximumHeaderListByteCount = 64U * 1024U,
                                                    .maximumHeaderFieldNameLength = 256U,
                                                    .maximumHeaderFieldValueLength = 64U * 1024U});
        std::vector<HpackHeaderField> responseFields;
        std::string decodeErrorText;
        ASSERT_TRUE(peerDecoder.decode(frames[0].payload + frames[1].payload, responseFields, &decodeErrorText)) << decodeErrorText;
        EXPECT_EQ(findHeaderValue(responseFields, ":status"), "200");
        EXPECT_EQ(findHeaderValue(responseFields, "x-big"), bigValue);
    }

    /**
     * @brief 钉住：流级窗口不足的数据不出帧，窗口以 WINDOW_UPDATE 或 SETTINGS 增量还回来后续发
     */
    TEST(Http2Connection, HoldsDataUntilTheStreamWindowAllowsSending)
    {
        Http2Connection connection;
        completeHandshake(connection, {namedSetting(Http2SettingIdentifier::InitialWindowSize, 10U)});
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        std::string errorText;
        ASSERT_TRUE(connection.sendResponseData(1U, std::string(30U, 'a'), true, &errorText)) << errorText;
        std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U) << "只有 10 字节的名额，第二帧不该出现";
        EXPECT_EQ(frames[0].payload.size(), 10U);
        EXPECT_EQ(frames[0].header.flags, 0U) << "队列里还有数据，END_STREAM 不许提前发";
        static_cast<void>(connection.takeReceivedData());

        // 对端把 SETTINGS_INITIAL_WINDOW_SIZE 调到 15（增量 +5）：ACK 之后续发 5 字节
        EXPECT_EQ(feed(connection, makeSettingsFrame({namedSetting(Http2SettingIdentifier::InitialWindowSize, 15U)})),
                  Http2ConnectionFeedStatus::NeedMore);
        frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 2U) << "先回 SETTINGS ACK，再续发正文";
        EXPECT_EQ(frames[0].header.type, Http2FrameType::Settings);
        EXPECT_EQ(frames[0].header.flags, kHttp2FlagAcknowledge);
        EXPECT_EQ(frames[1].header.type, Http2FrameType::Data);
        EXPECT_EQ(frames[1].payload.size(), 5U);

        // 流级窗口补 7 字节：再发 7
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(7U))), Http2ConnectionFeedStatus::NeedMore);
        frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U);
        EXPECT_EQ(frames[0].payload.size(), 7U);

        // 连接级窗口的更新解不开流级窗口已用尽的流
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 0, makeBigEndian32(20U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "流级窗口是 0，连接级窗口再大也发不出去";

        // 流级窗口补 8 字节：正好排空队列，END_STREAM 落在这一帧上
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(8U))), Http2ConnectionFeedStatus::NeedMore);
        frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U);
        EXPECT_EQ(frames[0].payload.size(), 8U);
        EXPECT_EQ(frames[0].header.flags, kHttp2FlagEndStream);
        EXPECT_EQ(frames[0].payload, std::string(8U, 'a'));
        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::Closed);
    }

    /**
     * @brief 钉住：连接级窗口同样卡住发送，必须等它的 WINDOW_UPDATE 才续发
     */
    TEST(Http2Connection, HoldsDataUntilTheConnectionWindowCatchesUp)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 70000 字节 > 连接级窗口的 65535：先发满窗口，剩下的排队
        const std::string body(70000U, 'b');
        std::string errorText;
        ASSERT_TRUE(connection.sendResponseData(1U, body, true, &errorText)) << errorText;
        std::size_t sentByteCount = 0;
        for (const Http2Frame &frame: parseFrames(connection.takeOutgoingBytes()))
        {
            EXPECT_EQ(frame.header.type, Http2FrameType::Data);
            sentByteCount += frame.payload.size();
        }
        EXPECT_EQ(sentByteCount, static_cast<std::size_t>(kHttp2InitialWindowSizeByteCount)) << "一次最多只能送出连接级窗口的大小";

        // 只补流级窗口：连接级窗口已用尽，仍然发不出去
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(100U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "连接级窗口是 0，流级窗口再大也发不出去";

        // 连接级窗口补 10000，但流级窗口只剩 100：这一轮只能续发 100 字节
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 0, makeBigEndian32(10000U))), Http2ConnectionFeedStatus::NeedMore);
        std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U);
        EXPECT_EQ(frames[0].payload.size(), 100U);
        EXPECT_EQ(frames[0].header.flags, 0U) << "队列里还剩 4365 字节，END_STREAM 不许提前发";

        // 流级窗口补足剩下的 4365：队列排空，END_STREAM 落在最后一帧上
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(4365U))), Http2ConnectionFeedStatus::NeedMore);
        frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U);
        EXPECT_EQ(frames[0].payload.size(), 4365U);
        EXPECT_EQ(frames[0].header.flags, kHttp2FlagEndStream);
    }

    /**
     * @brief 钉住：窗口增益导致窗口超过 2^31-1 时判 FLOW_CONTROL_ERROR（§6.9.1）
     */
    TEST(Http2Connection, RejectsWindowUpdateThatOverflowsTheWindow)
    {
        const std::string maximumIncrement = "\x7f\xff\xff\xff"; // 2^31-1
        // 连接级窗口溢出：65535 + (2^31-1) 超过上限
        Http2Connection connectionLevel;
        completeHandshake(connectionLevel);
        EXPECT_EQ(feed(connectionLevel, makeFrame(Http2FrameType::WindowUpdate, 0, 0, maximumIncrement)), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connectionLevel.errorCode(), Http2ErrorCode::FlowControlError);
        EXPECT_NE(connectionLevel.errorMessage().find("连接级发送窗口"), std::string::npos) << connectionLevel.errorMessage();

        // 流级窗口溢出：流窗口初值 65535，同样加上 2^31-1 就越界
        Http2Connection streamLevel;
        completeHandshake(streamLevel);
        ASSERT_EQ(feed(streamLevel, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(streamLevel, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, maximumIncrement)), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(streamLevel.errorCode(), Http2ErrorCode::FlowControlError);
        EXPECT_EQ(takeGoAwayErrorCode(streamLevel.takeOutgoingBytes()), Http2ErrorCode::FlowControlError);
    }

    /**
     * @brief 钉住：窗口正好等于 2^31-1 合法，再多 1 字节才判错（边界取上界本身而不是上界加一）
     */
    TEST(Http2Connection, AcceptsWindowExactlyAtTheLimit)
    {
        Http2Connection connection;
        completeHandshake(connection, {namedSetting(Http2SettingIdentifier::InitialWindowSize, kHttp2MaximumWindowSizeByteCount)});
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << "2^31-1 是合法窗口：" << connection.errorMessage();

        // 再加 1 字节就越界：§6.9.1 要求按 FLOW_CONTROL_ERROR 处理
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(1U))), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::FlowControlError);
    }

    /**
     * @brief 钉住：并发流数达到上限时新流回 REFUSED_STREAM，腾出名额后其余流照常服务（§5.1.2）
     */
    TEST(Http2Connection, RefusesStreamsBeyondMaximumConcurrentStreams)
    {
        Http2ConnectionConfiguration configuration;
        configuration.maximumConcurrentStreams = 2U;
        Http2Connection connection(configuration);
        completeHandshake(connection);

        // 两条流都不带 END_STREAM：它们会一直占着并发名额
        for (const std::uint32_t streamId: {1U, 3U})
        {
            EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, streamId, makePostRequestBlock())),
                      Http2ConnectionFeedStatus::NeedMore);
        }
        EXPECT_EQ(connection.openStreamCount(), 2U);
        EXPECT_EQ(connection.takeRequests().size(), 2U);

        // 第三条流被拒：REFUSED_STREAM，且不交出请求
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 5U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << "并发超限是流错误，连接继续：" << connection.errorMessage();
        EXPECT_TRUE(connection.takeRequests().empty());
        const std::vector<Http2Frame> resetFrames = takeRstStreamFrames(connection);
        ASSERT_EQ(resetFrames.size(), 1U);
        EXPECT_EQ(resetFrames.front().header.streamId, 5U);
        Http2RstStreamPayload payload;
        std::string errorText;
        ASSERT_TRUE(parseHttp2RstStreamPayload(resetFrames.front(), payload, &errorText)) << errorText;
        EXPECT_EQ(payload.errorCode, Http2ErrorCode::RefusedStream);
        EXPECT_NE(connection.lastStreamErrorMessage().find("并发流数"), std::string::npos) << connection.lastStreamErrorMessage();
        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(5U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::Closed);

        // 对端 RST 掉流 1 之后，流 7 又能被服务
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::RstStream, 0, 1U, makeBigEndian32(8U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(connection.openStreamCount(), 1U);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 7U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U);
        EXPECT_EQ(requests[0].streamId, 7U);
    }

    /**
     * @brief 钉住：RST_STREAM 终止过的流，其上的在途帧一律忽略（不判错、不回帧）
     */
    TEST(Http2Connection, IgnoresFramesOnStreamsTerminatedByReset)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // 本端因为请求头不合规 RST 掉流 1
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            hpackIndexedField(2) + hpackIndexedField(6))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(takeRstStreamFrames(connection).size(), 1U);

        // 对端可能还没看到 RST_STREAM：它在这条流上补发的帧必须被忽略而不是再判错
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, "late")), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(16U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::RstStream, 0, 1U, makeBigEndian32(8U))), Http2ConnectionFeedStatus::NeedMore);
        // 头块同样被丢弃，但字节必须解码：带增量索引的表示已经改动了本端解码器的动态表
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);

        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeRequests().empty()) << "已终止的流不再交出请求";
        EXPECT_TRUE(connection.takeReceivedData().empty()) << "已终止的流不再交出正文";
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "忽略语义下不得回敬任何控制帧";
    }

    /**
     * @brief 钉住：双向 END_STREAM 正常终止的流上，只有窗口/复位类帧可忽略，DATA 判连接错误 STREAM_CLOSED
     */
    TEST(Http2Connection, RejectsFramesAfterEndStreamOnCompletedStreams)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());
        std::string errorText;
        ASSERT_TRUE(connection.sendResponseHeaders(1U, 204U, {}, true, &errorText)) << errorText;
        static_cast<void>(connection.takeOutgoingBytes());

        // §5.1「closed」段：这两类帧必须忽略（对端可能还没看到 END_STREAM/RST_STREAM）
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 1U, makeBigEndian32(16U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::RstStream, 0, 1U, makeBigEndian32(8U))), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());

        // DATA 落在已正常终止的流上则是连接错误 STREAM_CLOSED
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, "x")), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::StreamClosed);
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::StreamClosed);
    }

    /**
     * @brief 钉住：偶数流号与倒退的流号都判连接错误 PROTOCOL_ERROR（§5.1.1）
     */
    TEST(Http2Connection, RejectsEvenAndBackwardStreamIdentifiers)
    {
        // 偶数流号属于服务端方向：本端不推送，收到即意外流号
        Http2Connection evenStream;
        completeHandshake(evenStream);
        EXPECT_EQ(feed(evenStream, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 2U,
                                             makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(evenStream.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(evenStream.errorMessage().find("偶数"), std::string::npos) << evenStream.errorMessage();

        // 新流号必须严格大于所有已用过的流号：先开 3，再想开 1 就是倒退
        Http2Connection backwardStream;
        completeHandshake(backwardStream);
        EXPECT_EQ(feed(backwardStream, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3U,
                                                 makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(backwardStream.takeRequests().size(), 1U);
        EXPECT_EQ(feed(backwardStream, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                                 makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(backwardStream.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(backwardStream.errorMessage().find("严格递增"), std::string::npos) << backwardStream.errorMessage();
    }

    /**
     * @brief 钉住：idle 流上的 DATA/WINDOW_UPDATE/RST_STREAM 判 PROTOCOL_ERROR，PRIORITY 才允许忽略
     */
    TEST(Http2Connection, RejectsStreamFramesOnIdleStreams)
    {
        // 从未开启的流上出现 DATA（§5.1「idle」段只允许 HEADERS 与 PRIORITY）
        Http2Connection idleData;
        completeHandshake(idleData);
        EXPECT_EQ(feed(idleData, makeFrame(Http2FrameType::Data, 0, 5U, "x")), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(idleData.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(idleData.errorMessage().find("idle"), std::string::npos) << idleData.errorMessage();

        // 从未开启的流上出现 WINDOW_UPDATE
        Http2Connection idleWindowUpdate;
        completeHandshake(idleWindowUpdate);
        EXPECT_EQ(feed(idleWindowUpdate, makeFrame(Http2FrameType::WindowUpdate, 0, 5U, makeBigEndian32(16U))), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(idleWindowUpdate.errorCode(), Http2ErrorCode::ProtocolError);

        // 从未开启的流上出现 RST_STREAM
        Http2Connection idleReset;
        completeHandshake(idleReset);
        EXPECT_EQ(feed(idleReset, makeFrame(Http2FrameType::RstStream, 0, 5U, makeBigEndian32(8U))), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(idleReset.errorCode(), Http2ErrorCode::ProtocolError);
    }

    /**
     * @brief 钉住：PRIORITY 在任何流上都被接受（不建流、不回帧），未知帧类型按 §4.1 忽略
     */
    TEST(Http2Connection, IgnoresPriorityAndUnknownFrameTypes)
    {
        Http2Connection connection;
        completeHandshake(connection);

        Http2Priority priority;
        priority.streamDependency = 0U;
        priority.weight = 10U;
        EXPECT_EQ(feed(connection, encodeHttp2PriorityFrame(priority, 5U)), Http2ConnectionFeedStatus::NeedMore);
        Http2StreamState streamState{};
        EXPECT_FALSE(connection.tryGetStreamState(5U, streamState)) << "PRIORITY 不得开启流（§5.3）";
        EXPECT_EQ(connection.openStreamCount(), 0U);

        // 未定义类型 0x55：编码器按契约拒绝产出，只能手拼帧头（§4.1 要求忽略）
        const std::string unknownFrame = makeFrameHeaderBytes(3U, 0x55U, 0U, 0U) + "abc";
        EXPECT_EQ(feed(connection, unknownFrame), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());
        EXPECT_TRUE(connection.takeRequests().empty());
    }

    /**
     * @brief 钉住：客户端发 PUSH_PROMISE 判 PROTOCOL_ERROR（§8.2 只允许服务端推送）
     */
    TEST(Http2Connection, RejectsPushPromiseFromClient)
    {
        Http2Connection connection;
        completeHandshake(connection);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::PushPromise, kHttp2FlagEndHeaders, 1U, std::string(4, '\0'))),
                  Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(connection.errorMessage().find("PUSH_PROMISE"), std::string::npos) << connection.errorMessage();
    }

    /**
     * @brief 钉住：PING 必须原样回声 8 字节，未请求过的 PING ACK 忽略
     */
    TEST(Http2Connection, EchoesPingAndIgnoresUnrequestedAcknowledgement)
    {
        Http2Connection connection;
        completeHandshake(connection);

        const std::string opaqueData = makeBytes({0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xFF});
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Ping, 0, 0, opaqueData)), Http2ConnectionFeedStatus::NeedMore);
        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(frames.size(), 1U);
        EXPECT_EQ(frames[0].header.type, Http2FrameType::Ping);
        EXPECT_EQ(frames[0].header.flags, kHttp2FlagAcknowledge);
        EXPECT_EQ(frames[0].header.streamId, 0U);
        Http2PingPayload echo;
        std::string errorText;
        ASSERT_TRUE(parseHttp2PingPayload(frames[0], echo, &errorText)) << errorText;
        EXPECT_TRUE(echo.isAcknowledgement);
        EXPECT_EQ(std::string(echo.opaqueData.begin(), echo.opaqueData.end()), opaqueData) << "8 字节必须原样回声（§6.7）";

        // 本片从不发 PING：对端主动送来的 ACK 忽略，不判错
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Ping, kHttp2FlagAcknowledge, 0, opaqueData)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());
    }

    /**
     * @brief 钉住：收到 GOAWAY 之后不再受理新流，既有流照常收尾（§6.8）
     */
    TEST(Http2Connection, StopsAcceptingNewStreamsAfterPeerGoAway)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        Http2GoAwayPayload goAwayPayload;
        goAwayPayload.lastStreamId = 1U;
        goAwayPayload.errorCode = Http2ErrorCode::NoError;
        goAwayPayload.debugData = "bye";
        EXPECT_EQ(feed(connection, encodeHttp2GoAwayFrame(goAwayPayload)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(connection.state(), Http2ConnectionState::Closing);
        Http2GoAwayPayload receivedGoAway;
        ASSERT_TRUE(connection.tryGetPeerGoAway(receivedGoAway));
        EXPECT_EQ(receivedGoAway.lastStreamId, 1U);
        EXPECT_EQ(receivedGoAway.errorCode, Http2ErrorCode::NoError);

        // 新流一律拒绝：REFUSED_STREAM 让对端知道这条流没被处理
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeRequests().empty());
        const std::vector<Http2Frame> resetFrames = takeRstStreamFrames(connection);
        ASSERT_EQ(resetFrames.size(), 1U);
        EXPECT_EQ(resetFrames.front().header.streamId, 3U);
        Http2RstStreamPayload payload;
        std::string errorText;
        ASSERT_TRUE(parseHttp2RstStreamPayload(resetFrames.front(), payload, &errorText)) << errorText;
        EXPECT_EQ(payload.errorCode, Http2ErrorCode::RefusedStream);

        // 既有流还能收尾：GOAWAY 之前的流做完就行
        ASSERT_TRUE(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText)) << errorText;
        const std::vector<Http2Frame> responseFrames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(responseFrames.size(), 1U);
        EXPECT_EQ(responseFrames[0].header.type, Http2FrameType::Headers);
        EXPECT_EQ(responseFrames[0].header.streamId, 1U);
        EXPECT_EQ(responseFrames[0].header.flags, kHttp2FlagEndStream | kHttp2FlagEndHeaders);
    }

    /**
     * @brief 钉住：尾部头块只校验语法、不交出字段，END_STREAM 照常半关对端方向（§8.1）
     */
    TEST(Http2Connection, ValidatesTrailerHeaderBlocksWithoutDeliveringThem)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 合法的尾部头块：字段有意丢弃（与 HttpParser 对分块 trailer 的既有处置一致），但流要按 END_STREAM 半关
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            hpackLiteralField("x-checksum", "42"))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeRequests().empty()) << "尾部头块不是新请求";
        EXPECT_TRUE(connection.takeReceivedData().empty());
        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::HalfClosedRemote);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());

        // 尾部头块里出现伪头：流错误（§8.1.2.1 要求尾部头块不得含伪头）
        Http2Connection withPseudo;
        completeHandshake(withPseudo);
        ASSERT_EQ(feed(withPseudo, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(withPseudo.takeRequests());
        EXPECT_EQ(feed(withPseudo, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                             hpackIndexedField(8))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_NE(expectStreamRejected(withPseudo, 1U).find("伪头"), std::string::npos);
    }

    /**
     * @brief 钉住：响应入口的失败路径不写任何字节，错误文案可操作；成功后重复发送同样被拒
     */
    TEST(Http2Connection, ResponseEntryRejectsUnknownStreamAndIllegalHeaderFields)
    {
        Http2Connection connection;
        completeHandshake(connection);
        std::string errorText = "脏数据";

        // 流不在账本里：连一个字节都不许写出去
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText));
        EXPECT_FALSE(errorText.empty()) << "失败必须给出可排查的中文原因";
        EXPECT_FALSE(connection.sendResponseData(1U, "x", false, &errorText));

        // 还没完成 SETTINGS 协商的连接同样拒绝响应
        Http2Connection notNegotiated;
        EXPECT_FALSE(notNegotiated.sendResponseHeaders(1U, 200U, {}, true, &errorText));

        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 状态码越界、头名与头值不合规：全部在入口拦下
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 42U, {}, true, &errorText));
        EXPECT_NE(errorText.find("状态码"), std::string::npos) << errorText;
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 1000U, {}, true, &errorText));
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 200U, {{"X-Test", "1"}}, true, &errorText));
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 200U, {{"connection", "keep-alive"}}, true, &errorText));
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 200U, {{"x-test", "a\rb"}}, true, &errorText));
        EXPECT_FALSE(connection.sendResponseHeaders(1U, 200U, {{":status", "200"}}, true, &errorText));
        EXPECT_FALSE(connection.sendResponseData(9U, "x", false, &errorText));
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "失败路径不得写入任何字节";

        // 正常发出响应之后，本端已经 END_STREAM：不允许再补正文
        ASSERT_TRUE(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText)) << errorText;
        static_cast<void>(connection.takeOutgoingBytes());
        EXPECT_FALSE(connection.sendResponseData(1U, "x", false, &errorText));
        EXPECT_FALSE(errorText.empty());
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());
    }

    /**
     * @brief 钉住：同一段字节逐字节喂与一次性喂，交出的请求、正文与待发字节完全一致
     */
    TEST(Http2Connection, ByteByByteFeedingMatchesSingleFeed)
    {
        const std::string headerBlock = makeMinimalGetRequestBlock();
        const std::size_t firstLength = headerBlock.size() / 2U;
        std::string script(kHttp2ConnectionPreface);
        script += makeSettingsFrame({namedSetting(Http2SettingIdentifier::MaxFrameSize, 32768U)});
        script += makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, makeRfc7541FirstRequestBlock());
        script += makeFrame(Http2FrameType::Headers, 0, 3U, headerBlock.substr(0, firstLength));
        script += makeFrame(Http2FrameType::Continuation, kHttp2FlagEndHeaders, 3U, headerBlock.substr(firstLength));
        script += makeFrame(Http2FrameType::Data, 0, 3U, "body-");
        script += makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 3U, "follows");
        script += makeFrame(Http2FrameType::Ping, 0, 0, std::string(8, 'Z'));

        Http2Connection byteWise;
        Http2Connection singleShot;
        for (const char byteValue: script)
        {
            // 逐字节喂：每次只给一个字节，状态机必须自己把帧、头块与流状态拼起来
            EXPECT_EQ(byteWise.feedBytes(&byteValue, 1U), Http2ConnectionFeedStatus::NeedMore);
        }
        EXPECT_EQ(feed(singleShot, script), Http2ConnectionFeedStatus::NeedMore);

        const std::vector<Http2Request> byteWiseRequests = byteWise.takeRequests();
        const std::vector<Http2Request> singleShotRequests = singleShot.takeRequests();
        ASSERT_EQ(byteWiseRequests.size(), 2U);
        ASSERT_EQ(singleShotRequests.size(), byteWiseRequests.size());
        for (std::size_t index = 0; index < byteWiseRequests.size(); ++index)
        {
            EXPECT_EQ(byteWiseRequests[index].streamId, singleShotRequests[index].streamId);
            EXPECT_EQ(byteWiseRequests[index].method, singleShotRequests[index].method);
            EXPECT_EQ(byteWiseRequests[index].path, singleShotRequests[index].path);
            EXPECT_EQ(byteWiseRequests[index].authority, singleShotRequests[index].authority);
            EXPECT_EQ(byteWiseRequests[index].hasBody, singleShotRequests[index].hasBody);
        }

        const std::vector<Http2ReceivedData> byteWiseBody = byteWise.takeReceivedData();
        const std::vector<Http2ReceivedData> singleShotBody = singleShot.takeReceivedData();
        ASSERT_EQ(byteWiseBody.size(), 2U);
        ASSERT_EQ(singleShotBody.size(), byteWiseBody.size());
        for (std::size_t index = 0; index < byteWiseBody.size(); ++index)
        {
            EXPECT_EQ(byteWiseBody[index].streamId, singleShotBody[index].streamId);
            EXPECT_EQ(byteWiseBody[index].data, singleShotBody[index].data);
            EXPECT_EQ(byteWiseBody[index].endStream, singleShotBody[index].endStream);
        }

        EXPECT_FALSE(byteWise.hasFailed()) << byteWise.errorMessage();
        EXPECT_EQ(byteWise.state(), singleShot.state());
        EXPECT_EQ(byteWise.openStreamCount(), singleShot.openStreamCount());
        EXPECT_EQ(byteWise.takeOutgoingBytes(), singleShot.takeOutgoingBytes()) << "两种喂法吐出的字节必须逐字节相同";
    }
} // namespace AsynGyanis::Net
