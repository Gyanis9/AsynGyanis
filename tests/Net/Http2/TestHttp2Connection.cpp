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
//      按对端 MAX_FRAME_SIZE 分片；发送入口按结论区分「该流已被对端取消/终止」与「连接不可用」；
//   5) 发送方向流控：窗口不足不出帧、WINDOW_UPDATE 与 SETTINGS_INITIAL_WINDOW_SIZE 续发、
//      连接级与流级窗口取小、窗口溢出判 FLOW_CONTROL_ERROR（§5.2、§6.9）；
//   6) 契约面：失败入口不写字节、错误出参可操作、逐字节喂入与一次性喂入结果一致；
//   7) 握手期状态：SETTINGS 待 ACK 可查询（含发帧时刻）、failConnection() 按指定错误码收口。
// 请求方向的头部字节有一部分直接取自规范：RFC 7541 C.3.1/C.4.1 的两个请求头块 dump 用作「黄金字节」，
// 其余请求头块由用例按静态表索引手工拼出（片段与取值都标了出处）。用例不起网络、不依赖外部服务。
//
// 逐项对照：帧类型与标志常量、静态表索引均取自 RFC 7540 §6 与 RFC 7541 Appendix A。

#include "Net/Http2/Http2Connection.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
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
         * @brief 由字节序列拼出二进制文本（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeBytes;

        /**
         * @brief 把 32 位无符号数按大端写成 4 字节（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeBigEndian32;

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
        // 七项本端参数按 §6.5.2 的参数标识顺序逐项对照，取值来源是 Http2ConnectionConfiguration 的默认值
        const std::vector<std::pair<Http2SettingIdentifier, std::uint32_t>> expectedParameters = {
            {Http2SettingIdentifier::HeaderTableSize, static_cast<std::uint32_t>(kHpackDefaultDynamicTableSizeByteCount)},
            {Http2SettingIdentifier::EnablePush, 0U},
            {Http2SettingIdentifier::MaxConcurrentStreams, 100U},
            {Http2SettingIdentifier::InitialWindowSize, kHttp2InitialWindowSizeByteCount},
            {Http2SettingIdentifier::MaxFrameSize, kHttp2DefaultMaximumFrameSize},
            {Http2SettingIdentifier::MaxHeaderListSize, 16U * 1024U},
            {Http2SettingIdentifier::EnableConnectProtocol, 1U}};
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
     * @brief 钉住请求头 content-length 的取值口径：非十进制取值、重复且冲突都按流错误拒绝
     * @details 与 h1 侧同一口径（RFC 9110 §8.6）：长度有歧义时中间设备与业务可能各按一种读法理解
     *          正文边界，正是请求走私的形态。这是**流**错误而不是连接错误：RST_STREAM 这条流，
     *          连接继续服务其它流。
     */
    TEST(Http2Connection, RejectsMalformedAndConflictingRequestContentLength)
    {
        const auto expectStreamRejected = [](const std::string &headerBlock)
        {
            Http2Connection connection;
            completeHandshake(connection);
            EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, headerBlock)),
                      Http2ConnectionFeedStatus::NeedMore);
            EXPECT_TRUE(connection.takeRequests().empty()) << "长度有歧义的请求不得交给业务";

            const std::vector<Http2Frame> resetFrames = takeRstStreamFrames(connection);
            ASSERT_EQ(resetFrames.size(), 1U) << "应当中止这条流";
            Http2RstStreamPayload payload;
            std::string errorText;
            ASSERT_TRUE(parseHttp2RstStreamPayload(resetFrames.front(), payload, &errorText)) << errorText;
            EXPECT_EQ(payload.errorCode, Http2ErrorCode::ProtocolError);
            EXPECT_FALSE(connection.hasFailed()) << "流错误不该终止连接：" << connection.errorMessage();
        };

        // 取值不是十进制数字
        expectStreamRejected(makeMinimalGetRequestBlock() + hpackLiteralField("content-length", "5x"));

        // 重复出现且前后冲突（5 与 6）
        expectStreamRejected(makeMinimalGetRequestBlock() + hpackLiteralField("content-length", "5") +
                             hpackLiteralField("content-length", "6"));
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
        ASSERT_EQ(connection.sendResponseHeaders(1U, 200U, {{"content-type", "text/plain"}}, false, &errorText),
                  Http2ResponseSendStatus::Sent) << errorText;
        ASSERT_EQ(connection.sendResponseData(1U, "Hello, ", false, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        ASSERT_EQ(connection.sendResponseData(1U, "world!", true, &errorText), Http2ResponseSendStatus::Sent) << errorText;

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
        ASSERT_EQ(connection.sendResponseData(1U, body, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
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
        ASSERT_EQ(connection.sendResponseHeaders(1U, 200U, {{"x-big", bigValue}}, false, &errorText), Http2ResponseSendStatus::Sent)
                << errorText;
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
        ASSERT_EQ(connection.sendResponseData(1U, std::string(30U, 'a'), true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
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
        ASSERT_EQ(connection.sendResponseData(1U, body, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
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
     * @brief 钉住：消费掉对端正文后按量回 WINDOW_UPDATE（连接级与流级各一条），没到阈值就先攒着
     */
    TEST(Http2Connection, CreditsConsumedDataWithWindowUpdates)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 三片共 32868 字节：单帧不超过 16384，总量超过半个窗口（32767）但不越接收窗口，不该判错
        const std::string firstChunk(16384U, 'a');
        const std::string secondChunk(16384U, 'b');
        const std::string thirdChunk(100U, 'c');
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, firstChunk)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, secondChunk)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1U, thirdChunk)),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        const std::vector<Http2ReceivedData> receivedData = connection.takeReceivedData();
        ASSERT_EQ(receivedData.size(), 3U);
        EXPECT_EQ(receivedData[0].data, firstChunk);
        EXPECT_EQ(receivedData[0].flowControlByteCount, firstChunk.size()) << "流控账要按帧负载原长记";
        EXPECT_TRUE(receivedData[2].endStream);

        std::string errorText;
        // 第一片消费完还没到半个窗口：按阈值策略先攒着，一个字节都不发
        ASSERT_TRUE(connection.creditReceivedData(1U, receivedData[0].flowControlByteCount, &errorText)) << errorText;
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "没到半个窗口的消费量不该逐帧回敬 WINDOW_UPDATE";

        // 第二片消费完累计 32768 >= 32767：连接级与流级各回一条，增量就是累计消费量
        ASSERT_TRUE(connection.creditReceivedData(1U, receivedData[1].flowControlByteCount, &errorText)) << errorText;
        const std::vector<Http2Frame> updateFrames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(updateFrames.size(), 2U) << "连接级与流级窗口都要还";
        EXPECT_EQ(updateFrames[0].header.streamId, 0U) << "先还连接级窗口";
        EXPECT_EQ(updateFrames[1].header.streamId, 1U) << "再还流级窗口";
        for (const Http2Frame &frame: updateFrames)
        {
            EXPECT_EQ(frame.header.type, Http2FrameType::WindowUpdate);
            Http2WindowUpdatePayload payload;
            std::string parseErrorText;
            ASSERT_TRUE(parseHttp2WindowUpdatePayload(frame, payload, &parseErrorText)) << parseErrorText;
            EXPECT_EQ(payload.windowSizeIncrement, firstChunk.size() + secondChunk.size()) << "增量应当等于这一段累计消费量";
        }

        // 第三片（100 字节）消费掉：没到下一轮阈值，仍然不发帧
        ASSERT_TRUE(connection.creditReceivedData(1U, receivedData[2].flowControlByteCount, &errorText)) << errorText;
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());

        // 契约面：连接失败之后不再受理消费回报
        Http2Connection failedConnection;
        completeHandshake(failedConnection);
        ASSERT_EQ(feed(failedConnection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 2U,
                                                  makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::Failed);
        static_cast<void>(failedConnection.takeOutgoingBytes());
        EXPECT_FALSE(failedConnection.creditReceivedData(2U, 16U, &errorText));
        EXPECT_FALSE(errorText.empty()) << "失败必须给出中文原因";
        EXPECT_TRUE(failedConnection.creditReceivedData(2U, 0U, &errorText)) << "零字节消费是空操作，不算失败";
    }

    /**
     * @brief 钉住：对端发出的 DATA 突破接收窗口时判连接错误 FLOW_CONTROL_ERROR（§6.9.1）
     */
    TEST(Http2Connection, RejectsDataBeyondTheReceiveWindow)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 帧层单帧上限是 16384，因此用 4 片凑出 65536 > 65535：最后一片必然把窗口扣成负数
        const std::string chunk(16384U, 'x');
        for (int frameIndex = 0; frameIndex < 3; ++frameIndex)
        {
            EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, chunk)), Http2ConnectionFeedStatus::NeedMore);
            EXPECT_FALSE(connection.hasFailed()) << "前 3 片应当合法：" << connection.errorMessage();
        }
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, chunk)), Http2ConnectionFeedStatus::Failed);
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::FlowControlError);
        EXPECT_NE(connection.errorMessage().find("接收窗口"), std::string::npos) << connection.errorMessage();
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::FlowControlError);
    }

    /**
     * @brief 钉住：已终止流上被忽略的在途 DATA 也要把连接级窗口还回去（否则连接窗口会被慢慢吃掉）
     */
    TEST(Http2Connection, ReturnsConnectionWindowForIgnoredDataOnTerminatedStreams)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // 本端因为请求头不合规 RST 掉流 1：其后到达的 DATA 属于对端的在途数据，按 §5.1 忽略
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            hpackIndexedField(2) + hpackIndexedField(6))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(takeRstStreamFrames(connection).size(), 1U);

        // 攒够半个连接窗口（32767）的忽略数据：连接级窗口必须因此回一次 WINDOW_UPDATE
        const std::string chunk(16384U, 'y');
        for (int frameIndex = 0; frameIndex < 3; ++frameIndex)
        {
            EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, chunk)), Http2ConnectionFeedStatus::NeedMore);
        }
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeReceivedData().empty()) << "已终止的流不再交出正文";

        const std::vector<Http2Frame> updateFrames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(updateFrames.size(), 1U) << "被丢弃的正文消费在连接级窗口上，应当回一条 WINDOW_UPDATE";
        EXPECT_EQ(updateFrames[0].header.type, Http2FrameType::WindowUpdate);
        EXPECT_EQ(updateFrames[0].header.streamId, 0U) << "流已终止：只还连接级窗口，不发流级帧";
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
        ASSERT_EQ(connection.sendResponseHeaders(1U, 204U, {}, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
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
        ASSERT_EQ(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        const std::vector<Http2Frame> responseFrames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(responseFrames.size(), 1U);
        EXPECT_EQ(responseFrames[0].header.type, Http2FrameType::Headers);
        EXPECT_EQ(responseFrames[0].header.streamId, 1U);
        EXPECT_EQ(responseFrames[0].header.flags, kHttp2FlagEndStream | kHttp2FlagEndHeaders);
    }

    /**
     * @brief 钉住：尾部头块只校验语法、不交出字段；END_STREAM 照常半关对端方向，并交出一条零长收尾片段（§8.1）
     */
    TEST(Http2Connection, ValidatesTrailerHeaderBlocksWithoutDeliveringThem)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 合法的尾部头块：字段有意丢弃（与 HttpParser 对分块 trailer 的既有处置一致），但流要按 END_STREAM 半关。
        // 交出的那一条是**零长**的收尾信号：上层只按 Http2ReceivedData::endStream 判定正文收齐，
        // 少了它，以尾部头块收尾的请求永远等不到收齐、不会进路由
        EXPECT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            hpackLiteralField("x-checksum", "42"))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
        EXPECT_TRUE(connection.takeRequests().empty()) << "尾部头块不是新请求";
        const std::vector<Http2ReceivedData> trailerData = connection.takeReceivedData();
        ASSERT_EQ(trailerData.size(), 1U) << "收尾信号必须交给上层：以尾部头块收尾的请求靠它才知道正文收齐了";
        EXPECT_EQ(trailerData[0].streamId, 1U);
        EXPECT_TRUE(trailerData[0].data.empty()) << "交出的只有收尾信号，尾部头块的字段一个都不在里面";
        EXPECT_TRUE(trailerData[0].endStream);
        EXPECT_EQ(trailerData[0].flowControlByteCount, 0U);
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
     * @brief 钉住：响应入口按结论区分「这条流不可写」与「连接不可用」，失败路径不写任何字节，错误文案可操作
     */
    TEST(Http2Connection, ResponseEntryRejectsUnknownStreamAndIllegalHeaderFields)
    {
        Http2Connection connection;
        completeHandshake(connection);
        std::string errorText = "脏数据";

        // 流不在账本里：这是「该流已不可写响应」，连接本身照旧可用——调用方据此只停这条流
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText), Http2ResponseSendStatus::StreamNotWritable);
        EXPECT_FALSE(errorText.empty()) << "失败必须给出可排查的中文原因";
        EXPECT_EQ(connection.sendResponseData(1U, "x", false, &errorText), Http2ResponseSendStatus::StreamNotWritable);
        EXPECT_FALSE(connection.hasFailed()) << "流级结论不得把连接判死";

        // 还没完成 SETTINGS 协商的连接整条不可用
        Http2Connection notNegotiated;
        EXPECT_EQ(notNegotiated.sendResponseHeaders(1U, 200U, {}, true, &errorText), Http2ResponseSendStatus::ConnectionUnavailable);
        EXPECT_NE(errorText.find("SETTINGS"), std::string::npos) << errorText;

        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 状态码越界、头名与头值不合规：全部在入口拦下，结论是用法错误 Rejected
        EXPECT_EQ(connection.sendResponseHeaders(1U, 42U, {}, true, &errorText), Http2ResponseSendStatus::Rejected);
        EXPECT_NE(errorText.find("状态码"), std::string::npos) << errorText;
        EXPECT_EQ(connection.sendResponseHeaders(1U, 1000U, {}, true, &errorText), Http2ResponseSendStatus::Rejected);
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {{"X-Test", "1"}}, true, &errorText), Http2ResponseSendStatus::Rejected);
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {{"connection", "keep-alive"}}, true, &errorText),
                  Http2ResponseSendStatus::Rejected);
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {{"x-test", "a\rb"}}, true, &errorText), Http2ResponseSendStatus::Rejected);
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {{":status", "200"}}, true, &errorText), Http2ResponseSendStatus::Rejected);
        EXPECT_EQ(connection.sendResponseData(9U, "x", false, &errorText), Http2ResponseSendStatus::StreamNotWritable);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "失败路径不得写入任何字节";

        // 正常发出响应之后，本端已经 END_STREAM：不允许再补正文
        ASSERT_EQ(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        static_cast<void>(connection.takeOutgoingBytes());
        EXPECT_EQ(connection.sendResponseData(1U, "x", false, &errorText), Http2ResponseSendStatus::Rejected);
        EXPECT_FALSE(errorText.empty());
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());
    }

    /**
     * @brief 钉住：对端 RST_STREAM 掉一条流后，发往该流的响应结论是 StreamNotWritable 且连接可以继续
     *        ——同连接上另一条流照旧收响应，连接不进入失败态
     */
    TEST(Http2Connection, ReportsStreamNotWritableWhenPeerResetsTheStream)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // 两条并发流：流 1 请求头不带 END_STREAM（还在等正文），流 3 是完整请求
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(connection.takeRequests().size(), 2U);

        // 对端取消流 1（CANcel，§5.4.2）：该流被终止，账本里随即不可写
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::RstStream, 0, 1U, makeBigEndian32(static_cast<std::uint32_t>(Http2ErrorCode::Cancel)))),
                  Http2ConnectionFeedStatus::NeedMore);
        Http2StreamState resetStreamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, resetStreamState));
        EXPECT_EQ(resetStreamState, Http2StreamState::Closed);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        // 头与正文都给出 StreamNotWritable：调用方据此只停这条流，而不是收口整条连接
        std::string errorText;
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {}, false, &errorText), Http2ResponseSendStatus::StreamNotWritable);
        EXPECT_EQ(connection.sendResponseData(1U, "x", true, &errorText), Http2ResponseSendStatus::StreamNotWritable);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "被取消的流上不得写出任何响应字节";

        // 连接照旧可用：流 3 的响应正常排出，连接不失败、也不转关闭中
        ASSERT_EQ(connection.sendResponseHeaders(3U, 200U, {}, false, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        EXPECT_EQ(connection.sendResponseData(3U, "still-served", true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        EXPECT_FALSE(connection.hasFailed());
        EXPECT_EQ(connection.state(), Http2ConnectionState::Open) << "一条流被取消不该让连接进入关闭中或失败态";
        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_GE(frames.size(), 2U);
        for (const Http2Frame &frame: frames)
        {
            EXPECT_EQ(frame.header.streamId, 3U) << "被取消的流上不该出现任何响应帧";
        }
    }

    /**
     * @brief 钉住：对端 RST_STREAM 掉一条还有正文在发送队列里的流时，队列随之丢弃——连接窗口
     *        后来变大也放不出那些字节（该流的发送窗口当时仍为正，正是能把它放出来的条件）
     */
    TEST(Http2Connection, DiscardsQueuedResponseDataWhenPeerResetsTheStream)
    {
        Http2Connection connection;
        // 对端把流级初值调到 100000（大于连接级窗口 65535）：先被连接窗口卡住，队列里还剩正文，
        // 而该流的发送窗口仍为正——「连接窗口后来变大」因此能把残留数据放出来，正好用来钉住丢弃
        completeHandshake(connection, {namedSetting(Http2SettingIdentifier::InitialWindowSize, 100000U)});
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                            makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 70000 字节 > 连接级窗口的 65535：先发满窗口，剩下的 4465 字节排队
        const std::string body(70000U, 'q');
        std::string errorText;
        ASSERT_EQ(connection.sendResponseData(1U, body, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        std::size_t sentByteCount = 0;
        for (const Http2Frame &frame: parseFrames(connection.takeOutgoingBytes()))
        {
            EXPECT_EQ(frame.header.type, Http2FrameType::Data);
            sentByteCount += frame.payload.size();
        }
        EXPECT_EQ(sentByteCount, static_cast<std::size_t>(kHttp2InitialWindowSizeByteCount)) << "一次最多只能送出连接级窗口的大小";

        // 对端取消这条流：队列里剩下的 4465 字节（以及待发的 END_STREAM）随终止一并丢弃
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::RstStream, 0, 1U,
                                             makeBigEndian32(static_cast<std::uint32_t>(Http2ErrorCode::Cancel)))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "被取消的流的待发数据不得再上线";
        Http2StreamState resetStreamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, resetStreamState));
        EXPECT_EQ(resetStreamState, Http2StreamState::Closed);

        // 连接级窗口变大：账本里那条已终止的流不该把丢弃的字节放出来（它的流级窗口当时仍为正）
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::WindowUpdate, 0, 0, makeBigEndian32(4096U))),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "连接窗口变大之后，被取消流上的残留数据不得被放出来";

        // 该流本身也再不可写，连接照旧可用
        EXPECT_EQ(connection.sendResponseData(1U, "x", true, &errorText), Http2ResponseSendStatus::StreamNotWritable);
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
    }

    /**
     * @brief 钉住：SETTINGS 待 ACK 状态可查询、发帧时刻被记下，收到 ACK 后状态清除
     */
    TEST(Http2Connection, ReportsSettingsAwaitingAcknowledgementUntilThePeerAcknowledges)
    {
        Http2Connection connection;
        EXPECT_FALSE(connection.hasSettingsAwaitingAcknowledgement()) << "前奏还没到齐，本端还没发出 SETTINGS";

        // 前奏收齐即发初始 SETTINGS：此后状态为「待 ACK」，发帧时刻必须是刚刚
        const std::chrono::steady_clock::time_point beforePrefaceTime = std::chrono::steady_clock::now();
        EXPECT_EQ(feed(connection, std::string(kHttp2ConnectionPreface)), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.hasSettingsAwaitingAcknowledgement());
        EXPECT_GE(connection.lastSettingsSentTime(), beforePrefaceTime) << "发帧时刻必须落在前奏收齐之后";
        EXPECT_LE(connection.lastSettingsSentTime(), std::chrono::steady_clock::now());
        EXPECT_FALSE(connection.takeOutgoingBytes().empty()) << "此刻待发字节里应当是初始 SETTINGS";

        // 对端自己的 SETTINGS 到齐也不等于 ACK 到了：状态保持
        EXPECT_EQ(feed(connection, makeSettingsFrame({})), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.hasSettingsAwaitingAcknowledgement()) << "收到对端 SETTINGS 不代表本端 SETTINGS 被 ACK";

        // 收到 ACK 即清除
        EXPECT_EQ(feed(connection, makeSettingsAckFrame()), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_FALSE(connection.hasSettingsAwaitingAcknowledgement());
    }

    /**
     * @brief 钉住：failConnection() 以指定错误码把 GOAWAY 排进待发字节并转入失败态；NO_ERROR 与重复调用被拒
     */
    TEST(Http2Connection, FailsConnectionWithRequestedErrorCode)
    {
        Http2Connection connection;
        completeHandshake(connection);
        std::string errorText = "脏数据";

        // NO_ERROR 是收尾通告的码：failConnection() 必须拒绝，让调用方走 sendGoAway()
        EXPECT_FALSE(connection.failConnection(Http2ErrorCode::NoError, "正常收尾走 sendGoAway()", &errorText));
        EXPECT_NE(errorText.find("sendGoAway"), std::string::npos) << errorText;
        EXPECT_FALSE(connection.hasFailed());
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "被拒的调用不得写入任何字节";

        // 指定错误码：GOAWAY 带上它，连接转入粘滞失败态
        ASSERT_TRUE(connection.failConnection(Http2ErrorCode::SettingsTimeout, "对端没有 ACK 本端 SETTINGS", &errorText)) << errorText;
        EXPECT_TRUE(connection.hasFailed());
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::SettingsTimeout);
        EXPECT_NE(connection.errorMessage().find("ACK"), std::string::npos) << connection.errorMessage();
        EXPECT_EQ(takeGoAwayErrorCode(connection.takeOutgoingBytes()), Http2ErrorCode::SettingsTimeout);

        // 失败态粘滞：GOAWAY 只发一次，重复调用被拒
        EXPECT_FALSE(connection.failConnection(Http2ErrorCode::ProtocolError, "再来一次", &errorText));
        EXPECT_FALSE(errorText.empty());
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::SettingsTimeout) << "第一个原因才是根因，不得改写";
        EXPECT_TRUE(connection.takeOutgoingBytes().empty());
    }

    /**
     * @brief 钉住：以尾部头块收尾的正文会让上层看到一条零长、endStream 为 true 的片段
     * @details 上层（会话）只按 `Http2ReceivedData::endStream` 判定「正文收齐」，不读流状态；
     *          连接层若在尾部头块分支只把流置成 half-closed (remote) 而不产出这条收尾片段，
     *          这条请求会一直等不到收齐、永远不路由（客户端只能等到超时）
     */
    TEST(Http2Connection, ReportsBodyCompletionWhenTrailersEndTheStream)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // POST 头块不带 END_STREAM：这条流还在等正文
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        ASSERT_EQ(connection.takeRequests().size(), 1U);
        EXPECT_TRUE(connection.takeReceivedData().empty()) << "还没收到正文";

        // 正文片段不带 END_STREAM：上层按它攒正文，但此刻还不知道正文收齐了
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, "part")), Http2ConnectionFeedStatus::NeedMore);
        std::vector<Http2ReceivedData> receivedData = connection.takeReceivedData();
        ASSERT_EQ(receivedData.size(), 1U);
        EXPECT_EQ(receivedData[0].streamId, 1U);
        EXPECT_EQ(receivedData[0].data, "part");
        EXPECT_FALSE(receivedData[0].endStream) << "本片没有 END_STREAM：正文还没收齐";

        // 尾部头块带 END_STREAM（§8.1）：正文到此为止，必须让上层看到收尾
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U,
                                             hpackLiteralField("x-trailer", "done"))),
                  Http2ConnectionFeedStatus::NeedMore);
        receivedData = connection.takeReceivedData();
        ASSERT_EQ(receivedData.size(), 1U) << "尾部头块必须带出一条收尾片段，否则上层永远等不到正文收齐";
        EXPECT_EQ(receivedData[0].streamId, 1U);
        EXPECT_TRUE(receivedData[0].data.empty()) << "收尾片段是零长的：尾部头块本身不是正文";
        EXPECT_TRUE(receivedData[0].endStream);
        EXPECT_EQ(receivedData[0].flowControlByteCount, 0U) << "零长片段不占流控窗口，不会让对端的窗口被多还";

        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::HalfClosedRemote) << "对端方向收尾，本端方向还开着";
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        // 本端照旧能在这条流上回响应：尾部头块只结束了对端方向
        std::string errorText;
        EXPECT_EQ(connection.sendResponseHeaders(1U, 200U, {}, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
    }

    /**
     * @brief 钉住：发完完整响应后可以请对端中止请求正文——RST_STREAM(NO_ERROR) 只终止这条流，
     *        连接照旧可用，且在途补发的 DATA 被忽略而不会把连接判死
     */
    TEST(Http2Connection, RequestsPeerToAbortStreamAfterEarlyResponse)
    {
        Http2Connection connection;
        completeHandshake(connection);
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, makePostRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        static_cast<void>(connection.takeRequests());

        // 先发出完整响应（413）：正文超限时就是这样「先应答、再请对端别传了」
        std::string errorText = "脏数据";
        ASSERT_EQ(connection.sendResponseHeaders(1U, 413U, {}, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        static_cast<void>(connection.takeOutgoingBytes());

        ASSERT_TRUE(connection.abortStream(1U, "响应已发出，不再需要剩余正文", &errorText)) << errorText;
        const std::vector<Http2Frame> abortFrames = parseFrames(connection.takeOutgoingBytes());
        ASSERT_EQ(abortFrames.size(), 1U) << "中止只发一条 RST_STREAM";
        EXPECT_EQ(abortFrames[0].header.type, Http2FrameType::RstStream);
        EXPECT_EQ(abortFrames[0].header.streamId, 1U);
        EXPECT_EQ(abortFrames[0].header.flags, 0) << "RST_STREAM 不带标志";

        // 错误码必须是 NO_ERROR：RFC 9113 §8.1 的「请对端无错地中止发送」，不是把这条流判成出错
        Http2RstStreamPayload payload;
        std::string parseErrorText;
        ASSERT_TRUE(parseHttp2RstStreamPayload(abortFrames[0], payload, &parseErrorText)) << parseErrorText;
        EXPECT_EQ(payload.errorCode, Http2ErrorCode::NoError);

        Http2StreamState streamState{};
        ASSERT_TRUE(connection.tryGetStreamState(1U, streamState));
        EXPECT_EQ(streamState, Http2StreamState::Closed);
        EXPECT_FALSE(connection.hasFailed()) << "中止单流不该把连接判死";

        // 对端在收到 RST_STREAM 之前补发的 DATA：忽略、不判错，也不能再交给上层（流已终止）
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Data, 0, 1U, "late")), Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(connection.takeReceivedData().empty()) << "已终止流的正文不该再交给上层";
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();

        // 已经终止的流与不存在的流都拒绝中止，且不写任何字节
        static_cast<void>(connection.takeOutgoingBytes());
        EXPECT_FALSE(connection.abortStream(1U, "重复中止", &errorText));
        EXPECT_FALSE(errorText.empty()) << "失败必须给出可排查的中文原因";
        EXPECT_FALSE(connection.abortStream(99U, "不存在的流", &errorText));
        EXPECT_TRUE(connection.takeOutgoingBytes().empty()) << "被拒的调用不得写入任何字节";

        // 连接照旧可用：另一条流的信息性请求能正常应答
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 3U,
                                             makeMinimalGetRequestBlock())),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_EQ(connection.takeRequests().size(), 1U);
        ASSERT_EQ(connection.sendResponseHeaders(3U, 200U, {}, true, &errorText), Http2ResponseSendStatus::Sent) << errorText;
        EXPECT_EQ(connection.state(), Http2ConnectionState::Open) << "单流中止不该让连接进入关闭中或失败态";
    }

    /**
     * @brief 钉住：本端初始 SETTINGS 里必须通告 ENABLE_CONNECT_PROTOCOL = 1（RFC 8441 §3）
     * @details 客户端只有在看到这个参数之后才允许发扩展 CONNECT，因此它不能只在文档里支持
     */
    TEST(Http2Connection, AdvertisesExtendedConnectSupport)
    {
        Http2Connection connection;
        ASSERT_EQ(feed(connection, std::string(kHttp2ConnectionPreface)), Http2ConnectionFeedStatus::NeedMore);

        const std::vector<Http2Frame> frames = parseFrames(connection.takeOutgoingBytes());
        const Http2Frame *settingsFrame = nullptr;
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.type == Http2FrameType::Settings)
            {
                settingsFrame = &frame;
                break;
            }
        }
        ASSERT_NE(settingsFrame, nullptr) << "前奏收齐后必须先发本端 SETTINGS";

        // SETTINGS 负载是 6 字节一组的「16 位标识 + 32 位取值」（§6.5）；这里手解，避免依赖被测实现
        bool hasExtendedConnectSetting = false;
        ASSERT_EQ(settingsFrame->payload.size() % 6U, 0U);
        for (std::size_t offset = 0; offset + 6U <= settingsFrame->payload.size(); offset += 6U)
        {
            const auto identifier = static_cast<std::uint16_t>(
                    (static_cast<std::uint8_t>(settingsFrame->payload[offset]) << 8) |
                    static_cast<std::uint8_t>(settingsFrame->payload[offset + 1]));
            const auto value = static_cast<std::uint32_t>(
                    (static_cast<std::uint8_t>(settingsFrame->payload[offset + 2]) << 24) |
                    (static_cast<std::uint8_t>(settingsFrame->payload[offset + 3]) << 16) |
                    (static_cast<std::uint8_t>(settingsFrame->payload[offset + 4]) << 8) |
                    static_cast<std::uint8_t>(settingsFrame->payload[offset + 5]));
            if (identifier == static_cast<std::uint16_t>(Http2SettingIdentifier::EnableConnectProtocol))
            {
                hasExtendedConnectSetting = true;
                EXPECT_EQ(value, 1U) << "ENABLE_CONNECT_PROTOCOL 必须通告 1，否则对端不该发扩展 CONNECT";
            }
        }
        EXPECT_TRUE(hasExtendedConnectSetting) << "初始 SETTINGS 里没有 ENABLE_CONNECT_PROTOCOL";
        EXPECT_FALSE(connection.hasFailed());
    }

    /**
     * @brief 钉住：带 :protocol=websocket 的扩展 CONNECT 能交到上层，且五个伪头各自到位（RFC 8441 §4）
     */
    TEST(Http2Connection, AcceptsExtendedConnectForWebSocketTunnel)
    {
        Http2Connection connection;
        completeHandshake(connection);

        // 扩展 CONNECT 的伪头集合与普通 CONNECT 相反：:scheme 与 :path 都必须出现
        std::string headerBlock;
        headerBlock += hpackLiteralField(2, "CONNECT");
        headerBlock += hpackIndexedField(6);
        headerBlock += hpackLiteralField(4, "/chat");
        headerBlock += hpackLiteralField(1, "localhost");
        headerBlock += hpackLiteralField(":protocol", "websocket");
        headerBlock += hpackLiteralField("sec-websocket-version", "13");
        ASSERT_EQ(feed(connection, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, headerBlock)),
                  Http2ConnectionFeedStatus::NeedMore);

        const std::vector<Http2Request> requests = connection.takeRequests();
        ASSERT_EQ(requests.size(), 1U) << "扩展 CONNECT 必须作为一条请求交出，而不是被当成畸形报文拒掉";
        EXPECT_EQ(requests[0].method, "CONNECT");
        EXPECT_EQ(requests[0].protocol, "websocket");
        EXPECT_EQ(requests[0].scheme, "http");
        EXPECT_EQ(requests[0].path, "/chat");
        EXPECT_EQ(requests[0].authority, "localhost");
        EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
    }

    /**
     * @brief 钉住：:protocol 只允许出现在 CONNECT 上，且扩展 CONNECT 的 :scheme/:path/:authority 一个都不能少
     */
    TEST(Http2Connection, RejectsMisplacedOrIncompleteProtocolPseudoHeader)
    {
        // GET 带 :protocol：RFC 8441 §4 只把它定义给 CONNECT
        Http2Connection withGet;
        completeHandshake(withGet);
        std::string getBlock = makeMinimalGetRequestBlock() + hpackLiteralField(":protocol", "websocket");
        ASSERT_EQ(feed(withGet, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, getBlock)),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(withGet.takeRequests().empty()) << "非 CONNECT 的 :protocol 不该交出请求";
        EXPECT_NE(expectStreamRejected(withGet, 1U).find(":protocol"), std::string::npos);

        // 扩展 CONNECT 缺 :path：普通 CONNECT 要求省略它，扩展 CONNECT 恰恰要求给出它
        Http2Connection withoutPath;
        completeHandshake(withoutPath);
        std::string noPathBlock;
        noPathBlock += hpackLiteralField(2, "CONNECT");
        noPathBlock += hpackIndexedField(6);
        noPathBlock += hpackLiteralField(1, "localhost");
        noPathBlock += hpackLiteralField(":protocol", "websocket");
        ASSERT_EQ(feed(withoutPath, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, noPathBlock)),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(withoutPath.takeRequests().empty());
        EXPECT_NE(expectStreamRejected(withoutPath, 1U).find(":path"), std::string::npos);

        // 值不是 token：协议名有语法要求，不能原样放过
        Http2Connection withBadValue;
        completeHandshake(withBadValue);
        std::string badValueBlock;
        badValueBlock += hpackLiteralField(2, "CONNECT");
        badValueBlock += hpackIndexedField(6);
        badValueBlock += hpackLiteralField(4, "/chat");
        badValueBlock += hpackLiteralField(1, "localhost");
        badValueBlock += hpackLiteralField(":protocol", "web socket");
        ASSERT_EQ(feed(withBadValue, makeFrame(Http2FrameType::Headers, kHttp2FlagEndStream | kHttp2FlagEndHeaders, 1U, badValueBlock)),
                  Http2ConnectionFeedStatus::NeedMore);
        EXPECT_TRUE(withBadValue.takeRequests().empty());
        EXPECT_NE(expectStreamRejected(withBadValue, 1U).find(":protocol"), std::string::npos);
    }

    /**
     * @brief 钉住：对端 SETTINGS 里的 ENABLE_CONNECT_PROTOCOL 只允许 0/1，其它取值按连接错误收场（RFC 8441 §3）
     */
    TEST(Http2Connection, RejectsIllegalEnableConnectProtocolSetting)
    {
        Http2Connection connection;
        ASSERT_EQ(feed(connection, std::string(kHttp2ConnectionPreface)), Http2ConnectionFeedStatus::NeedMore);
        const std::string clientSettings =
                encodeHttp2SettingsFrame(Http2SettingsPayload{
                        .parameters = {{static_cast<std::uint16_t>(Http2SettingIdentifier::EnableConnectProtocol), 2U}}});
        EXPECT_EQ(feed(connection, clientSettings), Http2ConnectionFeedStatus::Failed);
        EXPECT_TRUE(connection.hasFailed());
        EXPECT_EQ(connection.errorCode(), Http2ErrorCode::ProtocolError);
        EXPECT_NE(connection.errorMessage().find("ENABLE_CONNECT_PROTOCOL"), std::string::npos) << connection.errorMessage();
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
