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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace AsynGyanis::TestSupport
{
    /// 每个形状连跑这么多次再摊平：单次读数会被「临时串先分配后释放」这类顺序细节影响
    inline constexpr std::uint64_t kMeasurementIterations = 1000U;

    /**
     * @brief 库以共享形态提供时，本探针是否已经失去观测能力
     * @details Windows 上「替换全局 operator new」只在替换所在的那个模块内生效：五个 DLL 里的分配走
     *          它们各自链接进来的 CRT operator new，不进本可执行体的钩子，于是读数恒为 0——此时
     *          「>0」的判据必红，而「==0」的判据会在真有分配时假绿。Linux 的动态链接让可执行体里的
     *          定义覆盖整个进程，所以这条判据只对 Windows 成立。
     * @note 判据写在 AllocationProbe.cpp 而不是头里：头里的 constexpr 值会让 MSVC 认定 GTEST_SKIP()
     *       之后的用例体不可达（C4702），而本仓库把告警当错误。
     */
    extern const bool kAllocationProbeIsBlind;

    /// 测量窗口内累计的分配次数（relaxed，只当读数用）
    extern std::atomic<std::uint64_t> allocationCount;

    /// 测量窗口内累计申请的字节数
    extern std::atomic<std::uint64_t> allocationBytes;

    /// 直方图的桶宽（字节）：24 字节以内的碎片各占一桶，够分辨「一块串」与「一块 deque 分块」
    inline constexpr std::size_t kAllocationHistogramBucketBytes = 16U;

    /// 直方图桶数：最后一桶是溢出桶，收所有不小于 16 * 桶数 的申请
    inline constexpr std::size_t kAllocationHistogramBucketCount = 200U;

    /// 按申请大小分桶的分配次数快照
    using AllocationHistogram = std::array<std::uint64_t, kAllocationHistogramBucketCount>;

    /**
     * @brief 「第 N 次分配失败」开关：挂上之后**本线程**的第 N 次 operator new 抛 bad_alloc，随后自动解除
     * @details 本探针原先只能数分配，而 `std::bad_alloc` 没有别的注入点——于是好几处
     *          「在 `noexcept` 边界上兜住分配失败」的处置一直钉不住，只能在注释里写补齐路径。
     *          这里就是那条路径：只失败**一次**，因为被测体的兜底分支自己也要分配内存
     *          （摘表、投唤醒），一直失败会把它打成另一种形状，判据就说不清在钉什么。
     * @note 计数按线程各算一份（全局计数会被别的线程偷掉：事件循环线程空闲时也在分配，
     *        于是「没掐到」会表现为随机失败）。这与探针读数本身的口径一致——见头里
     *        「计数只在单线程测量窗口内取样」那条。
     * @note 与计数同样的可见性限制：库以共享形态提供时 DLL 内的分配不经过本可执行体的钩子
     *       （见 kAllocationProbeIsBlind），用例必须先 ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND()。
     * @note 用例不能只断「没崩」：那在开关根本没掐到的时候也成立。要么断
     *       injectedAllocationFailureCount() 涨过，要么断言的正是「失败之后走的那条出口」。
     */
    class AllocationFailureGuard
    {
    public:
        /// @param failureAfterAllocations 从挂上这一刻起第几次分配要失败，必须大于 0
        explicit AllocationFailureGuard(std::uint64_t failureAfterAllocations) noexcept;

        AllocationFailureGuard(const AllocationFailureGuard &) = delete;

        AllocationFailureGuard &operator=(const AllocationFailureGuard &) = delete;

        /// 解除本层开关并恢复上一层（支持嵌套挂法）
        ~AllocationFailureGuard() noexcept;

    private:
        std::uint64_t m_previousTarget{0U}; ///< 挂上之前的目标值，0 表示外层没挂
    };

    /// 至今被开关实际掐掉过几次分配：用例用它自证「注入真的发生了」而不是空过
    [[nodiscard]] std::uint64_t injectedAllocationFailureCount() noexcept;

    /// 清零上面的注入计数（与挂开关配对，免得读到上一用例留下的数）
    void resetInjectedAllocationFailureCount() noexcept;

    /**
     * @brief 清零大小直方图：与 snapshotAllocationHistogram() 配对量出一段窗口的分布
     */
    void resetAllocationHistogram() noexcept;

    /**
     * @brief 取当前大小直方图快照
     * @return AllocationHistogram 各桶的累计分配次数（第 i 桶对应 [i*16, i*16+15] 字节）
     */
    [[nodiscard]] AllocationHistogram snapshotAllocationHistogram() noexcept;

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
     * @brief 把 body 连跑指定次数，给出分配原值与摊平读数
     * @details 供一次操作就要毫秒级的重形状（TLS 握手这类）自己指定次数：默认的一千次会把
     *          整份用例的时限吃掉，而摊平读数对次数的要求只是「够抵消顺序噪声」
     * @tparam Body 可调用体，返回一个与「做成了多少」成正比的标记
     * @param iterationCount 连跑次数，必须大于 0（摊平读数按它做除法）
     * @param body 被测形状：只做事、不断言
     * @return AllocationProfile 分配总次数与字节数，外加所有标记之和
     */
    template<typename Body>
    [[nodiscard]] AllocationProfile measureOperations(const std::uint64_t iterationCount, const Body &body)
    {
        const std::uint64_t beganCount = allocationCount.load(std::memory_order_relaxed);
        const std::uint64_t beganBytes = allocationBytes.load(std::memory_order_relaxed);

        std::uint64_t resultSum = 0;
        for (std::uint64_t iteration = 0; iteration < iterationCount; ++iteration)
        {
            resultSum += static_cast<std::uint64_t>(body());
        }

        AllocationProfile profile;
        profile.totalAllocations        = allocationCount.load(std::memory_order_relaxed) - beganCount;
        profile.totalBytes              = allocationBytes.load(std::memory_order_relaxed) - beganBytes;
        profile.allocationsPerOperation = profile.totalAllocations / iterationCount;
        profile.bytesPerOperation       = profile.totalBytes / iterationCount;
        profile.resultSum               = resultSum;
        return profile;
    }

    /**
     * @brief 按默认次数（kMeasurementIterations）测量，供轻量形状直接用
     * @tparam Body 可调用体，返回一个与「做成了多少」成正比的标记
     * @param body 被测形状：只做事、不断言
     * @return AllocationProfile 分配总次数与字节数，外加所有标记之和
     */
    template<typename Body>
    [[nodiscard]] AllocationProfile measurePerOperation(const Body &body)
    {
        return measureOperations(kMeasurementIterations, body);
    }
} // namespace AsynGyanis::TestSupport

/// 分配台账用例的开场守卫：库以共享形态提供时本探针看不见 DLL 内的分配，用例按 SKIP 报出。
/// 写成宏是因为 GTEST_SKIP() 只能在用例函数体内展开——放进助手函数里会让用例继续跑完。
/// 判据是运行期常量而不是 constexpr：后者会让 MSVC 把 GTEST_SKIP() 之后的用例体判成不可达代码（C4702）。
#define ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND()                                                                                                                                   \
    do                                                                                                                                                                             \
    {                                                                                                                                                                              \
        if (::AsynGyanis::TestSupport::kAllocationProbeIsBlind)                                                                                                                    \
        {                                                                                                                                                                          \
            GTEST_SKIP() << "库以共享形态提供：Windows 上替换全局 operator new 只覆盖本可执行体，DLL 内的分配不进钩子";                                                            \
        }                                                                                                                                                                          \
    } while (false)
