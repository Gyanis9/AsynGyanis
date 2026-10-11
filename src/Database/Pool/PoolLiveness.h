/**
 * @file PoolLiveness.h
 * @brief 连接池存活令牌 — 「池正在收尾」与「连接正在归还」之间的同步点
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 归还路径必须把「判活」与「调用池」放在同一个临界区里完成：只读一个原子量的话，
 *          并发销毁时判活刚通过、调用就踩空（池已析构）。池析构在锁内置假，因此它要么排在
 *          已在锁内的归还方之后（等它走完才继续销毁成员），要么让后来者读到「池已停摆」而
 *          原地收手。持锁的区段只需覆盖「置假」与「判活 + 碰池」两侧，不必覆盖析构全程。
 */
#pragma once

#include "AsynGyanisExport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 池的存活令牌：一个互斥量加一个标志位，另附一份借出账本
     */
    struct ASYN_DATABASE_API PoolLiveness
    {
        /// 未登记槽位的哨兵（空包装、以及理论上不可达的「槽位全占」）
        static constexpr std::size_t kNoBorrowSlot = static_cast<std::size_t>(-1);

        /**
         * @brief 构造令牌并开好借出账本的槽位
         * @param borrowSlotCount 槽位数，取池的连接上限（在借数恒 ≤ 它）
         * @note 只用尺寸构造、不经 resize：`std::vector<std::atomic<…>>` 在重分配时要搬元素，
         *       而原子量不可搬——尺寸构造一次到位，绕开那条编译期约束
         */
        explicit PoolLiveness(const std::size_t borrowSlotCount) : borrowStartNanos(borrowSlotCount)
        {
        }

        /// 借出账本：每条在借连接的起始时刻按槽位记录（steady 纳秒；0 = 空闲槽）。
        /// @details 放在令牌上而不是池本体上，是为了与「活到最后一个借用者离开」对齐：
        ///          PooledConnection 可能在池析构之后才走收尾（帧活过池的场景），注销自己
        ///          那一格时若碰的是池对象就是释放后使用；账本随令牌共享生命期，登记与注销
        ///          只是一次原子写，不碰池的任何一把锁——抓取线程扫它算「最久在借」同理
        std::vector<std::atomic<std::int64_t>> borrowStartNanos;

        /**
         * @brief 认领一个空闲槽并写入起始时刻；槽位全占时给哨兵
         * @param startNanos 借出时刻（steady 纳秒）
         * @return 槽位下标；未认领到时为 kNoBorrowSlot
         * @note 在借数恒 ≤ 槽数（占位判定保证），因此走不到哨兵；真漏进来也只是少报一格
         */
        [[nodiscard]] std::size_t claimBorrowSlot(const std::int64_t startNanos) noexcept
        {
            for (std::size_t index = 0; index < borrowStartNanos.size(); ++index)
            {
                std::int64_t expected = 0;
                if (borrowStartNanos[index].compare_exchange_strong(expected, startNanos, std::memory_order_relaxed))
                {
                    return index;
                }
            }
            return kNoBorrowSlot;
        }

        /**
         * @brief 注销一个槽位（幂等：对已在空闲的槽再写一次 0 无害）
         * @param slot 槽位下标；kNoBorrowSlot 时空操作
         */
        void releaseBorrowSlot(const std::size_t slot) noexcept
        {
            if (slot != kNoBorrowSlot && slot < borrowStartNanos.size())
            {
                borrowStartNanos[slot].store(0, std::memory_order_relaxed);
            }
        }

        std::mutex mutex;         ///< 同步「池正在收尾」与「某连接正在归还」
        bool       isAlive{true}; ///< 池是否仍在服务
    };

} // namespace AsynGyanis::Database
