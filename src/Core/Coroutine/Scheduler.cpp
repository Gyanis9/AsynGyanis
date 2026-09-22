#include "Core/Coroutine/Scheduler.h"
#include "Platform/IO/EventNotifier.h"

#include <exception>
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

    void Scheduler::postLocal(std::function<void()> callable)
    {
        if (!callable)
        {
            return;
        }
        // 本线程独享，不加锁也不唤醒：这段代码本来就跑在所属循环上，排进本轮清空即可
        m_localCallables.push_back(std::move(callable));
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

        // 本地待执行代码优先于本地协程：它们多是「为即将运行的协程铺路」的动作
        //（例如定时到期的恢复），先做掉能让紧接着恢复的协程看到收敛后的状态
        if (!m_localCallables.empty())
        {
            std::function<void()> callable = std::move(m_localCallables.front());
            m_localCallables.pop_front();
            callable();
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
        // 第一阶段：排空本地待执行代码与本地队列。两段都反复回到开头，因为前一段执行期间
        // 可能又投来新的代码（例如定时器在恢复途中又判出新的到期项）
        while (true)
        {
            while (!m_localCallables.empty())
            {
                std::function<void()> callable = std::move(m_localCallables.front());
                m_localCallables.pop_front();
                callable();
            }
            if (m_localQueue.empty())
            {
                break;
            }
            const auto handle = m_localQueue.back();
            m_localQueue.pop_back();
            handle.resume();
        }

        // 第二阶段：分批取用全局队列，防止本地任务持续产生导致全局饥饿；每批不超过
        // kMaximumRemoteItemsPerPass 件，取满就把控制权还给调用方（见该常量的说明）。
        // 批处理缓冲提到循环外：跨批次复用已申请的容量，避免每轮都做一次堆分配
        std::vector<std::coroutine_handle<> > batch;
        std::deque<std::function<void()> >    callableBatch;
        while (true)
        {
            std::size_t takenHandleCount    = 0;
            std::size_t takenCallableCount  = 0;
            {
                const std::lock_guard lock(m_globalMutex);
                batch.clear();
                callableBatch.clear();

                // 回调先取、协程句柄补满剩余额度：与 runOne() 的优先级口径保持一致
                while (callableBatch.size() < kMaximumRemoteItemsPerPass && !m_remoteCallables.empty())
                {
                    callableBatch.push_back(std::move(m_remoteCallables.front()));
                    m_remoteCallables.pop_front();
                }
                takenCallableCount = callableBatch.size();

                const std::size_t remainingCapacity = kMaximumRemoteItemsPerPass - takenCallableCount;
                while (batch.size() < remainingCapacity && !m_globalQueue.empty())
                {
                    batch.push_back(m_globalQueue.front());
                    m_globalQueue.pop_front();
                }
                takenHandleCount = batch.size();

                // 计数按「本批实际取走数」递减而不是清零：还剩着没取的投递必须继续算待办，
                // 否则 hasWork() 会误报空闲、让循环带着积压睡在 epoll 上
                if (takenCallableCount > 0)
                {
                    m_remoteCallableCount.fetch_sub(takenCallableCount, std::memory_order_relaxed);
                }
                if (takenHandleCount > 0)
                {
                    m_globalCount.fetch_sub(takenHandleCount, std::memory_order_relaxed);
                }
            }

            if (batch.empty() && callableBatch.empty())
                break;

            // 单个回调/协程抛出不能把整批剩下的丢掉：先都跑完（只记住第一个异常），
            // 末尾再把异常传播出去。直接让异常穿透循环的话，未执行的投递会被静默销毁、
            // 那些协程帧永远不会被恢复（调用方按「投了就一定会跑」写代码）
            std::exception_ptr firstException;
            for (const auto &callable: callableBatch)
            {
                try
                {
                    callable();
                } catch (...)
                {
                    firstException = firstException ? firstException : std::current_exception();
                }
            }
            callableBatch.clear();

            for (const auto &handle: batch)
            {
                try
                {
                    handle.resume();
                } catch (...)
                {
                    firstException = firstException ? firstException : std::current_exception();
                }
            }

            // 批量处理期间可能重新产生本地任务，再次排空（含新投来的本地代码，同一处理口径）
            while (true)
            {
                while (!m_localCallables.empty())
                {
                    std::function<void()> callable = std::move(m_localCallables.front());
                    m_localCallables.pop_front();
                    try
                    {
                        callable();
                    } catch (...)
                    {
                        firstException = firstException ? firstException : std::current_exception();
                    }
                }
                if (m_localQueue.empty())
                {
                    break;
                }
                const auto handle = m_localQueue.back();
                m_localQueue.pop_back();
                try
                {
                    handle.resume();
                } catch (...)
                {
                    firstException = firstException ? firstException : std::current_exception();
                }
            }

            // 全部跑完之后再传播：异常语义不变（仍向目标循环抛出），但没有任何一条投递被丢掉
            if (firstException)
            {
                std::rethrow_exception(firstException);
            }

            // 这一批已做满上限：把控制权交回调用方，让它有机会去取 IO 事件，剩下的下一趟再取
            if (takenHandleCount + takenCallableCount >= kMaximumRemoteItemsPerPass)
            {
                break;
            }
        }
    }


    bool Scheduler::hasWork() const
    {
        // 待执行的本地代码也算待办：漏掉它会让循环带着「没事可做」的判断阻塞在 epoll 上，
        // 那段代码要等到下一次事件才被取出
        if (!m_localQueue.empty() || !m_localCallables.empty())
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
