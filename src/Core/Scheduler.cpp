#include "Scheduler.h"

#include <cstdint>
#include <unistd.h>
#include <vector>

namespace Core
{
    void Scheduler::setWakeupFd(const int fd) noexcept
    {
        m_wakeupFd = fd;
    }

    void Scheduler::schedule(const std::coroutine_handle<> handle)
    {
        if (handle)
        {
            m_localQueue.push_back(handle);
        }
    }

    void Scheduler::scheduleRemote(const std::coroutine_handle<> handle)
    {
        if (!handle)
        {
            return;
        }
        {
            std::lock_guard lock(m_globalMutex);
            m_globalQueue.push_back(handle);
            m_globalCount.fetch_add(1, std::memory_order_relaxed);
        }

        if (m_wakeupFd >= 0)
        {
            constexpr uint64_t val = 1;
            // eventfd 写入可能因计数器溢出而失败（EAGAIN），此时目标线程仍会在下次
            // epoll_wait 超时后处理全局队列任务，不会永久丢失
            if (::write(m_wakeupFd, &val, sizeof(val)) < 0)
            {
                // errno 可能为 EAGAIN（计数器满）或 EBADF（fd 已关闭），
                // 两种情况任务均已安全入队，无需额外处理
            }
        }
    }

    bool Scheduler::runOne()
    {
        // 处理本地队列
        if (!m_localQueue.empty())
        {
            const auto handle = m_localQueue.back();
            m_localQueue.pop_back();
            handle.resume();
            return true;
        }

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

        return false;
    }

    void Scheduler::runAll()
    {
        // 第一阶段：排空本地队列
        while (!m_localQueue.empty())
        {
            const auto handle = m_localQueue.back();
            m_localQueue.pop_back();
            handle.resume();
        }

        // 第二阶段：批量窃取全局队列，防止本地任务持续产生导致全局饥饿
        while (true)
        {
            std::vector<std::coroutine_handle<>> batch;
            {
                std::lock_guard lock(m_globalMutex);
                batch.reserve(m_globalQueue.size());
                while (!m_globalQueue.empty())
                {
                    batch.push_back(m_globalQueue.front());
                    m_globalQueue.pop_front();
                }
                m_globalCount.store(0, std::memory_order_relaxed);
            }

            if (batch.empty())
                break;

            for (const auto &handle: batch)
            {
                handle.resume();
            }

            // 批量处理期间可能重新产生本地任务，再次排空
            while (!m_localQueue.empty())
            {
                const auto handle = m_localQueue.back();
                m_localQueue.pop_back();
                handle.resume();
            }
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

        return nullptr;
    }

    bool Scheduler::hasWork() const
    {
        if (!m_localQueue.empty())
        {
            return true;
        }
        return m_globalCount.load(std::memory_order_relaxed) > 0;
    }

    size_t Scheduler::localQueueSize() const
    {
        return m_localQueue.size();
    }

}
