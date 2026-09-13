#include "Core/Coroutine/Scheduler.h"
#include "Platform/IO/EventNotifier.h"

#include <vector>

namespace AsynGyanis::Core
{
    void Scheduler::setWakeupNotifier(Platform::EventNotifier *const notifier) noexcept
    {
        m_wakeup = notifier;
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

        if (m_wakeup)
        {
            // 唤醒可能阻塞在 epoll_wait 中的目标 EventLoop
            // 唤醒失败时目标线程仍会在下次 epoll_wait 超时后处理全局队列任务，不会永久丢失
            m_wakeup->notify();
        }
    }

    void Scheduler::postRemote(std::function<void()> callable)
    {
        if (!callable)
        {
            return;
        }
        {
            std::lock_guard lock(m_globalMutex);
            m_remoteCallables.push_back(std::move(callable));
            m_remoteCallableCount.fetch_add(1, std::memory_order_relaxed);
        }

        if (m_wakeup)
        {
            // 与 scheduleRemote() 同一套保证：唤醒失败也不会丢，目标线程下次从 epoll_wait 醒来时照样处理
            m_wakeup->notify();
        }
    }

    bool Scheduler::runOne()
    {
        // 跨线程投递的普通代码优先跑：它们多是「把刚接下的连接装进本循环」这类前置动作，
        // 先做掉能让紧随其后的读写立刻有对象可服务
        {
            std::function<void()> callable;
            {
                std::lock_guard lock(m_globalMutex);
                if (!m_remoteCallables.empty())
                {
                    callable = std::move(m_remoteCallables.front());
                    m_remoteCallables.pop_front();
                    m_remoteCallableCount.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            if (callable)
            {
                callable();
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

        // 本地队列已空（上面命中时会提前返回）：再处理全局队列中的跨线程任务
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

        // 第二阶段：批量窃取全局队列，防止本地任务持续产生导致全局饥饿。
        // 批处理缓冲提到循环外：跨批次复用已申请的容量，避免每轮都做一次堆分配
        std::vector<std::coroutine_handle<> > batch;
        std::deque<std::function<void()> >    callableBatch;
        while (true)
        {
            {
                std::lock_guard lock(m_globalMutex);
                batch.clear();
                batch.reserve(m_globalQueue.size());
                while (!m_globalQueue.empty())
                {
                    batch.push_back(m_globalQueue.front());
                    m_globalQueue.pop_front();
                }
                m_globalCount.store(0, std::memory_order_relaxed);

                // 回调与协程各自成批取出再执行：执行期间可能又有新投递，下一轮循环会接着处理
                callableBatch.swap(m_remoteCallables);
                m_remoteCallableCount.store(0, std::memory_order_relaxed);
            }

            if (batch.empty() && callableBatch.empty())
                break;

            for (const auto &callable: callableBatch)
            {
                callable();
            }
            callableBatch.clear();

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


    bool Scheduler::hasWork() const
    {
        if (!m_localQueue.empty())
        {
            return true;
        }
        return m_globalCount.load(std::memory_order_relaxed) > 0 || m_remoteCallableCount.load(std::memory_order_relaxed) > 0;
    }

    size_t Scheduler::localQueueSize() const
    {
        return m_localQueue.size();
    }

}
