#include "Scheduler.h"

namespace Core
{
    void Scheduler::schedule(std::coroutine_handle<> handle)
    {
        if (handle)
        {
            m_localQueue.push_back(handle);
        }
    }

    void Scheduler::scheduleRemote(const std::coroutine_handle<> handle)
    {
        if (!handle)
            return;
        std::lock_guard lock(m_globalMutex);
        m_globalQueue.push_back(handle);
    }

    bool Scheduler::runOne()
    {
        // 优先处理全局队列中的跨线程任务
        {
            std::lock_guard lock(m_globalMutex);
            if (!m_globalQueue.empty())
            {
                const auto handle = m_globalQueue.front();
                m_globalQueue.pop_front();
                handle.resume();
                return true;
            }
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
        // NOTE: 不检查 m_globalQueue 以避免不必要的锁竞争
        return false;
    }

    size_t Scheduler::localQueueSize() const
    {
        return m_localQueue.size();
    }

}
