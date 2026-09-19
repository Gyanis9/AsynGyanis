// TestQuicFrame.cpp —— QUIC 帧层（RFC 9000 §19）的编解码用例
//
// 覆盖五块：
//   1) 规范形态的字节：每种帧按 §19 的字段图手算期望字节再逐字节比（变长整数的档位、STREAM 的
//      OFF/LEN/FIN 位、ACK 的 First ACK Range 与 gap 递推都在这一步核到）；
//   2) 解码：逐型 std::get 核字段，并检查多帧序列的顺序与 quicFrameTypeValue 的取值；
//   3) 两条容易被写错的规则：0x1c 形态的 Frame Type 字段**恒在**（不知道触发帧时填 0），
//      以及 LEN 位为 0 的 STREAM 数据延伸到载荷末尾（重新编码会变形态但不改变语义）；
//   4) ACK 区间的递推：largest = previous_smallest - gap - 2 算出负包号一律判 Malformed（§19.3.1），
//      编码侧同样拒绝会算出负数的区间输入；
//   5) 拒绝面：帧类型非最短编码（§16 里唯一的例外）、未定义类型、各字段越出载荷末尾、
//      NEW_CONNECTION_ID 的连接标识长度不在 1..20；
//   6) ACK 区间的折叠助手 `buildQuicAcknowledgementRanges`：相邻合并、超出确认值的不认、
//      段数封顶后砍最老的区间、空集合时兜底成只含最大确认值那一段。
// 用例全是纯计算，不起网络也不依赖外部服务。

#include "Net/Quic/Codec/QuicFrame.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::containsText;
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;
        using AsynGyanis::Net::TestSupport::toUnsignedBytes;

        /**
         * @brief 编出一帧的字节
         * @param frame 帧
         * @return std::vector<std::uint8_t> 帧字节
         */
        std::vector<std::uint8_t> encodeOne(const QuicFrame &frame)
        {
            std::string bytes;
            appendQuicFrame(bytes, frame);
            return toUnsignedBytes(bytes);
        }

        /**
         * @brief 解出「恰好一帧」的载荷，多帧或失败都返回空
         * @details 帧里的 CRYPTO/STREAM/标识等字段是**指向载荷的视图**（生产接口就是这么定的，
         *          为的是不拷载荷），所以载荷的存储必须由调用方持有并活得比帧久；
         *          本助手若自己拿局部 vector，返回的帧当场悬空（ASan 实测 heap-use-after-free）。
         * @param hexadecimalText 载荷的十六进制文本
         * @param storage 出参：解码用的字节，必须与返回的帧在同一作用域内存活
         * @return std::optional<QuicFrame> 唯一的一帧
         */
        std::optional<QuicFrame> decodeSingle(std::string_view hexadecimalText, std::vector<std::uint8_t> &storage)
        {
            storage = makeBytesFromHex(hexadecimalText);
            const auto frames = decodeQuicFrames(std::span<const std::uint8_t>(storage));
            if (!frames.has_value() || frames->size() != 1)
            {
                storage.clear();
                return std::nullopt;
            }
            return frames->front();
        }

        /**
         * @brief 拼一段有稳定存储的字节，供帧里的视图字段引用
         * @param hexadecimalText 十六进制文本
         * @return std::vector<std::uint8_t> 字节容器
         */
        std::vector<std::uint8_t> bytesOf(std::string_view hexadecimalText)
        {
            return makeBytesFromHex(hexadecimalText);
        }
    } // namespace

    /**
     * @brief 单字段帧的规范字节：类型值与变长整数的档位都要按 §19 的字段图落在同一些字节上
     */
    TEST(QuicFrame, EncodesSingleFieldFramesToCanonicalBytes)
    {
        struct Case
        {
            QuicFrame frame;
            const char *hexadecimalText;
        };

        const std::vector<Case> cases{
                {QuicPaddingFrame{}, "00"},
                {QuicPingFrame{}, "01"},
                {QuicHandshakeDoneFrame{}, "1e"},
                {QuicMaxDataFrame{1000}, "1043e8"},
                {QuicMaxStreamDataFrame{4, 2000}, "110447d0"},
                {QuicMaxStreamsFrame{3, false}, "1203"},
                {QuicMaxStreamsFrame{3, true}, "1303"},
                {QuicDataBlockedFrame{4000}, "144fa0"},
                {QuicStreamDataBlockedFrame{4, 1000}, "150443e8"},
                {QuicStreamsBlockedFrame{7, false}, "1607"},
                {QuicStreamsBlockedFrame{7, true}, "1707"},
                {QuicRetireConnectionIdFrame{3}, "1903"},
                {QuicStopSendingFrame{4, 0x0a}, "05040a"},
                {QuicResetStreamFrame{4, 0x0a, 2000}, "04040a47d0"},
        };

        for (const Case &testCase: cases)
        {
            EXPECT_EQ(encodeOne(testCase.frame), makeBytesFromHex(testCase.hexadecimalText))
                    << "帧类型 0x" << std::hex << quicFrameTypeValue(testCase.frame);
        }
    }

    /**
     * @brief 带数据与带变长文本的帧：CRYPTO/STREAM/NEW_TOKEN/路径验证两帧/两种 CONNECTION_CLOSE
     */
    TEST(QuicFrame, EncodesDataBearingFrames)
    {
        const auto abc = bytesOf("616263");
        EXPECT_EQ(encodeOne(QuicCryptoFrame{0, std::span<const std::uint8_t>(abc)}), makeBytesFromHex("060003616263"));

        const auto oneByte = bytesOf("ff");
        EXPECT_EQ(encodeOne(QuicStreamFrame{0, 0, std::span<const std::uint8_t>(oneByte), false}), makeBytesFromHex("0a0001ff"));

        const auto twoBytes = bytesOf("0102");
        EXPECT_EQ(encodeOne(QuicStreamFrame{4, 8, std::span<const std::uint8_t>(twoBytes), true}), makeBytesFromHex("0f0408020102"));

        const auto token = bytesOf("abcd");
        EXPECT_EQ(encodeOne(QuicNewTokenFrame{std::span<const std::uint8_t>(token)}), makeBytesFromHex("0702abcd"));

        const QuicPathChallengeFrame challenge{{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
        EXPECT_EQ(encodeOne(challenge), makeBytesFromHex("1a0102030405060708"));
        const QuicPathResponseFrame response{{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
        EXPECT_EQ(encodeOne(response), makeBytesFromHex("1b0102030405060708"));

        const auto bye = bytesOf("627965");
        EXPECT_EQ(encodeOne(QuicConnectionCloseFrame{1, std::uint64_t{6}, std::span<const std::uint8_t>(bye)}),
                  makeBytesFromHex("1c010603627965"));
        EXPECT_EQ(encodeOne(QuicConnectionCloseFrame{10, std::nullopt, std::span<const std::uint8_t>{}}),
                  makeBytesFromHex("1d0a00"));
    }

    /**
     * @brief ACK 帧的线格式：首区间走 First ACK Range，其后每段是 (gap, 长度)，且 0x03 才带三个 ECN 计数
     */
    TEST(QuicFrame, EncodesAcknowledgementFrameInWireForm)
    {
        QuicAcknowledgementFrame single;
        single.largestAcknowledgedPacketNumber = 5;
        single.ranges = {{0, 5}};
        EXPECT_EQ(encodeOne(single), makeBytesFromHex("0205000005"));

        // ACK Delay 是**线上值**而不是微秒：300 原样落到第三个字段，乘除 2^指数都不在本层
        single.acknowledgementDelay = 300;
        EXPECT_EQ(encodeOne(single), makeBytesFromHex("0205412c0005"));

        single.hasEcnCounts = true;
        single.acknowledgementDelay = 0;
        EXPECT_EQ(encodeOne(single), makeBytesFromHex("0305000005000000"));

        // 区间 [8,10] 与 [4,4]：First ACK Range=2，下一段 gap = 8-4-2 = 2、长度 0
        QuicAcknowledgementFrame twoRanges;
        twoRanges.largestAcknowledgedPacketNumber = 10;
        twoRanges.ranges = {{8, 10}, {4, 4}};
        EXPECT_EQ(encodeOne(twoRanges), makeBytesFromHex("020a0001020200"));
    }

    /**
     * @brief 逐型解码并核字段：区间还原成绝对包号、标识与令牌是指向载荷的视图
     */
    TEST(QuicFrame, DecodesEachFrameTypeIntoItsFields)
    {
        // 每条帧各自的载荷都要与解出的帧同域存活：视图字段指向的就是这些字节
        std::vector<std::uint8_t> streamStorage;
        const auto stream = decodeSingle("0f0408020102", streamStorage);
        ASSERT_TRUE(stream.has_value());
        const auto &decodedStream = std::get<QuicStreamFrame>(*stream);
        EXPECT_EQ(decodedStream.streamId, 4U);
        EXPECT_EQ(decodedStream.offset, 8U);
        EXPECT_TRUE(decodedStream.isFinal);
        EXPECT_EQ(std::vector<std::uint8_t>(decodedStream.data.begin(), decodedStream.data.end()), bytesOf("0102"));

        std::vector<std::uint8_t> acknowledgementStorage;
        const auto acknowledgement = decodeSingle("020a0001020200", acknowledgementStorage);
        ASSERT_TRUE(acknowledgement.has_value());
        const auto &decodedAcknowledgement = std::get<QuicAcknowledgementFrame>(*acknowledgement);
        EXPECT_EQ(decodedAcknowledgement.largestAcknowledgedPacketNumber, 10U);
        EXPECT_FALSE(decodedAcknowledgement.hasEcnCounts);
        // 线格式的两段区间还原成绝对包号，正是编码时给的那两段
        EXPECT_EQ(decodedAcknowledgement.ranges, (std::vector<QuicAcknowledgementRange>{{8, 10}, {4, 4}}));

        std::vector<std::uint8_t> delayStorage;
        const auto withDelay = decodeSingle("0205412c0005", delayStorage);
        ASSERT_TRUE(withDelay.has_value());
        // 同一个 300 解回来还是 300：本层不拿指数去乘（那需要知道对端的 ack_delay_exponent）
        EXPECT_EQ(std::get<QuicAcknowledgementFrame>(*withDelay).acknowledgementDelay, 300U);

        std::vector<std::uint8_t> ecnStorage;
        const auto withEcn = decodeSingle("0305000005010203", ecnStorage);
        ASSERT_TRUE(withEcn.has_value());
        const auto &decodedEcn = std::get<QuicAcknowledgementFrame>(*withEcn);
        EXPECT_TRUE(decodedEcn.hasEcnCounts);
        EXPECT_EQ(decodedEcn.ecnCounts, (std::array<std::uint64_t, 3>{1, 2, 3}));

        std::vector<std::uint8_t> identifierStorage;
        const auto newConnectionId = decodeSingle("180100088394c8f03e515708a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", identifierStorage);
        ASSERT_TRUE(newConnectionId.has_value());
        const auto &decodedIdentifier = std::get<QuicNewConnectionIdFrame>(*newConnectionId);
        EXPECT_EQ(decodedIdentifier.sequenceNumber, 1U);
        EXPECT_EQ(decodedIdentifier.retirePriorTo, 0U);
        EXPECT_EQ(std::vector<std::uint8_t>(decodedIdentifier.connectionId.begin(), decodedIdentifier.connectionId.end()),
                  bytesOf("8394c8f03e515708"));
        EXPECT_EQ(std::vector<std::uint8_t>(decodedIdentifier.statelessResetToken.begin(),
                                            decodedIdentifier.statelessResetToken.end()),
                  bytesOf("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"));

        std::vector<std::uint8_t> closeStorage;
        const auto close = decodeSingle("1c010603627965", closeStorage);
        ASSERT_TRUE(close.has_value());
        const auto &decodedClose = std::get<QuicConnectionCloseFrame>(*close);
        EXPECT_EQ(decodedClose.errorCode, 1U);
        ASSERT_TRUE(decodedClose.triggeredFrameType.has_value());
        EXPECT_EQ(*decodedClose.triggeredFrameType, 6U);

        // 0x1d 形态线上不带 Frame Type：解出来必须是 nullopt，而不是「触发帧是 PADDING」
        std::vector<std::uint8_t> applicationCloseStorage;
        const auto applicationClose = decodeSingle("1d0a00", applicationCloseStorage);
        ASSERT_TRUE(applicationClose.has_value());
        EXPECT_FALSE(std::get<QuicConnectionCloseFrame>(*applicationClose).triggeredFrameType.has_value());

        std::vector<std::uint8_t> blockedStorage;
        const auto blocked = decodeSingle("1707", blockedStorage);
        ASSERT_TRUE(blocked.has_value());
        EXPECT_TRUE(std::get<QuicStreamsBlockedFrame>(*blocked).isUnidirectional);
    }

    /**
     * @brief 一个载荷里按顺序解出多帧，读位置逐帧前移
     */
    TEST(QuicFrame, DecodesMultipleFramesInOrder)
    {
        const auto payload = bytesOf("011043e80006000141");
        const auto frames = decodeQuicFrames(payload);
        ASSERT_TRUE(frames.has_value()) << frames.error().message;
        ASSERT_EQ(frames->size(), 4U);
        EXPECT_TRUE(std::holds_alternative<QuicPingFrame>(frames->at(0)));
        EXPECT_TRUE(std::holds_alternative<QuicMaxDataFrame>(frames->at(1)));
        EXPECT_EQ(std::get<QuicMaxDataFrame>(frames->at(1)).maximumData, 1000U);
        EXPECT_TRUE(std::holds_alternative<QuicPaddingFrame>(frames->at(2)));
        EXPECT_TRUE(std::holds_alternative<QuicCryptoFrame>(frames->at(3)));
        EXPECT_EQ(std::get<QuicCryptoFrame>(frames->at(3)).data.size(), 1U);
    }

    /**
     * @brief LEN 位为 0 的 STREAM：数据延伸到载荷末尾；重新编码会换成带 Length 的规范形态
     */
    TEST(QuicFrame, DecodesStreamWithoutLengthBitToPayloadEnd)
    {
        std::vector<std::uint8_t> streamStorage;
        const auto stream = decodeSingle("0804abcd", streamStorage);
        ASSERT_TRUE(stream.has_value());
        const auto &decodedStream = std::get<QuicStreamFrame>(*stream);
        EXPECT_EQ(decodedStream.streamId, 4U);
        EXPECT_EQ(decodedStream.offset, 0U) << "OFF 位为 0 时偏移就是 0（§19.8）";
        EXPECT_FALSE(decodedStream.isFinal);
        EXPECT_EQ(std::vector<std::uint8_t>(decodedStream.data.begin(), decodedStream.data.end()), bytesOf("abcd"));

        // 同一帧的两种线格式：语义相同、字节不同，因此 round-trip 只对规范形态逐字节比
        EXPECT_EQ(encodeOne(*stream), makeBytesFromHex("0a0402abcd"));
    }

    /**
     * @brief 规范形态的字节 round-trip：解出来再编回去必须逐字节相同
     */
    TEST(QuicFrame, RoundTripsCanonicalEncodings)
    {
        const std::vector<const char *> canonical{
                "00",
                "01",
                "1e",
                "0205000005",
                "0305000005000000",
                "020a0001020200",
                "04040a47d0",
                "05040a",
                "060003616263",
                "0702abcd",
                "0a0001ff",
                "0f0408020102",
                "1043e8",
                "110447d0",
                "1203",
                "1303",
                "144fa0",
                "150443e8",
                "1607",
                "1707",
                "180100088394c8f03e515708a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
                "1903",
                "1a0102030405060708",
                "1b0102030405060708",
                "1c010603627965",
                "1d0a00",
        };

        for (const char *hexadecimalText: canonical)
        {
            // 存储与帧同域，且声明在帧之前：帧里的视图指向这些字节
            std::vector<std::uint8_t> storage;
            const auto frame = decodeSingle(hexadecimalText, storage);
            ASSERT_TRUE(frame.has_value()) << "规范形态解不回：" << hexadecimalText;
            EXPECT_EQ(encodeOne(*frame), makeBytesFromHex(hexadecimalText)) << "round-trip 变了字节：" << hexadecimalText;
        }
    }

    /**
     * @brief 帧类型是唯一要求最短编码的变长整数（§16 末段与 §12.4），非最短一律 Malformed
     */
    TEST(QuicFrame, RejectsNonMinimalFrameTypeEncoding)
    {
        // 0x4001 是 PING(0x01) 的两字节写法：值合法，但帧类型不许这么编
        const auto frames = decodeQuicFrames(bytesOf("4001"));
        ASSERT_FALSE(frames.has_value());
        EXPECT_EQ(frames.error().kind, QuicDecodeErrorKind::Malformed);
        EXPECT_TRUE(containsText(frames.error().message, "最短")) << frames.error().message;

        // 对照：同一非最短写法用在**取值**字段上必须照收，否则就把 §16 的「除帧类型外都合法」反着实现了
        std::vector<std::uint8_t> paddedStorage;
        const auto paddedValue = decodeSingle("1080000003", paddedStorage);
        ASSERT_TRUE(paddedValue.has_value());
        EXPECT_EQ(std::get<QuicMaxDataFrame>(*paddedValue).maximumData, 3U);
    }

    /**
     * @brief 未定义与扩展帧类型（含 DATAGRAM 的 0x30）一律 Malformed，交上层按 PROTOCOL_VIOLATION 收口
     */
    TEST(QuicFrame, RejectsUndefinedFrameTypes)
    {
        for (const char *hexadecimalText: {"20", "2f", "30", "31"})
        {
            const auto frames = decodeQuicFrames(bytesOf(hexadecimalText));
            ASSERT_FALSE(frames.has_value()) << hexadecimalText;
            EXPECT_EQ(frames.error().kind, QuicDecodeErrorKind::Malformed) << hexadecimalText;
            EXPECT_TRUE(containsText(frames.error().message, "未定义")) << frames.error().message;
        }
    }

    /**
     * @brief 字段越出载荷末尾一律 Truncated：长度字段说谎、数据不够、路径验证数据不够都要拦下
     */
    TEST(QuicFrame, RejectsTruncatedFrames)
    {
        const std::vector<const char *> cases{
                "0600",       // CRYPTO 只有偏移，Length 字段缺失
                "060002ff",   // CRYPTO 声明 2 字节数据只剩 1
                "0a0002ff",   // STREAM 的 LEN 位为 1、声明 2 字节数据只剩 1
                "0701",       // NEW_TOKEN 声明 1 字节令牌却没给
                "1a0102",     // PATH_CHALLENGE 只有 2 字节，需要 8
                "1801000883", // NEW_CONNECTION_ID 的标识声明 8 字节只剩 1
                "1801000141", // 标识给了 1 字节，但没有 16 字节重置令牌
                "1c010603ff", // 原因短语声明 3 字节只剩 1
                "02050001",   // 区间计数 1 却缺后面的字段
        };

        for (const char *hexadecimalText: cases)
        {
            const auto frames = decodeQuicFrames(bytesOf(hexadecimalText));
            ASSERT_FALSE(frames.has_value()) << hexadecimalText << " 必须判失败";
            EXPECT_EQ(frames.error().kind, QuicDecodeErrorKind::Truncated) << hexadecimalText << " → " << frames.error().message;
        }
    }

    /**
     * @brief 递推得出负包号的 ACK 一律 Malformed（RFC 9000 §19.3.1 要求 FRAME_ENCODING_ERROR）
     */
    TEST(QuicFrame, RejectsNonsensicalAcknowledgementRanges)
    {
        // 首区间长度 5 大于最大确认包号 1
        const auto firstRange = decodeQuicFrames(bytesOf("0201000005"));
        ASSERT_FALSE(firstRange.has_value());
        EXPECT_EQ(firstRange.error().kind, QuicDecodeErrorKind::Malformed);
        EXPECT_TRUE(containsText(firstRange.error().message, "负")) << firstRange.error().message;

        // gap 大到让下一段的最大包号变负：previous_smallest(5) - gap(6) - 2
        const auto laterRange = decodeQuicFrames(bytesOf("02050001000600"));
        ASSERT_FALSE(laterRange.has_value());
        EXPECT_EQ(laterRange.error().kind, QuicDecodeErrorKind::Malformed);
        EXPECT_TRUE(containsText(laterRange.error().message, "负")) << laterRange.error().message;

        // 区间计数超过载荷所能容纳：先判失败而不是按 0 继续解
        const auto tooManyRanges = decodeQuicFrames(bytesOf("0205009d7f3e7d00"));
        ASSERT_FALSE(tooManyRanges.has_value());
        EXPECT_EQ(tooManyRanges.error().kind, QuicDecodeErrorKind::Malformed);
    }

    /**
     * @brief NEW_CONNECTION_ID 的连接标识长度必须落在 1..20（§19.15 明确按 FRAME_ENCODING_ERROR 处理）
     */
    TEST(QuicFrame, RejectsNewConnectionIdWithInvalidLength)
    {
        for (const char *hexadecimalText: {"18010000"
                                           "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
                                           "1801001500112233445566778899aabbccddeeff00112233"
                                           "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"})
        {
            const auto frames = decodeQuicFrames(bytesOf(hexadecimalText));
            ASSERT_FALSE(frames.has_value());
            EXPECT_EQ(frames.error().kind, QuicDecodeErrorKind::Malformed);
            EXPECT_TRUE(containsText(frames.error().message, "FRAME_ENCODING_ERROR")) << frames.error().message;
        }
    }

    /**
     * @brief 空载荷解出空序列：是否算 PROTOCOL_VIOLATION 是包级策略，留给上层
     */
    TEST(QuicFrame, EmptyPayloadDecodesToNoFrames)
    {
        const auto frames = decodeQuicFrames(bytesOf(""));
        ASSERT_TRUE(frames.has_value()) << frames.error().message;
        EXPECT_TRUE(frames->empty()) << "§12.4 要求载荷至少一帧，但那是上层判定";
    }

    /**
     * @brief 编码侧拒绝会算出负包号或自相矛盾的 ACK 区间输入
     */
    TEST(QuicFrame, EncodingRejectsInconsistentAcknowledgementRanges)
    {
        QuicAcknowledgementFrame empty;
        empty.largestAcknowledgedPacketNumber = 5;
        EXPECT_THROW(static_cast<void>(encodeOne(empty)), Base::InvalidArgumentException);

        // 首区间的最大值不等于声明的最大确认包号
        QuicAcknowledgementFrame mismatched;
        mismatched.largestAcknowledgedPacketNumber = 10;
        mismatched.ranges = {{8, 9}};
        EXPECT_THROW(static_cast<void>(encodeOne(mismatched)), Base::InvalidArgumentException);

        // 第二段与第一段之间留不出 gap 要求的至少一个未确认包
        QuicAcknowledgementFrame overlapping;
        overlapping.largestAcknowledgedPacketNumber = 10;
        overlapping.ranges = {{8, 10}, {6, 7}};
        EXPECT_THROW(static_cast<void>(encodeOne(overlapping)), Base::InvalidArgumentException);
    }

    /**
     * @brief 编码侧拒绝非法的连接标识长度与重置令牌长度，别把本端 bug 发到对端去
     */
    TEST(QuicFrame, EncodingRejectsInvalidNewConnectionId)
    {
        const auto identifier = bytesOf("8394c8f03e515708");
        const auto shortToken = bytesOf("a0a1a2");
        const auto fullToken = bytesOf("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");

        EXPECT_THROW(static_cast<void>(encodeOne(QuicNewConnectionIdFrame{
                             1, 0, std::span<const std::uint8_t>{}, std::span<const std::uint8_t>(fullToken)})),
                     Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(encodeOne(QuicNewConnectionIdFrame{
                             1, 0, std::span<const std::uint8_t>(identifier), std::span<const std::uint8_t>(shortToken)})),
                     Base::InvalidArgumentException);
    }

    /**
     * @brief 帧类型值取自帧本身：STREAM 的低位变体、ACK 的 ECN 位与两种 CLOSE 形态都要对上
     */
    TEST(QuicFrame, ReportsFrameTypeValueIncludingFlagVariants)
    {
        const auto emptyData = std::span<const std::uint8_t>{};
        EXPECT_EQ(quicFrameTypeValue(QuicFrame{QuicStreamFrame{0, 0, emptyData, false}}), 0x0aULL);
        EXPECT_EQ(quicFrameTypeValue(QuicFrame{QuicStreamFrame{0, 8, emptyData, false}}), 0x0eULL);
        EXPECT_EQ(quicFrameTypeValue(QuicFrame{QuicStreamFrame{0, 0, emptyData, true}}), 0x0bULL);
        EXPECT_EQ(quicFrameTypeValue(QuicFrame{QuicStreamFrame{0, 8, emptyData, true}}), 0x0fULL);

        QuicAcknowledgementFrame acknowledgement;
        acknowledgement.ranges = {{0, 0}};
        EXPECT_EQ(quicFrameTypeValue(acknowledgement), 0x02ULL);
        acknowledgement.hasEcnCounts = true;
        EXPECT_EQ(quicFrameTypeValue(acknowledgement), 0x03ULL);

        EXPECT_EQ(quicFrameTypeValue(QuicConnectionCloseFrame{0, std::uint64_t{0}, emptyData}), 0x1cULL);
        EXPECT_EQ(quicFrameTypeValue(QuicConnectionCloseFrame{0, std::nullopt, emptyData}), 0x1dULL);
    }

    /**
     * @brief ACK 区间的折叠：相邻合并、跳过超过确认值的、段数封顶，以及空集合时的兜底
     */
    TEST(QuicFrame, BuildsAcknowledgementRangesByMergingAndCapping)
    {
        EXPECT_EQ(buildQuicAcknowledgementRanges({0, 1, 2, 3, 4}, 4),
                  (std::vector<QuicAcknowledgementRange>{{0, 4}}));
        EXPECT_EQ(buildQuicAcknowledgementRanges({0, 1, 3, 4}, 4),
                  (std::vector<QuicAcknowledgementRange>{{3, 4}, {0, 1}}));
        // 确认值之外的包号不属于本帧：2 收到过但这次不认
        EXPECT_EQ(buildQuicAcknowledgementRanges({0, 1, 2}, 1),
                  (std::vector<QuicAcknowledgementRange>{{0, 1}}));
        // 空集合也要守住「区间非空且首段含最大确认值」的不变式，否则会编出一个自相矛盾的帧
        EXPECT_EQ(buildQuicAcknowledgementRanges({}, 7),
                  (std::vector<QuicAcknowledgementRange>{{7, 7}}));

        std::set<std::uint64_t> isolated;
        for (std::uint64_t packetNumber = 1; packetNumber <= 79; packetNumber += 2)
        {
            isolated.insert(packetNumber);
        }
        const std::vector<QuicAcknowledgementRange> capped = buildQuicAcknowledgementRanges(isolated, 79);
        ASSERT_EQ(capped.size(), kQuicMaximumAcknowledgementRanges);
        EXPECT_EQ(capped.front(), (QuicAcknowledgementRange{79, 79}));
        EXPECT_EQ(capped.back(), (QuicAcknowledgementRange{79 - 2 * (kQuicMaximumAcknowledgementRanges - 1),
                                                          79 - 2 * (kQuicMaximumAcknowledgementRanges - 1)}));
        // 砍掉的是最老的那些：1 号包不再被覆盖，而首段仍含最大确认值
        EXPECT_GT(capped.back().smallestAcknowledged, 1U);
    }
} // namespace AsynGyanis::Net
