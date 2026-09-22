#include "AllocationProbe.h"

#include <cstdlib>

namespace AsynGyanis::TestSupport
{
    std::atomic<std::uint64_t> allocationCount{0};
    std::atomic<std::uint64_t> allocationBytes{0};
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
