/**
 * @file ReloadRoundGate.h
 * @brief 热重载轮次的节流闸门：保证「每次配置变更都会被某一轮重读覆盖」
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace AsynGyanis::Base::Detail
{
    /**
     * @brief 热重载的「一轮在跑、期间又来变更」节流闸门
     *
     * @details 用「单调递增的世代号 + 一把执行权」而不是「pending + dirty 两面旗」：dirty 是**共享**的
     *          欠账，收尾可以并发（一轮在 `pending=false` 之后仍然活着，另一轮已经开跑并收尾），
     *          于是停在中途的那一轮会把别人记下的欠账吃掉，随后自己抢不到接力权——那笔变更再没人重读，
     *          配置永久停在旧值，直到用户下一次改动。世代号只会变大、不会被消耗，每轮只与**自己那份**
     *          开始时的快照比，因此谁也不能替别人把信息清掉。
     * @note 只描述时序协议，不读盘也不起线程；交错顺序可以直接按调用序列复现，便于单独验证。
     */
    class ReloadRoundGate
    {
    public:
        /**
         * @brief 记下一次「配置又要重读」的事实
         * @details 必须在尝试 claim 之前调用：本轮抢不到执行权时，正在跑的那一轮要靠世代号的差别
         *          决定要不要再来一轮
         */
        void noteChanged() noexcept
        {
            m_generation.fetch_add(1, std::memory_order_acq_rel);
        }

        /**
         * @brief 试图占住一轮重载的执行权
         * @return true 占到了，调用方必须启动一轮并在结束时 releaseRound
         * @return false 已有一轮在跑，它那轮的收尾会看到这次的世代差别
         */
        [[nodiscard]] bool claimRound() noexcept
        {
            bool expected = false;
            return m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
        }

        /**
         * @brief 取当前世代号，作为「本轮开始前」的快照
         * @return std::uint64_t 世代号；初值为 0，之后每次 noteChanged 加一
         */
        [[nodiscard]] std::uint64_t observedGeneration() const noexcept
        {
            return m_generation.load(std::memory_order_acquire);
        }

        /**
         * @brief 交还执行权：本轮跑完了，但**尚未**决定要不要再来一轮
         * @details 与 `claimAnotherRound` 拆成两步，是为了让「交还之后、决定之前被抢占」这个真实
         *          可能的中间态在测试里可复现——旧写法正是在这一格里把别人的欠账吃掉
         */
        void finishRunning() noexcept
        {
            m_running.store(false, std::memory_order_release);
        }

        /**
         * @brief 判断本轮之后是否又有变更，有则占住下一轮
         * @param observed 本轮开始时的世代快照
         * @return true 有新变更且已占到执行权，调用方应再启动一轮
         * @return false 没有新变更，或别的触发者已占到那一轮（它会比对自己那份快照）
         */
        [[nodiscard]] bool claimAnotherRound(std::uint64_t observed) noexcept
        {
            return observedGeneration() != observed && claimRound();
        }

        /**
         * @brief 一轮结束：交还执行权，并回答「还要不要再跑一轮」
         * @details 顺序是先交还再比对——反过来会让「交还之后、比对之前」到达的变更没人接力
         * @param observed 本轮开始时的世代快照
         * @return true 已重新占到下一轮，调用方应再启动一轮
         * @return false 没有欠账，或执行权已被别的触发者接走
         */
        [[nodiscard]] bool releaseRound(const std::uint64_t observed) noexcept
        {
            finishRunning();
            return claimAnotherRound(observed);
        }

    private:
        std::atomic<std::uint64_t> m_generation{0};  ///< 「配置又要重读」的累计次数，只增不减
        std::atomic<bool>          m_running{false}; ///< 是否已有一轮占住执行权
    };
} // namespace AsynGyanis::Base::Detail
