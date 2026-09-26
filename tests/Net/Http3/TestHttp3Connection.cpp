/**
 * 覆盖 HTTP/3 协议状态机：流的分类、控制流规则、帧交错、头部判定接线、QPACK 阻塞与额度归还。
 * 载体是一个假传输层——开流口给号、写出口攒字节、额度口记账，因此状态机全程在内存里跑完，
 * 不碰 socket 也不碰事件循环。字节级的跨实现对照在进程外做（scripts/h3_acceptance.py 用 aioquic）。
 */

#include "Net/Http3/Http3Connection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <random>
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
    using AsynGyanis::Net::QpackErrorKind;
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

        /**
         * @brief 收下一段待发字节，累计到 pendingQueueByteLimit 为止
         * @details 口径与真实流层一致：上界说的是「这条流的队列里现在最多压多少字节」，不是
         *          「这一次调用允许多少字节」。同一轮 flush 里续交第二次才会拿到 0
         * @return std::size_t 被收下的字节数——余下的仍归调用方
         */
        std::size_t write(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
        {
            const std::size_t writtenSoFarByteCount = writtenByteCounts[streamId];
            const std::size_t roomByteCount         = pendingQueueByteLimit > writtenSoFarByteCount ? pendingQueueByteLimit - writtenSoFarByteCount : 0;
            const std::size_t acceptedByteCount     = std::min(data.size(), roomByteCount);
            writtenByteCounts[streamId]             = writtenSoFarByteCount + acceptedByteCount;
            outboundBytes[streamId].append(reinterpret_cast<const char *>(data.data()), acceptedByteCount);
            // 收尾只随整段收下一起落定（真实流层就是这么判的：半段就 FIN 等于截断正文还骗对端发完了）
            if (endStream && acceptedByteCount == data.size())
            {
                endedStreams.push_back(streamId);
            }
            writeCallCount++;
            return acceptedByteCount;
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

        /// 所有流上已交给传输层的字节总数：用来钉「作废之后不再长待发内容」这条不变式
        [[nodiscard]] std::size_t totalWrittenByteCount() const
        {
            std::size_t total = 0;
            for (const auto &[streamId, byteCount]: writtenByteCounts)
            {
                total += byteCount;
            }
            return total;
        }

        std::vector<std::int64_t>           openedUnidirectionalStreamIds{};
        std::vector<std::int64_t>           endedStreams{};
        std::map<std::int64_t, std::string> outboundBytes{};
        std::map<std::int64_t, std::size_t> writtenByteCounts{};
        std::map<std::int64_t, std::size_t> creditedByteCounts{};
        int                                 writeCallCount{0};
        bool                                isCrediterProvided{true};
        /// 这条流在假传输层的待发队列里最多压多少字节：默认无限（全收），调到已交出的量即「队列已满」
        std::size_t pendingQueueByteLimit{std::numeric_limits<std::size_t>::max()};

    private:
        std::int64_t nextUnidirectionalStreamId{3};
    };

    /// 会话侧收到的事件流水：只记「哪条流上发生了什么」，字段内容原样存下来供断言
    struct EventLog
    {
        struct HeaderField
        {
            std::int64_t streamId{0};
            std::string  name;
            std::string  value;
            bool         isTrailers{false}; ///< 该字段来自尾段还是头段：会话据此决定落哪一档
        };

        std::vector<HeaderField>                             headerFields{};
        std::vector<std::int64_t>                            headerBlocksReceived{};
        std::vector<std::int64_t>                            trailerBlocksReceived{};
        std::string                                          bodyBytes{};
        std::vector<std::int64_t>                            requestsEnded{};
        std::vector<std::int64_t>                            streamsClosed{};
        std::vector<std::pair<std::int64_t, Http3ErrorCode>> streamsReset{};
        std::vector<std::pair<std::int64_t, std::string>>    malformedRequests{};
        std::vector<std::pair<Http3ErrorCode, std::string>>  connectionClosures{};
    };

    /// 建一个接好假传输层的协议层：三条本端单向流在构造里就开出来
    std::unique_ptr<Http3Connection> makeConnection(FakeTransport &transport, EventLog &events, Http3Connection::LocalSettings settings = {})
    {
        Http3Connection::Callbacks callbacks;
        callbacks.onHeaderField = [&events](const std::int64_t streamId, const std::string_view name, const std::string_view value, const bool isTrailers)
        { events.headerFields.push_back(EventLog::HeaderField{.streamId = streamId, .name = std::string(name), .value = std::string(value), .isTrailers = isTrailers}); };
        callbacks.onHeaderBlockReceived = [&events](const std::int64_t streamId, const bool isTrailers)
        {
            if (isTrailers)
            {
                events.trailerBlocksReceived.push_back(streamId);
            } else
            {
                events.headerBlocksReceived.push_back(streamId);
            }
        };
        callbacks.onBodyBytes = [&events](const std::int64_t streamId, const std::span<const std::uint8_t> bytes)
        {
            static_cast<void>(streamId);
            events.bodyBytes.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        };
        callbacks.onRequestEnded     = [&events](const std::int64_t streamId) { events.requestsEnded.push_back(streamId); };
        callbacks.onStreamClosed     = [&events](const std::int64_t streamId) { events.streamsClosed.push_back(streamId); };
        callbacks.onStreamReset      = [&events](const std::int64_t streamId, const Http3ErrorCode errorCode) { events.streamsReset.emplace_back(streamId, errorCode); };
        callbacks.onMalformedRequest = [&events](const std::int64_t streamId, const std::string_view reason)
        { events.malformedRequests.emplace_back(streamId, std::string(reason)); };
        callbacks.onConnectionClosed = [&events](const Http3ErrorCode errorCode, const std::string_view reason)
        { events.connectionClosures.emplace_back(errorCode, std::string(reason)); };

        return std::make_unique<Http3Connection>([&transport]() { return transport.openUnidirectionalStream(); },
                                                 [&transport](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
                                                 { return transport.write(streamId, data, endStream); }, [&transport](const std::int64_t streamId, const std::size_t byteCount)
                                                 { transport.credit(streamId, byteCount); }, std::move(callbacks), settings);
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
    constexpr std::int64_t kRequestStreamId     = 0;

    /**
     * @brief 用一个独立的编码器产出头段字节
     * @details 这里的目的是给状态机喂「结构正确的字节」，QPACK 自身的字节正确性由 TestQpack 的逐字节
     *          断言与进程外的 aioquic 探针负责，本文件不重复那份判据。
     */
    [[nodiscard]] std::string encodeSection(std::vector<QpackHeaderField> fields, std::string &encoderStreamBytes)
    {
        // 刻意用一个「对端不接动态表」的编码器：产出的头段 Required Insert Count 为 0，
        // 不需要编码器流指令就能直接解，于是本文件能把「交付路径」单独测清楚。
        // 动态表引用与阻塞-续解那条路径由 BlockedFieldSectionIsDeliveredOnceTheEncoderStreamCatchesUp 覆盖
        QpackEncoder encoder(0, 0, 0);
        std::string  headerBlock;
        const auto   encoded = encoder.encodeFieldSection(static_cast<std::uint64_t>(kRequestStreamId), std::span<const QpackHeaderField>(fields), headerBlock, encoderStreamBytes);
        EXPECT_TRUE(encoded.has_value()) << encoded.error().message;
        return headerBlock;
    }

    /// 一个最小合法请求的头字段
    [[nodiscard]] std::vector<QpackHeaderField> minimalRequestFields()
    {
        return {
                QpackHeaderField{":method", "GET"}, QpackHeaderField{":scheme", "https"}, QpackHeaderField{":authority", "example.com"},
                QpackHeaderField{":path", "/json"}, QpackHeaderField{"accept", "*/*"},
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
    EventLog      events;
    const auto    connection = makeConnection(transport, events);

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
    Http3Connection connectionWithoutStreams(nullptr, [](std::int64_t, std::span<const std::uint8_t>, bool) -> std::size_t { return 0; }, nullptr, {});
    EXPECT_FALSE(connectionWithoutStreams.isUsable());
    EXPECT_FALSE(connectionWithoutStreams.isBroken()) << "建不起来不等于协议错，不该上报连接错误码";
}

TEST(Http3Connection, PeerSettingsEnableTheDynamicTableWithinTheLocalCeiling)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    // 对端公布动态表容量 4096（两字节档 0x5000）：本端编码器生效容量取 min(4096, 自家 4096)
    const std::string settingsPayload = makeFrame(0x04, std::string("\x01\x50\x00", 3));
    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + settingsPayload), false);

    EXPECT_FALSE(connection->isBroken());
    EXPECT_EQ(connection->peerTableCapacityByteCount(), 4096u);
}

TEST(Http3Connection, RequestHeadIsDeliveredFieldByFieldAndReportedAsOneBlock)
{
    FakeTransport     transport;
    EventLog          events;
    auto              connection = makeConnection(transport, events);
    std::string       encoderBytes;
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
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    auto          fields     = minimalRequestFields();
    fields.back().name       = "Accept"; // 大写：RFC 9114 §4.2 判畸形
    std::string       encoderBytes;
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
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "abc")), false);

    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::FrameUnexpected);
    EXPECT_FALSE(connection->isBroken()) << "另一条流与整条连接都还健康";
}

TEST(Http3Connection, ContentLengthDisagreeingWithBodyEndsTheStreamAsMessageError)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    auto          fields     = minimalRequestFields();
    fields.emplace_back("content-length", "5");
    std::string       encoderBytes;
    const std::string headerBlock = encodeSection(fields, encoderBytes);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "abc")), true); // 声明 5，实收 3

    EXPECT_TRUE(events.requestsEnded.empty()) << "对账不过就不算收全，不能派发业务";
    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::MessageError);
}

TEST(Http3Connection, BodyBytesAreCreditedByTheReceiverNotTheProtocolLayer)
{
    FakeTransport     transport;
    EventLog          events;
    auto              connection = makeConnection(transport, events);
    std::string       encoderBytes;
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
    FakeTransport     transport;
    EventLog          events;
    auto              connection = makeConnection(transport, events);
    std::string       encoderBytes;
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
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    std::string   encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);

    connection->noteStreamCancelledByPeer(kRequestStreamId);
    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::RequestCancelled);

    const auto submitted = connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}}, true);
    EXPECT_FALSE(submitted.has_value()) << "流没了就该拒绝提交，让会话只作废这一条响应";
    EXPECT_FALSE(connection->isBroken());
    EXPECT_EQ(transport.writtenOf(kRequestStreamId), 0u) << "已排的字节也要丢掉";
}

/**
 * @brief 传输层只收下一半时，余下的字节留在协议层缓冲里续交，既不截断也不重复
 * @details 对端长期不授窗口时传输层的待发队列会到界并拒收，本层必须留住那段字节等下一次 flush。
 *          交出去就丢副本的话，响应正文会静静少一截，而收尾标记照样上线——对端看到的是一个合法但
 *          缺字的响应，比直接失败更难查
 */
TEST(Http3Connection, PartiallyAcceptedBodyIsRetainedAndResentInOrder)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    std::string   encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);
    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}}, false).has_value());
    ASSERT_TRUE(connection->appendResponseBody(kRequestStreamId, bytesOfText("0123456789abcdef"), false).has_value());

    // 先记下这条流一共要交多少字节，再让假传输层的队列只装到倒数第 4 个字节为止
    const std::size_t totalByteCount = connection->pendingOutputByteCount(kRequestStreamId);
    ASSERT_GT(totalByteCount, 4u);
    transport.pendingQueueByteLimit = totalByteCount - 4;
    connection->flush();
    EXPECT_EQ(transport.writtenOf(kRequestStreamId), totalByteCount - 4);
    EXPECT_EQ(connection->pendingOutputByteCount(kRequestStreamId), 4u) << "没收下的那截要留在本层，不能当作已交付";
    EXPECT_TRUE(transport.endedStreams.empty());

    transport.pendingQueueByteLimit = std::numeric_limits<std::size_t>::max();
    connection->flush();
    EXPECT_EQ(transport.writtenOf(kRequestStreamId), totalByteCount);
    EXPECT_EQ(transport.bytesOf(kRequestStreamId).size(), totalByteCount) << "续交既不能丢字节也不能重放";
    EXPECT_EQ(connection->pendingOutputByteCount(kRequestStreamId), 0u);

    // 队列还满着的时候收尾不能先上线：否则对端看到的是一个合法但缺了尾字的响应
    ASSERT_TRUE(connection->appendResponseBody(kRequestStreamId, bytesOfText("tail"), true).has_value());
    transport.pendingQueueByteLimit = transport.writtenOf(kRequestStreamId);
    connection->flush();
    EXPECT_EQ(transport.writtenOf(kRequestStreamId), totalByteCount) << "队列没地方，线上不该多出任何字节";
    EXPECT_TRUE(transport.endedStreams.empty()) << "没交完就不该告诉对端本端收尾了";

    transport.pendingQueueByteLimit = std::numeric_limits<std::size_t>::max();
    connection->flush();
    EXPECT_EQ(transport.endedStreams.size(), 1u);
    EXPECT_GT(transport.writtenOf(kRequestStreamId), totalByteCount);
    EXPECT_EQ(transport.writtenOf(kRequestStreamId), transport.bytesOf(kRequestStreamId).size());
}

TEST(Http3Connection, UnknownUnidirectionalStreamTypeIsDiscardedButStillCredited)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    // 类型 0x41：本端不认识（RFC 9114 §6.2.1 要求忽略而不是报错）
    connection->consumeStreamData(14, bytesOfText(std::string("\x41", 1) + std::string(20, 'x')), false);

    EXPECT_FALSE(connection->isBroken());
    EXPECT_EQ(transport.creditedOf(14), 21u) << "丢弃的字节同样要还额度，否则对端会卡在自己耗尽的窗口上";
    EXPECT_TRUE(events.connectionClosures.empty());
}

TEST(Http3Connection, DuplicateControlStreamBreaksTheConnection)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

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
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), true);

    EXPECT_TRUE(connection->isBroken());
    EXPECT_EQ(connection->connectionErrorCode(), Http3ErrorCode::ClosedCriticalStream);
}

TEST(Http3Connection, SettingsOnRequestStreamFailsOnlyThatStream)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x04, "")), false);

    ASSERT_EQ(events.streamsReset.size(), 1u);
    EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::FrameUnexpected);
    EXPECT_FALSE(connection->isBroken());
}

TEST(Http3Connection, BlockedFieldSectionIsDeliveredOnceTheEncoderStreamCatchesUp)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    // 对端先把 SETTINGS 与「动态表插入」分两趟送：头段先到、指令后到，就构成 §2.2.1 的阻塞
    std::string encoderBytes;
    std::string headerBlock;
    {
        QpackEncoder peerEncoder(4096, 100, 4096);
        // 第一次编码即插入：产出引用动态表的头段与对应指令
        const auto first = peerEncoder.encodeFieldSection(0, std::span<const QpackHeaderField>(minimalRequestFields()), headerBlock, encoderBytes);
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
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    std::string   encoderBytes;
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
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings withoutExtended;
    withoutExtended.isExtendedConnectEnabled = false;
    auto strictConnection                    = makeConnection(transport, events, withoutExtended);

    std::string                         encoderBytes;
    const std::vector<QpackHeaderField> tunnelFields{QpackHeaderField{":method", "CONNECT"}, QpackHeaderField{":scheme", "https"}, QpackHeaderField{":authority", "example.com"},
                                                     QpackHeaderField{":path", "/ws"}, QpackHeaderField{":protocol", "websocket"}};
    strictConnection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(tunnelFields, encoderBytes))), false);
    EXPECT_EQ(events.malformedRequests.size(), 1u) << "没声明 ENABLE_CONNECT_PROTOCOL 就不该收下 :protocol";

    FakeTransport permittingTransport;
    EventLog      permittingEvents;
    auto          permittingConnection = makeConnection(permittingTransport, permittingEvents);
    permittingConnection->consumeStreamData(4, bytesOfText(makeFrame(0x01, encodeSection(tunnelFields, encoderBytes))), false);
    EXPECT_TRUE(permittingEvents.malformedRequests.empty());
    EXPECT_EQ(permittingEvents.headerBlocksReceived.size(), 1u);
}

/**
 * @brief 不成帧的恶意字节流不得越界读写，也不得让连接出现「作废了还在长字节」的状态
 * @details 协议层的所有输入都来自不可信对端，这一条用固定种子的伪随机字节 + 全部流类别扫一遍：
 *          钉的是内存安全（整仓 Debug 带 ASan，越界与悬垂会直接报）与两条状态不变式，
 *          而不是某个具体判定结果——随机用例负责让崩溃无处藏，判定分支由前面的具名用例逐个钉。
 */
TEST(Http3Connection, HostileByteStreamsStaySafeAndSelfConsistent)
{
    static constexpr std::uint64_t kSeed = 20260919;
    std::mt19937                   generator(static_cast<std::uint32_t>(kSeed));

    for (int round = 0; round < 400; ++round)
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events);
        ASSERT_TRUE(connection->isUsable());

        // 五类流号都要扫到：请求流、对端控制流与两条 QPACK 流、以及未知类型的单向流
        const std::int64_t streamIds[]      = {0, 2, 6, 10, 14};
        const std::size_t  payloadByteCount = generator() % 48;
        std::string        payload(payloadByteCount, '\0');
        for (auto &byte: payload)
        {
            byte = static_cast<char>(generator() % 256);
        }
        const std::int64_t streamId = streamIds[generator() % 5];
        connection->consumeStreamData(streamId, bytesOfText(payload), (round % 5) == 0);
        connection->flush();

        // 不变式一：状态只可能是「可用」或「带线上错误码的作废」，不存在第三种
        EXPECT_TRUE(connection->isUsable() || connection->isBroken()) << "第 " << round << " 轮";
        // 不变式二：作废原因只记一次，且必须是可命名的错误码
        EXPECT_LE(events.connectionClosures.size(), 1u) << "第 " << round << " 轮：收口通知重复发出";
        if (connection->isBroken())
        {
            EXPECT_NE(connection->connectionErrorCode(), Http3ErrorCode::NoError) << "第 " << round << " 轮";
            // 不变式三：作废之后既不再排字节也不再外发，否则销毁前的窗口里还在长内存
            const std::size_t bytesAfterBreak = transport.totalWrittenByteCount();
            static_cast<void>(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "500"}}, true));
            connection->consumeStreamData(kRequestStreamId, bytesOfText(payload), false);
            connection->flush();
            EXPECT_EQ(transport.totalWrittenByteCount(), bytesAfterBreak) << "第 " << round << " 轮：作废之后仍在发字节";
        }
    }
}

/// 先喂完对端 SETTINGS、请求头段与一段正文：尾段相关的三条用例都从这个前置态出发
void feedHeadAndBody(Http3Connection &connection)
{
    std::string encoderBytes;
    connection.consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
    connection.consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);
    connection.consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "abc")), false);
}

TEST(Http3Connection, TrailersSectionIsAcceptedAfterTheBodyAndMarkedAsTrailers)
{
    // 钉住 §4.1 的头段/尾段顺序：正文之后再来的 HEADERS 是尾段，不得有伪头，且要带上
    // 「这是尾段」的通知让会话按既有口径处理
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    feedHeadAndBody(*connection);

    std::string                         encoderBytes;
    const std::vector<QpackHeaderField> trailerFields{QpackHeaderField{"x-checksum", "abc123"}};
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(trailerFields, encoderBytes))), true);

    EXPECT_TRUE(events.malformedRequests.empty()) << "合法尾段不该被打回";
    ASSERT_EQ(events.trailerBlocksReceived.size(), 1u);
    EXPECT_EQ(events.trailerBlocksReceived[0], kRequestStreamId);
    EXPECT_EQ(events.requestsEnded.size(), 1u) << "尾段之后的 END_STREAM 才算请求收全";

    // 字段本身也要带着「这是尾段」交出去：会话据此落进 trailer 一档。
    // 反过来头段必须报假——报成全真会让业务把头部的值当成尾部的
    const auto isChecksumField = [](const EventLog::HeaderField &field) { return field.name == "x-checksum"; };
    const auto checksum        = std::ranges::find_if(events.headerFields, isChecksumField);
    ASSERT_TRUE(checksum != events.headerFields.end()) << "尾段字段没被交出去";
    EXPECT_TRUE(checksum->isTrailers) << "尾段字段必须标成尾段";
    EXPECT_EQ(checksum->value, "abc123");
    ASSERT_FALSE(events.headerFields.empty());
    EXPECT_FALSE(events.headerFields.front().isTrailers) << "头段字段被标成了尾段";
}

TEST(Http3Connection, PseudoHeaderInTrailersIsRejected)
{
    // 同一个判定器跨头段持有：尾段里再来一个伪头必须被打死（§4.3），
    // 这条同时证明「第二个头段被认成尾段」而不是「又一个头段」
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    feedHeadAndBody(*connection);

    std::string                         encoderBytes;
    const std::vector<QpackHeaderField> badTrailers{QpackHeaderField{":method", "GET"}};
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(badTrailers, encoderBytes))), false);

    ASSERT_EQ(events.malformedRequests.size(), 1u);
    EXPECT_EQ(events.trailerBlocksReceived.size(), 0u) << "判定没过就不该把尾段交上去";
}

TEST(Http3Connection, BodyAfterTrailersFailsTheStream)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    feedHeadAndBody(*connection);
    std::string encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection({QpackHeaderField{"x-checksum", "z"}}, encoderBytes))), false);
    ASSERT_EQ(events.trailerBlocksReceived.size(), 1u);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "more")), false);
    EXPECT_FALSE(events.streamsReset.empty()) << "尾段之后的正文属于非法消息顺序（§4.1.2）";
}

TEST(Http3Connection, PeerGoAwayMaxPushIdAndUnknownSettingsAreTolerated)
{
    // 对端 GOAWAY / MAX_PUSH_ID / 本端不认识设置项都属于「收下但不作为」：
    // 把它们判成错误会让 Chrome/curl 这类客户端直接连不上
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    std::string controlBytes = streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x33\x01", 2));
    controlBytes += makeFrame(0x07, std::string("\x04", 1));
    controlBytes += makeFrame(0x0d, std::string("\x0a", 1));
    controlBytes += makeFrame(0x03, std::string("\x02", 1));
    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(controlBytes), false);

    EXPECT_FALSE(connection->isBroken()) << "未知设置项与这些控制帧都不该判错";
    EXPECT_TRUE(events.connectionClosures.empty());
}

TEST(Http3Connection, FirstControlFrameMustBeSettings)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x07, std::string("\x04", 1))), false);

    EXPECT_TRUE(connection->isBroken());
    EXPECT_EQ(connection->connectionErrorCode(), Http3ErrorCode::MissingSettings);
}

TEST(Http3Connection, SecondSettingsOrBodyOnControlStreamBreaksConnection)
{
    FakeTransport transport;
    EventLog      events;
    auto          first = makeConnection(transport, events);
    first->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "") + makeFrame(0x04, "")), false);
    EXPECT_EQ(first->connectionErrorCode(), Http3ErrorCode::FrameUnexpected) << "第二个 SETTINGS 属重复（§7.2.4.1）";

    FakeTransport secondTransport;
    EventLog      secondEvents;
    auto          second = makeConnection(secondTransport, secondEvents);
    second->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "") + makeFrame(0x00, "x")), false);
    EXPECT_TRUE(second->isBroken()) << "控制流上不承载正文";
}

TEST(Http3Connection, CancellingAnUnknownStreamChangesNothing)
{
    // 承载层的取消通知可能晚于本端的回收：这里必须安静地什么都不做，而不是造出一份状态来
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    connection->noteStreamCancelledByPeer(999);

    EXPECT_FALSE(connection->isBroken());
    EXPECT_TRUE(events.streamsReset.empty());
    EXPECT_TRUE(events.streamsClosed.empty());
}

TEST(Http3Connection, AppendingBodyAfterTheStreamFinishedVoidansThatWrite)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    std::string   encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), true);

    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "204"}}, true).has_value());
    connection->flush();
    const auto late = connection->appendResponseBody(kRequestStreamId, bytesOfText("tail"), false);
    EXPECT_FALSE(late.has_value()) << "已收尾的流上再写正文要报失败，让会话丢弃这一段";
    EXPECT_FALSE(connection->isBroken()) << "这只作废该响应，不牵连连接";
}

/// 数控制流字节里有几个 GOAWAY，并给出第一个报出的标识（前缀那一字节是流类型，要跳过）
[[nodiscard]] std::pair<std::size_t, std::uint64_t> inspectGoAwayFrames(const std::string &controlBytes)
{
    AsynGyanis::Net::Http3FrameReader reader(4096);
    static_cast<void>(reader.feed(bytesOfText(controlBytes.substr(1))));

    std::size_t   goAwayCount      = 0;
    std::uint64_t firstAnnouncedId = 0;
    while (true)
    {
        const auto frame = reader.nextFrame();
        if (!frame.has_value() || !frame->has_value())
        {
            break; // 没有完整帧（或已进入错误态）就收手：本助手只数已经解得出来的 GOAWAY
        }
        if (const auto *goAway = std::get_if<AsynGyanis::Net::Http3GoAwayFrame>(&frame->value()); goAway != nullptr)
        {
            if (goAwayCount == 0)
            {
                firstAnnouncedId = goAway->streamIdOrPushId;
            }
            ++goAwayCount;
        }
    }
    return {goAwayCount, firstAnnouncedId};
}

/**
 * @brief 排空通告报的是「最后一条已受理流之后的下一条流号」，且只发一次
 * @details RFC 9114 §5.2 的语义是「等于或高于该标识都被拒绝」，把已受理的那条流本身报进去
 *          就等于告诉对端「它不会被处理」，正好与「照常处理完」矛盾
 */
TEST(Http3Connection, DrainAnnouncementCoversStreamsAfterTheLastAcceptedOne)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    std::string encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), true);

    ASSERT_TRUE(connection->beginGracefulDrain().has_value());
    EXPECT_TRUE(connection->isDraining());
    connection->flush();

    const auto [goAwayCount, announcedId] = inspectGoAwayFrames(transport.bytesOf(3));
    EXPECT_EQ(goAwayCount, 1U) << "控制流上应当只有一条 GOAWAY";
    EXPECT_EQ(announcedId, 4U) << "已受理流 0，通告要报下一条客户端双向流号 4";

    // 幂等：第二次调用不再补发（后发的标识不得比先发的大，重复发也没有新信息）
    ASSERT_TRUE(connection->beginGracefulDrain().has_value());
    connection->flush();
    EXPECT_EQ(inspectGoAwayFrames(transport.bytesOf(3)).first, 1U) << "重复排空不该再发一条 GOAWAY";
}

/// 一条请求都没收过时，通告标识按 §5.2 取 0
TEST(Http3Connection, DrainAnnouncementIsZeroBeforeAnyRequest)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    ASSERT_TRUE(connection->beginGracefulDrain().has_value());
    connection->flush();

    const auto [goAwayCount, announcedId] = inspectGoAwayFrames(transport.bytesOf(3));
    EXPECT_EQ(goAwayCount, 1U);
    EXPECT_EQ(announcedId, 0U) << "还没受理任何请求时，第一条流号（0）就该被拒绝";
}

/**
 * @brief 通告之后的新流不处理也不回应，但要显式取消，且额度照还、连接不受牵连
 * @details 不还会让对端卡在自己耗尽的流控窗口上；判成连接错误则会把同连接上已受理的请求一起废掉
 */
TEST(Http3Connection, RequestsAfterTheDrainAnnouncementAreIgnoredButCredited)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);

    std::string encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), true);
    ASSERT_TRUE(connection->beginGracefulDrain().has_value());

    const std::string lateRequestBytes = makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes));
    events.headerFields.clear();
    events.requestsEnded.clear();
    connection->consumeStreamData(8, bytesOfText(lateRequestBytes), true);

    EXPECT_TRUE(events.headerFields.empty()) << "通告之后的新流不该再交出任何头字段";
    EXPECT_TRUE(events.requestsEnded.empty()) << "这条流也不该被当作「请求收齐」交给上层";
    EXPECT_EQ(transport.creditedOf(8), lateRequestBytes.size()) << "拒绝不等于不还额度：不还会把对端卡在窗口上";
    EXPECT_FALSE(connection->isBroken()) << "拒收一条新流是排空的正常结局，不该作废连接";
    // §5.2 的 SHOULD：不处理之外还要显式取消这条流，对端才不必等到连接收尾才知道结果
    ASSERT_EQ(events.streamsReset.size(), 1U) << "通告之后的新流没有被交代一次取消";
    EXPECT_EQ(events.streamsReset.front().first, 8);
    EXPECT_EQ(events.streamsReset.front().second, Http3ErrorCode::RequestRejected);

    // 通告之前已受理的流照常能答：这是「已受理的处理完」这条承诺的实质
    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}}, true).has_value());
    connection->flush();
    EXPECT_GT(transport.writtenOf(kRequestStreamId), 0U) << "已受理的响应发不出去，排空就失去了意义";
}

/**
 * @brief 钉住：响应头段越过对端通告的 SETTINGS_MAX_FIELD_SECTION_SIZE 时只作废那一条流
 * @details 依据 RFC 9114 §4.2.2：这一项约束的正是本端发出去的头段。不判就把处置权交给对端，
 *          而它常见做法是收掉整条连接。本端宁可只中止这一条流，且拒绝时一个字节都不上线
 *          （上了线的部分头段会让对端的 QPACK 状态与本端错开）。
 */
TEST(Http3Connection, RefusesResponseFieldSectionBeyondThePeerAdvertisedLimit)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    std::string   encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);
    // 对端只肯收 60 字节的头段：:status 200 这一项就是名长 7 + 值长 3 + 32 = 42 字节
    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x06\x3C", 2))), false);
    ASSERT_FALSE(connection->isBroken());

    const std::vector<QpackHeaderField> oversizedFields{QpackHeaderField{":status", "200"}, QpackHeaderField{"x-big", std::string(40U, 'v')}};
    const auto                          refused = connection->submitResponseHead(kRequestStreamId, oversizedFields, true);
    ASSERT_FALSE(refused.has_value()) << "越过对端通告上限的头段不该照样发出去";
    EXPECT_EQ(refused.error().kind, QpackErrorKind::InvalidLocalState) << refused.error().message;
    EXPECT_NE(refused.error().message.find("SETTINGS_MAX_FIELD_SECTION_SIZE"), std::string::npos) << refused.error().message;
    EXPECT_FALSE(connection->isBroken()) << "越限的响应头段只该作废这一条流，不该判死连接";
    EXPECT_TRUE(transport.bytesOf(kRequestStreamId).empty()) << "拒绝就不该有半个头段上线";

    // 这条流没被拆掉：合规的那份响应照常交出
    EXPECT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}}, true).has_value());
    connection->flush();
    EXPECT_GT(transport.writtenOf(kRequestStreamId), 0U) << "合规响应应能写出";
}

/**
 * @brief 钉住：对端没通告 SETTINGS_MAX_FIELD_SECTION_SIZE 时按「不约束」处理，不因这项拒发
 * @details §7.2.4.1 的默认值就是不限。没收到就必须当成不约束，否则等于替对端编一个它没说过的上限。
 */
TEST(Http3Connection, StillSubmitsLargeFieldSectionsWhenThePeerAdvertisesNoLimit)
{
    FakeTransport transport;
    EventLog      events;
    auto          connection = makeConnection(transport, events);
    std::string   encoderBytes;
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);

    // 没收到 SETTINGS_MAX_FIELD_SECTION_SIZE 就必须按「不约束」处理（§7.2.4.1 的默认值），
    // 否则等于替对端编一个它没说过的上限
    const std::vector<QpackHeaderField> largeFields{QpackHeaderField{":status", "200"}, QpackHeaderField{"x-big", std::string(3000U, 'v')}};
    EXPECT_TRUE(connection->submitResponseHead(kRequestStreamId, largeFields, true).has_value());
    EXPECT_FALSE(connection->isBroken());
}
