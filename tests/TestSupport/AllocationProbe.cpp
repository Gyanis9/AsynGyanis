#include "AllocationProbe.h"

#include <cstdlib>

namespace AsynGyanis::TestSupport
{
    // 判据落在实现文件而不是头里：头里的 constexpr 值会让 MSVC 认定 GTEST_SKIP() 之后的用例体不可达
    // （C4702，本仓库 /WX 下即错误），而运行期常量不会做那个推断。
    // 取的是**本可执行体**链接到的模块形态：五个模块任一以共享库提供，DLL 内的分配就不进这里的钩子。
#if defined(_WIN32) &&                                                                                                                                                             \
        (defined(ASYN_BASE_SHARED_LIB) || defined(ASYN_CORE_SHARED_LIB) || defined(ASYN_NET_SHARED_LIB) || defined(ASYN_DATABASE_SHARED_LIB) || defined(ASYN_PLATFORM_SHARED_LIB))
    const bool kAllocationProbeIsBlind = true;
#else
    const bool kAllocationProbeIsBlind = false;
#endif
} // namespace AsynGyanis::TestSupport

namespace
{
    /// 每桶一个原子量：替换掉的 operator new 会在任意线程被调用，普通数组会撞车
    std::array<std::atomic<std::uint64_t>, AsynGyanis::TestSupport::kAllocationHistogramBucketCount> allocationHistogram{};

    /// 「第 N 次分配失败」的目标：**按线程各算一份**，0 表示本线程没挂
    /// @details 全局计数轴上挂目标会被别的线程偷掉（EventLoop 线程空闲时也会分配），
    ///          那样用例看到的「没掐到」是随机现象。探针的读数本来就只在单线程测量窗口内取
    ///          （见头里的说明），开关跟着同一口径走
    thread_local std::uint64_t threadAllocationCount{0U};
    thread_local std::uint64_t threadFailureTarget{0U};

    /// 实际被掐掉的次数：用例靠它自证注入真的发生过
    std::atomic<std::uint64_t> injectedFailureCount{0U};

    /**
     * @brief 把一次申请按大小归桶
     * @param size 本次申请的字节数
     */
    void recordAllocationSize(const std::size_t size) noexcept
    {
        // 桶下标按 16 字节一档，超出一律落溢出桶：溢出桶 nonzero 就说明有单次申请大得离谱
        const std::size_t bucketIndex = size / AsynGyanis::TestSupport::kAllocationHistogramBucketBytes;
        allocationHistogram[bucketIndex < AsynGyanis::TestSupport::kAllocationHistogramBucketCount ? bucketIndex : AsynGyanis::TestSupport::kAllocationHistogramBucketCount - 1U]
                .fetch_add(1U, std::memory_order_relaxed);
    }

    /**
     * @brief 记一次分配，并判这一次要不要按开关失败
     * @param size 本次申请的字节数
     * @return bool true 正常放行；false 这一次要失败（开关随即解除，只失败一次）
     */
    bool recordAllocation(const std::size_t size) noexcept
    {
        using namespace AsynGyanis::TestSupport;
        allocationCount.fetch_add(1U, std::memory_order_relaxed);
        allocationBytes.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
        recordAllocationSize(size);

        const std::uint64_t thisThreadCount = ++threadAllocationCount;
        // 用完即解：让第 N 次这一次失败，之后的分配恢复正常，
        // 被测体的兜底分支才能继续走它自己的那几步分配
        const std::uint64_t target = threadFailureTarget;
        if (target != 0U && thisThreadCount >= target)
        {
            threadFailureTarget = 0U;
            injectedFailureCount.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
} // namespace

namespace AsynGyanis::TestSupport
{
    std::atomic<std::uint64_t> allocationCount{0};
    std::atomic<std::uint64_t> allocationBytes{0};

    void resetAllocationHistogram() noexcept
    {
        for (auto &bucket: allocationHistogram)
        {
            bucket.store(0U, std::memory_order_relaxed);
        }
    }

    AllocationHistogram snapshotAllocationHistogram() noexcept
    {
        AllocationHistogram snapshot{};
        for (std::size_t bucketIndex = 0; bucketIndex < kAllocationHistogramBucketCount; ++bucketIndex)
        {
            snapshot[bucketIndex] = allocationHistogram[bucketIndex].load(std::memory_order_relaxed);
        }
        return snapshot;
    }

    std::uint64_t injectedAllocationFailureCount() noexcept
    {
        return injectedFailureCount.load(std::memory_order_relaxed);
    }

    void resetInjectedAllocationFailureCount() noexcept
    {
        injectedFailureCount.store(0U, std::memory_order_relaxed);
    }

    AllocationFailureGuard::AllocationFailureGuard(const std::uint64_t failureAfterAllocations) noexcept
    {
        // 目标值落在「本线程自己的分配计数」那条数轴上：挂上时读一次当下计数，加上「第 N 次」。
        // 0 是「没挂」的哨兵，所以 failureAfterAllocations 写 0 时按「下一次分配就失败」处理
        const std::uint64_t zeroBasedN = failureAfterAllocations == 0U ? 1U : failureAfterAllocations;
        m_previousTarget               = threadFailureTarget;
        threadFailureTarget            = threadAllocationCount + zeroBasedN;
    }

    AllocationFailureGuard::~AllocationFailureGuard() noexcept
    {
        // 解除本层并恢复外层：用例中途断言失败退出时，开关不能漏给后面的用例
        threadFailureTarget = m_previousTarget;
    }
} // namespace AsynGyanis::TestSupport

// 全局替换：本可执行体里所有走 operator new 的分配都过这里（静态链接进来的第三方也一样）。
// 转发给 malloc/free，语义与默认实现一致，只是多记两笔数，并听「第 N 次失败」开关一次。
// size 为 0 时按 1 字节申请，保证零字节的分配请求也留下一次可数的记账
[[nodiscard]] void *operator new(const std::size_t size)
{
    if (!recordAllocation(size))
    {
        throw std::bad_alloc();
    }
    void *const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

[[nodiscard]] void *operator new[](const std::size_t size)
{
    if (!recordAllocation(size))
    {
        throw std::bad_alloc();
    }
    void *const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

[[nodiscard]] void *operator new(const std::size_t size, const std::nothrow_t &) noexcept
{
    if (!recordAllocation(size))
    {
        return nullptr; // nothrow 档的契约就是不抛：交回空指针
    }
    return std::malloc(size == 0U ? 1U : size);
}

[[nodiscard]] void *operator new[](const std::size_t size, const std::nothrow_t &) noexcept
{
    if (!recordAllocation(size))
    {
        return nullptr;
    }
    return std::malloc(size == 0U ? 1U : size);
}

void operator delete(void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, const std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, const std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, const std::nothrow_t &) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, const std::nothrow_t &) noexcept
{
    std::free(pointer);
}
