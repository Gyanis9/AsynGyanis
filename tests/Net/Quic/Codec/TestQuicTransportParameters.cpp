// TestQuicTransportParameters.cpp —— 传输参数编解码（RFC 9000 §18）用例
//
// 覆盖四块：
//   1) 逐字节向量：一组服务端参数编出来必须等于交叉校验过的 101 字节，解回去也要逐字段相等。
//      向量由本机 site-packages 里的 aioquic 1.3.0 解过一遍确认取值对得上，不是自产自验；
//      第二条向量直接取自那份独立实现的编码器，钉成「只解不编」的互操作样本；
//   2) 陌生标识必须忽略而不是判错（§7.4.2）：第二条向量里带着 max_datagram_frame_size(0x20) 与
//      保留标识 0x1b（§18.1 那一档），本实现不认识它们也照常解出其余各项；
//   3) 取值规则：允许非最短整数编码（§16 只对帧类型要求最短），但不容许取值里藏字段；
//      四个整型项的区间、连接标识 20 字节上限、令牌必须恰好 16 字节、disable 项必须零长；
//   4) 存在性规则：§7.3 要求两端都带 initial_source_connection_id、服务端还须带
//      original_destination_connection_id，而「零长的连接标识」是合法写法（§7.3 末段）不能误判成缺失。
// 解出来的结构体自持字节（不像帧那层是视图），因此把原文留在局部量里也不会悬空。

#include "Net/Quic/Codec/QuicTransportParameters.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;
        using AsynGyanis::Net::TestSupport::toUnsignedBytes;

        /// 一组服务端参数的字节写法（101 字节）：本端编码器要逐字节复刻它
        constexpr std::string_view kServerParametersHex =
                "000808aded6e71ba8d5f"
                "0f088394c8f03e515708"
                "1008f0eec687a7eb7f48"
                "01026710"
                "03048000fff7"
                "040480010000"
                "050480010000"
                "060480004000"
                "070480004000"
                "08024080"
                "09024400"
                "0a0103"
                "0b0119"
                "0e0104"
                "0210c5004f2c3d9b3a17b6c8d2e4f0123456"
                "0c00";

        /// 那份独立实现编出的一组客户端参数（72 字节），末尾两项是本实现不认识的标识
        constexpr std::string_view kInteropClientParametersHex =
                "010480007530"
                "03048000fff7"
                "040480010000"
                "050480010000"
                "060480010000"
                "070480010000"
                "08024400"
                "09024400"
                "0a0103"
                "0b0119"
                "0e0104"
                "0f088394c8f03e515708"
                "200480010000"
                "1b0107";

        /// 只带必填项的最小合法参数：负向用例在它前面接坏项，免得失败原因落到「缺 ISCID」上
        constexpr std::string_view kMinimalClientParametersHex = "0f088394c8f03e515708";

        /**
         * @brief 把若干参数的字节接在最小合法参数之前
         * @param prefix 待接的字节（十六进制）
         * @return std::string 拼好的原文
         */
        std::string withMinimalParameters(const std::string_view prefix)
        {
            return std::string{prefix} + std::string{kMinimalClientParametersHex};
        }

        /**
         * @brief 造出 kServerParametersHex 对应的那份参数
         * @return QuicTransportParameters 整型项取非默认值，三个连接标识与令牌都在
         */
        QuicTransportParameters makeServerParameters()
        {
            QuicTransportParameters parameters;
            parameters.originalDestinationConnectionId = makeBytesFromHex("08aded6e71ba8d5f");
            parameters.maximumIdleTimeoutMilliseconds = 10000;
            parameters.maximumUdpPayloadSize = 65527;
            parameters.initialMaximumData = 65536;
            parameters.initialMaximumStreamDataBidirectionalLocal = 65536;
            parameters.initialMaximumStreamDataBidirectionalRemote = 16384;
            parameters.initialMaximumStreamDataUnidirectional = 16384;
            parameters.initialMaximumBidirectionalStreams = 128;
            parameters.initialMaximumUnidirectionalStreams = 1024;
            parameters.acknowledgmentDelayExponent = 3;
            parameters.maximumAcknowledgmentDelayMilliseconds = 25;
            parameters.activeConnectionIdLimit = 4;
            const auto tokenBytes = makeBytesFromHex("c5004f2c3d9b3a17b6c8d2e4f0123456");
            std::array<std::uint8_t, kQuicStatelessResetTokenLength> token{};
            std::ranges::copy(tokenBytes, token.begin());
            parameters.statelessResetToken = token;
            parameters.disableActiveMigration = true;
            parameters.initialSourceConnectionId = makeBytesFromHex("8394c8f03e515708");
            parameters.retrySourceConnectionId = makeBytesFromHex("f0eec687a7eb7f48");
            return parameters;
        }

        /**
         * @brief 从十六进制原文解参数
         * @param hexadecimalText 参数原文
         * @param senderRole 对端角色
         * @return 解出来的参数或解码错误
         */
        std::expected<QuicTransportParameters, QuicDecodeError>
        decodeHex(const std::string_view hexadecimalText, const QuicTransportParameterSenderRole senderRole)
        {
            return decodeQuicTransportParameters(makeBytesFromHex(hexadecimalText), senderRole);
        }

        /**
         * @brief 断言一段原文被判失败，且类别符合预期
         * @details 类别也要钉：把「格式错」和「字节不够」混成一种，上层回给对端的错误码就选不准
         * @param hexadecimalText 参数原文
         * @param senderRole 对端角色
         * @param expectedKind 预期的失败类别
         */
        void expectRejected(const std::string_view hexadecimalText,
                            const QuicTransportParameterSenderRole senderRole,
                            const QuicDecodeErrorKind expectedKind)
        {
            const auto decoded = decodeHex(hexadecimalText, senderRole);
            ASSERT_FALSE(decoded.has_value()) << "这段字节本该被拒：" << hexadecimalText;
            EXPECT_EQ(decoded.error().kind, expectedKind) << decoded.error().message;
        }
    } // namespace

    /**
     * @brief 编码逐字节复刻向量，解码再逐字段还原
     */
    TEST(QuicTransportParameters, RoundTripsTheCrossCheckedVector)
    {
        std::string bytes;
        appendQuicTransportParameters(bytes, makeServerParameters());
        EXPECT_EQ(toUnsignedBytes(bytes), makeBytesFromHex(kServerParametersHex));

        const auto decoded = decodeHex(kServerParametersHex, QuicTransportParameterSenderRole::Server);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        EXPECT_EQ(*decoded, makeServerParameters());
    }

    /**
     * @brief 另一份实现写的字节要能解，未识别的标识只跳过不判错
     */
    TEST(QuicTransportParameters, DecodesParametersWrittenByAnotherImplementation)
    {
        const auto decoded = decodeHex(kInteropClientParametersHex, QuicTransportParameterSenderRole::Client);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        EXPECT_EQ(decoded->maximumIdleTimeoutMilliseconds, 30000);
        EXPECT_EQ(decoded->initialMaximumStreamDataBidirectionalRemote, 65536);
        EXPECT_EQ(decoded->initialMaximumBidirectionalStreams, 1024);
        EXPECT_EQ(decoded->activeConnectionIdLimit, 4);
        ASSERT_TRUE(decoded->initialSourceConnectionId.has_value());
        EXPECT_EQ(*decoded->initialSourceConnectionId, makeBytesFromHex("8394c8f03e515708"));
        EXPECT_FALSE(decoded->disableActiveMigration);
        // 0x20（max_datagram_frame_size）与保留标识 0x1b 都被忽略， absent 的项按 §18.2 取默认
        EXPECT_EQ(decoded->maximumUdpPayloadSize, kQuicDefaultMaximumUdpPayloadSize);
    }

    /**
     * @brief 整数项允许非最短编码，但不容许取值里再藏字节
     */
    TEST(QuicTransportParameters, AcceptsWideIntegerEncodingButRejectsTrailingBytes)
    {
        // 0x01 的长度域是 8，整数本身写成 8 字节档（前缀 11）的 10000：§16 只把「必须最短」留给帧类型
        const auto wide = decodeHex(withMinimalParameters("0108c000000000002710"),
                                    QuicTransportParameterSenderRole::Client);
        ASSERT_TRUE(wide.has_value()) << wide.error().message;
        EXPECT_EQ(wide->maximumIdleTimeoutMilliseconds, 10000);

        // 同样声明 3 字节，可整数只占 2 字节，多出来的 1 字节没有任何定义能解释
        expectRejected(withMinimalParameters("0103671000"),
                       QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
    }

    /**
     * @brief 整型项的取值区间按 §18.2 卡住，边界值本身合法
     */
    TEST(QuicTransportParameters, RejectsOutOfRangeIntegerValues)
    {
        // max_udp_payload_size 小于 1200 非法，正好 1200 合法
        expectRejected(withMinimalParameters("030244af"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        const auto atUdpFloor = decodeHex(withMinimalParameters("030244b0"), QuicTransportParameterSenderRole::Client);
        ASSERT_TRUE(atUdpFloor.has_value()) << atUdpFloor.error().message;
        EXPECT_EQ(atUdpFloor->maximumUdpPayloadSize, 1200);

        // ack_delay_exponent 大于 20 非法
        expectRejected(withMinimalParameters("0a0115"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        // max_ack_delay 达到 2^14 非法（这个值超出 2 字节档，只能写成 4 字节档）
        expectRejected(withMinimalParameters("0b0480004000"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        // active_connection_id_limit 小于 2 非法
        expectRejected(withMinimalParameters("0e0101"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        // 流数上限大于 2^60 非法（§4.6：再按「上限 * 4 + 首号」算流号就超出变长整数的表达能力）。
        // 0x08 是 initial_max_streams_bidi、0x09 是 unidirectional，两档各钉一条，防止只给一档设界。
        // 取值写成 8 字节档：0xD0 是「前缀 11 + 高位 0x10」，故 d0...01 即 2^60 + 1
        expectRejected(withMinimalParameters("0808d000000000000001"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        expectRejected(withMinimalParameters("0908d000000000000001"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        // 恰好 2^60 是允许的上界
        const auto atStreamLimitCeiling = decodeHex(withMinimalParameters("0808d000000000000000"),
                                                    QuicTransportParameterSenderRole::Client);
        ASSERT_TRUE(atStreamLimitCeiling.has_value()) << atStreamLimitCeiling.error().message;
        EXPECT_EQ(atStreamLimitCeiling->initialMaximumBidirectionalStreams, std::uint64_t{1} << 60);
    }

    /**
     * @brief 已知标识出现第二次即判错（§7.4）
     */
    TEST(QuicTransportParameters, RejectsDuplicatedKnownParameter)
    {
        expectRejected(withMinimalParameters("0102671001026710"),
                       QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
    }

    /**
     * @brief 定长与零长项的长度不合、连接标识超 20 字节，都判格式错
     */
    TEST(QuicTransportParameters, RejectsWrongLengthValues)
    {
        // stateless_reset_token 必须恰好 16 字节
        expectRejected(withMinimalParameters("0204aabbccdd"), QuicTransportParameterSenderRole::Server,
                       QuicDecodeErrorKind::Malformed);
        // disable_active_migration 是零长项
        expectRejected(withMinimalParameters("0c01ff"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        // 连接标识超过 v1 的 20 字节上限：长度域 0x15 就是 21 字节
        expectRejected(withMinimalParameters("0015" + std::string(42, 'a')),
                       QuicTransportParameterSenderRole::Server,
                       QuicDecodeErrorKind::Malformed);
    }

    /**
     * @brief 服务端专属的四个标识由客户端交来即判错（§18.2 末段）
     */
    TEST(QuicTransportParameters, RejectsServerOnlyParametersFromClient)
    {
        expectRejected(withMinimalParameters("000808aded6e71ba8d5f"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        expectRejected(withMinimalParameters("0210c5004f2c3d9b3a17b6c8d2e4f0123456"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        // preferred_address 本实现不建模，但它仍在禁止名单里，不会被漏成「未知即忽略」
        expectRejected(withMinimalParameters("0d00"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
        expectRejected(withMinimalParameters("1008f0eec687a7eb7f48"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Malformed);
    }

    /**
     * @brief 必填的连接标识缺了就判错，零长的那个不算缺
     */
    TEST(QuicTransportParameters, RequiresTheHandshakeConnectionIdParameters)
    {
        // 只有一项整型参数，没有任何连接标识参数
        expectRejected("01026710", QuicTransportParameterSenderRole::Client, QuicDecodeErrorKind::Malformed);
        // 选了零长连接标识时仍要带上零长的取值（§7.3 末段），所以这条合法
        const auto zeroLength = decodeHex("0f00", QuicTransportParameterSenderRole::Client);
        ASSERT_TRUE(zeroLength.has_value()) << zeroLength.error().message;
        ASSERT_TRUE(zeroLength->initialSourceConnectionId.has_value());
        EXPECT_TRUE(zeroLength->initialSourceConnectionId->empty());
        // 服务端还须带 original_destination_connection_id，只带 ISCID 的字节换成服务端交来就要判错
        expectRejected(kMinimalClientParametersHex, QuicTransportParameterSenderRole::Server,
                       QuicDecodeErrorKind::Malformed);
    }

    /**
     * @brief 字段越过原文末尾的按截断处理，好与真正的格式错分开
     */
    TEST(QuicTransportParameters, ReportsTruncationSeparately)
    {
        // 长度域声明 8 字节，只剩 4 字节
        expectRejected("000808aded", QuicTransportParameterSenderRole::Server, QuicDecodeErrorKind::Truncated);
        // 末尾多出一个 8 字节档整数的开头，标识域读不完
        expectRejected(withMinimalParameters("c0"), QuicTransportParameterSenderRole::Client,
                       QuicDecodeErrorKind::Truncated);
    }
} // namespace AsynGyanis::Net
