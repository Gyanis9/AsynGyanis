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
        /// 同一次收尾交上来的原因文案：它是调用方唯一能读到「这条流为什么死」的地方
        std::vector<std::pair<std::int64_t, std::string>>   streamResetReasons{};
        std::vector<std::pair<std::int64_t, std::string>>   malformedRequests{};
        std::vector<std::pair<Http3ErrorCode, std::string>> connectionClosures{};
    };

    /// 建一个接好假传输层的协议层：三条本端单向流在构造里就开出来。角色默认服务端，出站一侧的用例传 Client
    std::unique_ptr<Http3Connection> makeConnection(FakeTransport &transport, EventLog &events, Http3Connection::LocalSettings settings = {},
                                                    AsynGyanis::Net::QuicConnectionRole role = AsynGyanis::Net::QuicConnectionRole::Server)
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
        callbacks.onRequestEnded = [&events](const std::int64_t streamId) { events.requestsEnded.push_back(streamId); };
        callbacks.onStreamClosed = [&events](const std::int64_t streamId) { events.streamsClosed.push_back(streamId); };
        callbacks.onStreamReset  = [&events](const std::int64_t streamId, const Http3ErrorCode errorCode, const std::string_view reason, const bool /*isDecidedByPeer*/)
        {
            events.streamsReset.emplace_back(streamId, errorCode);
            events.streamResetReasons.emplace_back(streamId, std::string(reason));
        };
        callbacks.onMalformedRequest = [&events](const std::int64_t streamId, const std::string_view reason)
        { events.malformedRequests.emplace_back(streamId, std::string(reason)); };
        callbacks.onConnectionClosed = [&events](const Http3ErrorCode errorCode, const std::string_view reason)
        { events.connectionClosures.emplace_back(errorCode, std::string(reason)); };

        return std::make_unique<Http3Connection>([&transport]() { return transport.openUnidirectionalStream(); },
                                                 [&transport](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
                                                 { return transport.write(streamId, data, endStream); }, [&transport](const std::int64_t streamId, const std::size_t byteCount)
                                                 { transport.credit(streamId, byteCount); }, std::move(callbacks), settings, role);
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
    /// 本端发起的三条单向流（构造顺序按 RFC 9114 §6.2.1：控制流 3、编码器流 7、解码器流 11）
    constexpr std::int64_t kLocalEncoderStreamId = 7;

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

    connection->noteStreamCancelledByPeer(kRequestStreamId, static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled), true);
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

/**
 * @brief 头段还阻塞着就收到 END_STREAM：那一下收尾要推迟到指令补齐，不能把空消息报给上层
 * @details 现场是自家出站 h3 客户端打自家服务端：响应的头段引用动态表，而它要的插入指令排在头段
 *          之后才到（QPACK 的两条流之间，传输层不保证先后）。就地按 END_STREAM 收尾会让
 *          `onRequestEnded` 赶在任何 `onHeaderField` 之前到达，拿「已完」当「已收齐」的那层于是
 *          交回一份零字段的响应——正文反倒可能已经计过错
 * @note 证伪：把收尾判定改回不看 `isFieldSectionBlocked`（或摘掉续解点的补收尾），本条在第一段
 *       的 `requestsEnded` 判据处立刻红
 */
TEST(Http3Connection, EndStreamOnBlockedFieldSectionDefersTheMessageEnd)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    std::string encoderBytes;
    std::string headerBlock;
    {
        QpackEncoder peerEncoder(4096, 100, 4096);
        const auto   first = peerEncoder.encodeFieldSection(0, std::span<const QpackHeaderField>(minimalRequestFields()), headerBlock, encoderBytes);
        ASSERT_TRUE(first.has_value()) << first.error().message;
    }
    ASSERT_FALSE(encoderBytes.empty()) << "对端确实插了动态表，这条用例的前提才成立";

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
    // 头段与 FIN 同批到达，而它引用的指令还在路上：既不能交付字段，也不能宣布这条消息收完了
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), true);
    EXPECT_TRUE(events.headerFields.empty()) << "内容没齐就不该交出任何字段";
    EXPECT_TRUE(events.requestsEnded.empty()) << "头段还没齐就把「收完」报上去：上层会拿一份空消息去作答";

    // 指令到达：先交付字段，再补上被推迟的那一下收尾
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(encoderBytes), false);
    EXPECT_FALSE(events.headerFields.empty()) << "补齐之后字段要交出去";
    ASSERT_EQ(events.requestsEnded.size(), 1u) << "补齐之后要补上被推迟的收尾";
    EXPECT_EQ(events.requestsEnded[0], kRequestStreamId);
    EXPECT_TRUE(events.streamsClosed.empty()) << "本端还没作答，这条流的两侧没齐，状态不该回收";

    // 本端作答并收尾：此刻两侧都收完，状态要在这里回收（推迟收尾不能把流留成永久驻留）
    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "204"}}, true).has_value());
    connection->flush();
    EXPECT_EQ(events.streamsClosed.size(), 1u) << "两侧都收完之后要把这条流收掉";
    EXPECT_FALSE(connection->isBroken());
}

/**
 * @brief 头段挂起期间正文照常到达：续解之后那一段仍要按「头段」交付，不能被当成尾段
 * @details 判据取自自家两型的实测现场——服务端响应的头段引用动态表、指令排在头段之后，而正文帧夹在
 *          中间先到。按「续解时是否已收过正文」判身份就会把这一判成尾段：响应头明明解全了，上层却收到
 *          「头块序列非法」，一条答对的响应被本端自己作废
 * @note 证伪：把交付点改回 `state.isBodyStarted`，本条在 `malformedRequests` 判据处红
 */
TEST(Http3Connection, BodyArrivingWhileFieldSectionBlockedDoesNotTurnTheHeadIntoTrailers)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    std::string encoderBytes;
    std::string headerBlock;
    {
        QpackEncoder peerEncoder(4096, 100, 4096);
        const auto   first = peerEncoder.encodeFieldSection(0, std::span<const QpackHeaderField>(minimalRequestFields()), headerBlock, encoderBytes);
        ASSERT_TRUE(first.has_value()) << first.error().message;
    }
    ASSERT_FALSE(encoderBytes.empty()) << "对端确实插了动态表，这条用例的前提才成立";

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
    // 头段挂起，紧跟其后的正文帧照常收下：此刻 isBodyStarted 已经是真
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headerBlock)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "payload")), true);
    EXPECT_TRUE(events.headerFields.empty());
    EXPECT_EQ(events.bodyBytes, "payload") << "正文不必等头段，先收下才对得上流控";

    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(encoderBytes), false);
    EXPECT_TRUE(events.malformedRequests.empty()) << "挂起的头段被按尾段判了非法序列（正文先到不该改它的身份）";
    EXPECT_FALSE(events.headerFields.empty()) << "头段要在补齐之后交出去";
    // 头段齐了、正文也收完并带 END_STREAM：这条请求到此就是完整的，必须派发出去
    // （与上一条用例「推迟收尾」的那一下同一位置）
    ASSERT_EQ(events.requestsEnded.size(), 1u) << "补齐之后没补上被推迟的收尾：请求永远躺在会话外面没人派";
    EXPECT_EQ(events.requestsEnded[0], kRequestStreamId);
    EXPECT_TRUE(events.streamsClosed.empty()) << "本端还没作答，这条流的两侧没齐，状态不该回收";
}

/**
 * @brief 同一条流上头段与尾段都挂起：指令补齐后两段要按序各交付一次，身份不能互换
 * @details 现场取自自家出站 h3 客户端打自家服务端：响应头段引用动态表、尾段也插了新项，两段的指令
 *          一起排在头段之后才到。本端只在流上记一个「挂起的是哪一类」的槽，于是尾段那一次到达把它改写，
 *          补齐时把**头段**按尾段交出去——响应头里的 `:status` 立刻被判定器按 §4.3 判成「伪头出现在尾段」。
 *          另一半是续解只走一段：解码器按流存的是队列，而 `feedEncoderStream` 每条流只报一个标识，
 *          交付完队首就返回，后面那段再没有唤醒点，尾字段静默消失
 * @note 证伪：把续解点改回「只交付一段就返回」，本条在尾段那一判据红；把交付身份改回读那个单槽，
 *       本条在 `malformedRequests` 与头段计数处红
 */
TEST(Http3Connection, HeadAndTrailerSectionsBlockedOnTheSameStreamAreBothDelivered)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    const std::vector<QpackHeaderField> headFields = minimalRequestFields();
    const std::vector<QpackHeaderField> trailerFields{QpackHeaderField{"x-checksum", "616263"}};
    std::string                         headBlock;
    std::string                         trailerBlock;
    std::string                         instructions;
    {
        QpackEncoder peerEncoder(4096, 100, 4096);
        std::string  sectionInstructions;
        const auto   head = peerEncoder.encodeFieldSection(kRequestStreamId, std::span<const QpackHeaderField>(headFields), headBlock, sectionInstructions);
        ASSERT_TRUE(head.has_value()) << head.error().message;
        instructions += sectionInstructions;
        sectionInstructions.clear();
        const auto trailers = peerEncoder.encodeFieldSection(kRequestStreamId, std::span<const QpackHeaderField>(trailerFields), trailerBlock, sectionInstructions);
        ASSERT_TRUE(trailers.has_value()) << trailers.error().message;
        instructions += sectionInstructions;
    }
    ASSERT_FALSE(instructions.empty()) << "对端确实插了动态表，两段才会都挂起";

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headBlock)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "payload")), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, trailerBlock)), true);
    EXPECT_TRUE(events.headerFields.empty()) << "指令没齐就交出了字段";
    EXPECT_TRUE(events.malformedRequests.empty()) << "挂起期间不该有任何判定结论";

    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(instructions), false);

    EXPECT_TRUE(events.malformedRequests.empty()) << "挂起的段被按错了身份交付";
    ASSERT_EQ(events.headerBlocksReceived.size(), 1u) << "头段要作为头段交付一次";
    ASSERT_EQ(events.trailerBlocksReceived.size(), 1u) << "队首之后那段尾段没被续解：尾字段静默丢了";
    std::size_t headFieldCount    = 0;
    std::size_t trailerFieldCount = 0;
    for (const auto &field: events.headerFields)
    {
        field.isTrailers ? ++trailerFieldCount : ++headFieldCount;
        if (field.name == "x-checksum")
        {
            EXPECT_TRUE(field.isTrailers) << "尾段里的字段被当作了请求头部";
            EXPECT_EQ(field.value, "616263");
        }
    }
    EXPECT_EQ(headFieldCount, headFields.size()) << "头段交出的字段数对不上";
    EXPECT_EQ(trailerFieldCount, trailerFields.size()) << "尾段交出的字段数对不上";
    ASSERT_EQ(events.requestsEnded.size(), 1u) << "两段都交付完才该补上被推迟的收尾";
    EXPECT_FALSE(connection->isBroken());
}

/**
 * @brief 尾段引用的是表里的老项（它自己不需要新指令），也不能抢在挂起的头段之前交付
 * @details RFC 9204 §2.2.1 要求一条流上的头块按发送序解出。Required Insert Count 说的是「这段需要的
 *          插入数」，后来那段完全可以比前一段小，于是它就绕过了挂起判定：本端把尾段先交上去，判定器
 *          按 §4.1 报「尾段出现在头段之前」——一条合法响应被本端自己作废
 * @note 证伪：摘掉解码器里「本流已有挂起段则后来那段一并挂起」那道闸，本条在 `malformedRequests`
 *       与「指令到达前不交付」两处红
 */
TEST(Http3Connection, TrailerSectionBehindABlockedHeadIsNotDeliveredOutOfOrder)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    std::string headBlock;
    std::string instructions;
    {
        QpackEncoder peerEncoder(4096, 100, 4096);
        const auto   head = peerEncoder.encodeFieldSection(kRequestStreamId, std::span<const QpackHeaderField>(minimalRequestFields()), headBlock, instructions);
        ASSERT_TRUE(head.has_value()) << head.error().message;
    }
    ASSERT_FALSE(instructions.empty()) << "头段确实引用了尚未到达的插入";
    // 尾段由一个「不接动态表」的对端编码（encodeSection 里那台）：Required Insert Count 为 0，
    // 本端不等任何指令就能解开它
    std::string                         unusedInstructions;
    const std::vector<QpackHeaderField> trailerFields{QpackHeaderField{"x-checksum", "616263"}};
    const std::string                   trailerBlock = encodeSection(trailerFields, unusedInstructions);
    EXPECT_TRUE(unusedInstructions.empty()) << "这台对端不插表，尾段才应当无需指令即可解开";

    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headBlock)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "payload")), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, trailerBlock)), true);
    EXPECT_TRUE(events.headerFields.empty()) << "头段还压着就把尾段交了上去";
    EXPECT_TRUE(events.malformedRequests.empty()) << "按发送序等的两段，不该被本端判成非法序列";

    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(instructions), false);

    EXPECT_TRUE(events.malformedRequests.empty()) << "补齐之后仍被判了非法序列";
    ASSERT_EQ(events.headerBlocksReceived.size(), 1u) << "头段没在指令到达后交付";
    ASSERT_EQ(events.trailerBlocksReceived.size(), 1u) << "尾段没在头段之后交付";
    ASSERT_EQ(events.requestsEnded.size(), 1u);
}

/**
 * @brief 本端作废一条流时，要把该流在解码器侧的头块记账取消掉，而不是留到连接结束
 * @details 全仓只有 `noteStreamCancelledByPeer`（对端 RESET 那条）会调解码器的收尾。本端自己判畸形
 *          （`rejectRequestHead`）或自己重置（`failStream`）时，那条流上「已解出待确认的头块」与
 *          「还压着的挂起段」都没人清：前者让这条流的 Section Ack 永远发不出去，后者是本端为对端
 *          保留的原始字节——一个对端可以只发注定被判畸形的头段、或者注定被重置的流，按流号一条条把
 *          这两张表撑下去，而它每开一条流都换一个号。按 RFC 9204 §4.4.2/§2.1.3，解码侧放弃一条流就
 *          应当在解码器流上发 Stream Cancellation，这既清本端的账，也让对端尽早释放它那边因这条流
 *          而拖住的动态表引用与阻塞名额
 * @note 证伪：摘掉 `rejectRequestHead` 或 `failStream` 里新加的收尾，对应那一格红；把「有没有账」这道
 *       判断改成无条件发，第三格（畸形但整段不含动态表引用那格）红——没账可取消的流不该多出这条指令
 */
TEST(Http3Connection, AbandonedStreamCancelsItsFieldSectionsInTheDecoder)
{
    // 本端发起的三条单向流里解码器流是第三个（上一条用例已钉过它的类型前缀是 0x03）
    constexpr std::int64_t kLocalDecoderStreamId = 11;
    // 流 0 的取消指令：'01' 加 6 位前缀的流标识，单字节档就是 0x40
    const std::string streamCancellationForStreamZero = std::string(1, static_cast<char>(0x40));

    // 格一：头段解出来了但判定不过（大写头名）——本端不再解这条消息，要留下取消指令
    {
        FakeTransport                  transport;
        EventLog                       events;
        Http3Connection::LocalSettings settings;
        settings.qpackMaximumTableCapacityByteCount = 4096;
        auto connection                             = makeConnection(transport, events, settings);

        std::string headBlock;
        std::string instructions;
        {
            QpackEncoder peerEncoder(4096, 100, 4096);
            auto         fields = minimalRequestFields();
            fields.back().name  = "Accept"; // 大写：RFC 9114 §4.2 判畸形
            const auto encoded  = peerEncoder.encodeFieldSection(kRequestStreamId, std::span<const QpackHeaderField>(fields), headBlock, instructions);
            ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
        }
        ASSERT_FALSE(instructions.empty()) << "对端插了表，这段的 Required Insert Count 才非 0，本端才会留下待确认的记录";

        connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
        connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02) + instructions), false);
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headBlock)), false);
        ASSERT_EQ(events.malformedRequests.size(), 1u) << "前提：头段被判畸形交回会话";
        connection->flush();
        EXPECT_NE(transport.bytesOf(kLocalDecoderStreamId).find(streamCancellationForStreamZero), std::string::npos) << "本端不再解这条流，却没有在解码器流上取消它的头块";
    }

    // 格二：头段还压着而本端把这条流重置了（请求流上出现 SETTINGS 帧）
    {
        FakeTransport                  transport;
        EventLog                       events;
        Http3Connection::LocalSettings settings;
        settings.qpackMaximumTableCapacityByteCount = 4096;
        auto connection                             = makeConnection(transport, events, settings);

        std::string headBlock;
        std::string instructions;
        {
            QpackEncoder peerEncoder(4096, 100, 4096);
            const auto   encoded = peerEncoder.encodeFieldSection(kRequestStreamId, std::span<const QpackHeaderField>(minimalRequestFields()), headBlock, instructions);
            ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
        }
        connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
        connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, headBlock)), false);
        ASSERT_TRUE(events.headerFields.empty()) << "前提：头段还压在解码器里";

        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x04, std::string("\x01\x50\x00", 3))), false);
        ASSERT_EQ(events.streamsReset.size(), 1u) << "前提：这条流被本端重置";
        connection->flush();
        EXPECT_NE(transport.bytesOf(kLocalDecoderStreamId).find(streamCancellationForStreamZero), std::string::npos) << "重置一条流之后，本端仍替它留着挂起的头块字节";
    }

    // 反向格：本端确实作废了这条流，但它一个头块都没解成（Required Insert Count 为 0，也没挂起段）——
    // 没账可取消时不该多出这条指令
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events);
        auto          fields     = minimalRequestFields();
        fields.back().name       = "Accept"; // 大写：RFC 9114 §4.2 判畸形，而这段不含任何动态表引用
        std::string encoderBytes;
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(fields, encoderBytes))), false);

        ASSERT_EQ(events.malformedRequests.size(), 1u) << "前提：头段被判畸形";
        EXPECT_TRUE(encoderBytes.empty()) << "前提：这台对端不插表，本端不为这段留任何账";
        connection->flush();
        EXPECT_EQ(transport.bytesOf(kLocalDecoderStreamId).find(streamCancellationForStreamZero), std::string::npos)
                << "没有可取消的账就不该发这条指令：那会把对端这条流上照常等待确认的状态打乱";
    }
}

/**
 * @brief 对端发来 GOAWAY：通告值及以上的本端请求流按 H3_REQUEST_REJECTED 交上去，本端也不再算健康
 * @details RFC 9114 §5.2 的原句是「Requests or pushes with the indicated identifier or greater are
 *          rejected by the sender of the GOAWAY」，§7 接着说这些请求「will not be processed. Clients can
 *          safely retry unprocessed requests on a different HTTP connection」，而 §5.2 又要求
 *          「Endpoints MUST NOT initiate new requests … after receipt of a GOAWAY frame from the peer」。
 *          本端过去只把这个帧写进 DEBUG 日志：在途的那些流没人给结局（只能各自等时限），连接也照常算健康
 *          被池继续派发，于是每条新请求都去撞一次拒绝。三格分别是：通告值 0 把已开的流 0 判掉、通告值 4
 *          只判 4 而不牵连 0、以及服务端角色下一条请求流都不动（对端客户端发来的 GOAWAY 里那个标识是
 *          **推送流号**，本端从不推送）
 * @note 证伪：摘掉逐流处置那个循环，格一与格二红在 streamsReset 上；摘掉「只挑本端发起的双向流」那个筛子，
 *       格三红（服务端角色会把对端的请求流当成自己被拒的流）；摘掉 `m_isPeerGoAwayReceived` 的记账，
 *       格一红在两处（那一位本身，与「GOAWAY 之后不许再开新请求」那道闸）；只摘那道闸，格一红在
 *       submitRequestHead 那一处
 */
TEST(Http3Connection, PeerGoAwayRejectsTheUnprocessedRequestStreamsAndBarsNewOnes)
{
    const std::vector<QpackHeaderField> requestFields{QpackHeaderField{":method", "POST"}, QpackHeaderField{":scheme", "https"}, QpackHeaderField{":authority", "example.com"},
                                                      QpackHeaderField{":path", "/upload"}};
    // 本端是客户端时，对端（服务端）发起的单向流号 ≡ 3 (mod 4)（RFC 9000 §2.1）；夹具那个 kPeerControlStreamId=2
    // 是「本端是服务端」时的对端流号，用在这里会被当成字节回到了本端自己发起的流上，整段被忽略
    constexpr std::int64_t kPeerServerControlStreamId = 3;

    // 格一：GOAWAY(0) 把本端已开的那条请求流判成「对端不再受理」
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);
        ASSERT_TRUE(connection->submitRequestHead(kRequestStreamId, requestFields, false).has_value());
        connection->consumeStreamData(kPeerServerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), false);
        EXPECT_FALSE(connection->isPeerGoAwayReceived()) << "前提：还没收到 GOAWAY";

        connection->consumeStreamData(kPeerServerControlStreamId, bytesOfText(makeFrame(0x07, std::string(1, '\x00'))), false);

        EXPECT_TRUE(connection->isPeerGoAwayReceived()) << "§5.2：收到 GOAWAY 之后本端不得再在这条连接上发起新请求";
        ASSERT_EQ(events.streamsReset.size(), 1U) << "在途那条流没人给结局，它只能等自己的时限";
        EXPECT_EQ(events.streamsReset[0].first, kRequestStreamId);
        EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::RequestRejected) << "§7 说这些请求不会被处理：交上去的码要让上层认得出「可以当没发过、换条连接重来」";
        EXPECT_FALSE(connection->isBroken()) << "GOAWAY 是优雅收场，不该把整条连接判死";

        // §5.2 的 MUST NOT 落在协议层自己手里：GOAWAY 之后再提新请求要当场被拒，
        // 而不是提上去等对端再拒一次（客户端那条健康位只是替池省掉这次注定失败的往返）
        const auto refused = connection->submitRequestHead(8, requestFields, false);
        EXPECT_FALSE(refused.has_value()) << "收到 GOAWAY 之后本端还能在这条连接上发起新请求";
        EXPECT_EQ(events.streamsReset.size(), 1U) << "被挡下的那次提交不该在流上留下痕迹";
    }

    // 格二：通告值之下的流不受牵连（§5.2 说的是「该标识**及以上**」）
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);
        ASSERT_TRUE(connection->submitRequestHead(kRequestStreamId, requestFields, false).has_value());
        ASSERT_TRUE(connection->submitRequestHead(4, requestFields, false).has_value());
        connection->consumeStreamData(kPeerServerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), false);

        connection->consumeStreamData(kPeerServerControlStreamId, bytesOfText(makeFrame(0x07, std::string(1, '\x04'))), false);

        ASSERT_EQ(events.streamsReset.size(), 1U) << "只该判掉通告值及以上的那条";
        EXPECT_EQ(events.streamsReset[0].first, 4) << "判错了流：通告值之下的那条可能已被处理，不能当没发过";
    }

    // 格三：服务端角色下对端的 GOAWAY 说的是推送流号，一条请求流都不该动
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events);
        std::string   encoderBytes;
        connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, "")), false);
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), encoderBytes))), false);
        ASSERT_EQ(events.headerBlocksReceived.size(), 1U) << "前提：对端那条请求已经收下";

        connection->consumeStreamData(kPeerControlStreamId, bytesOfText(makeFrame(0x07, std::string(1, '\x00'))), false);

        EXPECT_TRUE(events.streamsReset.empty()) << "客户端发来的 GOAWAY 里那个标识是推送流号（§5.2），本端从不推送，请求流不该被牵连";
        EXPECT_TRUE(connection->isPeerGoAwayReceived()) << "这一位照记：它说的是「对端发过 GOAWAY」这件事本身";
    }
}

/**
 * @brief 过渡响应（1xx）占不掉「这条流唯一的头段」那一位，随后的最终响应照常交付
 * @details 本端服务端三条通道都能发过渡响应（`HttpResponse::sendInformational`，且带
 *          `Expect: 100-continue` 的 h2/h3 请求由会话自动补一个 100），h1 的出站解析器按 RFC 9112 §6.4
 *          把它当「最终响应之前的一声招呼」丢掉。h3 入站此前没有这一支：第一段被交给判定器并记下
 *          「头段已过」，于是那条真响应按 RFC 9114 §4.1 判成「同一消息里出现了第二个头段」——
 *          一个用 Expect 的出站请求被自家服务端的 100 打死。RFC 9114 §4.1/§5.1 说清过渡响应是一份
 *          **独立**的消息，不占最终响应的头段位，也不该把它的字段交给业务
 * @note 证伪：摘掉入站的过渡响应分支（让 1xx 走正常头段交付），本条第一格在 `malformedRequests`
 *       与「头段恰好一次」两处红；把这一支写成「一切头段都用一次性判定器」，第二格（两条最终响应）红
 */
TEST(Http3Connection, InformationalResponseSectionDoesNotTakeTheHeadSlotFromTheFinalResponse)
{
    // 格一：100 之后跟 200，交付的只有 200 那一份
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);

        std::string       encoderBytes;
        const std::string interimBlock = encodeSection({QpackHeaderField{":status", "100"}}, encoderBytes);
        const std::string finalBlock   = encodeSection({QpackHeaderField{":status", "200"}, QpackHeaderField{"x-final", "yes"}}, encoderBytes);

        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, interimBlock)), false);
        EXPECT_TRUE(events.headerFields.empty()) << "过渡响应的字段不该交给上层";
        EXPECT_TRUE(events.headerBlocksReceived.empty()) << "过渡响应不该被当成这条流的头段";

        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, finalBlock)), false);
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "{}")), true);

        EXPECT_TRUE(events.malformedRequests.empty()) << "最终响应被自家服务端的 100 顶成了「第二个头段」";
        ASSERT_EQ(events.headerBlocksReceived.size(), 1u) << "最终响应要作为这条流的头段交付一次";
        ASSERT_EQ(events.headerFields.size(), 2u) << "过渡响应的 :status 混进了业务读到的字段里";
        EXPECT_EQ(events.headerFields[0].name, ":status");
        EXPECT_EQ(events.headerFields[0].value, "200");
        EXPECT_EQ(events.bodyBytes, "{}") << "正文要照常交出来：这条响应没被判死";
        EXPECT_TRUE(events.requestsEnded.size() == 1U) << "收齐之后要把这条请求交上去";
        EXPECT_FALSE(connection->isBroken());
    }

    // 格二：两条**最终**响应仍然非法——豁免只给 1xx，别把这道闸一并拆掉
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);

        std::string       encoderBytes;
        const std::string finalBlock = encodeSection({QpackHeaderField{":status", "200"}}, encoderBytes);
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, finalBlock)), false);
        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, finalBlock)), false);
        EXPECT_EQ(events.headerBlocksReceived.size(), 1u) << "第一条最终响应照常交付";
        EXPECT_EQ(events.malformedRequests.size(), 1u) << "第二条最终响应仍要按「同一消息里的第二个头段」拒掉";
    }
}

/**
 * @brief 对端打断一条流时，交上去的原因要点名对端那一帧与它给的码
 * @details 过去这一路交上去的只有流号：原因文案由调用方自己编一句「对端发了 RESET_STREAM」，
 *          而对端给的应用层错误码在传输层就被丢掉了。h3 客户端认「服务端没做任何应用层处理就拒了
 *          这条请求」靠的正是那个码（RFC 9114 §7 的 H3_REQUEST_REJECTED，认出来才敢把非幂等请求
 *          换条连接重来），STOP_SENDING 则是另一回事——它只说「对端不再收」，不带任何保证。
 *          交上去的 `Http3ErrorCode` 仍是 H3_REQUEST_CANCELLED：那一位是「本端据此收口这条流」的值，
 *          服务端会把它回声进自己那一帧，而对端可以给任何 62 位整数（含 h3 码空间之外的 0）
 * @note 证伪：把应用层错误码从流层那道队列里摘掉（只交流号），格一红在原因文案上；把复位与叫停
 *       混成一种形状报，格二红
 */
TEST(Http3Connection, PeerStreamAbortNamesThePeersFrameAndErrorCode)
{
    const std::vector<QpackHeaderField> requestFields{QpackHeaderField{":method", "POST"}, QpackHeaderField{":scheme", "https"}, QpackHeaderField{":authority", "example.com"},
                                                      QpackHeaderField{":path", "/upload"}};

    // 格一：对端用 H3_REQUEST_REJECTED 复位——这个码要出现在调用方读得到的那句原因里
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);
        ASSERT_TRUE(connection->submitRequestHead(kRequestStreamId, requestFields, false).has_value());

        connection->noteStreamCancelledByPeer(kRequestStreamId, static_cast<std::uint64_t>(Http3ErrorCode::RequestRejected), true);

        ASSERT_EQ(events.streamResetReasons.size(), 1U);
        EXPECT_NE(events.streamResetReasons[0].second.find("H3_REQUEST_REJECTED"), std::string::npos)
                << "对端给的码被丢掉了：客户端再没有依据判这条请求能不能当没发过重来，实际原因：" << events.streamResetReasons[0].second;
        EXPECT_NE(events.streamResetReasons[0].second.find("RESET_STREAM"), std::string::npos);
        ASSERT_EQ(events.streamsReset.size(), 1U);
        EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::RequestCancelled) << "交上去的码是本端收口这条流用的，不该原样回声对端给的那个";
    }

    // 格二：STOP_SENDING 是「对端不再收」，不能报成复位
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);
        ASSERT_TRUE(connection->submitRequestHead(kRequestStreamId, requestFields, false).has_value());

        connection->noteStreamCancelledByPeer(kRequestStreamId, static_cast<std::uint64_t>(Http3ErrorCode::RequestRejected), false);

        ASSERT_EQ(events.streamResetReasons.size(), 1U);
        EXPECT_NE(events.streamResetReasons[0].second.find("STOP_SENDING"), std::string::npos) << "实际原因：" << events.streamResetReasons[0].second;
        EXPECT_EQ(events.streamResetReasons[0].second.find("RESET_STREAM"), std::string::npos) << "叫停被报成复位：那句文案会让上层以为对端保证过「没处理」";
    }
}

/**
 * @brief 一条流没有最终头段就收尾时，文案要分清「只收到过渡响应」与「一个段都没收到」，并按角色说流名
 * @details 两种形状的排查方向完全不同：前者是对端发了 1xx 却没跟最终响应（RFC 9114 §4.1 要求响应以
 *          带非 1xx 的 :status 的头段收尾），后者是对端开了流什么也没写就 FIN。过去两者共用一句
 *          「请求流上没有头段就结束了」，而本端是客户端时这条流上装的其实是响应——排查的人被指向
 *          一个没发生过的形状，还去查了一个不存在的请求
 * @note 证伪：摘掉 `isInformationalSectionSeen` 那一位（两种形状合用一句话），格一红；把流名写死成
 *       「请求流」，格一与格二红在流名上
 */
TEST(Http3Connection, NamesTheShapeOfAStreamThatEndedWithoutAFinalHeadSection)
{
    // 格一：客户端只收到 103 就 FIN（那一刀跟着这段头块一起来）
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);
        std::string   encoderBytes;

        connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection({QpackHeaderField{":status", "103"}}, encoderBytes))), true);

        ASSERT_EQ(events.streamsReset.size(), 1U);
        EXPECT_EQ(events.streamsReset[0].second, Http3ErrorCode::MessageError);
        ASSERT_EQ(events.streamResetReasons.size(), 1U);
        EXPECT_NE(events.streamResetReasons[0].second.find("过渡响应"), std::string::npos) << "实际文案：" << events.streamResetReasons[0].second;
        EXPECT_NE(events.streamResetReasons[0].second.find("响应流"), std::string::npos) << "本端是客户端，这条流上装的是响应：" << events.streamResetReasons[0].second;
    }

    // 格二：客户端一个段都没收到就 FIN —— 与格一是两种形状，文案不能是同一句
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events, {}, AsynGyanis::Net::QuicConnectionRole::Client);

        connection->consumeStreamData(kRequestStreamId, bytesOfText(std::string{}), true);

        ASSERT_EQ(events.streamResetReasons.size(), 1U);
        EXPECT_NE(events.streamResetReasons[0].second.find("没有头段"), std::string::npos) << "实际文案：" << events.streamResetReasons[0].second;
        EXPECT_EQ(events.streamResetReasons[0].second.find("过渡响应"), std::string::npos) << "一个段都没收到，不该说成「只收到过渡响应」";
    }

    // 格三：服务端角色下仍说「请求流」（这条流上装的确实是请求）
    {
        FakeTransport transport;
        EventLog      events;
        auto          connection = makeConnection(transport, events);

        connection->consumeStreamData(kRequestStreamId, bytesOfText(std::string{}), true);

        ASSERT_EQ(events.streamResetReasons.size(), 1U);
        EXPECT_NE(events.streamResetReasons[0].second.find("请求流"), std::string::npos) << "实际文案：" << events.streamResetReasons[0].second;
    }
}

/**
 * @brief 一条流收口之后，本端编码器替它留的动态表引用与阻塞名额要还不回 Section Ack 的对端也能归还
 * @details 本端发出去的头块引用了动态表项时，编码器替这条流记两样东西：那些表项的引用计数，和一个
 *          「可能让对端阻塞」的名额（名额总数按对端 SETTINGS_QPACK_BLOCKED_STREAMS 封顶，RFC 9204 §2.1.2
 *          要求「任何时刻」都不超）。平时这两样由对端的 Section Ack 或 Stream Cancellation 归还，而
 *          `closeStreamIfDone`（两侧都收完、状态被摘掉）那条正常出口一样都不还：一个只读不回话的对端
 *          把名额占满之后，本端此后所有流都再也插不进动态表（退化成本端不用表，且再也回不来），
 *          而 `m_pendingSectionsByStreamId` 按只增不减的流号一条条攒下去。收口之后再没有那条流的
 *          头块要解，引用与名额本就该放；表项淘汰另有「对端已确认收到」那道闸兜着（§2.1.1：
 *          绝对索引不小于已知接收计数的项不可淘汰），所以这一步不会把对端还要用的表项提前挤掉
 * @note 证伪：摘掉 `closeStreamIfDone` 里的归还，本条在「第二条流仍要插得进动态表」那处红；被重置那条
 *       出口的另一半见 ResetStreamGivesBackItsEncoderBlockingSlot
 */
TEST(Http3Connection, ClosedStreamGivesBackItsEncoderBlockingSlot)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    // 对端通告：动态表容量 4096、最多只允许 1 条流处于可能阻塞的状态——占满一个名额就等于关掉本端的表
    const std::string peerSettings = std::string("\x01\x50\x00", 3) + std::string("\x07\x01", 2);
    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, peerSettings)), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);
    connection->consumeStreamData(kPeerDecoderStreamId, bytesOfText(streamTypePrefix(0x03)), true);

    std::string       requestEncoderBytes;
    const std::string requestBlock = encodeSection(minimalRequestFields(), requestEncoderBytes);

    // 第一条流：请求收齐、本端带一条表里没有的头部作答并收尾，两侧都完之后这条流被摘掉
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, requestBlock)), false);
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "payload")), true);
    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}, QpackHeaderField{"x-served-by", "first-stream"}}, true).has_value());
    connection->flush();
    ASSERT_EQ(events.streamsClosed.size(), 1u) << "前提：两侧都收完之后这条流要收掉";
    const std::size_t encoderBytesAfterFirst = transport.bytesOf(kLocalEncoderStreamId).size();
    ASSERT_GT(encoderBytesAfterFirst, std::string("\x02", 1).size()) << "前提：这段响应确实在编码器流上留下了指令";

    // 第二条流：换一个响应头，按附录 C 应当再插一项。对端一条解码器流指令都不回
    constexpr std::int64_t kSecondRequestStreamId = 4;
    connection->consumeStreamData(kSecondRequestStreamId, bytesOfText(makeFrame(0x01, requestBlock)), false);
    connection->consumeStreamData(kSecondRequestStreamId, bytesOfText(makeFrame(0x00, "payload")), true);
    ASSERT_TRUE(connection->submitResponseHead(kSecondRequestStreamId, {QpackHeaderField{":status", "200"}, QpackHeaderField{"x-served-by", "second-stream"}}, true).has_value());
    connection->flush();

    EXPECT_GT(transport.bytesOf(kLocalEncoderStreamId).size(), encoderBytesAfterFirst) << "第一条流收口后没归还阻塞名额，本端从此再也插不进动态表：编码器流上第二条指令都没了";
    EXPECT_FALSE(connection->isBroken());
}

/**
 * @brief 本端自己重置一条流，同样要把替它记的编码器阻塞名额放回去
 * @details 与上一条同一笔账的另一条出口：`failStream` 之前只清解码侧，不清编码器侧。这里的现场是
 *          「声明的 content-length 与实收正文不符」——本端按 §4.1.2 重置这条流，而它此前已经在这条流上
 *          发过一段引用动态表的响应，对端不会再来确认一条被重置的流
 * @note 证伪：摘掉 `failStream` 里的归还，本条在「第二条流仍要插得进动态表」那处红
 */
TEST(Http3Connection, ResetStreamGivesBackItsEncoderBlockingSlot)
{
    FakeTransport                  transport;
    EventLog                       events;
    Http3Connection::LocalSettings settings;
    settings.qpackMaximumTableCapacityByteCount = 4096;
    auto connection                             = makeConnection(transport, events, settings);

    const std::string peerSettings = std::string("\x01\x50\x00", 3) + std::string("\x07\x01", 2);
    connection->consumeStreamData(kPeerControlStreamId, bytesOfText(streamTypePrefix(0x00) + makeFrame(0x04, peerSettings)), false);
    connection->consumeStreamData(kPeerEncoderStreamId, bytesOfText(streamTypePrefix(0x02)), false);

    auto lyingFields = minimalRequestFields();
    lyingFields.push_back(QpackHeaderField{"content-length", "3"}); // 声明 3 字节
    std::string       requestEncoderBytes;
    const std::string requestBlock = encodeSection(lyingFields, requestEncoderBytes);

    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x01, requestBlock)), false);
    ASSERT_EQ(events.headerBlocksReceived.size(), 1u) << "前提：请求头段收下";
    ASSERT_TRUE(connection->submitResponseHead(kRequestStreamId, {QpackHeaderField{":status", "200"}, QpackHeaderField{"x-served-by", "first-stream"}}, true).has_value());
    connection->flush();
    const std::size_t encoderBytesAfterFirst = transport.bytesOf(kLocalEncoderStreamId).size();
    ASSERT_GT(encoderBytesAfterFirst, std::string("\x02", 1).size()) << "前提：这段响应在编码器流上留下了指令";

    // 实收 7 字节并 END_STREAM：与声明不符，本端按 §4.1.2 重置这条流
    connection->consumeStreamData(kRequestStreamId, bytesOfText(makeFrame(0x00, "payload")), true);
    ASSERT_EQ(events.streamsReset.size(), 1u) << "前提：这条流被本端重置";

    constexpr std::int64_t kSecondRequestStreamId = 4;
    connection->consumeStreamData(kSecondRequestStreamId, bytesOfText(makeFrame(0x01, encodeSection(minimalRequestFields(), requestEncoderBytes))), false);
    ASSERT_TRUE(connection->submitResponseHead(kSecondRequestStreamId, {QpackHeaderField{":status", "200"}, QpackHeaderField{"x-served-by", "second-stream"}}, true).has_value());
    connection->flush();

    EXPECT_GT(transport.bytesOf(kLocalEncoderStreamId).size(), encoderBytesAfterFirst) << "被重置的流没归还阻塞名额，本端此后在所有流上都插不进动态表";
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

    connection->noteStreamCancelledByPeer(999, static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled), true);

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
