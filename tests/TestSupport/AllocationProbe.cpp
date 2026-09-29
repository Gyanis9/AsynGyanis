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
} // namespace AsynGyanis::TestSupport

namespace
{
    /**
     * @brief 记一次分配：relaxed 足够，这两个数只当读数用，不靠它们同步任何状态
     */
    void recordAllocation(const std::size_t size) noexcept
    {
        using namespace AsynGyanis::TestSupport;
        allocationCount.fetch_add(1U, std::memory_order_relaxed);
        allocationBytes.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
        recordAllocationSize(size);
    }
} // namespace

// 全局替换：本可执行体里所有走 operator new 的分配都过这里（静态链接进来的第三方也一样）。
// 转发给 malloc/free，语义与默认实现一致，只是多记两笔数。
// size 为 0 时按 1 字节申请，保证零字节的分配请求也留下一次可数的记账
[[nodiscard]] void *operator new(const std::size_t size)
{
    recordAllocation(size);
    void *const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

[[nodiscard]] void *operator new[](const std::size_t size)
{
    recordAllocation(size);
    void *const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

[[nodiscard]] void *operator new(const std::size_t size, const std::nothrow_t &) noexcept
{
    recordAllocation(size);
    return std::malloc(size == 0U ? 1U : size);
}

[[nodiscard]] void *operator new[](const std::size_t size, const std::nothrow_t &) noexcept
{
    recordAllocation(size);
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
