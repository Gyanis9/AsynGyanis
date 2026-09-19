// TestQuicReassemblyBuffer.cpp —— 按偏移重组字节的缓存（RFC 9000 §7.5）用例
//
// 对端可以按任意边界重发同一段字节，因此这里的核心契约是「每个偏移恰好交付一次」：
// 覆盖：1) 缺口未补时一律不交付；2) 新段起点落在已缓存段中间时要把那一段一起并进来
//       （只按起点建索引会让它永远排不干）；3) 新段盖住已缓存段的尾巴时不丢尾部字节；
//       4) 已交付前缀之下的重发被剪掉；5) 真正的缺口保持分开，缓存计数随合并与交付增减。
// 纯计算，不涉及网络。

#include "Net/Quic/QuicReassemblyBuffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 交付一次并转成文本，比对「按序、不重不漏」最直接
        std::string drainText(QuicReassemblyBuffer &buffer)
        {
            std::vector<std::uint8_t> out;
            std::ignore = buffer.drain(out);
            return std::string(out.begin(), out.end());
        }

        /// 交入一段文本，省掉每处 reinterpret 的样板
        void insertText(QuicReassemblyBuffer &buffer, const std::uint64_t offset, const std::string_view text)
        {
            const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
            buffer.insert(offset, bytes);
        }
    } // namespace

    /**
     * @brief 只有一直排到交付点的段才算可交付
     */
    TEST(QuicReassemblyBuffer, HoldsFragmentsUntilTheExpectedOffsetArrives)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 4, "56789");            // [4,9)，前四个字节还空着
        EXPECT_EQ(drainText(buffer), "");
        EXPECT_EQ(buffer.bufferedByteCount(), 5U);
        EXPECT_EQ(buffer.deliveredOffset(), 0U);

        insertText(buffer, 0, "ab");
        EXPECT_EQ(drainText(buffer), "ab") << "接上交付点的段该立刻交出去，哪怕后面还有缺口";

        insertText(buffer, 2, "34");
        EXPECT_EQ(drainText(buffer), "3456789");
        EXPECT_EQ(buffer.deliveredOffset(), 9U);
        EXPECT_EQ(buffer.bufferedByteCount(), 0U);
    }

    /**
     * @brief 新段的起点落在已缓存段中间：两段必须并成一段，否则尾部永远排不干
     */
    TEST(QuicReassemblyBuffer, MergesFragmentStartingInsideACachedRange)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 10, "abcdefghij");               // [10,20)
        insertText(buffer, 15, "KLMNOPQRST");               // [15,25)，起点落在上一段中间（重发换了分片大小）
        insertText(buffer, 0, "0123456789");                // [0,10) 补齐头部

        EXPECT_EQ(drainText(buffer), "0123456789abcdeKLMNOPQRST");
        EXPECT_EQ(buffer.deliveredOffset(), 25U);
        EXPECT_EQ(buffer.bufferedByteCount(), 0U);
    }

    /**
     * @brief 新段盖住已缓存段的尾部：被盖住的部分以本次交入为准，超出右边界的那一截不能丢
     */
    TEST(QuicReassemblyBuffer, KeepsTailBeyondNewFragmentWhenRangesOverlap)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 4, "EFGH");          // [4,8)
        insertText(buffer, 0, "ABCDxy");        // [0,6)，只盖住上一段的前两个字节
        EXPECT_EQ(drainText(buffer), "ABCDxyGH");

        QuicReassemblyBuffer whole;
        insertText(whole, 0, "0123456789");     // [0,10)
        insertText(whole, 2, "AB");             // 完全被盖住的一段：不缩小右边界
        EXPECT_EQ(drainText(whole), "01AB456789") << "只有 AB 两个字节被覆盖，其余原样";
        EXPECT_EQ(whole.bufferedByteCount(), 0U) << "合并后只剩一段，且已经排干";
    }

    /**
     * @brief 已交付前缀之下的重发剪掉，只补没见过的尾巴（§7.5）
     */
    TEST(QuicReassemblyBuffer, TrimsBytesBelowDeliveredOffset)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 0, "01234");
        EXPECT_EQ(drainText(buffer), "01234");

        insertText(buffer, 1, "123");           // 整段都在已交付范围内
        EXPECT_EQ(drainText(buffer), "");
        EXPECT_EQ(buffer.bufferedByteCount(), 0U);

        insertText(buffer, 3, "34567");         // 后两个字节是新的
        EXPECT_EQ(drainText(buffer), "567");
        EXPECT_EQ(buffer.deliveredOffset(), 8U);
    }

    /**
     * @brief 真有缺口时两段保持分开，计数只统计缓存里的那一段
     */
    TEST(QuicReassemblyBuffer, KeepsRealGapsApart)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 0, "abcde");         // [0,5)
        insertText(buffer, 10, "ijklm");        // [10,15)，中间空 5 字节
        EXPECT_EQ(drainText(buffer), "abcde");
        EXPECT_EQ(buffer.bufferedByteCount(), 5U);
        EXPECT_EQ(buffer.deliveredOffset(), 5U);
        EXPECT_EQ(drainText(buffer), "") << "缺口没补上之前不该再交付任何东西";
    }

    /**
     * @brief 完全相同的重发不产生重复字节，也不虚增缓存
     */
    TEST(QuicReassemblyBuffer, AbsorbsExactRetransmissionWithoutDuplicates)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 0, "0123456789");
        insertText(buffer, 0, "0123456789");
        insertText(buffer, 0, "0123456789");
        EXPECT_EQ(buffer.bufferedByteCount(), 10U);
        EXPECT_EQ(drainText(buffer), "0123456789");
        EXPECT_EQ(buffer.deliveredOffset(), 10U);
    }

    /**
     * @brief 一次交入把多段连成一片：左右各两段都被并进来
     */
    TEST(QuicReassemblyBuffer, JoinsSeveralCachedRangesAtOnce)
    {
        QuicReassemblyBuffer buffer;
        insertText(buffer, 0, "ab");            // [0,2)
        insertText(buffer, 6, "gh");            // [6,8)
        insertText(buffer, 10, "jkl");          // [10,13)
        EXPECT_EQ(drainText(buffer), "ab") << "只剩 [6,13) 悬着";
        insertText(buffer, 2, "cdefghi");       // [2,9)：把 [6,8) 与 [10,13) 之外的缺口一并填上
        EXPECT_EQ(drainText(buffer), "cdefghi") << "[10,13) 与 [2,9) 之间还空着第 9 字节";
        insertText(buffer, 9, "i");
        EXPECT_EQ(drainText(buffer), "ijkl") << "补齐空洞后三段该一起排干";
        EXPECT_EQ(buffer.bufferedByteCount(), 0U);
    }
} // namespace AsynGyanis::Net
