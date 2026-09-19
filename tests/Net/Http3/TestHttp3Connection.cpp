/**
 * 覆盖 HTTP/3 协议状态机：流的分类、控制流规则、帧交错、头部判定接线、QPACK 阻塞与额度归还。
 * 载体是一个假传输层——开流口给号、写出口攒字节、额度口记账，因此状态机全程在内存里跑完，
 * 不碰 socket 也不碰事件循环。字节级的跨实现对照在 TestHttp3Session 的 nghttp3 真字节用例里。
 */

#include "Net/Http3/Http3Connection.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Net::Http3Connection;
    using AsynGyanis::Net::Http3ErrorCode;
    using AsynGyanis::Net::Http3Frame;
    using AsynGyanis::Net::QpackEncoder;
    using AsynGyanis::Net::QpackHeaderField;

    /// 假传输层：记录三件事——开出过哪些流、每条流写过什么字节、归还过多少额度
    class FakeTransport
    {
    public:
        /// 本端发起的单向流号按 RFC 9000 §2.1 是 3 (mod 4)
        std::int64_t openUnidirectionalStream()
        {
            const std::int64_t streamId = nextUnidirectionalStreamId;
            nextUnidirectionalStreamId += 4;
            openedUnidirectionalStreamIds.push_back(streamId);
            return streamId;
        }

        void write(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
        {
            writtenByteCounts[streamId] += data.size();
            outboundBytes[streamId].append(reinterpret_cast<const char *>(data.data()), data.size());
            if (endStream)
            {
                endedStreams.push_back(streamId);
            }
            writeCallCount++;
        }

        void credit(const std::int64_t streamId, const std::size_t byteCount)
        {
            creditedByteCounts[streamId] += byteCount;
        }

        [[nodiscard]] std::string bytesOf(const std::int64_t streamId) const
        {
            const auto entry = outboundBytes.find(streamId);
            return entry == outboundBytes.end() ? std::string{} : entry->second;
        }

        [[nodiscard]] std::size_t creditedOf(const std::int64_t streamId) const
        {
            const auto entry = creditedByteCounts.find(streamId);
            return entry == creditedByteCounts.end() ? 0 : entry->second;
        }

        [[nodiscard]] std::size_t writtenOf(const std::int64_t streamId) const
        {
            const auto entry = writtenByteCounts.find(streamId);
            return entry == writtenByteCounts.end() ? 0 : entry->second;
        }

        std::vector<std::int64_t> openedUnidirectionalStreamIds{};
        std::vector<std::int64_t> endedStreams{};
        std::map<std::int64_t, std::string> outboundBytes{};
        std::map<std::int64_t, std::size_t> writtenByteCounts{};
        std::map<std::int64_t, std::size_t> creditedByteCounts{};
        int writeCallCount{0};
        bool isCrediterProvided{true};

    private:
        std::int64_t nextUnidirectionalStreamId{3};
    };

    /// 会话侧收到的事件流水：只记「哪条流上发生了什么」，字段内容原样存下来供断言
    struct EventLog
    {
        struct HeaderField
        {
            std::int64_t streamId{0};
            std::string name;
            std::string value;
        };

        std::vector<HeaderField> headerFields{};
        std::vector<std::int64_t> headerBlocksReceived{};
        std::vector<std::int64_t> trailerBlocksReceived{};
        std::string bodyBytes{};
        std::vector<std::int64_t> requestsEnded{};
        std::vector<std::int64_t> streamsClosed{};
        std::vector<std::pair<std::int64_t, Http3ErrorCode>> streamsReset{};
        std::vector<std::pair<std::int64_t, std::string>> malformedRequests{};
        std::vector<std::pair<Http3ErrorCode, std::string>> connectionClosures{};
    };

    /// 建一个接好假传输层的协议层：三条本端单向流在构造里就开出来
    std::unique_ptr<Http3Connection> makeConnection(FakeTransport &transport, EventLog &events,
                                                    Http3Connection::LocalSettings settings = {})
    {
        Http3Connection::Callbacks callbacks;
        callbacks.onHeaderField = [&events](const std::int64_t streamId, const std::string_view name, const std::string_view value)
        {
            events.headerFields.push_back(EventLog::HeaderField{.streamId = streamId, .name = std::string(name), .value = std::string(value)});
        };
        callbacks.onHeaderBlockReceived = [&events](const std::int64_t streamId, const bool isTrailers)
        {
            if (isTrailers)
            {
                events.trailerBlocksReceived.push_back(streamId);
            }
            else
            {
                events.headerBlocksReceived.push_back(streamId);
            }
        };
        callbacks.onBodyBytes = [&events](const std::int64_t streamId, const std::span<const std::uint8_t> bytes)
        {
            static_cast<void>(streamId);
            events.bodyBytes.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        };
        callbacks.onRequestEnded = [&events](const std::int64_t streamId) { events.requestsEnded.push_back(streamId); };
        callbacks.onStreamClosed = [&events](const std::int64_t streamId) { events.streamsClosed.push_back(streamId); };
        callbacks.onStreamReset = [&events](const std::int64_t streamId, const Http3ErrorCode errorCode)
        {
            events.streamsReset.emplace_back(streamId, errorCode);
        };
        callbacks.onMalformedRequest = [&events](const std::int64_t streamId, const std::string_view reason)
        {
            events.malformedRequests.emplace_back(streamId, std::string(reason));
        };
        callbacks.onConnectionClosed = [&events](const Http3ErrorCode errorCode, const std::string_view reason)
        {
            events.connectionClosures.emplace_back(errorCode, std::string(reason));
        };

        return std::make_unique<Http3Connection>(
                [&transport]() { return transport.openUnidirectionalStream(); },
                [&transport](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
                { transport.write(streamId, data, endStream); },
                [&transport](const std::int64_t streamId, const std::size_t byteCount) { transport.credit(streamId, byteCount); },
                std::move(callbacks), settings);
    }

    /// 把字符串按字节交给状态机（它只认「指针 + 长度」）
    [[nodiscard]] std::span<const std::uint8_t> bytesOfText(const std::string &text)
    {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
    }

    /// 单向流开头的类型前缀
    [[nodiscard]] std::string streamTypePrefix(const std::uint64_t streamType)
    {
        // 测试用到的类型都落在单字节档里（0x00..0x03），直接一个字节写出
        return std::string(1, static_cast<char>(streamType));
    }

    /// 对端（客户端）发起的单向流号：控制流 2、编码器流 6、解码器流 10（≡ 2 mod 4）
    constexpr std::int64_t kPeerControlStreamId = 2;
    constexpr std::int64_t kPeerEncoderStreamId = 6;
    constexpr std::int64_t kPeerDecoderStreamId = 10;
    constexpr std::int64_t kRequestStreamId = 0;

    /**
     * @brief 用一个独立的编码器产出头段字节
     * @details 这里的目的是给状态机喂「结构正确的字节」，QPACK 自身的字节正确性由 TestQpack 与
     *          TestHttp3Session 里对 nghttp3 的真字节对拍负责，本文件不重复那份判据。
     */
    [[nodiscard]] std::string encodeSection(std::vector<QpackHeaderField> fields, std::string &encoderStreamBytes)
    {
        // 刻意用一个「对端不接动态表」的编码器：产出的头段 Required Insert Count 为 0，
        // 不需要编码器流指令就能直接解，于是本文件能把「交付路径」单独测清楚。
        // 动态表引用与阻塞-续解那条路径由 BlockedFieldSectionIsDeliveredOnceTheEncoderStreamCatchesUp 覆盖
        QpackEncoder encoder(0, 0, 0);
        std::string headerBlock;
        const auto encoded = encoder.encodeFieldSection(static_cast<std::uint64_t>(kRequestStreamId), std::span<const QpackHeaderField>(fields),
                                                        headerBlock, encoderStreamBytes);
        EXPECT_TRUE(encoded.has_value()) << encoded.error().message;
        return headerBlock;
    }

    /// 一个最小合法请求的头字段
    [[nodiscard]] std::vector<QpackHeaderField> minimalRequestFields()
    {
        return {
                QpackHeaderField{":method", "GET"},
                QpackHeaderField{":scheme", "https"},
                QpackHeaderField{":authority", "example.com"},
                QpackHeaderField{":path", "/json"},
                QpackHeaderField{"accept", "*/*"},
        };
    }

    /// 帧的「类型 + 长度 + 载荷」手工拼法：只用于本文件里构造合法帧
    [[nodiscard]] std::string makeFrame(const std::uint64_t frameType, const std::string &payload)
    {
        std::string bytes;
        bytes.push_back(static_cast<char>(frameType));
        bytes.push_back(static_cast<char>(payload.size())); // 测试里的载荷都不到 64 字节，单字节档够用
        bytes += payload;
        return bytes;
    }
} // namespace

TEST(Http3Connection, OpensThreeUnidirectionalStreamsAndStartsControlWithSettings)
{
    FakeTransport transport;
    EventLog events;
    const auto connection = makeConnection(transport, events);

    ASSERT_TRUE(connection->isUsable());
    // 控制流、编码器流、解码器流：顺序按 RFC 9114 §6.2.1 与 RFC 9204 §4.2
    ASSERT_EQ(transport.openedUnidirectionalStreamIds.size(), 3u);
    EXPECT_EQ(transport.openedUnidirectionalStreamIds[0], 3);
    EXPECT_EQ(transport.openedUnidirectionalStreamIds[1], 7);
    EXPECT_EQ(transport.openedUnidirectionalStreamIds[2], 11);

    const std::string controlBytes = transport.bytesOf(3);
    ASSERT_FALSE(controlBytes.empty());
    EXPECT_EQ(static_cast<unsigned char>(controlBytes[0]), 0x00) << "控制流的类型前缀必须是 0x00";
    // 前缀之后紧跟 SETTINGS 帧：类型 0x04，且它必须排在任何别的帧之前
    EXPECT_EQ(static_cast<unsigned char>(controlBytes[1]), 0x04) << "控制流的第一个帧必须是 SETTINGS";
    // 长度按「帧头 2 字节 + 载荷」自洽核对，而不是钉一个手算数字：四项设置里
    // MAX_FIELD_SECTION_SIZE=65536 走四字节档，手算最容易在这里错一位
    const std::size_t settingsPayloadByteCount = static_cast<unsigned char>(controlBytes[2]);
    EXPECT_EQ(controlBytes.size(), 3u + settingsPayloadByteCount) << "SETTINGS 声明的长度要正好盖住首帧";
    EXPECT_GT(settingsPayloadByteCount, 11u) << "四项已知设置合起来至少 12 字节";
    EXPECT_NE(controlBytes.find(std::string("\x01\x50\x00", 3), 0), std::string::npos) << "应公布 QPACK 动态表容量 4096（两字节档 0x5000）";
    EXPECT_NE(controlBytes.find("\x08\x01", 0), std::string::npos) << "应声明支持扩展 CONNECT";
    EXPECT_EQ(static_cast<unsigned char>(transport.bytesOf(7)[0]), 0x02) << "编码器流前缀";
    EXPECT_EQ(static_cast<unsigned char>(transport.bytesOf(11)[0]), 0x03) << "解码器流前缀";
}

TEST(Http3Connection, MissingOpenerOrWriterMakesTheSessionUnusableInsteadOfThrowing)
{
    // 开流口为空属装配错误：只让协议层不可用，不抛异常把整条连接拖垮
    Http3Connection connectionWithoutStreams(nullptr, [](std::int64_t, std::span<const std::uint8_t>, bool) {}, nullptr, {});
    EXPECT_FALSE(connectionWithoutStreams.isUsable());
    EXPECT_FALSE(connectionWithoutStreams.isBroken()) << "建不起来不等于协议错，不该上报连接错误码";
}

TEST(Http3Connection, PeerSettingsEnableTheDynamicTableWithinTheLocalCeiling)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);

    // 对端公布动态表容量 4096（两字节档 0x5000）：本端编码器生效容量取 min(4096, 自家 4096)
    const std::string settingsPayload = makeFrame(0x04, std::string("\x01\x50\x00", 3));
    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + settingsPayload), false);

    EXPECT_FALSE(connection->isBroken());
    EXPECT_EQ(connection->peerTableCapacityByteCount(), 4096u);
}

TEST(Http3Connection, RequestHeadIsDeliveredFieldByFieldAndReportedAsOneBlock)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    std::string encoderBytes;
    const std::string headerBlock = encodeSection(minimalRequestFields(), encoderBytes);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), true);

    ASSERT_EQ(events.headerFields.size(), 5u) << "伪头要原样交给会话，由它做业务映射";
    EXPECT_EQ(events.headerFields[0].name, ":method");
    EXPECT_EQ(events.headerFields[0].value, "GET");
    EXPECT_EQ(events.headerFields[4].name, "accept");
    ASSERT_EQ(events.headerBlocksReceived.size(), 1u);
    EXPECT_EQ(events.headerBlocksReceived[0], kRequestStreamId);
    // END_STREAM 与头段同趟到达、且没声明 content-length：请求此刻收全
    ASSERT_EQ(events.requestsEnded.size(), 1u);
    EXPECT_TRUE(events.malformedRequests.empty());
}

TEST(Http3Connection, UppercaseFieldNameMakesTheRequestHeadRejected)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    auto fields = minimalRequestFields();
    fields.back().name = "Accept"; // 大写：RFC 9114 §4.2 判畸形
    std::string encoderBytes;
    const std::string headerBlock = encodeSection(fields, encoderBytes);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), false);

    ASSERT_EQ(events.malformedRequests.size(), 1u) << "畸形头段要交回会话按错误响应处置，而不是打死连接";
    EXPECT_TRUE(events.headerFields.empty()) << "整段判定过了才逐字段交出，判定失败一个都不许漏给上层";
    EXPECT_FALSE(connection->isBroken());
    // 会话可以照常在这条流上作答（RFC 9114 §4.1.2 允许先答再收流）
    const std::vector<QpackHeaderField> responseFields{QpackHeaderField{":status", "400"}};
    EXPECT_TRUE(connection->submitResponseHead(kRequestStreamId, responseFields, true).has_value());
}

TEST(Http3Connection, BodyBeforeHeadersIsRejectedAsUnexpectedFrameOnThatStreamOnly)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "abc")), false);

    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::FrameUnexpected);
    EXPECT_FALSE(connection->isBroken()) << "另一条流与整条连接都还健康";
}

TEST(Http3Connection, ContentLengthDisagreeingWithBodyEndsTheStreamAsMessageError)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    auto fields = minimalRequestFields();
    fields.emplace_back("content-length", "5");
    std::string encoderBytes;
    const std::string headerBlock = encodeSection(fields, encoderBytes);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "abc")), true); // 声明 5，实收 3

    EXPECT_TRUE(events.requestsEnded.empty()) << "对账不过就不算收全，不能派发业务";
    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::MessageError);
}

TEST(Http3Connection, BodyBytesAreCreditedByTheReceiverNotTheProtocolLayer)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    std::string encoderBytes;
    const std::string headerBlock = encodeSection(minimalRequestFields(), encoderBytes);

    // 头段：帧头 2 字节可立刻还，载荷已被解码器消费也一并还（它不再回到流上）
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), false);
    const std::size_t creditedAfterHead = transport.creditedOf(kRequestStreamId);
    EXPECT_EQ(creditedAfterHead, headerBlock.size() + 2) << "头段的帧头与载荷都算已消费";

    // 正文：DATA 载荷到达即还等于没有背压，因此一分都不还；但 DATA 帧自己的 2 字节帧头
    // 已经被本协议层消费掉，那部分额度要还（否则窗口会被帧头一点点吃光）
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "hello world")), false);
    EXPECT_EQ(transport.creditedOf(kRequestStreamId), creditedAfterHead + 2) << "只多还了一个 DATA 帧头的额度";
    EXPECT_EQ(events.bodyBytes, "hello world");
}

TEST(Http3Connection, ResponseHeadAndBodyBecomeOneHeadersFrameThenOneDataFrame)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    std::string encoderBytes;
    const std::string headerBlock = encodeSection(minimalRequestFields(), encoderBytes);
    // GET 没有正文：头段这一趟就带上 END_STREAM，于是本端收尾之后两侧都完成，该发流关闭通知
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), true);

    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}}, false).has_value());
    EXPECT_FALSE(connection->isLocalStreamFinished(kRequestStreamId));
    connection->flush(); // 待发字节要过一次 flush 才交给传输层
    const std::string afterHead = transport.bytesOf(kRequestStreamId);
    EXPECT_EQ(static_cast<unsigned char>(afterHead[0]), 0x01) << "响应先出一个 HEADERS 帧";

    ASSERT_TRUE(connection->appendResponseBody(kRequestStreamId, bytesOfText("ok"), false).has_value());
    connection->flush();
    const std::string afterBody = transport.bytesOf(kRequestStreamId);
    EXPECT_NE(afterBody.find("\x00\x02ok", 0), std::string::npos) << "正文单独成帧：类型 0x00、长度 2";

    ASSERT_TRUE(connection->appendResponseBody(kRequestStreamId, {}, true).has_value());
    connection->flush();
    EXPECT_TRUE(connection->isLocalStreamFinished(kRequestStreamId));
    // 对端还没收尾（不会自己 END_STREAM 的 POST 除外）：本端收尾即两侧完成，会话据此回收状态
    EXPECT_EQ(events.streamsClosed.size(), 1u) << "对端已 END_STREAM 且本端已收尾时应发流关闭通知";
}

TEST(Http3Connection, SubmittingToACancelledStreamOnlyVoidansThatResponse)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    std::string encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);

    connection->noteStreamCancelledByPeer(kRequestStreamId);
    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::RequestCancelled);

    const auto submitted = connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}}, true);
    EXPECT_FALSE(submitted.has_value()) << "流没了就该拒绝提交，让会话只作废这一条响应";
    EXPECT_FALSE(connection->isBroken());
    EXPECT_EQ(transport.writtenOf(kRequestStreamId), 0u) << "已排的字节也要丢掉";
}

TEST(Http3Connection, UnknownUnidirectionalStreamTypeIsDiscardedButStillCredited)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);

    // 类型 0x41：本端不认识（RFC 9114 §6.2.1 要求忽略而不是报错）
    connection->consumeStreamData(14, bytesOfText(std::string("\x41", 1) + std::string(20, 'x')), false);

    EXPECT_FALSE(connection->isBroken());
    EXPECT_EQ(transport.creditedOf(14), 21u) << "丢弃的字节同样要还额度，否则对端会卡在自己耗尽的窗口上";
    EXPECT_TRUE(events.connectionClosures.empty());
}

TEST(Http3Connection, DuplicateControlStreamBreaksTheConnection)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), false);
    // 第二条控制流：关键流重复（RFC 9114 §6.2.1）
    connection->consumeStreamData(18, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), false);

    EXPECT_TRUE(connection->isBroken());
    EXPECT_EQ(connection->connectionErrorCode(), Http3ErrorCode::StreamCreationError);
    ASSERT_EQ(events.connectionClosures.size(), 1u);
    EXPECT_EQ(events.connectionClosures[0].first, Http3ErrorCode::StreamCreationError);
    EXPECT_EQ(events.connectionClosures[0].second, connection->connectionErrorReason()) << "通知里的原因与连接上记的原因是同一份";
}

TEST(Http3Connection, ControlStreamClosedEarlyBreaksTheConnection)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), true);

    EXPECT_TRUE(connection->isBroken());
    EXPECT_EQ(connection->connectionErrorCode(), Http3ErrorCode::ClosedCriticalStream);
}

TEST(Http3Connection, SettingsOnRequestStreamFailsOnlyThatStream)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x04, "")), false);

    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::FrameUnexpected);
    EXPECT_FALSE(connection->isBroken());
}

TEST(Http3Connection, BlockedFieldSectionIsDeliveredOnceTheEncoderStreamCatchesUp)
{
    FakeTransport transport;
    EventLog events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection = makeConnection(transport, events, settings);

    // 对端先把 SETTINGS 与「动态表插入」分两趟送：头段先到、指令后到，就构成 §2.2.1 的阻塞
    std::string encoderBytes;
    std::string headerBlock;
    {
        QpackEncoder peerEncoder(4096, 100, 4096);
        // 第一次编码即插入：产出引用动态表的头段与对应指令
        const auto first = peerEncoder.encodeFieldSection(0, std::span<const QpackHeaderField>(minimalRequestFields()), headerBlock,
                                                         encoderBytes);
        ASSERT_TRUE(first.has_value()) << first.error().message;
    }
    ASSERT_FALSE(encoderBytes.empty()) << "对端确实插了动态表，这条用例的前提才成立";

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
    // 头段先到：此刻本端还没收到指令，必须挂起而不是报错
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), false);
    EXPECT_TRUE(events.headerBlocksReceived.empty()) << "内容没齐就不该把半份请求交上去";

    // 指令到达之后自动续解
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(encoderBytes), false);
    EXPECT_EQ(events.headerBlocksReceived.size(), 1u) << "补齐之后要把挂起的头段解出来交给上层";
    EXPECT_FALSE(connection->isBroken());
}

TEST(Http3Connection, FlushHandsEveryStreamItsOwnWriteAndDrainsTheQueue)
{
    FakeTransport transport;
    EventLog events;
    auto connection = makeConnection(transport, events);
    std::string encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);

    const int writesBefore = transport.writeCallCount;
    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "204"}}, true).has_value());
    connection->flush();

    EXPECT_GT(transport.writeCallCount, writesBefore) << "响应字节要能交给传输层";
    EXPECT_EQ(connection->pendingOutputByteCount(kRequestStreamId), 0u) << "一次 flush 就该把这条流排空";
    EXPECT_TRUE(connection->isLocalStreamFinished(kRequestStreamId));
}

TEST(Http3Connection, ExtendedConnectHeadIsAcceptedOnlyWhenWeAdvertiseSupport)
{
    FakeTransport transport;
    EventLog events;
    Http3Connection::LocalSettings withoutExtended;
    withoutExtended.isExtendedConnectEnabled = false;
    auto strictConnection = makeConnection(transport, events, withoutExtended);

    std::string encoderBytes;
    const std::vector<QpackHeaderField> tunnelFields{QpackHeaderField{":method", "CONNECT"}, QpackHeaderField{":scheme", "https"},
                                                     QpackHeaderField{":authority", "example.com"}, QpackHeaderField{":path", "/ws"},
                                                     QpackHeaderField{":protocol", "websocket"}};
    strictConnection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(tunnelFields, encoderBytes))), false);
    EXPECT_EQ(events.malformedRequests.size(), 1u) << "没声明 ENABLE_CONNECT_PROTOCOL 就不该收下 :protocol";

    FakeTransport permittingTransport;
    EventLog permittingEvents;
    auto permittingConnection = makeConnection(permittingTransport, permittingEvents);
    permittingConnection->consumeStreamData(4, bytesOfText(makeFrame(0x01, encodeSection(tunnelFields, encoderBytes))), false);
    EXPECT_TRUE(permittingEvents.malformedRequests.empty());
    EXPECT_EQ(permittingEvents.headerBlocksReceived.size(), 1u);
}
