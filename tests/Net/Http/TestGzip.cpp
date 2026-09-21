// Gzip 单元测试：压得进去也解得回来、空输入、不可压输入、重复内容确实变小 断言一律走 zlib 的 inflate 解开再逐字节比较：只比大小无法发现「压出来的字节解不开」，
// 而那正是响应压缩最怕的事（对端拿到一串解不开的正文）。
#include "Net/Http/Gzip.h"

#include <gtest/gtest.h>

#include <zlib.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 解开 gzip 容器（用例侧的自检工具）
         * @param input gzip 字节
         * @return std::optional<std::string> 原始内容；解不开时为空
         */
        std::optional<std::string> gunzip(const std::string_view input)
        {
            z_stream stream{};
            if (::inflateInit2(&stream, 15 + 16) != Z_OK)
            {
                return std::nullopt;
            }

            std::string output;
            output.reserve(input.size() * 4);

            stream.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
            stream.avail_in = static_cast<uInt>(input.size());

            std::string chunk(4096, '\0');
            int         result = Z_OK;
            while (result != Z_STREAM_END)
            {
                stream.next_out  = reinterpret_cast<Bytef *>(chunk.data());
                stream.avail_out = static_cast<uInt>(chunk.size());
                result           = ::inflate(&stream, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
                {
                    ::inflateEnd(&stream);
                    return std::nullopt;
                }
                const std::size_t producedBytes = chunk.size() - stream.avail_out;
                output.append(chunk.data(), producedBytes);
                if (result == Z_BUF_ERROR)
                {
                    break;
                }
            }

            ::inflateEnd(&stream);
            return output;
        }
    } // namespace

    /**
     * @brief 压缩后再解开必须与原内容逐字节一致
     */
    TEST(GzipTest, RoundTripsBackToTheOriginalBytes)
    {
        const std::string original = "AsynGyanis 响应压缩：这是一段中文与 ASCII 混排的正文，用来验证往返一致性。";

        const std::optional<std::string> compressed = gzipCompress(original);
        ASSERT_TRUE(compressed.has_value()) << "压缩返回失败";

        const std::optional<std::string> restored = gunzip(*compressed);
        ASSERT_TRUE(restored.has_value()) << "压缩结果解不开";
        EXPECT_EQ(*restored, original);
    }

    /**
     * @brief 复用流按调用复位：不得把上一条响应的字典带进这一条
     * @details gzipCompress 改为复用 thread_local deflate 流 + 每条 deflateReset。若复位不彻底，
     *          同一输入第二次压缩会吃到上一条字典而与第一次不一致，或不同输入交错后解不回原文。
     *          这里把「复用 + 复位」钉成与「每条新建流」逐字节一致。
     */
    TEST(GzipTest, ReusesStreamAcrossCallsWithoutLeakingContext)
    {
        const std::string repeated = "复用的 deflate 流不得把上一条响应的字典带进这一条：中文 + ASCII 混排正文。";
        const std::optional<std::string> first = gzipCompress(repeated);
        const std::optional<std::string> second = gzipCompress(repeated);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*first, *second) << "同一输入连压两次必须逐字节一致（证明按调用复位、无跨调用字典残留）";

        // 多种输入交错压缩 + gunzip，逐条往返一致，覆盖「不同长度复用同一条流」
        const std::vector<std::string> payloads{std::string("短"), std::string(), std::string(2048, 'k')};
        for (const std::string &payload: payloads)
        {
            const std::optional<std::string> reCompressed = gzipCompress(payload);
            ASSERT_TRUE(reCompressed.has_value());
            const std::optional<std::string> restoredPayload = gunzip(*reCompressed);
            ASSERT_TRUE(restoredPayload.has_value());
            EXPECT_EQ(*restoredPayload, payload);
        }
    }

    /**
     * @brief 空输入也要产出可解开的 gzip 字节
     */
    TEST(GzipTest, HandlesEmptyInput)
    {
        const std::optional<std::string> compressed = gzipCompress("");
        ASSERT_TRUE(compressed.has_value());

        const std::optional<std::string> restored = gunzip(*compressed);
        ASSERT_TRUE(restored.has_value());
        EXPECT_TRUE(restored->empty());
    }

    /**
     * @brief 不可压内容（伪随机字节）也要能原样往返
     */
    TEST(GzipTest, HandlesIncompressibleInput)
    {
        std::string randomBytes;
        randomBytes.reserve(4096);
        unsigned int state = 0x12345678U;
        for (int index = 0; index < 4096; ++index)
        {
            // 线性同余：够乱以至于几乎压不动，而用例本身是可复现的
            state = state * 1103515245U + 12345U;
            randomBytes.push_back(static_cast<char>((state >> 16) & 0xFF));
        }

        const std::optional<std::string> compressed = gzipCompress(randomBytes);
        ASSERT_TRUE(compressed.has_value());

        const std::optional<std::string> restored = gunzip(*compressed);
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ(*restored, randomBytes);
    }

    /**
     * @brief 高度重复的内容确实变小：确认不是「原样套了个 gzip 头」
     */
    TEST(GzipTest, ShrinksRepetitiveContent)
    {
        const std::string repetitive(64 * 1024, 'A');

        const std::optional<std::string> compressed = gzipCompress(repetitive);
        ASSERT_TRUE(compressed.has_value());
        EXPECT_LT(compressed->size(), repetitive.size() / 10) << "重复内容几乎没压下去，压缩链路可能没生效";
        EXPECT_EQ(*gunzip(*compressed), repetitive);
    }
} // namespace AsynGyanis::Net
