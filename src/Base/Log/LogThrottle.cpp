#include "Base/Log/LogThrottle.h"

#include <chrono>

namespace AsynGyanis::Base
{
    LogThrottle::LogThrottle(const std::chrono::milliseconds interval) noexcept : m_interval(interval)
    {
    }

    bool LogThrottle::acquire() noexcept
    {
        if (m_interval.count() <= 0)
        {
            return true;
        }

        const std::uint64_t now                    = nowMilliseconds();
        std::uint64_t       nextPassAtMilliseconds = m_nextPassAtMilliseconds.load(std::memory_order_relaxed);
        if (now < nextPassAtMilliseconds ||
            !m_nextPassAtMilliseconds.compare_exchange_strong(nextPassAtMilliseconds, now + static_cast<std::uint64_t>(m_interval.count()), std::memory_order_relaxed))
        {
            // 两条都归「压掉」：还在窗口内，或这一拍被别的线程抢走了（CAS 失败时 nextPassAtMilliseconds
            // 已被写成对方立下的下一个窗口，必然晚于此刻）
            m_droppedSincePass.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        // 先抢到窗口、再交出上一段的条数：中间被抢占时，并发的那一路只会读到上一段的计数，
        // 既不会少一份计数也不会少一条日志
        m_droppedReported.store(m_droppedSincePass.exchange(0U, std::memory_order_relaxed), std::memory_order_relaxed);
        return true;
    }

    std::uint64_t LogThrottle::droppedCount() const noexcept
    {
        return m_droppedReported.load(std::memory_order_relaxed);
    }

    std::uint64_t LogThrottle::nowMilliseconds() noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

} // namespace AsynGyanis::Base
