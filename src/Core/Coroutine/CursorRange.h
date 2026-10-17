/**
 * @file CursorRange.h
 * @brief 把「next() 推进 + 当前行取值」式的游标包成一段惰性视图
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include <cstddef>
#include <iterator>

namespace AsynGyanis::Core
{
    /**
     * @brief 游标视图：让 `for (auto &row : cursorRange(cursor))` 等价于一条 `while (cursor.next())` 循环
     *
     * @details 存在的理由与「不自研 generator」同源：本仓有几处「服务端逐行交结果」的游标
     *          （`DatabaseResult` 的流式读是其中最典型的一处），消费它们天然就是上面那条循环。
     *          标准库的惰性视图（`std::generator`、`std::ranges::views`）要么还没进这条工具链、
     *          要么要求协程产出端，而这里要的只是把现成的推进式游标喂给 range-for——
     *          因此给的是适配器，不是生成器；等标准 `std::generator` 可用时这一层可以整体换掉。
     *
     * @details 语义按 std::istream_iterator 同族定（输入范围）：
     *          游标是单遍的，视图因此只能遍历一次，`begin()` 会把游标推进到第一行——
     *          换句话说 `begin()` 不是只读操作，它调用一次 `next()`。这一点必须记牢：
     *          对着同一个视图跑两遍 range-for，第二遍拿到的是空（游标已经在末尾），
     *          这不是缺陷而是游标本身的性质（`next()` 只能往前）。
     *
     * @details 惰性三档都在判据里（见 TestCursorRange）：不预读（begin 只推进一行）、
     *          `break` 之后不再推进、末尾那次 `next()` 返回 false 即结束。
     *
     * @tparam CursorType 游标类型，必须提供 `bool next()`（返回 false 表示没有下一行）
     */
    template<typename CursorType>
    class CursorRange
    {
    public:
        /**
         * @brief 游标视图的输入迭代器
         * @details 空迭代器（`m_cursor == nullptr`）即末尾哨兵；`begin()` 构造时先推进一行，
         *          此后每次 `++` 再推一行，推不动就把自己变成哨兵。解引用交回的是游标本身
         *          （当前行挂在它上面），不是它的副本——复制一份行数据要多一次分配，
         *          而多数消费点读几列就走
         */
        class Iterator
        {
        public:
            using iterator_concept  = std::input_iterator_tag; ///< 单遍：游标推不回去
            using iterator_category = std::input_iterator_tag; ///< 同上（C++17 兼容字段）
            using value_type        = CursorType;              ///< 元素类型
            using difference_type   = std::ptrdiff_t;          ///< 迭代器差值类型（输入范围不要求可减）
            using pointer           = CursorType *;            ///< 元素指针
            using reference         = CursorType &;            ///< 元素引用：就是游标本身

            /// 末尾哨兵
            Iterator() noexcept = default;

            /**
             * @brief 构造并推进到第一行
             * @param cursor 待遍历的游标（调用期间必须保持存活）
             */
            explicit Iterator(CursorType *cursor) noexcept : m_cursor(cursor)
            {
                advance();
            }

            /**
             * @brief 取当前行（就是游标本身）
             * @return CursorType& 游标引用；末尾哨兵上解引用是未定义行为，与标准输入迭代器一致
             */
            [[nodiscard]] CursorType &operator*() const noexcept
            {
                return *m_cursor;
            }

            /**
             * @brief 取当前行的成员
             * @return CursorType* 游标指针
             */
            [[nodiscard]] CursorType *operator->() const noexcept
            {
                return m_cursor;
            }

            /**
             * @brief 推进到下一行；推不动即变成末尾哨兵
             * @return Iterator& 自身
             */
            Iterator &operator++() noexcept
            {
                advance();
                return *this;
            }

            /// 后置自增（输入迭代器允许返回 void）
            void operator++(int) noexcept
            {
                advance();
            }

            /**
             * @brief 比较两个迭代器
             * @param other 另一个迭代器
             * @return true 两者指向同一位置（末尾哨兵之间相等）
             */
            [[nodiscard]] bool operator==(const Iterator &other) const noexcept
            {
                return m_cursor == other.m_cursor;
            }

        private:
            /// 推一行；推不动就归位成末尾哨兵。判据是游标自己的 next()，本类不猜它内部还剩多少行
            void advance() noexcept
            {
                if (m_cursor != nullptr && !m_cursor->next())
                {
                    m_cursor = nullptr;
                }
            }

            CursorType *m_cursor{nullptr}; ///< 当前游标；空表示已到末尾
        };

        /**
         * @brief 构造视图
         * @param cursor 待遍历的游标（调用期间必须保持存活）
         */
        explicit CursorRange(CursorType &cursor) noexcept : m_cursor(&cursor)
        {
        }

        /**
         * @brief 取出起始迭代器（会推进游标一行）
         * @return Iterator 已停在可以读的第一行上的迭代器；游标推到末尾时返回末尾哨兵
         */
        [[nodiscard]] Iterator begin() const noexcept
        {
            return Iterator{m_cursor};
        }

        /**
         * @brief 取出末尾哨兵
         * @return Iterator 空迭代器
         */
        [[nodiscard]] Iterator end() const noexcept
        {
            return Iterator{};
        }

    private:
        CursorType *m_cursor{nullptr}; ///< 被遍历的游标（非拥有）
    };

    /**
     * @brief 构造游标视图的工厂（省去写模板实参）
     * @tparam CursorType 游标类型，由实参推导
     * @param cursor 待遍历的游标
     * @return CursorRange<CursorType> 惰性视图
     * @note 与 `std::views::all` 一族的取向一致：不进容器、不拷贝元素、只把现成的推进接口
     *       翻译成 range 协议；因此不要把它交给任何会遍历两次的算法（那是单遍游标使然，
     *       见类注释）
     */
    template<typename CursorType>
    [[nodiscard]] CursorRange<CursorType> cursorRange(CursorType &cursor) noexcept
    {
        return CursorRange<CursorType>{cursor};
    }

} // namespace AsynGyanis::Core
