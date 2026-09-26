#include "Net/Http/HttpMemoryBudget.h"

#include "Base/Log/LogMacros.h"

namespace AsynGyanis::Net
{
    HttpMemoryBudget::HttpMemoryBudget(const std::size_t maximumTotalBytes) noexcept : m_maximumTotalBytes(maximumTotalBytes)
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
        std::size_t currentBytes = m_reservedBytes.load(std::memory_order_relaxed);
        for (;;)
        {
            // 归还多于已预留时按 0 收住，绝不回绕：无符号回绕会把账目变成天文数字，
            // tryReserve 里的「上限减当前值」随即跟着回绕，整个预算从「拒绝超限」翻转成
            // 「放行一切」——一个记账错误就地把防线变成放行门，这比账目偏小严重得多
            const std::size_t remainingBytes = currentBytes > byteCount ? currentBytes - byteCount : 0U;
            if (remainingBytes == 0U && currentBytes < byteCount)
            {
                // 只在异常分支告警：正常归还路径不进这里，因此这条日志本身就是缺陷证据
                LOG_WARN_FMT("HttpMemoryBudget: 归还 {} 字节超过当前已预留的 {} 字节，账目按 0 收住；"
                             "这说明某处重复归还或漏记预留，需按调用路径排查",
                             byteCount, currentBytes);
            }

            if (m_reservedBytes.compare_exchange_weak(currentBytes, remainingBytes, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                return;
            }
        }
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
