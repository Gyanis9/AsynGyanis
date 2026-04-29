#include "Scheduler.h"

#include <cstdint>
#include <unistd.h>

namespace Core
{
    void Scheduler::schedule(std::coroutine_handle<> handle)
    {
        if (handle)
        {
            m_localQueue.push_back(handle);
        }
    }

    void Scheduler::setWakeupFd(const int fd) noexcept
    {
        m_wakeupFd = fd;
    }

    void Scheduler::scheduleRemote(const std::coroutine_handle<> handle)
    {
        if (!handle)
            return;
        {
            std::lock_guard lock(m_globalMutex);
            m_globalQueue.push_back(handle);
            m_globalCount.fetch_add(1, std::memory_order_relaxed);
        }

        if (m_wakeupFd >= 0)
        {
            constexpr uint64_t val = 1;
            [[maybe_unused]] auto _ = ::write(m_wakeupFd, &val, sizeof(val));
        }
    }

    bool Scheduler::runOne()
    {
        // 优先处理全局队列中的跨线程任务
        std::coroutine_handle<> globalHandle = nullptr;
        {
            std::lock_guard lock(m_globalMutex);
            if (!m_globalQueue.empty())
            {
                globalHandle = m_globalQueue.front();
                m_globalQueue.pop_front();
                m_globalCount.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        if (globalHandle)
        {
            globalHandle.resume();
            return true;
        }

        // 处理本地队列
        if (!m_localQueue.empty())
        {
            const auto handle = m_localQueue.back();
            m_localQueue.pop_back();
            handle.resume();
            return true;
        }

        return false;
    }

    void Scheduler::runAll()
    {
        while (hasWork())
        {
            runOne();
        }
    }

    std::coroutine_handle<> Scheduler::stealFrom(Scheduler &other)
    {
        // 从其他 Scheduler 的全局队列窃取
        {
            std::lock_guard lock(other.m_globalMutex);
            if (!other.m_globalQueue.empty())
            {
                const auto handle = other.m_globalQueue.front();
                other.m_globalQueue.pop_front();
                other.m_globalCount.fetch_sub(1, std::memory_order_relaxed);
                return handle;
            }
        }

        // 从其他 Scheduler 的本地队列窃取（后半部分）
        if (other.m_localQueue.size() > 1)
        {
            // 窃取前半部分（FIFO 公平性更好）
            if (const size_t stealCount = other.m_localQueue.size() / 2; stealCount > 0)
            {
                const auto handle = other.m_localQueue[0];
                other.m_localQueue.erase(other.m_localQueue.begin());
                return handle;
            }
        }

        return nullptr;
    }

    bool Scheduler::hasWork() const
    {
        if (!m_localQueue.empty())
            return true;
        return m_globalCount.load(std::memory_order_relaxed) > 0;
    }

    size_t Scheduler::localQueueSize() const
    {
        return m_localQueue.size();
    }

}
