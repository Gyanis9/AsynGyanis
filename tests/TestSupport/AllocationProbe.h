/**
 * @file AllocationProbe.h
 * @brief 分配计数探针：把「某个形状每次操作碰几次堆」钉成可断言的读数
 * @author Gyanis
 * @date 2026-09-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 探针靠替换全局 operator new 计数，实体定义在 AllocationProbe.cpp，需与被测用例
 *          一起编进同一个可执行体。计数只在单线程测量窗口内取样，因此它是读数而不是同步点。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace AsynGyanis::TestSupport
{
    /// 每个形状连跑这么多次再摊平：单次读数会被「临时串先分配后释放」这类顺序细节影响
    inline constexpr std::uint64_t kMeasurementIterations = 1000U;

    /// 测量窗口内累计的分配次数（relaxed，只当读数用）
    extern std::atomic<std::uint64_t> allocationCount;

    /// 测量窗口内累计申请的字节数
    extern std::atomic<std::uint64_t> allocationBytes;

    /**
     * @brief 一次测量的结果：窗口内的分配原值，外加摊平到每次操作的读数
     * @details 摊平是整除，因此「每次 0 次」这个读数掩盖得住一千次里的零星几次分配。
     *          凡是要钉「稳态一次都不碰堆」的形状，判据一律用 totalAllocations 原值。
     */
    struct AllocationProfile
    {
        std::uint64_t totalAllocations{0};        ///< 整个测量窗口的分配总次数
        std::uint64_t totalBytes{0};              ///< 整个测量窗口申请的字节总数
        std::uint64_t allocationsPerOperation{0}; ///< 每次操作的分配次数
        std::uint64_t bytesPerOperation{0};       ///< 每次操作申请的字节数
        std::uint64_t resultSum{0};               ///< 被测体标记之和，用于证明它真的跑了
    };

    /**
     * @brief 把 body 连跑 kMeasurementIterations 次，给出分配原值与摊平读数
     * @tparam Body 可调用体，返回一个与「做成了多少」成正比的标记
     * @param body 被测形状：只做事、不断言
     * @return AllocationProfile 分配总次数与字节数，外加所有标记之和
     */
    template<typename Body>
    [[nodiscard]] AllocationProfile measurePerOperation(const Body &body)
    {
        const std::uint64_t beganCount = allocationCount.load(std::memory_order_relaxed);
        const std::uint64_t beganBytes = allocationBytes.load(std::memory_order_relaxed);

        std::uint64_t resultSum = 0;
        for (std::uint64_t iteration = 0; iteration < kMeasurementIterations; ++iteration)
        {
            resultSum += static_cast<std::uint64_t>(body());
        }

        AllocationProfile profile;
        profile.totalAllocations        = allocationCount.load(std::memory_order_relaxed) - beganCount;
        profile.totalBytes              = allocationBytes.load(std::memory_order_relaxed) - beganBytes;
        profile.allocationsPerOperation = profile.totalAllocations / kMeasurementIterations;
        profile.bytesPerOperation       = profile.totalBytes / kMeasurementIterations;
        profile.resultSum               = resultSum;
        return profile;
    }
} // namespace AsynGyanis::TestSupport
