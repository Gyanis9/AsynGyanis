// 游标视图（Core::cursorRange）的三档惰性判据：begin 只推进一行、break 之后不再推进、
// 末尾那次 next() 返回 false 即收尾；外加单遍语义与「交出的是游标本身」两条形状判据。
// 全部用它自己的打桩游标，不依赖 Database 模块。

#include "Core/Coroutine/CursorRange.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 打桩游标：按给定的行数逐行推进，并记录 next() 被调用了几次
         * @details 计数是这一组判据的关键：「有没有惰性」只能从「推了几次」读出来——
         *          把所有行先读进一个 vector 再交出去的实现，行数与值都对，但调用次数会多一次
         */
        class StubCursor
        {
        public:
            explicit StubCursor(const std::size_t rowCount) noexcept : m_rowCount(rowCount), m_remaining(rowCount)
            {
            }

            /**
             * @brief 推进一行
             * @return true 还有一行（m_currentRow 已更新）；false 已到末尾
             */
            [[nodiscard]] bool next() noexcept
            {
                ++m_nextCallCount;
                if (m_remaining == 0)
                {
                    return false;
                }
                --m_remaining;
                m_currentRow = static_cast<std::int64_t>(m_rowCount - m_remaining);
                return true;
            }

            /// 当前行的值（1 起算）
            [[nodiscard]] std::int64_t value() const noexcept
            {
                return m_currentRow;
            }

            /// next() 累计被调用的次数
            [[nodiscard]] std::size_t nextCallCount() const noexcept
            {
                return m_nextCallCount;
            }

        private:
            std::size_t  m_rowCount{0};      ///< 总行数
            std::size_t  m_remaining{0};     ///< 还剩几行可推
            std::int64_t m_currentRow{0};    ///< 当前行的值
            std::size_t  m_nextCallCount{0}; ///< next() 调用次数
        };

        /**
         * @brief 跑一遍视图并把每行的值收下来
         * @param cursor 打桩游标
         * @return std::vector<std::int64_t> 依次读到的值
         */
        [[nodiscard]] std::vector<std::int64_t> collectRows(StubCursor &cursor)
        {
            std::vector<std::int64_t> values;
            for (const StubCursor &row: cursorRange(cursor))
            {
                values.push_back(row.value());
            }
            return values;
        }
    } // namespace

    /**
     * @brief 钉住整段遍历：行序与行数都对，且恰好多一次 next()（那次返回 false）
     * @details 多出来的那一次不是浪费：没有它就没法区分「读完了」与「还没读完」。
     *          判据两半——值序列相等、调用次数 == 行数 + 1；把视图改成「先全读进 vector」
     *          的实现，前者照样过、后者当场差一次
     */
    TEST(CursorRange, WalksEveryRowAndStopsOnTheFirstFailedAdvance)
    {
        StubCursor cursor(3);

        const std::vector<std::int64_t> values = collectRows(cursor);

        EXPECT_EQ(values, (std::vector<std::int64_t>{1, 2, 3}));
        EXPECT_EQ(cursor.nextCallCount(), 4U) << "调用次数不是「行数 + 1」：视图要么预读，要么少推了一次";
    }

    /**
     * @brief 钉住「推进是一行一行来的」：停在哪一行就只推过那么多次
     * @details 这条是「惰性」最容易说错的一半。写法上不用 `break` 正是为了可证：循环里
     *          无条件 break 会让循环第二次判断成为不可达代码（MSVC C4702，`/WX` 下直接构不过），
     *          而且那样测到的是「循环怎么退」；这里手工拿着迭代器走两步再停下，
     *          测的是视图真正承诺的那件事——`begin()` 只推一行、每 `++` 也只推一行
     */
    TEST(CursorRange, AdvancesExactlyOneRowPerStepAndStopsWhereTheCallerStops)
    {
        StubCursor                    cursor(5);
        const CursorRange<StubCursor> view = cursorRange(cursor);

        CursorRange<StubCursor>::Iterator position = view.begin();
        ASSERT_NE(position, view.end());
        EXPECT_EQ(cursor.nextCallCount(), 1U) << "begin() 不止推了一行，视图在预读";
        EXPECT_EQ(position->value(), 1);
        EXPECT_EQ((*position).value(), 1) << "解引用与 -> 取到的不是同一行";

        ++position;
        EXPECT_EQ(cursor.nextCallCount(), 2U) << "一次 ++ 推进了多行";
        EXPECT_EQ(position->value(), 2);

        // 调用方在这里停手：游标就停在第二行，不会再有人推它
        EXPECT_EQ(cursor.nextCallCount(), 2U);
    }

    /**
     * @brief 钉住空游标不执行循环体，并且只推一次就收尾
     */
    TEST(CursorRange, EmptyCursorRunsNoIteration)
    {
        StubCursor cursor(0);

        std::size_t iterations = 0;
        for ([[maybe_unused]] const StubCursor &row: cursorRange(cursor))
        {
            ++iterations;
        }

        EXPECT_EQ(iterations, 0U);
        EXPECT_EQ(cursor.nextCallCount(), 1U) << "空游标也只该问一次「有下一行吗」";
    }

    /**
     * @brief 钉住交出的是游标本身（同一对象），不是它的副本
     * @details 复制一份就要多一次分配，而多数消费点读几列就走；更要紧的是副本上读到的是快照，
     *          与「迭代器指向当前行」的语义不符
     */
    TEST(CursorRange, YieldsTheCursorItselfRatherThanACopy)
    {
        StubCursor cursor(2);

        bool isSameObject = true;
        for (const StubCursor &row: cursorRange(cursor))
        {
            isSameObject = isSameObject && (&row == &cursor);
        }

        EXPECT_TRUE(isSameObject) << "视图交出了副本，解引用与游标的身份断了";
    }

    /**
     * @brief 钉住「只能遍历一次」这条单遍语义：第二遍是空的，且只多花一次探测
     * @details 游标只能往前，所以这是它的性质而不是视图的缺陷——写下来是为了让下一个人
     *          不要把它当 bug 去「修」（真要在两处消费，就该把值收进容器再分发）
     */
    TEST(CursorRange, IsSinglePassBecauseTheCursorOnlyMovesForward)
    {
        StubCursor cursor(2);

        EXPECT_EQ(collectRows(cursor).size(), 2U);
        const std::size_t callsAfterFirstPass = cursor.nextCallCount();

        EXPECT_TRUE(collectRows(cursor).empty()) << "第二遍竟然读到了行：游标被推回去了？";
        EXPECT_EQ(cursor.nextCallCount(), callsAfterFirstPass + 1U) << "第二遍不是「问一次就收尾」";
    }

} // namespace AsynGyanis::Core
