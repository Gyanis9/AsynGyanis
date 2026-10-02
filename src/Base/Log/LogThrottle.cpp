#include "Base/Log/LogThrottle.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>

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

    // ============================================================================
    // 按 key 分档的那份表
    // ============================================================================

    LogThrottleRegistry &LogThrottleRegistry::instance() noexcept
    {
        static LogThrottleRegistry registry;
        return registry;
    }

    LogThrottleDecision LogThrottleRegistry::acquire(const std::string_view key, const std::chrono::milliseconds interval) noexcept
    {
        try
        {
            const std::lock_guard lock(m_mutex);

            if (const auto found = m_index.find(key); found != m_index.end())
            {
                // 先把自己挪到表头（最近触碰）再问闸门：淘汰从表尾下手，正在被用的那条不能顺手被淘掉
                m_touchOrder.splice(m_touchOrder.begin(), m_touchOrder, found->second);
            } else
            {
                if (m_touchOrder.size() >= kMaximumTrackedKeys)
                {
                    auto victim = m_touchOrder.end();
                    --victim;
                    m_index.erase(victim->key);
                    m_touchOrder.erase(victim);
                }
                m_touchOrder.emplace_front(std::string{key}, interval);
                static_cast<void>(m_index.emplace(m_touchOrder.front().key, m_touchOrder.begin()));
            }

            // 新键与老键走同一次判定：LogThrottle 的首次 acquire 必然放行（窗口从这一刻立起来），
            // 而 interval 非正数那条「不压」的口径也不必在这里再复述一遍
            LogThrottle &throttle = m_touchOrder.front().throttle;
            if (!throttle.acquire())
            {
                return {};
            }
            return LogThrottleDecision{.isPassed = true, .droppedCount = throttle.droppedCount()};
        } catch (const std::exception &)
        {
            // 建一条目要分配的只有键文本与一个链表节点。分不出来时放行而不是压掉：压掉的那一条没有任何
            // 地方补记，运维看到的是「这个告警从此没响过」，比多几行日志难查得多（头文件同一口径）
            return LogThrottleDecision{.isPassed = true, .droppedCount = 0U};
        }
    }

    std::size_t LogThrottleRegistry::trackedKeyCount() const noexcept
    {
        const std::lock_guard lock(m_mutex);
        return m_touchOrder.size();
    }

} // namespace AsynGyanis::Base
