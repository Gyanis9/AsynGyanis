/**
 * @file QuicReceivedPacketNumbers.h
 * @brief 已收包号的区间集合：QUIC 的重复包判定与 ACK 区间原料
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>

namespace AsynGyanis::Net
{
    /**
     * @brief 一条连接在某个包号空间里「已经收过哪些包号」
     * @details 存连续区间而不是逐个包号：RFC 9000 §17.1 要求发送方按序递增用号，乱序只留下少量空洞，
     *          因此几千个包号在区间表示下通常是一两个节点。这既是收包路径上最后一笔逐包分配的去向，
     *          也是每空间最多 `kQuicMaximumTrackedPacketNumbers` 个包号的滞留上界。
     * @note 包号按 RFC 9000 §17.1 不超过 2^62-1，因此相邻判断里的 +1 不会回绕。
     */
    class QuicReceivedPacketNumbers
    {
    public:
        QuicReceivedPacketNumbers() = default;

        /**
         * @brief 按给出顺序记入一批包号
         * @param packetNumbers 包号列表，顺序随意（相邻的照样会并段）
         * @note 为的是「拿一串包号摆出某个状态」这类写法短得下去：测试与替身都按字面量摆集合
         */
        QuicReceivedPacketNumbers(std::initializer_list<std::uint64_t> packetNumbers)
        {
            for (const std::uint64_t packetNumber: packetNumbers)
            {
                static_cast<void>(insert(packetNumber));
            }
        }

        /**
         * @brief 记一个刚解密成功的包号，并与相邻区间自动合并
         * @param packetNumber 还原成绝对值之后的包号
         * @return true 这是没见过的那个号，调用方可以继续处理包内的帧
         * @return false 这个号已经记过（重复包），调用方必须整包丢弃——RFC 9000 §13.1 禁止把同一个
         *         包号交付两次，否则流偏移会被重复应用
         */
        bool insert(std::uint64_t packetNumber);

        /**
         * @brief 从最老的一端裁到不超过给定额度
         * @param maximumTracked 允许留存的包号总数；最老那一段会被截短而不是整段丢掉
         */
        void dropOldestUntil(std::size_t maximumTracked) noexcept;

        /// @return 当前覆盖的包号总数（不是区间个数），增量维护因此是 O(1)
        [[nodiscard]] std::size_t trackedPacketNumberCount() const noexcept
        {
            return m_trackedPacketNumberCount;
        }

        /// @return 升序、互不重叠也互不相邻的区间表，键为区间首、值为区间尾（两端都含）
        [[nodiscard]] const std::map<std::uint64_t, std::uint64_t> &ranges() const noexcept
        {
            return m_ranges;
        }

    private:
        std::map<std::uint64_t, std::uint64_t> m_ranges{};                    ///< 首包号 → 尾包号，两端都含
        std::size_t                            m_trackedPacketNumberCount{0}; ///< 覆盖的包号总数，与区间表同步增减
    };
} // namespace AsynGyanis::Net
