// TestHttp2Frame.cpp —— HTTP/2 帧层（RFC 7540 §4/§6/§7）的单元测试
//
// 覆盖四块：
//   1) 帧头 9 字节的编解码与字段边界（24 位长度、31 位流号、R 位从严判错）；
//   2) 各类型负载的编解码：SETTINGS/PING/GOAWAY/RST_STREAM/WINDOW_UPDATE 的字节按 RFC 7540 §6.4/
//      §6.5/§6.7/§6.8/§6.9 的字段图逐字段拼出（规范给的是字段图而不是逐字节 dump），取值全部取自
//      §6.5.2 列出的初始值与 §7 的错误码表；DATA/HEADERS 的带 padding 与带优先级字段的形态按
//      §6.1 图 4/5 与 §6.2 图 6/7 自造；
//   3) 拒绝面：R 位非 0、长度超本端上限、类型要求的固定长度不符、连接级帧带流号、padding 越界、
//      priority 依赖自身流号、WINDOW_UPDATE 增量 0、超出累计读取上限；
//   4) 增量解码契约：任意字节边界可切开续上、未知帧类型与未定义标志位跳过而不是判错、错误粘滞且
//      不消费字节、产出未取走时不再消费字节、takeFrame() 的用法错误。
// 用例都是纯计算，不起网络、不依赖任何外部服务，因此不存在等待信号到达的时序问题。

#include "Net/Http2/Http2Frame.h"

#include "NetTestSupport.h"

#include "Http2TestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

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
         * @details 用例需要构造出「编码器拒绝产出」的非法帧头（例如 R 位置位、长度越界、未知类型），
         *          因此这里不接受任何校验、逐字节写出。
         * @param payloadLength 负载长度（按 24 位截断写出，仅供越界用例使用）
         * @param typeValue 帧类型取值
         * @param flags 标志位
         * @param streamIdField 流号字段的 32 位原始取值（含 R 位）
         * @return std::string 9 字节帧头
         */
        std::string makeFrameHeaderBytes(const std::uint32_t payloadLength, const unsigned char typeValue,
                                         const unsigned char flags, const std::uint32_t streamIdField)
        {
            std::string header;
            header.push_back(static_cast<char>((payloadLength >> 16) & 0xFFU));
            header.push_back(static_cast<char>((payloadLength >> 8) & 0xFFU));
            header.push_back(static_cast<char>(payloadLength & 0xFFU));
            header.push_back(static_cast<char>(typeValue));
            header.push_back(static_cast<char>(flags));
            header += makeBigEndian32(streamIdField);
            return header;
        }

        /**
         * @brief 用编码器拼出一帧（定义见 Http2TestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeFrame;

        /**
         * @brief 把一段字节喂给解码器
         * @param decoder 解码器
         * @param bytes 本次喂入的字节
         * @return Http2FrameDecodeStatus 本次结论
         */
        Http2FrameDecodeStatus feed(Http2FrameDecoder &decoder, const std::string_view bytes)
        {
            return decoder.parse(bytes.data(), bytes.size());
        }

        /**
         * @brief 断言收到一帧并取出来
         * @param decoder 解码器
         * @param bytes 本次喂入的字节
         * @return Http2Frame 取出的帧；未产出帧时返回默认帧，且用例已被记为失败
         */
        Http2Frame feedAndTakeFrame(Http2FrameDecoder &decoder, const std::string_view bytes)
        {
            const Http2FrameDecodeStatus status = feed(decoder, bytes);
            EXPECT_EQ(status, Http2FrameDecodeStatus::Frame) << "本应产出一帧，错误：" << decoder.errorMessage();
            if (status != Http2FrameDecodeStatus::Frame)
            {
                return {};
            }
            return decoder.takeFrame();
        }

        /**
         * @brief 断言这段字节被判错，并交出中文原因
         * @param decoder 解码器
         * @param bytes 本次喂入的字节
         * @return std::string 错误文案，已断言非空
         */
        std::string feedAndExpectError(Http2FrameDecoder &decoder, const std::string_view bytes)
        {
            EXPECT_EQ(feed(decoder, bytes), Http2FrameDecodeStatus::Error) << "这段输入本应被判错";
            EXPECT_TRUE(decoder.hasError());
            EXPECT_FALSE(decoder.errorMessage().empty()) << "失败必须给出可排查的原因";
            return decoder.errorMessage();
        }

        /**
         * @brief 一次把整串字节喂完并取出全部帧
         * @details 按 consumedByteCount() 推进偏移，因此这同时也在钉「返回 Frame 时只消费本帧字节」
         *          这条契约。
         * @param decoder 解码器
         * @param stream 整串字节
         * @return std::vector<Http2Frame> 按顺序取出的帧
         */
        std::vector<Http2Frame> decodeAllInOneFeed(Http2FrameDecoder &decoder, const std::string_view stream)
        {
            std::vector<Http2Frame> frames;
            std::size_t offset = 0;
            while (offset < stream.size())
            {
                const Http2FrameDecodeStatus status = decoder.parse(stream.data() + offset, stream.size() - offset);
                if (status != Http2FrameDecodeStatus::Frame)
                {
                    // 记一次失败即停：继续跑只会重复同一个结论
                    ADD_FAILURE() << "本应产出一帧，错误：" << decoder.errorMessage();
                    return frames;
                }

                EXPECT_GT(decoder.consumedByteCount(), 0U);
                offset += decoder.consumedByteCount();
                frames.push_back(decoder.takeFrame());
            }
            return frames;
        }

        /**
         * @brief 把同一串字节按每 1 字节喂入并取出全部帧
         * @param decoder 解码器
         * @param stream 整串字节
         * @return std::vector<Http2Frame> 按顺序取出的帧
         */
        std::vector<Http2Frame> decodeByteByByte(Http2FrameDecoder &decoder, const std::string_view stream)
        {
            std::vector<Http2Frame> frames;
            for (const char byteValue: stream)
            {
                const Http2FrameDecodeStatus status = decoder.parse(&byteValue, 1);
                if (status == Http2FrameDecodeStatus::Frame)
                {
                    frames.push_back(decoder.takeFrame());
                    continue;
                }
                if (status == Http2FrameDecodeStatus::Error)
                {
                    ADD_FAILURE() << "逐字节喂入不应判错，原因：" << decoder.errorMessage();
                    return frames;
                }
            }
            return frames;
        }
    } // namespace

    // ============================================================================
    // 枚举取值与错误码映射：取值一旦改动就是改协议
    // ============================================================================

    /**
     * @brief 钉住帧类型、错误码的取值就是 RFC 7540 §6/§7 定义的那些，以及失败类别到错误码的映射
     */
    TEST(Http2Frame, TypeAndErrorCodeValuesMatchRfc7540)
    {
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::Data), 0x0U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::Headers), 0x1U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::Priority), 0x2U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::RstStream), 0x3U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::Settings), 0x4U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::PushPromise), 0x5U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::Ping), 0x6U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::GoAway), 0x7U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::WindowUpdate), 0x8U);
        EXPECT_EQ(static_cast<unsigned>(Http2FrameType::Continuation), 0x9U);

        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::NoError), 0x0U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::ProtocolError), 0x1U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::InternalError), 0x2U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::FlowControlError), 0x3U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::SettingsTimeout), 0x4U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::StreamClosed), 0x5U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::FrameSizeError), 0x6U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::RefusedStream), 0x7U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::Cancel), 0x8U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::CompressionError), 0x9U);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::ConnectError), 0xaU);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::EnhanceYourCalm), 0xbU);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::InadequateSecurity), 0xcU);
        EXPECT_EQ(static_cast<unsigned>(Http2ErrorCode::Http11Required), 0xdU);

        // 失败类别到错误码的映射：超限不是对端违规，按 §7 对「可能造成过量负载」的建议回 ENHANCE_YOUR_CALM
        EXPECT_EQ(toHttp2ErrorCode(Http2FrameErrorKind::None), Http2ErrorCode::NoError);
        EXPECT_EQ(toHttp2ErrorCode(Http2FrameErrorKind::ProtocolError), Http2ErrorCode::ProtocolError);
        EXPECT_EQ(toHttp2ErrorCode(Http2FrameErrorKind::FrameSizeError), Http2ErrorCode::FrameSizeError);
        EXPECT_EQ(toHttp2ErrorCode(Http2FrameErrorKind::LimitExceeded), Http2ErrorCode::EnhanceYourCalm);
    }

    // ============================================================================
    // 帧头：9 字节布局与字段边界（RFC 7540 §4.1）
    // ============================================================================

    /**
     * @brief 钉住帧头的 9 字节布局：24 位长度 + 8 位类型 + 8 位标志 + 31 位流号（全大端）
     */
    TEST(Http2Frame, EncodesFrameHeaderInNineBytesBigEndian)
    {
        Http2FrameHeader header;
        header.payloadLength = 0x010203;
        header.type = Http2FrameType::Continuation;
        header.flags = 0x24;
        header.streamId = 0x00000005;

        const std::string encoded = encodeHttp2FrameHeader(header);

        EXPECT_EQ(encoded, makeBytes({0x01, 0x02, 0x03, 0x09, 0x24, 0x00, 0x00, 0x00, 0x05}));
        EXPECT_EQ(encoded.size(), kHttp2FrameHeaderByteCount);

        Http2FrameHeader decoded;
        ASSERT_TRUE(decodeHttp2FrameHeader(encoded, decoded)) << "编码器产出的帧头必须能被解码器读回";
        EXPECT_EQ(decoded.payloadLength, header.payloadLength);
        EXPECT_EQ(decoded.type, header.type);
        EXPECT_EQ(decoded.flags, header.flags);
        EXPECT_EQ(decoded.streamId, header.streamId);
    }

    /**
     * @brief 长度域是 24 位、流号是 31 位：满值可编，多一位即拒绝（静默截断会让对端按错误的边界切帧）
     */
    TEST(Http2Frame, FrameHeaderFieldRangesAreEnforcedOnEncode)
    {
        Http2FrameHeader header;
        header.payloadLength = kHttp2MaximumFramePayloadByteCount;
        header.type = Http2FrameType::Data;
        header.streamId = 1;
        EXPECT_EQ(encodeHttp2FrameHeader(header).substr(0, 3), makeBytes({0xff, 0xff, 0xff}));

        header.payloadLength = kHttp2MaximumFramePayloadByteCount + 1;
        EXPECT_THROW(static_cast<void>(encodeHttp2FrameHeader(header)), Base::InvalidArgumentException);

        header.payloadLength = 0;
        header.streamId = kHttp2MaximumStreamId + 1;
        EXPECT_THROW(static_cast<void>(encodeHttp2FrameHeader(header)), Base::InvalidArgumentException);

        // 未定义类型不允许被编出去：对端只会忽略它，发出去等于白占带宽
        header.streamId = 1;
        header.type = static_cast<Http2FrameType>(0xF);
        EXPECT_THROW(static_cast<void>(encodeHttp2FrameHeader(header)), Base::InvalidArgumentException);

        // 负载整体超长同样在编码侧挡住（encodeHttp2Frame 先判后转，不会静默回绕）
        EXPECT_THROW(static_cast<void>(encodeHttp2Frame(Http2FrameType::Data, 0, 1,
                                                       std::string(static_cast<std::size_t>(kHttp2MaximumFramePayloadByteCount) + 1, 'x'))),
                     Base::InvalidArgumentException);
    }

    /**
     * @brief R 位非 0 判错（独立入口与增量解码器两条路径）
     * @details RFC 7540 §4.1 允许接收侧忽略该位，本实现从严：放行会让「这一帧是什么」取决于对端是否
     *          在写未定义的扩展，而这些帧在本端无法被正确解释。
     */
    TEST(Http2Frame, RejectsFrameHeaderWithReservedBitSet)
    {
        const std::string headerWithReservedBit = makeFrameHeaderBytes(0, 0x0, 0, 0x80000001U);

        Http2FrameHeader header;
        std::string reason;
        EXPECT_FALSE(decodeHttp2FrameHeader(headerWithReservedBit, header, &reason));
        EXPECT_TRUE(containsText(reason, "R")) << "原因里要写清是哪个字段越界：" << reason;

        Http2FrameDecoder decoder;
        reason = "残留";
        EXPECT_TRUE(containsText(feedAndExpectError(decoder, headerWithReservedBit), "R"));
        EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::ProtocolError);
        EXPECT_EQ(toHttp2ErrorCode(decoder.errorKind()), Http2ErrorCode::ProtocolError);
    }

    /**
     * @brief 帧头不足 9 字节时独立入口判错，而增量解码器只当「还差数据」
     */
    TEST(Http2Frame, ShortFrameHeaderIsRejectedOnlyByTheStandaloneEntry)
    {
        const std::string frame = makeFrame(Http2FrameType::Ping, 0, 0, std::string(8, '\x00'));

        Http2FrameHeader header;
        std::string reason;
        EXPECT_FALSE(decodeHttp2FrameHeader(std::string_view(frame.data(), 8), header, &reason));
        EXPECT_FALSE(reason.empty());

        Http2FrameDecoder decoder;
        EXPECT_EQ(feed(decoder, std::string_view(frame.data(), 8)), Http2FrameDecodeStatus::NeedMore);
        EXPECT_EQ(decoder.consumedByteCount(), 8U) << "增量解码器要把不足的字节全部吃掉，等后面的字节";
    }

    // ============================================================================
    // RFC 7540 §6.5：SETTINGS
    // ============================================================================

    /**
     * @brief 钉住 SETTINGS 的线格式，取值取 §6.5.2 列出的初始值（连接前奏里最典型的一帧）
     * @details RFC 7540 §3.5 要求客户端连接前奏以 SETTINGS 开帧，§6.5.3 要求收到后立刻回一个带 ACK
     *          的空 SETTINGS —— 两种形态都在这里钉住。
     */
    TEST(Http2Frame, EncodesSettingsFramesOfTheConnectionPreface)
    {
        Http2SettingsPayload payload;
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::HeaderTableSize), 4096});
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::EnablePush), 1});
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::InitialWindowSize), 65535});

        const std::string settingsFrame = encodeHttp2SettingsFrame(payload);
        // 帧头：长度 18 = 3 个参数 × 6 字节，类型 0x4，无标志，流号 0（SETTINGS 是连接级帧）
        const std::string expected = makeBytes({0x00, 0x00, 0x12, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}) +
                                     makeBytes({0x00, 0x01, 0x00, 0x00, 0x10, 0x00}) + makeBytes({0x00, 0x02, 0x00, 0x00, 0x00, 0x01}) +
                                     makeBytes({0x00, 0x04, 0x00, 0x00, 0xff, 0xff});
        EXPECT_EQ(settingsFrame, expected);

        // ACK 帧：长度 0、ACK 标志置位、流号 0（§6.5）
        Http2SettingsPayload acknowledgement;
        acknowledgement.isAcknowledgement = true;
        EXPECT_EQ(encodeHttp2SettingsFrame(acknowledgement),
                  makeBytes({0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00}));

        // 带参数却置 ACK 是用法错误：对端按连接错误处理（§6.5）
        acknowledgement.parameters.push_back({0x0, 0x0});
        EXPECT_THROW(static_cast<void>(encodeHttp2SettingsFrame(acknowledgement)), Base::InvalidArgumentException);
    }

    /**
     * @brief 解码 SETTINGS：六个具名参数都能取到，未知标识原样保留而不判错
     * @details RFC 7540 §6.5.2 要求忽略未知标识；§6.5 要求同一标识重复出现时以最后一次为准。
     */
    TEST(Http2Frame, DecodesSettingsWithNamedAndUnknownParameters)
    {
        Http2SettingsPayload payload;
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::MaxConcurrentStreams), 100});
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::MaxFrameSize), 32768});
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::MaxHeaderListSize), 8192});
        payload.parameters.push_back({0x1234, 0xABCDEF}); // 未知标识：必须被忽略而不是判错
        payload.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::MaxFrameSize), 40000});

        Http2FrameDecoder decoder;
        const Http2Frame frame = feedAndTakeFrame(decoder, encodeHttp2SettingsFrame(payload));
        EXPECT_EQ(frame.header.type, Http2FrameType::Settings);
        EXPECT_EQ(frame.header.streamId, 0U);

        Http2SettingsPayload decoded;
        ASSERT_TRUE(parseHttp2SettingsPayload(frame, decoded)) << "帧层交出的 SETTINGS 负载必须能解";
        ASSERT_EQ(decoded.parameters.size(), 5U) << "未知标识也要原样收下，丢掉就没人知道对端说了什么";
        EXPECT_FALSE(decoded.isAcknowledgement);

        std::uint32_t value = 0;
        EXPECT_TRUE(tryGetHttp2Setting(decoded, Http2SettingIdentifier::MaxConcurrentStreams, value));
        EXPECT_EQ(value, 100U);
        EXPECT_TRUE(tryGetHttp2Setting(decoded, Http2SettingIdentifier::MaxHeaderListSize, value));
        EXPECT_EQ(value, 8192U);
        EXPECT_TRUE(tryGetHttp2Setting(decoded, Http2SettingIdentifier::MaxFrameSize, value));
        EXPECT_EQ(value, 40000U) << "同一标识重复出现时取最后一次（RFC 7540 §6.5）";
        EXPECT_FALSE(tryGetHttp2Setting(decoded, Http2SettingIdentifier::EnablePush, value)) << "没带的参数不是错误";
        EXPECT_EQ(static_cast<unsigned>(decoded.parameters[3].identifier), 0x1234U);
        EXPECT_EQ(decoded.parameters[3].value, 0xABCDEFU);
    }

    // ============================================================================
    // RFC 7540 §6.4/§6.7/§6.8/§6.9：控制帧的线格式
    // ============================================================================

    /**
     * @brief 钉住 PING 的线格式（§6.7：8 字节不透明数据，ACK 由标志位表示）
     */
    TEST(Http2Frame, EncodesAndDecodesPingFrames)
    {
        Http2PingPayload ping;
        ping.opaqueData = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

        const std::string pingFrame = encodeHttp2PingFrame(ping);
        EXPECT_EQ(pingFrame, makeBytes({0x00, 0x00, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}) +
                                     makeBytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));

        Http2FrameDecoder decoder;
        Http2PingPayload decoded;
        ASSERT_TRUE(parseHttp2PingPayload(feedAndTakeFrame(decoder, pingFrame), decoded));
        EXPECT_FALSE(decoded.isAcknowledgement);
        EXPECT_EQ(decoded.opaqueData, ping.opaqueData);

        // ACK 形态：同一段数据、标志位为 0x1（§6.7 要求回送时把 ACK 置位）
        ping.isAcknowledgement = true;
        ASSERT_TRUE(parseHttp2PingPayload(feedAndTakeFrame(decoder, encodeHttp2PingFrame(ping)), decoded));
        EXPECT_TRUE(decoded.isAcknowledgement);
        EXPECT_EQ(decoded.opaqueData, ping.opaqueData);
    }

    /**
     * @brief 钉住 GOAWAY 的线格式（§6.8 图 13：保留位 + 31 位最后流号 + 32 位错误码 + 调试数据）
     */
    TEST(Http2Frame, EncodesAndDecodesGoAwayFrames)
    {
        Http2GoAwayPayload goAway;
        goAway.lastStreamId = 31;
        goAway.errorCode = Http2ErrorCode::ProtocolError;
        goAway.debugData = "bad frame";

        const std::string goAwayFrame = encodeHttp2GoAwayFrame(goAway);
        EXPECT_EQ(goAwayFrame, makeBytes({0x00, 0x00, 0x11, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00}) +
                                       makeBytes({0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x01}) + "bad frame");

        Http2FrameDecoder decoder;
        Http2GoAwayPayload decoded;
        ASSERT_TRUE(parseHttp2GoAwayPayload(feedAndTakeFrame(decoder, goAwayFrame), decoded));
        EXPECT_EQ(decoded.lastStreamId, 31U);
        EXPECT_EQ(decoded.errorCode, Http2ErrorCode::ProtocolError);
        EXPECT_EQ(decoded.debugData, "bad frame");

        // 调试数据可以为空：负载正好 8 字节是合法下界（§6.8 只要求不小于 8）
        goAway.debugData.clear();
        goAway.lastStreamId = 0;
        goAway.errorCode = Http2ErrorCode::EnhanceYourCalm;
        const std::string shortGoAway = encodeHttp2GoAwayFrame(goAway);
        EXPECT_EQ(shortGoAway.size(), kHttp2FrameHeaderByteCount + 8);
        ASSERT_TRUE(parseHttp2GoAwayPayload(feedAndTakeFrame(decoder, shortGoAway), decoded));
        EXPECT_EQ(decoded.errorCode, Http2ErrorCode::EnhanceYourCalm);
        EXPECT_TRUE(decoded.debugData.empty());
    }

    /**
     * @brief 钉住 RST_STREAM 与 WINDOW_UPDATE 的线格式（§6.4 图 9、§6.9 图 14）
     */
    TEST(Http2Frame, EncodesAndDecodesRstStreamAndWindowUpdateFrames)
    {
        Http2RstStreamPayload rstStream;
        rstStream.errorCode = Http2ErrorCode::Cancel;
        const std::string rstStreamFrame = encodeHttp2RstStreamFrame(rstStream, 3);
        EXPECT_EQ(rstStreamFrame, makeBytes({0x00, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x03}) +
                                          makeBytes({0x00, 0x00, 0x00, 0x08}));

        Http2WindowUpdatePayload windowUpdate;
        windowUpdate.windowSizeIncrement = 1;
        const std::string windowUpdateFrame = encodeHttp2WindowUpdateFrame(windowUpdate, 0);
        EXPECT_EQ(windowUpdateFrame, makeBytes({0x00, 0x00, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}) +
                                             makeBytes({0x00, 0x00, 0x00, 0x01}));

        Http2FrameDecoder decoder;
        Http2RstStreamPayload decodedRstStream;
        ASSERT_TRUE(parseHttp2RstStreamPayload(feedAndTakeFrame(decoder, rstStreamFrame), decodedRstStream));
        EXPECT_EQ(decodedRstStream.errorCode, Http2ErrorCode::Cancel);

        Http2WindowUpdatePayload decodedWindowUpdate;
        ASSERT_TRUE(parseHttp2WindowUpdatePayload(feedAndTakeFrame(decoder, windowUpdateFrame), decodedWindowUpdate));
        EXPECT_EQ(decodedWindowUpdate.windowSizeIncrement, 1U);

        // 流控窗口最大 2^31-1（§6.9.1）：满值可编；增量为 0 或超 31 位在编码侧就被拒
        windowUpdate.windowSizeIncrement = kHttp2MaximumStreamId;
        const std::string maximumWindowUpdate = encodeHttp2WindowUpdateFrame(windowUpdate, 7);
        ASSERT_TRUE(parseHttp2WindowUpdatePayload(feedAndTakeFrame(decoder, maximumWindowUpdate), decodedWindowUpdate));
        EXPECT_EQ(decodedWindowUpdate.windowSizeIncrement, kHttp2MaximumStreamId);
        EXPECT_FALSE(decoder.hasError());

        windowUpdate.windowSizeIncrement = 0;
        EXPECT_THROW(static_cast<void>(encodeHttp2WindowUpdateFrame(windowUpdate, 0)), Base::InvalidArgumentException);
        windowUpdate.windowSizeIncrement = kHttp2MaximumStreamId + 1;
        EXPECT_THROW(static_cast<void>(encodeHttp2WindowUpdateFrame(windowUpdate, 0)), Base::InvalidArgumentException);

        // 流级帧不允许流号为 0（§6.4）
        EXPECT_THROW(static_cast<void>(encodeHttp2RstStreamFrame(rstStream, 0)), Base::InvalidArgumentException);
    }

    // ============================================================================
    // RFC 7540 §6.1/§6.2：DATA 与 HEADERS 的 padding 与优先级字段
    // ============================================================================

    /**
     * @brief 钉住 DATA 帧：END_STREAM 标志与 §6.1 图 4 的 padding 剥离
     * @details 图 4 的布局是「Pad Length?(8) | Data(*) | Padding(*)」：首个字节是填充长度、填充在尾部，
     *          交付给上层的数据不含这两段。
     */
    TEST(Http2Frame, DecodesDataFrameWithPaddingAndEndStream)
    {
        Http2FrameDecoder decoder;

        Http2DataPayload sent;
        sent.endStream = true;
        sent.data = "hello";
        const std::string plainFrame = encodeHttp2DataFrame(sent, 1);

        Http2DataPayload decoded;
        ASSERT_TRUE(parseHttp2DataPayload(feedAndTakeFrame(decoder, plainFrame), decoded));
        EXPECT_TRUE(decoded.endStream);
        EXPECT_EQ(decoded.data, "hello");

        // 带 padding 的形态：标志为 END_STREAM|PADDED，Pad Length = 4、数据 "abc"、尾部 4 字节填充
        const std::string paddedData = makeBytes({0x00, 0x00, 0x08, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01}) +
                                       makeBytes({0x04}) + "abc" + makeBytes({0x00, 0x00, 0x00, 0x00});
        const Http2Frame paddedFrame = feedAndTakeFrame(decoder, paddedData);
        EXPECT_EQ(paddedFrame.header.payloadLength, 8U) << "线上的长度含填充长度字节与尾部填充";
        EXPECT_EQ(paddedFrame.payload, "abc") << "padding 必须在帧层剥掉";

        ASSERT_TRUE(parseHttp2DataPayload(paddedFrame, decoded));
        EXPECT_TRUE(decoded.endStream);
        EXPECT_EQ(decoded.data, "abc");

        // END_STREAM 不置位时正文未结束（§6.1）
        const std::string openFrame = makeFrame(Http2FrameType::Data, 0, 1, "more");
        ASSERT_TRUE(parseHttp2DataPayload(feedAndTakeFrame(decoder, openFrame), decoded));
        EXPECT_FALSE(decoded.endStream);
        EXPECT_EQ(decoded.data, "more");

        // DATA 必须关联到一条流（§6.1）
        Http2DataPayload noStream;
        noStream.data = "x";
        EXPECT_THROW(static_cast<void>(encodeHttp2DataFrame(noStream, 0)), Base::InvalidArgumentException);
    }

    /**
     * @brief 钉住 HEADERS 帧：§6.2 图 6/7 的 padding 与 5 字节优先级字段都在头块片段之前
     * @details 布局顺序是「Pad Length?(8) | E(1)+Stream Dependency(31) | Weight(8) | Header Block
     *          Fragment(*) | Padding(*)」，三层都得按序剥掉才是交给 HPACK 的片段。
     */
    TEST(Http2Frame, DecodesHeadersFrameWithPaddingAndPriority)
    {
        // 自造：Pad Length = 2，优先级字段 E=1 依赖流 5 权重 15，片段 0x82 0x86，尾部 2 字节填充
        const std::string headersFrame =
                makeFrame(Http2FrameType::Headers,
                          static_cast<unsigned char>(kHttp2FlagEndHeaders | kHttp2FlagPriority | kHttp2FlagPadded), 3,
                          makeBytes({0x02}) + makeBytes({0x80, 0x00, 0x00, 0x05, 0x0f}) + makeBytes({0x82, 0x86}) +
                                  makeBytes({0x00, 0x00}));

        Http2FrameDecoder decoder;
        const Http2Frame frame = feedAndTakeFrame(decoder, headersFrame);

        EXPECT_EQ(frame.header.type, Http2FrameType::Headers);
        EXPECT_EQ(static_cast<unsigned>(frame.header.flags),
                  static_cast<unsigned>(kHttp2FlagEndHeaders | kHttp2FlagPriority | kHttp2FlagPadded));
        EXPECT_TRUE(frame.hasPriority);
        EXPECT_TRUE(frame.priority.isExclusive) << "E 位是优先级字段的最高位";
        EXPECT_EQ(frame.priority.streamDependency, 5U);
        EXPECT_EQ(static_cast<unsigned>(frame.priority.weight), 0x0fU) << "线上权重取值 0..255，实际权重是它加一（§5.3.2）";
        EXPECT_EQ(frame.payload, makeBytes({0x82, 0x86})) << "交给 HPACK 的是剥掉 padding 与优先级字段后的片段";

        Http2HeadersPayload decoded;
        ASSERT_TRUE(parseHttp2HeadersPayload(frame, decoded));
        EXPECT_TRUE(decoded.endHeaders);
        EXPECT_FALSE(decoded.endStream);
        EXPECT_TRUE(decoded.hasPriority);
        EXPECT_EQ(decoded.headerBlockFragment, makeBytes({0x82, 0x86}));

        // 头块续帧：END_HEADERS 在标志位上（§6.10），负载就是片段本身
        Http2ContinuationPayload continuation;
        continuation.endHeaders = true;
        continuation.headerBlockFragment = makeBytes({0x40, 0x01, 0x61, 0x01, 0x62});
        const Http2Frame continuationFrame = feedAndTakeFrame(decoder, encodeHttp2ContinuationFrame(continuation, 3));
        EXPECT_FALSE(continuationFrame.hasPriority);

        Http2ContinuationPayload decodedContinuation;
        ASSERT_TRUE(parseHttp2ContinuationPayload(continuationFrame, decodedContinuation));
        EXPECT_TRUE(decodedContinuation.endHeaders);
        EXPECT_EQ(decodedContinuation.headerBlockFragment, makeBytes({0x40, 0x01, 0x61, 0x01, 0x62}));
    }

    /**
     * @brief 编码器产出的 HEADERS/PRIORITY 帧能被自己解回：END_STREAM 与 END_HEADERS 都落在标志位
     */
    TEST(Http2Frame, RoundTripsHeadersAndPriorityFrames)
    {
        Http2HeadersPayload headers;
        headers.endStream = true;
        headers.endHeaders = false;
        headers.hasPriority = true;
        headers.priority.streamDependency = 1;
        headers.priority.isExclusive = false;
        headers.priority.weight = 200;
        headers.headerBlockFragment = makeBytes({0x82, 0x84});

        const std::string encodedHeaders = encodeHttp2HeadersFrame(headers, 5);
        EXPECT_EQ(static_cast<unsigned>(encodedHeaders[4]), static_cast<unsigned>(kHttp2FlagEndStream | kHttp2FlagPriority));
        EXPECT_EQ(static_cast<unsigned>(encodedHeaders[3]), 0x1U);

        Http2FrameDecoder decoder;
        Http2HeadersPayload decoded;
        ASSERT_TRUE(parseHttp2HeadersPayload(feedAndTakeFrame(decoder, encodedHeaders), decoded));
        EXPECT_TRUE(decoded.endStream);
        EXPECT_FALSE(decoded.endHeaders);
        EXPECT_TRUE(decoded.hasPriority);
        EXPECT_EQ(decoded.priority.streamDependency, 1U);
        EXPECT_EQ(static_cast<unsigned>(decoded.priority.weight), 200U);
        EXPECT_EQ(decoded.headerBlockFragment, makeBytes({0x82, 0x84}));

        // PRIORITY 帧的负载就是同一份 5 字节优先级字段（§6.3）
        Http2Priority priority;
        priority.streamDependency = 3;
        priority.weight = 7;
        const std::string priorityFrame = encodeHttp2PriorityFrame(priority, 5);

        Http2FrameDecoder priorityDecoder;
        const Http2Frame decodedPriorityFrame = feedAndTakeFrame(priorityDecoder, priorityFrame);
        EXPECT_EQ(decodedPriorityFrame.header.type, Http2FrameType::Priority);
        EXPECT_TRUE(decodedPriorityFrame.hasPriority);
        EXPECT_EQ(decodedPriorityFrame.priority.streamDependency, 3U);
        EXPECT_EQ(static_cast<unsigned>(decodedPriorityFrame.priority.weight), 7U);
        EXPECT_TRUE(decodedPriorityFrame.payload.empty()) << "优先级字段已被帧层取走，净负载为空";
    }

    // ============================================================================
    // 拒绝面：帧层自己就能判出的违规
    // ============================================================================

    /**
     * @brief 长度超过本端通告的 SETTINGS_MAX_FRAME_SIZE 判错，错误码是 FRAME_SIZE_ERROR
     * @details 上限必须在「声明」阶段生效（RFC 7540 §4.2）：否则对端报一个天文数字的长度，
     *          本端就会一直等下去，内存与连接都被一条永不完成的帧占着。
     */
    TEST(Http2Frame, RejectsFrameOverTheAdvertisedMaximumFrameSize)
    {
        const std::string declaredTooLarge = makeFrameHeaderBytes(kHttp2DefaultMaximumFrameSize + 1, 0x0, 0x0, 1);

        Http2FrameDecoder decoder;
        const std::string reason = feedAndExpectError(decoder, declaredTooLarge);

        EXPECT_TRUE(containsText(reason, "16384")) << "原因里要给上限数值：" << reason;
        EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::FrameSizeError);
        EXPECT_EQ(toHttp2ErrorCode(decoder.errorKind()), Http2ErrorCode::FrameSizeError);

        // 恰好等于上限的声明必须放行
        Http2FrameDecoder atLimitDecoder;
        EXPECT_EQ(feed(atLimitDecoder, makeFrameHeaderBytes(kHttp2DefaultMaximumFrameSize, 0x0, 0x0, 1)),
                  Http2FrameDecodeStatus::NeedMore);
        EXPECT_EQ(feed(atLimitDecoder, std::string(kHttp2DefaultMaximumFrameSize, 'x')), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(atLimitDecoder.takeFrame().payload.size(), kHttp2DefaultMaximumFrameSize);
    }

    /**
     * @brief 解码器的接收上限可按配置调整，非法取值在构造时就被拒
     * @details SETTINGS_MAX_FRAME_SIZE 的合法区间是 [16384, 16777215]（RFC 7540 §6.5.2），
     *          越界取值等于在通告一个非法设置项。
     */
    TEST(Http2Frame, FrameSizeLimitIsConfigurableWithinTheLegalRange)
    {
        EXPECT_NO_THROW(static_cast<void>(Http2FrameDecoder(Http2FrameLimits{16384, 0})));
        EXPECT_NO_THROW(static_cast<void>(Http2FrameDecoder(Http2FrameLimits{16777215, 0})));
        EXPECT_THROW(static_cast<void>(Http2FrameDecoder(Http2FrameLimits{16383, 0})), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(Http2FrameDecoder(Http2FrameLimits{16777216, 0})), Base::InvalidArgumentException);

        // 上限放宽到 2 MiB 后，同样一段 1 MiB 的声明就变成合法的了
        Http2FrameDecoder decoder(Http2FrameLimits{2ull * 1024 * 1024, 0});
        EXPECT_EQ(feed(decoder, makeFrameHeaderBytes(1024 * 1024, 0x0, 0x0, 1)), Http2FrameDecodeStatus::NeedMore);
        EXPECT_FALSE(decoder.hasError());
    }

    /**
     * @brief 各类型要求的固定长度不符时判 FRAME_SIZE_ERROR（RFC 7540 §6.3/§6.4/§6.5/§6.7/§6.8/§6.9）
     */
    TEST(Http2Frame, RejectsPayloadLengthAgainstTypeRequirements)
    {
        struct LengthProbe
        {
            std::uint32_t payloadLength; ///< 声明的负载长度
            unsigned char typeValue;     ///< 帧类型
            unsigned char flags;         ///< 标志位
            std::uint32_t streamId;      ///< 流号
            std::string_view expected;   ///< 错误文案里应出现的关键词
        };

        const std::vector<LengthProbe> probes{
                {4, 0x2, 0, 3, "PRIORITY"},      // PRIORITY 负载必须 5 字节
                {3, 0x3, 0, 3, "RST_STREAM"},    // RST_STREAM 负载必须 4 字节
                {7, 0x4, 0, 0, "SETTINGS"},      // SETTINGS 负载必须是 6 的整数倍
                {6, 0x4, 0x1, 0, "ACK"},         // 带 ACK 的 SETTINGS 负载必须为空（§6.5）
                {9, 0x6, 0, 0, "PING"},          // PING 负载必须 8 字节
                {7, 0x7, 0, 0, "GOAWAY"},        // GOAWAY 负载至少 8 字节
                {3, 0x8, 0, 0, "WINDOW_UPDATE"}, // WINDOW_UPDATE 负载必须 4 字节
        };

        for (const LengthProbe &probe: probes)
        {
            Http2FrameDecoder decoder;
            const std::string reason = feedAndExpectError(
                    decoder, makeFrameHeaderBytes(probe.payloadLength, probe.typeValue, probe.flags, probe.streamId));

            EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::FrameSizeError) << probe.expected;
            EXPECT_TRUE(containsText(reason, probe.expected)) << "原因里要点出是哪种帧：" << reason;
        }
    }

    /**
     * @brief 连接级帧不许带流号、流级帧不许流号为 0
     * @details 依据 §6.1/§6.2/§6.3/§6.4/§6.5/§6.7/§6.8/§6.10 各自对帧头流号的要求。
     */
    TEST(Http2Frame, RejectsStreamIdThatContradictsTheFrameType)
    {
        struct StreamProbe
        {
            std::uint32_t payloadLength; ///< 负载长度（按类型给合法值，避免先撞上长度判定）
            unsigned char typeValue;     ///< 帧类型
            std::uint32_t streamId;      ///< 流号
        };

        const std::vector<StreamProbe> probes{
                {0, 0x0, 0}, // DATA 必须关联到一条流
                {0, 0x1, 0}, // HEADERS 必须关联到一条流
                {5, 0x2, 0}, // PRIORITY 必须关联到一条流
                {4, 0x3, 0}, // RST_STREAM 必须关联到一条流
                {0, 0x9, 0}, // CONTINUATION 必须关联到一条流
                {0, 0x4, 1}, // SETTINGS 的流号必须为 0
                {8, 0x6, 1}, // PING 的流号必须为 0
                {8, 0x7, 1}, // GOAWAY 的流号必须为 0
        };

        for (const StreamProbe &probe: probes)
        {
            Http2FrameDecoder decoder;
            static_cast<void>(feedAndExpectError(
                    decoder, makeFrameHeaderBytes(probe.payloadLength, probe.typeValue, 0, probe.streamId)));

            EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::ProtocolError)
                    << "类型取值 " << static_cast<unsigned>(probe.typeValue) << " 流号 " << probe.streamId;
        }
    }

    /**
     * @brief padding 越界判错：填充长度必须严格小于帧负载长度（RFC 7540 §6.1）
     */
    TEST(Http2Frame, RejectsInvalidPadding)
    {
        {
            // Pad Length = 5，负载正好只有这 1 个字节：填充没有着落
            Http2FrameDecoder decoder;
            const std::string reason =
                    feedAndExpectError(decoder, makeFrame(Http2FrameType::Data, kHttp2FlagPadded, 1, makeBytes({0x05})));

            EXPECT_TRUE(containsText(reason, "填充")) << reason;
            EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::ProtocolError);
        }
        {
            // 置了 PADDED 却给出空负载：连填充长度字节都没有
            Http2FrameDecoder decoder;
            const std::string reason = feedAndExpectError(decoder, makeFrame(Http2FrameType::Data, kHttp2FlagPadded, 1, ""));

            EXPECT_TRUE(containsText(reason, "PADDED")) << reason;
        }
        {
            // 填充长度比负载还大（0x05 > 4 字节负载）
            Http2FrameDecoder decoder;
            EXPECT_TRUE(containsText(feedAndExpectError(
                                             decoder, makeFrame(Http2FrameType::Data, kHttp2FlagPadded, 1,
                                                                makeBytes({0x05, 0x01, 0x02, 0x03}))),
                                     "填充"));
        }
        {
            // 合法边界：Pad Length = 1、恰好剩 1 字节数据
            Http2FrameDecoder decoder;
            const Http2Frame frame =
                    feedAndTakeFrame(decoder, makeFrame(Http2FrameType::Data, kHttp2FlagPadded, 1, makeBytes({0x01, 'x', 0x00})));

            Http2DataPayload payload;
            ASSERT_TRUE(parseHttp2DataPayload(frame, payload));
            EXPECT_EQ(payload.data, "x");
        }
    }

    /**
     * @brief HEADERS 置了 PRIORITY 位但剥掉 padding 后不足 5 字节判错（RFC 7540 §6.2）
     */
    TEST(Http2Frame, RejectsHeadersWithTruncatedPriorityField)
    {
        Http2FrameDecoder decoder;
        const std::string reason = feedAndExpectError(
                decoder, makeFrame(Http2FrameType::Headers, kHttp2FlagPriority, 1, makeBytes({0x00, 0x00, 0x00, 0x01})));

        EXPECT_TRUE(containsText(reason, "PRIORITY")) << reason;
        EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::FrameSizeError);

        // 带 padding 时优先级字段按「剥掉填充之后」的长度判：3 字节数据 + 2 字节填充仍不足 5
        Http2FrameDecoder paddedDecoder;
        const std::string paddedReason = feedAndExpectError(
                paddedDecoder,
                makeFrame(Http2FrameType::Headers, static_cast<unsigned char>(kHttp2FlagPriority | kHttp2FlagPadded), 1,
                          makeBytes({0x02, 0x00, 0x00, 0x01, 0x00, 0x00})));

        EXPECT_TRUE(containsText(paddedReason, "PRIORITY")) << paddedReason;
    }

    /**
     * @brief 优先级字段依赖自身流号判错（RFC 7540 §5.3.1：流不能依赖自己）
     */
    TEST(Http2Frame, RejectsPriorityDependingOnItsOwnStream)
    {
        // HEADERS：E 位 + 依赖流 3，而帧本身就在流 3 上
        Http2FrameDecoder headersDecoder;
        const std::string headersReason = feedAndExpectError(
                headersDecoder, makeFrame(Http2FrameType::Headers, kHttp2FlagPriority, 3, makeBytes({0x00, 0x00, 0x00, 0x03, 0x10})));

        EXPECT_TRUE(containsText(headersReason, "依赖自己")) << headersReason;
        EXPECT_EQ(headersDecoder.errorKind(), Http2FrameErrorKind::ProtocolError);

        // PRIORITY 帧同理
        Http2FrameDecoder priorityDecoder;
        EXPECT_TRUE(containsText(feedAndExpectError(
                                         priorityDecoder,
                                         makeFrame(Http2FrameType::Priority, 0, 7, makeBytes({0x80, 0x00, 0x00, 0x07, 0x00}))),
                                 "依赖自己"));

        // 编码侧同样拒绝，不把必然被判错的帧发出去
        Http2Priority priority;
        priority.streamDependency = 7;
        EXPECT_THROW(static_cast<void>(encodeHttp2PriorityFrame(priority, 7)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(encodeHttp2PriorityFrame(priority, 0)), Base::InvalidArgumentException);
    }

    /**
     * @brief WINDOW_UPDATE 增量为 0 判错（RFC 7540 §6.9 明文禁止）
     */
    TEST(Http2Frame, RejectsWindowUpdateWithZeroIncrement)
    {
        Http2FrameDecoder decoder;
        const std::string reason =
                feedAndExpectError(decoder, makeFrame(Http2FrameType::WindowUpdate, 0, 0, makeBytes({0x00, 0x00, 0x00, 0x00})));

        EXPECT_TRUE(containsText(reason, "增量")) << reason;
        EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::ProtocolError);

        // 保留位（最高位）按 §6.9 在读取侧被忽略：置位不影响增量的解析
        Http2FrameDecoder reservedBitDecoder;
        Http2WindowUpdatePayload payload;
        ASSERT_TRUE(parseHttp2WindowUpdatePayload(
                feedAndTakeFrame(reservedBitDecoder, makeFrame(Http2FrameType::WindowUpdate, 0, 0, makeBytes({0x80, 0x00, 0x00, 0x01}))),
                payload));
        EXPECT_EQ(payload.windowSizeIncrement, 1U);
    }

    /**
     * @brief 具名负载解析函数在类型不符或帧形态手拼错误时也要能挡住，且出参契约与其它解析器一致
     */
    TEST(Http2Frame, PayloadParsersRejectWrongTypeAndMalformedFrames)
    {
        Http2FrameDecoder decoder;
        const Http2Frame pingFrame = feedAndTakeFrame(decoder, encodeHttp2PingFrame(Http2PingPayload{}));

        Http2SettingsPayload settings;
        Http2WindowUpdatePayload windowUpdate;
        Http2GoAwayPayload goAway;
        Http2RstStreamPayload rstStream;
        std::string reason = "残留";
        EXPECT_FALSE(parseHttp2SettingsPayload(pingFrame, settings, &reason));
        EXPECT_TRUE(containsText(reason, "SETTINGS")) << "类型不符要给出可操作的中文原因：" << reason;
        EXPECT_FALSE(parseHttp2WindowUpdatePayload(pingFrame, windowUpdate, nullptr));
        EXPECT_FALSE(parseHttp2GoAwayPayload(pingFrame, goAway, nullptr));
        EXPECT_FALSE(parseHttp2RstStreamPayload(pingFrame, rstStream, nullptr));

        // 手工构造一个「负载长度不是 6 的整数倍」的 SETTINGS：帧层不会交出这种帧，解析函数自身也得挡住
        Http2Frame malformed;
        malformed.header.type = Http2FrameType::Settings;
        malformed.payload = makeBytes({0x00, 0x01, 0x00});
        reason.clear();
        EXPECT_FALSE(parseHttp2SettingsPayload(malformed, settings, &reason));
        EXPECT_TRUE(containsText(reason, "SETTINGS")) << reason;

        // 出参契约：成功返回时也要把上一次的失败原因清掉，否则调用方会把旧原因当成这一次的
        Http2FrameDecoder okDecoder;
        const Http2Frame emptySettings = feedAndTakeFrame(okDecoder, encodeHttp2SettingsFrame({}));
        ASSERT_TRUE(parseHttp2SettingsPayload(emptySettings, settings, &reason));
        EXPECT_TRUE(reason.empty());
    }

    // ============================================================================
    // 未知帧类型与未定义标志位：必须忽略而不是判错（RFC 7540 §4.1）
    // ============================================================================

    /**
     * @brief 未定义类型（如 0xA、0xF）原样跳过，后续帧照常解出
     * @details RFC 7540 §4.1 要求忽略未知类型：判错会让「未来新增的帧类型」把整条连接打死。
     */
    TEST(Http2Frame, SkipsUnknownFrameTypesInsteadOfFailing)
    {
        // 未知类型只能手拼：编码器拒绝产出未定义类型的帧
        const std::string unknownFrames = makeFrameHeaderBytes(6, 0xA, 0xFF, 0) + "opaque" +
                                          makeFrameHeaderBytes(0, 0xF, 0x00, 0) + encodeHttp2PingFrame(Http2PingPayload{});

        Http2FrameDecoder decoder;
        const std::vector<Http2Frame> frames = decodeAllInOneFeed(decoder, unknownFrames);

        ASSERT_EQ(frames.size(), 3U);
        EXPECT_EQ(static_cast<unsigned>(frames[0].header.type), 0xAU);
        EXPECT_EQ(static_cast<unsigned>(frames[0].header.flags), 0xFFU) << "未定义的标志位按 §4.1 忽略，不判错也不改写";
        EXPECT_EQ(frames[0].payload, "opaque") << "未知类型的负载原样交出";
        EXPECT_TRUE(frames[1].payload.empty());
        EXPECT_EQ(frames[2].header.type, Http2FrameType::Ping);
        EXPECT_FALSE(decoder.hasError());
    }

    /**
     * @brief 已定义类型上的未定义标志位不影响解码（RFC 7540 §4.1：未定义的标志位必须被忽略）
     */
    TEST(Http2Frame, IgnoresUndefinedFlagsOfKnownTypes)
    {
        // PING 上置 0xF0（PING 只定义了 ACK=0x1）、DATA 上置 0x40（DATA 只定义了 END_STREAM 与 PADDED）
        const std::string pingWithStrayFlags = makeFrame(Http2FrameType::Ping, 0xF0, 0, std::string(8, '\x11'));
        const std::string dataWithStrayFlags = makeFrame(Http2FrameType::Data, 0x40, 1, "abc");

        Http2FrameDecoder decoder;
        Http2PingPayload ping;
        ASSERT_TRUE(parseHttp2PingPayload(feedAndTakeFrame(decoder, pingWithStrayFlags), ping));
        EXPECT_FALSE(ping.isAcknowledgement) << "ACK 位没置，其余位置位是无关的";
        EXPECT_EQ(static_cast<unsigned>(ping.opaqueData[0]), 0x11U);

        Http2DataPayload data;
        ASSERT_TRUE(parseHttp2DataPayload(feedAndTakeFrame(decoder, dataWithStrayFlags), data));
        EXPECT_FALSE(data.endStream);
        EXPECT_EQ(data.data, "abc");
    }

    // ============================================================================
    // 增量解码：切分等价、粘滞、复位、产出保护
    // ============================================================================

    /**
     * @brief 任意字节边界都能切开续上：每 1 字节喂入与一次喂入的结果完全相同
     * @details 帧串里混了带 padding 与优先级字段的 HEADERS、带 padding 的 DATA、空负载的 SETTINGS 与
     *          未知类型的帧，因此切点会落在帧头、负载、padding 与优先级字段中间。
     */
    TEST(Http2Frame, ByteByByteFeedingYieldsTheSameFramesAsOneFeed)
    {
        Http2SettingsPayload settings;
        settings.parameters.push_back({static_cast<std::uint16_t>(Http2SettingIdentifier::EnablePush), 1});

        std::string stream;
        stream += encodeHttp2SettingsFrame(settings);
        stream += makeFrame(Http2FrameType::Headers,
                            static_cast<unsigned char>(kHttp2FlagEndHeaders | kHttp2FlagPriority | kHttp2FlagPadded), 5,
                            makeBytes({0x01}) + makeBytes({0x00, 0x00, 0x00, 0x01, 0x03}) + "block" + makeBytes({0x00}));
        stream += makeFrame(Http2FrameType::Data, static_cast<unsigned char>(kHttp2FlagEndStream | kHttp2FlagPadded), 5,
                            makeBytes({0x02}) + "body" + makeBytes({0x00, 0x00}));
        stream += makeFrameHeaderBytes(7, 0xB, 0, 0) + "ignored";
        stream += encodeHttp2PingFrame(Http2PingPayload{});

        Http2FrameDecoder oneFeedDecoder;
        const std::vector<Http2Frame> oneFeedFrames = decodeAllInOneFeed(oneFeedDecoder, stream);

        Http2FrameDecoder byteByByteDecoder;
        const std::vector<Http2Frame> byteByByteFrames = decodeByteByByte(byteByByteDecoder, stream);

        ASSERT_EQ(oneFeedFrames.size(), 5U);
        ASSERT_EQ(byteByByteFrames.size(), oneFeedFrames.size());
        for (std::size_t index = 0; index < oneFeedFrames.size(); ++index)
        {
            EXPECT_EQ(byteByByteFrames[index].header.payloadLength, oneFeedFrames[index].header.payloadLength)
                    << "第 " << index << " 帧";
            EXPECT_EQ(byteByByteFrames[index].header.type, oneFeedFrames[index].header.type) << "第 " << index << " 帧";
            EXPECT_EQ(byteByByteFrames[index].payload, oneFeedFrames[index].payload) << "第 " << index << " 帧";
            EXPECT_EQ(byteByByteFrames[index].hasPriority, oneFeedFrames[index].hasPriority) << "第 " << index << " 帧";
        }

        // 混进去的 HEADERS 必须解出剥掉 padding 与优先级字段的片段
        EXPECT_EQ(oneFeedFrames[1].payload, "block");
        EXPECT_EQ(oneFeedFrames[2].payload, "body");
    }

    /**
     * @brief 需要更多数据时不产出帧，且本次喂入的字节全部被消费；返回 Frame 时只消费本帧字节
     */
    TEST(Http2Frame, NeedMoreConsumesEverythingAndFrameStopsAtItsEnd)
    {
        const std::string firstFrame = encodeHttp2PingFrame(Http2PingPayload{});
        const std::string secondFrame = makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1, "next");
        const std::string bothFrames = firstFrame + secondFrame;

        Http2FrameDecoder decoder;
        // 只喂帧头前 5 字节：切在帧头中间
        EXPECT_EQ(feed(decoder, std::string_view(bothFrames.data(), 5)), Http2FrameDecodeStatus::NeedMore);
        EXPECT_EQ(decoder.consumedByteCount(), 5U);
        EXPECT_FALSE(decoder.hasError());

        // 剩下的字节一次喂完：返回 Frame 时只消费到第一帧末尾
        EXPECT_EQ(decoder.parse(bothFrames.data() + 5, bothFrames.size() - 5), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.consumedByteCount(), firstFrame.size() - 5);
        EXPECT_EQ(decoder.takeFrame().header.type, Http2FrameType::Ping);

        // 第二帧自己单独喂也必须能交出来
        Http2FrameDecoder tailDecoder;
        const std::vector<Http2Frame> tailFrames = decodeByteByByte(tailDecoder, secondFrame);
        ASSERT_EQ(tailFrames.size(), 1U);
        EXPECT_EQ(tailFrames[0].payload, "next");
    }

    /**
     * @brief 零长度负载的帧在帧头收齐那一刻就交付，不需要等到下一次喂字节
     */
    TEST(Http2Frame, EmptySettingsFrameIsDeliveredWithoutExtraByte)
    {
        Http2FrameDecoder decoder;
        Http2SettingsPayload payload;
        payload.isAcknowledgement = true;

        EXPECT_EQ(feed(decoder, encodeHttp2SettingsFrame(payload)), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.consumedByteCount(), kHttp2FrameHeaderByteCount);

        Http2SettingsPayload decoded;
        ASSERT_TRUE(parseHttp2SettingsPayload(decoder.takeFrame(), decoded));
        EXPECT_TRUE(decoded.isAcknowledgement);
        EXPECT_TRUE(decoded.parameters.empty());
    }

    /**
     * @brief 错误态粘滞：判错后不再产出帧、不再消费字节，reset() 才能恢复
     */
    TEST(Http2Frame, ErrorStateIsStickyAndStopsConsumingBytes)
    {
        const std::string validFrame = encodeHttp2PingFrame(Http2PingPayload{});
        Http2FrameDecoder decoder;

        EXPECT_EQ(feed(decoder, makeFrameHeaderBytes(0, 0x6, 0, 1)), Http2FrameDecodeStatus::Error) << "PING 带流号必须判错";

        EXPECT_EQ(feed(decoder, validFrame), Http2FrameDecodeStatus::Error);
        EXPECT_EQ(decoder.consumedByteCount(), 0U) << "错误态下必须一字节不吃";
        EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::ProtocolError);

        // 复位把粘滞错误与累计计数一起清掉，之后按全新的字节流重新解码
        decoder.reset();
        EXPECT_FALSE(decoder.hasError());
        EXPECT_TRUE(decoder.errorMessage().empty());
        EXPECT_EQ(feed(decoder, validFrame), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.takeFrame().header.type, Http2FrameType::Ping);
    }

    /**
     * @brief 半成品帧状态在 reset() 后被清干净，不会影响下一条字节流
     */
    TEST(Http2Frame, ResetClearsPartialFrameState)
    {
        const std::string frame = makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1, "hello");
        Http2FrameDecoder decoder;

        // 先让状态停在「负载收到一半」
        EXPECT_EQ(feed(decoder, std::string_view(frame.data(), kHttp2FrameHeaderByteCount + 2)), Http2FrameDecodeStatus::NeedMore);
        decoder.reset();

        // 若半成品状态没清干净，这里会从错误的阶段继续解析而拿不到帧
        EXPECT_EQ(feedAndTakeFrame(decoder, frame).payload, "hello");
    }

    /**
     * @brief 已产出的帧不会被后续喂入的字节覆盖，且未取走时一字节不吃
     */
    TEST(Http2Frame, PendingFrameIsNotOverwrittenAndConsumesNoByte)
    {
        const std::string firstFrame = makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1, "one");
        const std::string secondFrame = makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1, "two");
        Http2FrameDecoder decoder;

        EXPECT_EQ(feed(decoder, firstFrame), Http2FrameDecodeStatus::Frame);

        // 产出还没取走：再喂什么都只回 Frame，且不消费字节——那些字节属于下一帧
        EXPECT_EQ(feed(decoder, firstFrame + secondFrame), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.consumedByteCount(), 0U);
        EXPECT_EQ(decoder.takeFrame().payload, "one");

        // 取走之后才继续消费：返回 Frame 时消费的正是第一帧的字节数
        EXPECT_EQ(feed(decoder, firstFrame + secondFrame), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.consumedByteCount(), firstFrame.size());
        EXPECT_EQ(decoder.takeFrame().payload, "one");
    }

    /**
     * @brief 还没产出帧就取帧属于用法错误，必须抛异常而不是静默给一个空帧
     */
    TEST(Http2Frame, TakingFrameBeforeItIsReadyThrows)
    {
        Http2FrameDecoder decoder;

        EXPECT_THROW(static_cast<void>(decoder.takeFrame()), Base::LogicException);
        // 用法错误归 std::logic_error 分支，不并入运行期故障的捕获面
        EXPECT_THROW(static_cast<void>(decoder.takeFrame()), std::logic_error);

        // 已经失败的解码器同样没有帧可取：取帧不是「清错误」的手段
        static_cast<void>(feed(decoder, makeFrameHeaderBytes(0, 0x6, 0, 1)));
        EXPECT_THROW(static_cast<void>(decoder.takeFrame()), Base::LogicException);
    }

    // ============================================================================
    // 累计读取上限：本端策略，触发后按 ENHANCE_YOUR_CALM 收场
    // ============================================================================

    /**
     * @brief 累计读取上限被突破时判错，且错误类别与「对端违规」可区分
     * @details 上限是防「长连接上无限读下去」的兜底闸门，考察的是本端策略而不是对端违规，因此单列
     *          LimitExceeded 一档（对应 ENHANCE_YOUR_CALM 而不是 PROTOCOL_ERROR）。
     */
    TEST(Http2Frame, TotalConsumedLimitTriggersEnhanceYourCalm)
    {
        const std::string twoFrames = makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1, "abcdef") +
                                      makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, 1, "ghijkl");

        // 上限 20 字节：第一帧（15 字节）收得下，第二帧的帧头读到第 6 个字节时越界
        Http2FrameDecoder decoder(Http2FrameLimits{kHttp2DefaultMaximumFrameSize, 20});
        EXPECT_EQ(feed(decoder, std::string_view(twoFrames.data(), 15)), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.totalConsumedByteCount(), 15U);
        EXPECT_EQ(decoder.takeFrame().payload, "abcdef");

        const std::string reason = feedAndExpectError(decoder, std::string_view(twoFrames.data() + 15, twoFrames.size() - 15));
        EXPECT_TRUE(containsText(reason, "上限")) << reason;
        EXPECT_EQ(decoder.errorKind(), Http2FrameErrorKind::LimitExceeded);
        EXPECT_EQ(toHttp2ErrorCode(decoder.errorKind()), Http2ErrorCode::EnhanceYourCalm);
        EXPECT_EQ(decoder.totalConsumedByteCount(), 20U) << "停在限额处不再往下读";

        // 复位把累计计数一起清零，之后的字节流按新预算重新计
        decoder.reset();
        EXPECT_EQ(decoder.totalConsumedByteCount(), 0U);
        EXPECT_EQ(feed(decoder, makeFrame(Http2FrameType::Ping, 0, 0, std::string(8, '\x00'))), Http2FrameDecodeStatus::Frame);
    }

    /**
     * @brief 缺省不限：累计计数照样统计，只是不会因为体量被拒
     */
    TEST(Http2Frame, TotalConsumedIsUnlimitedByDefault)
    {
        Http2FrameDecoder decoder;
        EXPECT_EQ(decoder.totalConsumedByteCount(), 0U);

        const std::string frame = encodeHttp2PingFrame(Http2PingPayload{});
        EXPECT_EQ(feed(decoder, frame), Http2FrameDecodeStatus::Frame);
        EXPECT_EQ(decoder.totalConsumedByteCount(), frame.size());
        EXPECT_FALSE(decoder.hasError());
    }
} // namespace AsynGyanis::Net
