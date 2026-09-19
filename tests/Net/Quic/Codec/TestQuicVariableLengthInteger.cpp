// TestQuicVariableLengthInteger.cpp —— QUIC 变长整数（RFC 9000 §16）的编解码用例
//
// 覆盖四块：
//   1) 正确性：RFC 9000 附录 A.1 给出的四条官方向量正反双向都核（0xc2197c5eff14e88c、0x9d7f3e7d、
//      0x7bbd、0x25），这是本层唯一能拿来的外部真值，逐字节比对而不是只核「能编出来」；
//   2) 档位边界：表 4 的四档可用位数 6/14/30/62 各自的满值与满值加一，以及 2^62-1 这个协议上限；
//   3) 规范里容易被误当成缺陷的一条：除帧类型外**允许非最短编码**（§16 末段），所以 0x4025 必须
//      解成 37 而不是被判非法——把它钉住，防止有人按 HTTP/2 的习惯加「非最短即拒绝」；
//   4) 拒绝面：字节不够（前缀声明的宽度大于剩余字节数）、空输入、值超 62 位、宽度不是 1/2/4/8、
//      值超出所选档位。
// 用例全是纯计算，不起网络、不依赖外部服务，因此不存在等待时序的问题。

#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
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
         * @brief 由字节序列拼出待解码的字节容器（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeUnsignedBytes;

        /**
         * @brief 编码结果转成可比对的字节容器（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::toUnsignedBytes;
    } // namespace

    /**
     * @brief 附录 A.1 的四条官方向量必须逐个解对，且交回的字节数正好是本数占用的宽度
     */
    TEST(QuicVariableLengthInteger, DecodesRfcAppendixA1Vectors)
    {
        const auto eightByteForm = makeUnsignedBytes({0xc2, 0x19, 0x7c, 0x5e, 0xff, 0x14, 0xe8, 0x8c});
        const auto fourByteForm  = makeUnsignedBytes({0x9d, 0x7f, 0x3e, 0x7d});
        const auto twoByteForm   = makeUnsignedBytes({0x7b, 0xbd});
        const auto oneByteForm   = makeUnsignedBytes({0x25});

        const auto decodedEight = decodeQuicVariableLengthInteger(eightByteForm);
        ASSERT_TRUE(decodedEight.has_value());
        EXPECT_EQ(decodedEight->value, 151288809941952652ULL);
        EXPECT_EQ(decodedEight->byteCount, 8U);

        const auto decodedFour = decodeQuicVariableLengthInteger(fourByteForm);
        ASSERT_TRUE(decodedFour.has_value());
        EXPECT_EQ(decodedFour->value, 494878333ULL);
        EXPECT_EQ(decodedFour->byteCount, 4U);

        const auto decodedTwo = decodeQuicVariableLengthInteger(twoByteForm);
        ASSERT_TRUE(decodedTwo.has_value());
        EXPECT_EQ(decodedTwo->value, 15293ULL);
        EXPECT_EQ(decodedTwo->byteCount, 2U);

        const auto decodedOne = decodeQuicVariableLengthInteger(oneByteForm);
        ASSERT_TRUE(decodedOne.has_value());
        EXPECT_EQ(decodedOne->value, 37ULL);
        EXPECT_EQ(decodedOne->byteCount, 1U);
    }

    /**
     * @brief 非最短编码合法（§16 末段：只有帧类型例外），0x4025 与 0x80000025 都要解成 37
     * @details 这一条刻意与 HTTP/2 HPACK 的规矩相反：那边非最短即报错，这里若照搬会把合法的
     *          长度域回填写法全部判成非法报文。
     */
    TEST(QuicVariableLengthInteger, AcceptsNonMinimalEncodingsBecauseRfcAllowsThem)
    {
        const auto twoBytePadded = makeUnsignedBytes({0x40, 0x25});
        const auto fourBytePadded = makeUnsignedBytes({0x80, 0x00, 0x00, 0x25});

        const auto decodedTwo = decodeQuicVariableLengthInteger(twoBytePadded);
        ASSERT_TRUE(decodedTwo.has_value()) << "非最短的 2 字节档必须照样收下";
        EXPECT_EQ(decodedTwo->value, 37ULL);
        EXPECT_EQ(decodedTwo->byteCount, 2U);

        const auto decodedFour = decodeQuicVariableLengthInteger(fourBytePadded);
        ASSERT_TRUE(decodedFour.has_value());
        EXPECT_EQ(decodedFour->value, 37ULL);
        EXPECT_EQ(decodedFour->byteCount, 4U);
    }

    /**
     * @brief 官方向量的反向：这些值按最少字节数编码，产出的字节序列必须与向量逐字节相同
     */
    TEST(QuicVariableLengthInteger, EncodesValuesToRfcSampleByteSequences)
    {
        struct Sample
        {
            std::uint64_t value;
            std::vector<std::uint8_t> bytes;
        };

        const std::vector<Sample> samples{
                {0ULL, makeUnsignedBytes({0x00})},
                {37ULL, makeUnsignedBytes({0x25})},
                {15293ULL, makeUnsignedBytes({0x7b, 0xbd})},
                {494878333ULL, makeUnsignedBytes({0x9d, 0x7f, 0x3e, 0x7d})},
                {151288809941952652ULL, makeUnsignedBytes({0xc2, 0x19, 0x7c, 0x5e, 0xff, 0x14, 0xe8, 0x8c})},
                {kQuicMaximumIntegerValue, makeUnsignedBytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
        };

        for (const Sample &sample: samples)
        {
            std::string bytes;
            appendQuicVariableLengthInteger(bytes, sample.value);
            const auto encodedBytes = toUnsignedBytes(bytes);
            EXPECT_EQ(encodedBytes, sample.bytes) << "值 " << sample.value << " 的编码与期望不符";

            // 编出来还要能原样解回同一个值与同一宽度，否则「回填长度域」这类用法会在下一层暴露
            const auto decoded = decodeQuicVariableLengthInteger(std::span<const std::uint8_t>(encodedBytes));
            ASSERT_TRUE(decoded.has_value()) << "编码器产出的字节自身解不回，值 " << sample.value;
            EXPECT_EQ(decoded->value, sample.value);
            EXPECT_EQ(decoded->byteCount, sample.bytes.size());
        }
    }

    /**
     * @brief 表 4 的四档边界：每档满值用本档、满值加一进下一档，2^62-1 之上没有合法档位
     */
    TEST(QuicVariableLengthInteger, WidthThresholdsMatchTableFour)
    {
        EXPECT_EQ(quicVariableLengthIntegerByteCount(0ULL), 1U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumOneByteIntegerValue), 1U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumOneByteIntegerValue + 1ULL), 2U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumTwoByteIntegerValue), 2U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumTwoByteIntegerValue + 1ULL), 4U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumFourByteIntegerValue), 4U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumFourByteIntegerValue + 1ULL), 8U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumIntegerValue), 8U);
        EXPECT_EQ(quicVariableLengthIntegerByteCount(kQuicMaximumIntegerValue + 1ULL), 0U)
                << "超出 62 位上限必须返回 0 而不是回绕成 1 档";
    }

    /**
     * @brief 长度域回填：显式占宽写出的字节，与 RFC 里那条非最短向量完全一致
     */
    TEST(QuicVariableLengthInteger, ExplicitWidthMatchesNonMinimalVector)
    {
        std::string twoByteWidth;
        appendQuicVariableLengthInteger(twoByteWidth, 37ULL, 2);
        EXPECT_EQ(toUnsignedBytes(twoByteWidth), makeUnsignedBytes({0x40, 0x25}));

        std::string fourByteWidth;
        appendQuicVariableLengthInteger(fourByteWidth, 37ULL, 4);
        EXPECT_EQ(toUnsignedBytes(fourByteWidth), makeUnsignedBytes({0x80, 0x00, 0x00, 0x25}));

        std::string eightByteWidth;
        appendQuicVariableLengthInteger(eightByteWidth, 37ULL, 8);
        EXPECT_EQ(toUnsignedBytes(eightByteWidth),
                  makeUnsignedBytes({0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25}));
    }

    /**
     * @brief 一个变长整数只吃自己那几字节：交回的字节数就是调用该前移的读位置
     */
    TEST(QuicVariableLengthInteger, ConsumesOnlyItsOwnBytes)
    {
        const auto buffer = makeUnsignedBytes({0x7b, 0xbd, 0x25});

        const auto first = decodeQuicVariableLengthInteger(buffer);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->value, 15293ULL);
        EXPECT_EQ(first->byteCount, 2U);

        const std::span<const std::uint8_t> remaining(buffer.begin() + static_cast<std::ptrdiff_t>(first->byteCount), buffer.end());
        const auto second = decodeQuicVariableLengthInteger(remaining);
        ASSERT_TRUE(second.has_value()) << "按 byteCount 前移后，下一个字段必须能接着解";
        EXPECT_EQ(second->value, 37ULL);
    }

    /**
     * @brief 前缀声明的宽度大于剩余字节数时判截断，且文案要带出「声明了几字节、还剩几字节」
     */
    TEST(QuicVariableLengthInteger, ReportsTruncationWhenDeclaredWidthExceedsData)
    {
        struct Case
        {
            std::vector<std::uint8_t> bytes; ///< 只给到本数开头若干字节的缓冲
            std::size_t declaredWidth;       ///< 首字节前缀声明的宽度
        };

        const std::vector<Case> cases{
                {makeUnsignedBytes({0xc2, 0x19}), 8},
                {makeUnsignedBytes({0x9d, 0x7f}), 4},
                {makeUnsignedBytes({0x7b}), 2},
        };

        for (const Case &testCase: cases)
        {
            const auto decoded = decodeQuicVariableLengthInteger(testCase.bytes);
            ASSERT_FALSE(decoded.has_value()) << "宽度 " << testCase.declaredWidth << " 只剩 " << testCase.bytes.size()
                                              << " 字节，必须判截断";
            EXPECT_EQ(decoded.error().kind, QuicDecodeErrorKind::Truncated);
            EXPECT_TRUE(containsText(decoded.error().message, "丢弃")) << "文案要写清后果：" << decoded.error().message;
        }
    }

    /**
     * @brief 空输入同样按截断处理，而不是回绕成 0 值（静默给 0 会让调用方以为读到一个零长字段）
     */
    TEST(QuicVariableLengthInteger, ReportsTruncationOnEmptyInput)
    {
        const auto decoded = decodeQuicVariableLengthInteger(std::span<const std::uint8_t>{});
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error().kind, QuicDecodeErrorKind::Truncated);
    }

    /**
     * @brief 编码侧的三样用法错误当场抛：值超 62 位上限、宽度不是四档之一、值超出所选档位
     */
    TEST(QuicVariableLengthInteger, RejectsUnencodableArguments)
    {
        std::string bytes;
        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, kQuicMaximumIntegerValue + 1ULL), Base::InvalidArgumentException);
        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, kQuicMaximumIntegerValue + 1ULL, 8), Base::InvalidArgumentException);

        // 3/5/6/7 这些宽度在首字节的 2 位前缀里表达不了，放行就会产出对端解不出的字节
        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, 1ULL, 3), Base::InvalidArgumentException);
        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, 1ULL, 0), Base::InvalidArgumentException);

        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, kQuicMaximumOneByteIntegerValue + 1ULL, 1),
                     Base::InvalidArgumentException);
        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, kQuicMaximumTwoByteIntegerValue + 1ULL, 2),
                     Base::InvalidArgumentException);
    }

    /**
     * @brief 非法参数不能把脏字节留在缓冲里：抛错之后缓冲内容要保持调用前的样子
     */
    TEST(QuicVariableLengthInteger, LeavesBufferUntouchedWhenRejectingArguments)
    {
        std::string bytes = "keep";
        EXPECT_THROW(appendQuicVariableLengthInteger(bytes, kQuicMaximumOneByteIntegerValue + 1ULL, 1),
                     Base::InvalidArgumentException);
        EXPECT_EQ(bytes, "keep") << "先算好再写：半途写入会污染调用方正在组装的报文";
    }
} // namespace AsynGyanis::Net
