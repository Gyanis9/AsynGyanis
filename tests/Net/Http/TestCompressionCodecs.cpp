// zstd 与 brotli 压缩构件的单元测试：压得进去也解得回来、空输入、重复内容变小、越界档位被夹取 与 TestGzip 同一判据：断言一律用各自库解回来逐字节比较——只比大小无法发现
// 「压出来的字节解不开」，而那正是响应压缩最怕的事。
#include "Net/Http/Compression.h"

#include <gtest/gtest.h>

#include <brotli/decode.h>
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 解开 zstd 帧（用例侧的自检工具）
         * @param input zstd 字节
         * @param expectedSize 原始长度（调用方已知）
         * @return std::optional<std::string> 原始内容；解不开或长度不符时为空
         */
        std::optional<std::string> unzstd(const std::string_view input, const std::size_t expectedSize)
        {
            std::string output(expectedSize, '\0');
            const std::size_t writtenLength = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
            if (ZSTD_isError(writtenLength) != 0 || writtenLength != expectedSize)
            {
                return std::nullopt;
            }
            return output;
        }

        /**
         * @brief 解开 brotli 流（用例侧的自检工具）
         * @param input brotli 字节
         * @param expectedSize 原始长度（调用方已知）
         * @return std::optional<std::string> 原始内容；解不开或长度不符时为空
         */
        std::optional<std::string> unbrotli(const std::string_view input, const std::size_t expectedSize)
        {
            std::string output(expectedSize, '\0');
            std::size_t decodedLength = output.size();
            if (BrotliDecoderDecompress(input.size(), reinterpret_cast<const std::uint8_t *>(input.data()), &decodedLength,
                                        reinterpret_cast<std::uint8_t *>(output.data())) != BROTLI_DECODER_RESULT_SUCCESS ||
                decodedLength != expectedSize)
            {
                return std::nullopt;
            }
            return output;
        }

        /// 一段高度重复的正文：压缩后应明显小于原文
        std::string makeRepetitiveBody()
        {
            const std::string sentence = "AsynGyanis 高性能服务器引擎：重复内容用来验证压缩确实生效。";
            std::string       body;
            for (int round = 0; round < 16; ++round)
            {
                body += sentence;
            }
            return body;
        }
    } // namespace

    /**
     * @brief zstd 压缩后再解开必须与原内容逐字节一致
     */
    TEST(CompressionCodecs, ZstdRoundTripsBackToTheOriginalBytes)
    {
        const std::string original = "AsynGyanis 响应压缩：这是一段中文与 ASCII 混排的正文，用来验证往返一致性。";

        const std::optional<std::string> compressed = zstdCompress(original);
        ASSERT_TRUE(compressed.has_value()) << "压缩返回失败";

        const std::optional<std::string> restored = unzstd(*compressed, original.size());
        ASSERT_TRUE(restored.has_value()) << "zstd 压出来的字节解不开";
        EXPECT_EQ(*restored, original);
    }

    /**
     * @brief 空输入也要给出可解压的合法帧（空指针防护回归）
     */
    TEST(CompressionCodecs, ZstdHandlesEmptyInput)
    {
        const std::optional<std::string> compressed = zstdCompress("");
        ASSERT_TRUE(compressed.has_value()) << "空输入的压缩返回失败";

        const std::optional<std::string> restored = unzstd(*compressed, 0);
        ASSERT_TRUE(restored.has_value()) << "空输入的 zstd 帧解不开";
        EXPECT_TRUE(restored->empty());
    }

    /**
     * @brief 重复内容压完应明显小于原文（否则压缩根本没生效）
     */
    TEST(CompressionCodecs, ZstdShrinksRepetitiveContent)
    {
        const std::string body = makeRepetitiveBody();

        const std::optional<std::string> compressed = zstdCompress(body);
        ASSERT_TRUE(compressed.has_value());
        EXPECT_LT(compressed->size(), body.size() / 4) << "重复内容的压缩率异常";
        EXPECT_EQ(*unzstd(*compressed, body.size()), body);
    }

    /**
     * @brief brotli 压缩后再解开必须与原内容逐字节一致
     */
    TEST(CompressionCodecs, BrotliRoundTripsBackToTheOriginalBytes)
    {
        const std::string original = "AsynGyanis 响应压缩：这是一段中文与 ASCII 混排的正文，用来验证往返一致性。";

        const std::optional<std::string> compressed = brotliCompress(original);
        ASSERT_TRUE(compressed.has_value()) << "压缩返回失败";

        const std::optional<std::string> restored = unbrotli(*compressed, original.size());
        ASSERT_TRUE(restored.has_value()) << "brotli 压出来的字节解不开";
        EXPECT_EQ(*restored, original);
    }

    /**
     * @brief 空输入也要给出可解压的合法流（MaxCompressedSize(0)=0 的缓冲防护回归）
     */
    TEST(CompressionCodecs, BrotliHandlesEmptyInput)
    {
        const std::optional<std::string> compressed = brotliCompress("");
        ASSERT_TRUE(compressed.has_value()) << "空输入的压缩返回失败";

        const std::optional<std::string> restored = unbrotli(*compressed, 0);
        ASSERT_TRUE(restored.has_value()) << "空输入的 brotli 流解不开";
        EXPECT_TRUE(restored->empty());
    }

    /**
     * @brief 重复内容压完应明显小于原文
     */
    TEST(CompressionCodecs, BrotliShrinksRepetitiveContent)
    {
        const std::string body = makeRepetitiveBody();

        const std::optional<std::string> compressed = brotliCompress(body);
        ASSERT_TRUE(compressed.has_value());
        EXPECT_LT(compressed->size(), body.size() / 4) << "重复内容的压缩率异常";
        EXPECT_EQ(*unbrotli(*compressed, body.size()), body);
    }

    /**
     * @brief 越界档位被夹取到合法区间而不是让压缩失败
     */
    TEST(CompressionCodecs, OutOfRangeLevelsAreClampedInsteadOfFailing)
    {
        const std::string original = "档位越界只应影响压缩率，不应让整次压缩失败。";

        const std::optional<std::string> zstdOutput = zstdCompress(original, 999);
        ASSERT_TRUE(zstdOutput.has_value()) << "zstd 越界级别应被夹取而不是失败";
        EXPECT_EQ(*unzstd(*zstdOutput, original.size()), original);

        const std::optional<std::string> brotliOutput = brotliCompress(original, 999);
        ASSERT_TRUE(brotliOutput.has_value()) << "brotli 越界质量应被夹取而不是失败";
        EXPECT_EQ(*unbrotli(*brotliOutput, original.size()), original);
    }
} // namespace AsynGyanis::Net
