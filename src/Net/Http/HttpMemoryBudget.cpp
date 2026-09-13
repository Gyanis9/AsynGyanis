#include "Net/Http/HttpMemoryBudget.h"

namespace AsynGyanis::Net
{
    HttpMemoryBudget::HttpMemoryBudget(const std::size_t maximumTotalBytes) noexcept :
        m_maximumTotalBytes(maximumTotalBytes)
    {
    }

    bool HttpMemoryBudget::tryReserve(const std::size_t byteCount) noexcept
    {
        std::size_t currentBytes = m_reservedBytes.load(std::memory_order_relaxed);

        // 竞争失败只重试不阻塞：预留发生在事件循环线程上，用锁会把所有循环串起来
        for (;;)
        {
            // 用减法而不是 current + byteCount 比较：加法在极端取值下会回绕，
            // 回绕后的「小值」会让一次超限预留被判成通过
            if (m_maximumTotalBytes != 0 && byteCount > m_maximumTotalBytes - currentBytes)
            {
                return false;
            }

            if (m_reservedBytes.compare_exchange_weak(currentBytes, currentBytes + byteCount, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                return true;
            }
        }
    }

    void HttpMemoryBudget::release(const std::size_t byteCount) noexcept
    {
        m_reservedBytes.fetch_sub(byteCount, std::memory_order_acq_rel);
    }

    std::size_t HttpMemoryBudget::reservedByteCount() const noexcept
    {
        return m_reservedBytes.load(std::memory_order_acquire);
    }

    std::size_t HttpMemoryBudget::maximumTotalBytes() const noexcept
    {
        return m_maximumTotalBytes;
    }
} // namespace AsynGyanis::Net
