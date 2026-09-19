// TestHttp3Frame.cpp —— HTTP/3 帧层（RFC 9114 §7.2 与 §6.2）的字节编解码用例
//
// 覆盖六块：
//   1) 规范形态的字节：每种帧按 §7.2 的字段图手工推导出期望字节再逐字节比（RFC 9114 通篇没有给出
//      完整的帧字节样例，因此推导过程写在每个用例的注释里：Type/Length 各占哪一档、载荷几条字段）；
//   2) round-trip：解出来再编回去，字节必须与原文一致，且解出的帧与手工构造的帧逐字段相等；
//   3) 字节边界穷举：同一份多帧字节流按 1/2/3/5/7/11/13 字节切片喂进 Http3FrameReader，
//      解出的帧序列与逐字节重编码结果必须完全相同（参数化夹具）；
//   4) 长度不自洽：载荷比字段短、比字段长、SETTINGS 参数残缺，一律 Malformed（§7.1 判 H3_FRAME_ERROR）；
//   5) 忽略面：未知帧类型、未知/保留设置项、未知与保留流类型都不得报错，且一条都不丢（§9、§7.2.4、§6.2）；
//   6) 上限面：声明长度超单帧上限、喂入块长超上限、构造上限为 0、错误粘滞与 reset()。
// 变长整数**允许非最短编码**是本层与 QUIC 帧层的一处刻意差异（RFC 9000 §16 的最短要求只管 QUIC 自己的
// Frame Type 字段），第 7 条用例把它钉住，免得日后有人照搬 QUIC 那侧的从严判断。
// 用例全是纯计算，不起网络也不依赖外部服务。

#include "Net/Http3/Http3Frame.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "NetTestSupport.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
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

        /// 一段不透明载荷，用作 HEADERS/PUSH_PROMISE 的头字段段占位：本层不看 QPACK 内容
        constexpr std::array<std::uint8_t, 2> kEncodedFieldSection{0xaa, 0xbb};

        /// 「hello」的字节，用作 DATA 载荷
        constexpr std::array<std::uint8_t, 5> kHelloBytes{0x68, 0x65, 0x6c, 0x6c, 0x6f};

        /// 未知帧的载荷占位
        constexpr std::array<std::uint8_t, 2> kOpaqueTail{0xff, 0x00};

        /**
         * @brief 编出一帧的字节
         * @param frame 帧
         * @return std::vector<std::uint8_t> 帧字节
         */
        std::vector<std::uint8_t> encodeOne(const Http3Frame &frame)
        {
            std::string bytes;
            appendHttp3Frame(bytes, frame);
            return toUnsignedBytes(bytes);
        }

        /**
         * @brief 把载荷视图收进 vector，供与十六进制期望值逐字节比
         * @details 帧里的载荷是 std::span，与 vector 之间没有可用的 ==（gtest 会直接报找不到运算符），
         *          统一在这里收成容器再比
         * @param view 载荷视图
         * @return std::vector<std::uint8_t> 载荷字节副本
         */
        [[nodiscard]] std::vector<std::uint8_t> bytesOf(const std::span<const std::uint8_t> view)
        {
            return std::vector<std::uint8_t>(view.begin(), view.end());
        }

        /**
         * @brief 把单个帧结构体抬成 Http3Frame，供 EXPECT_EQ 与解码结果逐字段比
         * @details variant 的 == 只接同类型的两侧，直接写「变体 == 具体帧」编译不过
         * @param frame 具体帧
         * @return Http3Frame 装载同一份字段的变体
         */
        [[nodiscard]] Http3Frame frameOf(const Http3Frame &frame)
        {
            return frame;
        }

        /**
         * @brief 把一段完整字节喂进调用方持有的解码器，取第一帧
         * @details 解码器必须活到返回的帧用完为止：帧里的载荷字段指向解码器的内部缓冲（接口就这么定的，
         *          为的是不拷载荷）。本助手刻意不把解码器做成返回值，否则当场悬空。
         * @param hexadecimalText 帧字节的十六进制文本
         * @param reader 调用方持有的解码器，单帧上限要够大
         * @return std::expected<std::optional<Http3Frame>, Http3FrameError> 与 nextFrame() 同义
         */
        [[nodiscard]] std::expected<std::optional<Http3Frame>, Http3FrameError>
        decodeSoleFrame(const std::string_view hexadecimalText, Http3FrameReader &reader)
        {
            const auto wireBytes = makeBytesFromHex(hexadecimalText);
            const auto fed = reader.feed(std::span<const std::uint8_t>(wireBytes));
            if (!fed.has_value())
            {
                return std::unexpected(fed.error());
            }
            return reader.nextFrame();
        }
    } // namespace

    /**
     * @brief 每种已知帧的规范字节
     * @details 推导式统一是「Type 的变长整数 + Length 的变长整数 + 载荷」：
     *          DATA "hello" → 00 + 05 + 68656c6c6f（§7.2.1 图 4）；
     *          CANCEL_PUSH 1024 → 03 + 02 + 4400（1024>63 走 2 字节档，0x40|0x04、0x00，§7.2.3 图 6）；
     *          SETTINGS 三项已知一项未知 → 04 + 0c + 01 5000 06 80004000 07 03 31 07
     *          （4096 走 2 字节档、16384>16383 走 4 字节档，载荷 3+5+2+2=12 字节，§7.2.4 图 7）；
     *          GOAWAY 的 2^62-1 → 07 + 08 + FF×8（8 字节档首字节 0xC0|0x3F=0xFF，§7.2.6 图 9）；
     *          MAX_PUSH_ID 1500 → 0d + 02 + 45dc（§7.2.7 图 10）；
     *          未知类型 0x21 → 21 + 02 + ff00（§7.2.8 保留族，按 §9 原样交出）；
     *          未知类型 0x40（=64）→ 4040 + 00（64 超单字节档，故类型本身占 2 字节）。
     */
    TEST(Http3FrameEncoding, WritesCanonicalBytesForEveryKnownFrame)
    {
        struct Case
        {
            Http3Frame frame;
            const char *hexadecimalText;
        };

        const std::vector<Case> cases{
                {Http3DataFrame{std::span<const std::uint8_t>(kHelloBytes)}, "000568656c6c6f"},
                {Http3DataFrame{{}}, "0000"},
                {Http3HeadersFrame{std::span<const std::uint8_t>(kEncodedFieldSection)}, "0102aabb"},
                {Http3CancelPushFrame{31}, "03011f"},
                {Http3CancelPushFrame{1024}, "03024400"},
                {Http3SettingsFrame{{{Http3SettingId::QpackMaxTableCapacity, 4096},
                                    {Http3SettingId::MaxFieldSectionSize, 16384},
                                    {Http3SettingId::QpackBlockedStreams, 3}},
                                   {{0x31, 7}}},
                 "040c015000068000400007033107"},
                {Http3SettingsFrame{{}, {}}, "0400"},
                {Http3PushPromiseFrame{9, std::span<const std::uint8_t>(kEncodedFieldSection)}, "050309aabb"},
                {Http3PushPromiseFrame{9, {}}, "050109"},
                {Http3GoAwayFrame{4}, "070104"},
                {Http3GoAwayFrame{4611686018427387899ULL}, "0708fffffffffffffffb"},
                {Http3GoAwayFrame{kQuicMaximumIntegerValue}, "0708ffffffffffffffff"},
                {Http3MaxPushIdFrame{1500}, "0d0245dc"},
                {Http3UnknownFrame{0x21, std::span<const std::uint8_t>(kOpaqueTail)}, "2102ff00"},
        };

        for (const Case &testCase: cases)
        {
            const auto expectedBytes = makeBytesFromHex(testCase.hexadecimalText);
            EXPECT_EQ(encodeOne(testCase.frame), expectedBytes)
                    << "帧类型 0x" << std::hex << http3FrameTypeValue(testCase.frame);

            // 解出来再编回去必须逐字节还原：这一步同时核 §7.1 的「长度自洽」
            Http3FrameReader reader(expectedBytes.size() + 8);
            const auto decoded = decodeSoleFrame(testCase.hexadecimalText, reader);
            ASSERT_TRUE(decoded.has_value()) << decoded.error().message << " 原文 " << testCase.hexadecimalText;
            ASSERT_TRUE(decoded->has_value()) << "字节不够解出一帧：" << testCase.hexadecimalText;
            EXPECT_EQ(**decoded, testCase.frame) << "解码结果与手工构造的帧不等：" << testCase.hexadecimalText;
            EXPECT_EQ(encodeOne(**decoded), expectedBytes) << "重编码不等于原文：" << testCase.hexadecimalText;
        }
    }

    /**
     * @brief 单向流开头的流类型前缀（§6.2 图 1）：只有一个变长整数，没有长度域
     * @details 0x00 控制流、0x01 推送流（§6.2.1/§6.2.2）、0x02/0x03 QPACK 编码器与解码器流（RFC 9204 §4.2）。
     *          保留族 0x21 与超单字节档的 0x40 也照普通变长整数写：本层不为流类型设特殊编码。
     */
    TEST(Http3FrameEncoding, AppendsUnidirectionalStreamTypeHeader)
    {
        const std::vector<std::pair<Http3StreamType, const char *>> cases{
                {Http3StreamType::Control, "00"},
                {Http3StreamType::Push, "01"},
                {Http3StreamType::QpackEncoder, "02"},
                {Http3StreamType::QpackDecoder, "03"},
        };
        for (const auto &[streamType, hexadecimalText]: cases)
        {
            std::string bytes;
            appendHttp3StreamTypeHeader(bytes, streamType);
            EXPECT_EQ(toUnsignedBytes(bytes), makeBytesFromHex(hexadecimalText))
                    << "流类型 0x" << std::hex << static_cast<unsigned int>(static_cast<std::uint64_t>(streamType));
        }

        // 0x40 = 64 已在单字节档之外，写成两字节；0x21 是保留族首个取值，写成单字节
        std::string reservedBytes;
        appendHttp3StreamTypeHeader(reservedBytes, static_cast<Http3StreamType>(0x21));
        EXPECT_EQ(toUnsignedBytes(reservedBytes), makeBytesFromHex("21")) << "§6.2.3 的保留流类型 0x21";
        std::string wideBytes;
        appendHttp3StreamTypeHeader(wideBytes, static_cast<Http3StreamType>(0x40));
        EXPECT_EQ(toUnsignedBytes(wideBytes), makeBytesFromHex("4040")) << "超单字节档的流类型走两字节档";

        // 推送流的头部在类型之后紧跟 Push ID（§6.2.2 图 2），由调用方自己续写
        std::string pushHeader;
        appendHttp3StreamTypeHeader(pushHeader, Http3StreamType::Push);
        appendQuicVariableLengthInteger(pushHeader, 1024);
        EXPECT_EQ(toUnsignedBytes(pushHeader), makeBytesFromHex("014400")) << "推送流头部 = 类型 0x01 + Push ID 1024";

        // 未知/保留流类型的分类：0x21、0x40、0x5f 落在 0x1f*N+0x21 族里，0x06、0x33、0x20 不在
        EXPECT_TRUE(isHttp3ReservedExtensionIdentifier(0x21)) << "§6.2.3/§7.2.4.1/§7.2.8 共用的保留族";
        EXPECT_TRUE(isHttp3ReservedExtensionIdentifier(0x40)) << "N=1 时 0x1f+0x21";
        EXPECT_TRUE(isHttp3ReservedExtensionIdentifier(0x5f)) << "N=2";
        EXPECT_FALSE(isHttp3ReservedExtensionIdentifier(0x20)) << "紧邻保留族之下，不属于它";
        EXPECT_FALSE(isHttp3ReservedExtensionIdentifier(0x33)) << "普通扩展标识";
        EXPECT_FALSE(isHttp3ReservedExtensionIdentifier(0x00)) << "不得因回绕把 0 判成保留";
    }

    /**
     * @brief 字段值超过 2^62-1 时编码侧当场抛错，且不往缓冲里写半截帧
     */
    TEST(Http3FrameEncoding, RejectsFieldValuesAboveTheSixtyTwoBitCeiling)
    {
        std::string bytes;
        // 2^62 已越出变长整数 8 字节档能表示的满值（RFC 9000 §16 表 4），没有合法编码
        EXPECT_THROW(appendHttp3Frame(bytes, Http3GoAwayFrame{kQuicMaximumIntegerValue + 1}),
                     Base::InvalidArgumentException)
                << "GOAWAY 的 Stream ID/Push ID 越界时必须抛用法错误";
        EXPECT_TRUE(bytes.empty()) << "载荷还没拼完就抛错，不该先写帧头";

        EXPECT_THROW(appendHttp3Frame(bytes, Http3UnknownFrame{kQuicMaximumIntegerValue + 1, {}}),
                     Base::InvalidArgumentException)
                << "未知帧的类型值同样受 62 位上限约束";
    }

    /**
     * @brief 逐型核解码出来的字段值
     * @details 重点在三处容易写错的地方：PUSH_PROMISE 在 Push ID 之后整段都是头字段段（可以为空）、
     *          SETTINGS 的未知项一条不丢、GOAWAY 只有一个整数（终稿 §7.2.6 图 9 没有第二个字段）。
     */
    TEST(Http3FrameDecoding, DecodesEachFrameLayoutFromWireBytes)
    {
        Http3FrameReader reader(64);

        const auto data = decodeSoleFrame("000568656c6c6f", reader);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        ASSERT_TRUE(data->has_value());
        EXPECT_EQ(**data, frameOf(Http3DataFrame{std::span<const std::uint8_t>(kHelloBytes)})) << "DATA 的载荷";

        const auto headers = decodeSoleFrame("0102aabb", reader);
        ASSERT_TRUE(headers.has_value()) << headers.error().message;
        ASSERT_TRUE(headers->has_value());
        EXPECT_EQ(**headers, frameOf(Http3HeadersFrame{std::span<const std::uint8_t>(kEncodedFieldSection)}))
                << "HEADERS 的头字段段";

        const auto settings = decodeSoleFrame("040c015000068000400007033107", reader);
        ASSERT_TRUE(settings.has_value()) << settings.error().message;
        ASSERT_TRUE(settings->has_value());
        const auto *settingsFrame = std::get_if<Http3SettingsFrame>(&**settings);
        ASSERT_NE(settingsFrame, nullptr) << "SETTINGS 应解成 Http3SettingsFrame";
        EXPECT_EQ(settingsFrame->settings.size(), 3u) << "认识的设置项条数";
        EXPECT_EQ(settingsFrame->settings[0].first, Http3SettingId::QpackMaxTableCapacity) << "0x01";
        EXPECT_EQ(settingsFrame->settings[0].second, 4096u) << "4096 的两字节档取值";
        EXPECT_EQ(settingsFrame->settings[1].first, Http3SettingId::MaxFieldSectionSize) << "0x06";
        EXPECT_EQ(settingsFrame->settings[1].second, 16384u) << "16384 的四字节档取值";
        EXPECT_EQ(settingsFrame->settings[2].first, Http3SettingId::QpackBlockedStreams) << "0x07 是终稿的 QPACK_BLOCKED_STREAMS";
        EXPECT_EQ(settingsFrame->unknownSettings.size(), 1u) << "未知设置项要能原样交出，不得丢弃";
        EXPECT_EQ(settingsFrame->unknownSettings[0].identifier, 0x31) << "未知标识";
        EXPECT_EQ(settingsFrame->unknownSettings[0].value, 7u) << "未知项的值";

        const auto cancelPush = decodeSoleFrame("03024400", reader);
        ASSERT_TRUE(cancelPush.has_value()) << cancelPush.error().message;
        ASSERT_TRUE(cancelPush->has_value());
        EXPECT_EQ(**cancelPush, frameOf(Http3CancelPushFrame{1024})) << "CANCEL_PUSH 的 Push ID";

        const auto pushPromise = decodeSoleFrame("050109", reader);
        ASSERT_TRUE(pushPromise.has_value()) << pushPromise.error().message;
        ASSERT_TRUE(pushPromise->has_value());
        const Http3Frame expectedPushPromise{Http3PushPromiseFrame{9, {}}};
        EXPECT_EQ(**pushPromise, expectedPushPromise) << "PUSH_PROMISE 允许头字段段为空";

        const auto goAway = decodeSoleFrame("0708ffffffffffffffff", reader);
        ASSERT_TRUE(goAway.has_value()) << goAway.error().message;
        ASSERT_TRUE(goAway->has_value());
        // §5.2：客户端方向的 GOAWAY 可以填 2^62-1，表示服务端可继续履行已承诺的推送
        EXPECT_EQ(**goAway, frameOf(Http3GoAwayFrame{kQuicMaximumIntegerValue})) << "GOAWAY 的 Stream ID/Push ID";

        const auto maxPushId = decodeSoleFrame("0d0245dc", reader);
        ASSERT_TRUE(maxPushId.has_value()) << maxPushId.error().message;
        ASSERT_TRUE(maxPushId->has_value());
        EXPECT_EQ(**maxPushId, frameOf(Http3MaxPushIdFrame{1500})) << "MAX_PUSH_ID 的 Push ID";
    }

    /**
     * @brief 变长整数允许非最短编码
     * @details 与 QUIC 帧层的刻意差异：RFC 9000 §16 末段那句「唯一要求最短编码」指的是 §12.4 的 QUIC
     *          Frame Type 字段，HTTP/3 的帧类型与长度不在其列，§7.1 只要求嵌套长度自洽（§10.8）。
     *          因此 Type 写成 4001（值 1）、Length 写成 80000003（值 3）都是合法帧，解出的字段值不变，
     *          而重新编码回到最短档位——编码侧写出的仍是规范形态，读侧不因为对端用了宽档位就打死连接。
     */
    TEST(Http3FrameDecoding, AcceptsNonMinimalIntegerEncodings)
    {
        Http3FrameReader reader(64);

        const auto headers = decodeSoleFrame("400180000003010203", reader);
        ASSERT_TRUE(headers.has_value()) << headers.error().message << "：非最短的帧类型/长度都不该报错";
        ASSERT_TRUE(headers->has_value());
        const auto *headersFrame = std::get_if<Http3HeadersFrame>(&**headers);
        ASSERT_NE(headersFrame, nullptr) << "0x4001 解出来就是类型 1 的 HEADERS";
        EXPECT_EQ(bytesOf(headersFrame->encodedFieldSection), makeBytesFromHex("010203"))
                << "头字段段按声明长度 3 切出";
        EXPECT_EQ(encodeOne(**headers), makeBytesFromHex("0103010203")) << "重新编码回到最短档位";
    }

    /**
     * @brief 未知帧类型与两类保留帧类型都不算解码失败
     * @details §9 要求「未知或不受支持的值必须忽略」，于是 0xa7 这类扩展类型、0x21/0x40 这类
     *          §7.2.8 的 0x1f*N+0x21 保留族都原样交出载荷；0x02/0x06/0x08/0x09 是 §7.2.8 从 HTTP/2
     *          继承下来的保留类型（收到按 H3_FRAME_UNEXPECTED），本层同样只交出类型值，处置留给连接层。
     */
    TEST(Http3FrameDecoding, HandsOverUnknownAndReservedFrameTypesWithoutFailing)
    {
        struct Case
        {
            const char *hexadecimalText;
            std::uint64_t expectedTypeValue;
            const char *expectedPayloadHexadecimalText;
        };

        const std::vector<Case> cases{
                {"2102ff00", 0x21, "ff00"},     // §7.2.8 保留族 N=0
                {"404000", 0x40, ""},           // §7.2.8 保留族 N=1，类型本身走两字节档
                {"0200", 0x02, ""},             // §7.2.8 从 HTTP/2 继承的保留类型（原 PRIORITY）
                {"40a703010203", 0xa7, "010203"} // 扩展自定义类型 167 超单字节档，故类型是 40a7
        };

        for (const Case &testCase: cases)
        {
            Http3FrameReader reader(64);
            const auto decoded = decodeSoleFrame(testCase.hexadecimalText, reader);
            ASSERT_TRUE(decoded.has_value()) << decoded.error().message << " 原文 " << testCase.hexadecimalText;
            ASSERT_TRUE(decoded->has_value()) << testCase.hexadecimalText;
            const auto *unknownFrame = std::get_if<Http3UnknownFrame>(&**decoded);
            ASSERT_NE(unknownFrame, nullptr) << "未知类型要落进 Http3UnknownFrame：" << testCase.hexadecimalText;
            EXPECT_EQ(unknownFrame->frameType, testCase.expectedTypeValue) << "类型值原样交出";
            EXPECT_EQ(bytesOf(unknownFrame->payload), makeBytesFromHex(testCase.expectedPayloadHexadecimalText))
                    << "载荷原文一条字节都不丢：" << testCase.hexadecimalText;
            EXPECT_EQ(encodeOne(**decoded), makeBytesFromHex(testCase.hexadecimalText)) << "忽略面也要能回编";
        }

        // 保留族与普通未知类型的区分点是式子本身，连接层据此选「忽略」还是 H3_FRAME_UNEXPECTED
        EXPECT_FALSE(isHttp3ReservedExtensionIdentifier(0x02)) << "0x02 属 §7.2.8 的另一类保留值";
        EXPECT_TRUE(isHttp3ReservedExtensionIdentifier(0x21)) << "0x21 属可忽略的保留族";
        EXPECT_EQ(http3FrameTypeName(0x21), "未知帧类型") << "本层不认识保留族";
        EXPECT_EQ(http3FrameTypeName(0x01), "HEADERS") << "认识的类型给规范名";
    }

    /**
     * @brief 载荷与声明长度不合的帧一律 Malformed
     * @details 五个方向不同的样例：
     *          0302 1fff —— CANCEL_PUSH 只该有一个整数，多出 1 字节尾巴（§7.1）；
     *          0301 40 —— 声明 1 字节却起了个两字节档的整数，字段越出末尾；
     *          0700 —— GOAWAY 的必填整数完全缺席；
     *          040101 —— SETTINGS 只剩标识、值不见；
     *          04020140 —— 值起了两字节档，声明长度里只剩 1 字节。
     */
    TEST(Http3FrameDecoding, RejectsPayloadThatDisagreesWithDeclaredLength)
    {
        struct Case
        {
            const char *hexadecimalText;
            const char *expectedTextInMessage;
        };

        const std::vector<Case> cases{
                {"03021fff", "没被任何字段占用"},
                {"030140", "越出载荷末尾"},
                {"0700", "越出载荷末尾"},
                {"040101", "越出载荷末尾"},
                {"04020140", "越出载荷末尾"},
        };

        for (const Case &testCase: cases)
        {
            Http3FrameReader reader(64);
            const auto decoded = decodeSoleFrame(testCase.hexadecimalText, reader);
            ASSERT_FALSE(decoded.has_value()) << "坏帧必须报错：" << testCase.hexadecimalText;
            EXPECT_EQ(decoded.error().kind, Http3FrameErrorKind::Malformed) << testCase.hexadecimalText;
            EXPECT_TRUE(containsText(decoded.error().message, testCase.expectedTextInMessage))
                    << testCase.hexadecimalText << " 的文案该含「" << testCase.expectedTextInMessage << "」，实际："
                    << decoded.error().message;
        }
    }

    /**
     * @brief 同一设置项出现两次时本层两条都留
     * @details §7.2.4：「同一个标识在 SETTINGS 帧里 MUST NOT 出现多次；接收方**可以**把它判成
     *          H3_SETTINGS_ERROR」。规范给的是「可以」而不是「必须」，所以判不判属于连接层策略，
     *          本层只保证重复项不被静默合并掉（用 map 就会把这条丢给运气）。
     */
    TEST(Http3FrameDecoding, KeepsDuplicateAndUnknownSettings)
    {
        Http3FrameReader reader(64);
        // 04 04 | 06 06 | 06 09 —— 两次 0x06（MAX_FIELD_SECTION_SIZE），值分别是 6 与 9
        const auto decoded = decodeSoleFrame("040406060609", reader);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        ASSERT_TRUE(decoded->has_value());
        const auto *settingsFrame = std::get_if<Http3SettingsFrame>(&**decoded);
        ASSERT_NE(settingsFrame, nullptr) << "SETTINGS 的变体选择";
        EXPECT_EQ(settingsFrame->settings.size(), 2u) << "重复项不得被静默合并";
        EXPECT_EQ(settingsFrame->settings[0].first, Http3SettingId::MaxFieldSectionSize);
        EXPECT_EQ(settingsFrame->settings[0].second, 6u) << "先到的一条";
        EXPECT_EQ(settingsFrame->settings[1].second, 9u) << "后到的一条也要留着，由连接层决定用哪个或判错";
        EXPECT_EQ(encodeOne(**decoded), makeBytesFromHex("040406060609")) << "回编要还原重复项";

        // §7.2.4.1 从 HTTP/2 继承下来的保留标识（0x02～0x05）落在未知项里，本层不判错
        Http3FrameReader reservedReader(64);
        const auto reserved = decodeSoleFrame("04020203", reservedReader);
        ASSERT_TRUE(reserved.has_value()) << reserved.error().message;
        ASSERT_TRUE(reserved->has_value());
        const auto *reservedFrame = std::get_if<Http3SettingsFrame>(&**reserved);
        ASSERT_NE(reservedFrame, nullptr);
        EXPECT_TRUE(reservedFrame->settings.empty()) << "0x02 不是本层认识的标识";
        ASSERT_EQ(reservedFrame->unknownSettings.size(), 1u);
        EXPECT_EQ(reservedFrame->unknownSettings[0].identifier, 0x02) << "保留标识交连接层按 §7.2.4.1 判 H3_SETTINGS_ERROR";
        EXPECT_EQ(reservedFrame->unknownSettings[0].value, 3u);
    }

    /**
     * @brief 设置项标识取值到枚举的映射
     * @details 认表与解码侧共用同一份：0x07 是 RFC 9204 §5 终稿的 QPACK_BLOCKED_STREAMS（草案时期占 0x02，
     *          而 0x02 在 HTTP/3 设置空间里是 §7.2.4.1 从 HTTP/2 继承的保留标识，收到该判错）。
     */
    TEST(Http3FrameDecoding, MapsKnownSettingIdentifiers)
    {
        EXPECT_EQ(http3SettingIdFromValue(0x01), Http3SettingId::QpackMaxTableCapacity) << "RFC 9204 §5";
        EXPECT_EQ(http3SettingIdFromValue(0x06), Http3SettingId::MaxFieldSectionSize) << "§7.2.4.1";
        EXPECT_EQ(http3SettingIdFromValue(0x07), Http3SettingId::QpackBlockedStreams) << "RFC 9204 §5";
        EXPECT_EQ(http3SettingIdFromValue(0x08), Http3SettingId::EnableConnectProtocol) << "RFC 9220 §3.2.1";
        EXPECT_FALSE(http3SettingIdFromValue(0x02).has_value()) << "HTTP/2 继承下来的保留标识";
        EXPECT_FALSE(http3SettingIdFromValue(0x33).has_value()) << "扩展定义过的设置项在本层是未知项";
        EXPECT_FALSE(http3SettingIdFromValue(0x21).has_value()) << "§7.2.4.1 的保留族";
    }

    /**
     * @brief 字节没到齐时 nextFrame() 交空 optional 而不是报错
     * @details 一帧被切成「半个类型」「半个长度」「半截载荷」三种缺法，每次都断言不报错、
     *          不消费，补齐后才出帧（§7.1 的两处长度域都是自定界的变长整数）。
     */
    TEST(Http3FrameReader, WaitsForMoreBytesInsteadOfReportingError)
    {
        Http3FrameReader reader(64);

        auto feedOne = [&reader](const std::string_view hexadecimalText)
        {
            const auto bytes = makeBytesFromHex(hexadecimalText);
            const auto fed = reader.feed(std::span<const std::uint8_t>(bytes));
            ASSERT_TRUE(fed.has_value()) << "喂入 " << hexadecimalText << " 失败：" << fed.error().message;
        };
        auto expectNoFrame = [&reader](const char *reason)
        {
            const auto pending = reader.nextFrame();
            ASSERT_TRUE(pending.has_value()) << reason << " 时不该报错，实际：" << pending.error().message;
            EXPECT_FALSE(pending->has_value()) << reason << " 时应交空 optional";
        };

        EXPECT_EQ(reader.pendingByteCount(), 0u) << "初态缓冲是空的";
        expectNoFrame("一个字节都没有");

        // 未知帧 0x40（64 超单字节档，类型占两字节）+ 长度 3 + 载荷 aabbcc，按五种缺法依次补齐
        feedOne("40");
        expectNoFrame("帧头第一个整数只起了个头");
        feedOne("40");
        expectNoFrame("类型收齐、长度域缺席");
        feedOne("03");
        expectNoFrame("帧头已齐、载荷一字未到");
        feedOne("aabb");
        expectNoFrame("载荷只到三分之二");
        feedOne("cc");

        const auto frame = reader.nextFrame();
        ASSERT_TRUE(frame.has_value()) << frame.error().message;
        ASSERT_TRUE(frame->has_value()) << "补齐后即可出帧";
        const auto *unknownFrame = std::get_if<Http3UnknownFrame>(&**frame);
        ASSERT_NE(unknownFrame, nullptr) << "类型 0x40 应落进未知帧";
        EXPECT_EQ(unknownFrame->frameType, 0x40) << "类型值按两字节档解出 64";
        EXPECT_EQ(bytesOf(unknownFrame->payload), makeBytesFromHex("aabbcc")) << "载荷按声明长度切出";
        EXPECT_EQ(reader.pendingByteCount(), 0u) << "整帧已交出，残余归零；这些字节留到下次调用才挪走";
    }

    /**
     * @brief 流干净收尾时残余字节数是「最后一帧被截断」的唯一观测点（§7.1）
     */
    TEST(Http3FrameReader, ReportsTruncatedTailThroughPendingByteCount)
    {
        Http3FrameReader reader(64);
        // DATA 声明 5 字节载荷，实际只来了 2 字节：不能报错，但也永远解不出帧
        const auto fed = reader.feed(std::span<const std::uint8_t>(makeBytesFromHex("00056865")));
        ASSERT_TRUE(fed.has_value()) << fed.error().message;
        const auto pending = reader.nextFrame();
        ASSERT_TRUE(pending.has_value()) << pending.error().message;
        EXPECT_FALSE(pending->has_value()) << "载荷未齐，连接层此刻要按 §7.1 判截断还是继续等";
        EXPECT_EQ(reader.pendingByteCount(), 4u) << "帧头 2 字节 + 已到载荷 2 字节都算残余";
    }

    /**
     * @brief 单帧上限的两道闸门与错误粘滞
     * @details 声明长度超上限（帧头一收齐就判，不干等）、喂入块长超上限、上限给 0 是用法错误；
     *          失败后解码器只重复同一个错，reset() 才恢复（§10.5 的过量负载防护由连接层落错误码）。
     */
    TEST(Http3FrameReader, EnforcesTheFrameSizeLimitAndLatchesFailure)
    {
        EXPECT_THROW(Http3FrameReader(0), Base::InvalidArgumentException)
                << "上限为 0 连最小的空帧都容不下";

        Http3FrameReader reader(16);
        // DATA 声明 100 字节载荷：100 超单字节档，长度域要走两字节档 0x4064（RFC 9000 §16），
        // 加 3 字节帧头 = 103 > 16
        const auto fed = reader.feed(std::span<const std::uint8_t>(makeBytesFromHex("004064")));
        ASSERT_TRUE(fed.has_value()) << fed.error().message;
        const auto exceeded = reader.nextFrame();
        ASSERT_FALSE(exceeded.has_value()) << "声明长度超上限必须当场报错";
        EXPECT_EQ(exceeded.error().kind, Http3FrameErrorKind::LimitExceeded);
        EXPECT_TRUE(containsText(exceeded.error().message, "超过单帧上限 16 字节")) << exceeded.error().message;

        // 粘滞：同一个错重复报出，且喂新字节也只报旧错
        const auto again = reader.nextFrame();
        ASSERT_FALSE(again.has_value());
        EXPECT_EQ(again.error().message, exceeded.error().message) << "旧错要原样重复，不能改口";
        const auto fedAgain = reader.feed(std::span<const std::uint8_t>(makeBytesFromHex("0000")));
        EXPECT_FALSE(fedAgain.has_value()) << "作废状态下不接受新字节";

        reader.reset();
        EXPECT_EQ(reader.pendingByteCount(), 0u) << "reset() 之后残余清零";
        const auto revived = reader.feed(std::span<const std::uint8_t>(makeBytesFromHex("0000")));
        ASSERT_TRUE(revived.has_value()) << "reset() 之后要能接着用";
        const auto emptyData = reader.nextFrame();
        ASSERT_TRUE(emptyData.has_value()) << emptyData.error().message;
        ASSERT_TRUE(emptyData->has_value()) << "空载荷的 DATA 是合法帧（§7.2.1 的 Data 长度任意）";
        EXPECT_EQ(**emptyData, frameOf(Http3DataFrame{})) << "0000 = DATA，声明长度 0";

        // 单次喂入 18 字节 > 上限 16：块长是调用方定的，按超限报出
        Http3FrameReader blockedReader(16);
        const auto tooBigBlock = blockedReader.feed(
                std::span<const std::uint8_t>(makeBytesFromHex("00000000000000000000000000000000000")));
        ASSERT_FALSE(tooBigBlock.has_value());
        EXPECT_EQ(tooBigBlock.error().kind, Http3FrameErrorKind::LimitExceeded);
        EXPECT_TRUE(containsText(tooBigBlock.error().message, "超过单帧上限")) << tooBigBlock.error().message;
    }

    /**
     * @brief 任意字节边界都要能切开续上
     * @details 一整段 5 帧的字节流（DATA/SETTINGS/GOAWAY/未知类型 0x21/空 DATA，各帧字节在编码用例里
     *          已按 RFC 推导核过）按步长 1、2、3、5、7、11、13 切片喂进来，解出的类型序列与逐字节
     *          重编码结果必须与步长无关。上限刻意只给 16 字节（比最长的 SETTINGS 帧略大），
     *          以便同时压住「缓冲不许按整段流长增长」。
     */
    class Http3FrameChunkedFeeding : public ::testing::TestWithParam<std::size_t>
    {
    protected:
        /// 五帧拼接的字节流，总长 31 字节
        static constexpr const char *s_streamHexadecimalText =
                "000568656c6c6f" "040c015000068000400007033107" "070104" "2102ff00" "0000";
    };

    TEST_P(Http3FrameChunkedFeeding, DecodesTheSameFramesRegardlessOfChunkSize)
    {
        const std::size_t stride = GetParam();
        const auto wireBytes = makeBytesFromHex(s_streamHexadecimalText);
        Http3FrameReader reader(16);

        std::vector<std::uint64_t> decodedTypeValues;
        std::string reEncodedBytes;
        std::size_t offset = 0;
        while (offset < wireBytes.size())
        {
            const std::size_t chunkLength = std::min(stride, wireBytes.size() - offset);
            const auto fed = reader.feed(std::span<const std::uint8_t>(wireBytes.data() + offset, chunkLength));
            ASSERT_TRUE(fed.has_value()) << "步长 " << stride << " 喂到第 " << offset << " 字节失败：" << fed.error().message;
            offset += chunkLength;

            // 真实读取循环就是「喂一段、把能解的帧取干净」，此处照做
            while (true)
            {
                const auto frame = reader.nextFrame();
                ASSERT_TRUE(frame.has_value()) << "步长 " << stride << " 在第 " << offset << " 字节处报错："
                                               << frame.error().message;
                if (!frame->has_value())
                {
                    break;
                }
                decodedTypeValues.push_back(http3FrameTypeValue(**frame));
                // 视图在下一次 feed()/nextFrame() 前有效，当场重编码正好把这条契约钉住
                appendHttp3Frame(reEncodedBytes, **frame);
            }
        }

        const std::vector<std::uint64_t> expectedTypeValues{0x00, 0x04, 0x07, 0x21, 0x00};
        EXPECT_EQ(decodedTypeValues, expectedTypeValues) << "步长 " << stride << " 解出的帧序列";
        EXPECT_EQ(toUnsignedBytes(reEncodedBytes), wireBytes) << "步长 " << stride << " 的逐字节回编结果";
        EXPECT_EQ(reader.pendingByteCount(), 0u) << "步长 " << stride << " 解完就不该有残余";
    }

    INSTANTIATE_TEST_SUITE_P(ChunkStrides, Http3FrameChunkedFeeding, ::testing::Values(1, 2, 3, 5, 7, 11, 13),
                             [](const ::testing::TestParamInfo<std::size_t> &information)
                             { return "stride" + std::to_string(information.param); });
} // namespace AsynGyanis::Net
