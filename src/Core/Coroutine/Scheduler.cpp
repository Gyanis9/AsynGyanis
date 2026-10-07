#include "Core/Coroutine/Scheduler.h"
#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/EventNotifier.h"

#include <exception>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    void Scheduler::setWakeupNotifier(Platform::EventNotifier *const notifier) noexcept
    {
        m_wakeup = notifier;
    }

    void Scheduler::setOwnerLoop(EventLoop *const loop) noexcept
    {
        m_ownerLoop = loop;
    }

    void Scheduler::assertLocalQueueUse(const char *const operation) const
    {
        // 没接线（独立构造的调度器）没有判据可查；接了线而循环没在跑也不查——那正是构造期
        // 与停机后的顺序交接，isOnOwnerThread() 已经把这一档放行
        if (m_ownerLoop == nullptr || m_ownerLoop->isOnOwnerThread())
        {
            return;
        }

        // 违约不静默：本地那两个容器无锁，两条线程同时进来会把彼此的元素打乱，而现场往往
        // 报在离肇因隔着几层的别处（与 Iocp::ExclusiveUse 拒绝并发进后端同一个理由）
        throw Base::LogicException("调度器的本地队列被外来线程使用：操作 " + std::string{operation} +
                                   " 想在这条循环正在跑的时候就地排队，而本地就绪队列无锁、只归跑 run() 的那条线程。"
                                   "外部线程请改走 Scheduler::scheduleRemote() 或 Scheduler::postRemote()");
    }

    void Scheduler::schedule(const std::coroutine_handle<> handle)
    {
        assertLocalQueueUse("Scheduler::schedule()");
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
        assertLocalQueueUse("Scheduler::postLocal()");
        if (!callable)
        {
            return;
        }
        // 本线程独享，不加锁也不唤醒：这段代码本来就跑在所属循环上（上面那一步刚把这句话变成判据）
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
        assertLocalQueueUse("Scheduler::runOne()");
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
                runGuarded(callable);
                return true;
            }
        }

        // 本地待执行代码优先于本地协程：它们多是「为即将运行的协程铺路」的动作
        // （例如定时到期的恢复），先做掉能让紧接着恢复的协程看到收敛后的状态
        if (!m_localCallables.empty())
        {
            std::function<void()> callable = std::move(m_localCallables.front());
            m_localCallables.pop_front();
            runGuarded(callable);
            return true;
        }

        // 处理本地队列
        if (!m_localQueue.empty())
        {
            const auto handle = m_localQueue.back();
            m_localQueue.pop_back();
            runGuarded([handle] { handle.resume(); });
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
            runGuarded([globalHandle] { globalHandle.resume(); });
            return true;
        }

        return false;
    }

    void Scheduler::runAll()
    {
        assertLocalQueueUse("Scheduler::runAll()");
        // 第一阶段：排空本地待执行代码与本地队列。两段都反复回到开头，因为前一段执行期间
        // 可能又投来新的代码（例如定时器在恢复途中又判出新的到期项）
        //
        // 逐条兜住：这一段原先没有守卫，而同函数里第二趟的同一份本地排空（见下面「批量处理期间
        // 可能重新产生本地任务」那段）是兜住的——同一条循环因此对本地投递与跨线程投递给出两种
        // 失败语义。少兜的那一种会让一条抛出的本地投递直接穿出 runAll，落到事件循环的泵上，
        // 而剩余尚未执行的本地代码会连同其持有物一起被静默析构。
        while (true)
        {
            while (!m_localCallables.empty())
            {
                std::function<void()> callable = std::move(m_localCallables.front());
                m_localCallables.pop_front();
                runGuarded(callable);
            }
            if (m_localQueue.empty())
            {
                break;
            }
            const auto handle = m_localQueue.back();
            m_localQueue.pop_back();
            runGuarded([handle] { handle.resume(); });
        }

        // 第二阶段：分批取用全局队列，防止本地任务持续产生导致全局饥饿；每批不超过
        // kMaximumRemoteItemsPerPass 件，取满就把控制权还给调用方（见该常量的说明）。
        // 批处理缓冲提到循环外：跨批次复用已申请的容量，避免每轮都做一次堆分配
        std::vector<std::coroutine_handle<>> batch;
        std::deque<std::function<void()>>    callableBatch;
        while (true)
        {
            std::size_t takenHandleCount   = 0;
            std::size_t takenCallableCount = 0;
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

            // 单个回调/协程抛出不能把整批剩下的丢掉：全部跑完，抛出的那几条就地收下并计数。
            // 直接让异常穿透循环的话，未执行的投递会被静默销毁、那些协程帧永远不会被恢复
            // （调用方按「投了就一定会跑」写代码）。
            //
            // 末尾不再把首个异常重抛出去：runAll() 的调用点是事件循环的泵，重抛等于让一条坏投递
            // 停掉整条循环——与上面第一阶段修掉的是同一个形状。失败的可观测性改由
            // failedDispatchCount() 与首条告警承担，「哪一条投递交了」的语义不受影响
            for (const auto &callable: callableBatch)
            {
                runGuarded(callable);
            }
            callableBatch.clear();

            for (const auto &handle: batch)
            {
                runGuarded([handle] { handle.resume(); });
            }

            // 批量处理期间可能重新产生本地任务，再次排空（含新投来的本地代码，同一处理口径）
            while (true)
            {
                while (!m_localCallables.empty())
                {
                    std::function<void()> callable = std::move(m_localCallables.front());
                    m_localCallables.pop_front();
                    runGuarded(callable);
                }
                if (m_localQueue.empty())
                {
                    break;
                }
                const auto handle = m_localQueue.back();
                m_localQueue.pop_back();
                runGuarded([handle] { handle.resume(); });
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

    std::size_t Scheduler::remotePendingCount() const noexcept
    {
        // 只读两条跨线程队列各自的原子计数：这两笔是投递方在锁外也维护着的，因此本函数不需要
        // m_globalMutex，也就不会与循环取活儿的那一趟抢锁（读到的和至差一件，观测口径可接受）
        return m_globalCount.load(std::memory_order_relaxed) + m_remoteCallableCount.load(std::memory_order_relaxed);
    }

    std::size_t Scheduler::failedDispatchCount() const noexcept
    {
        return m_failedDispatchCount.load(std::memory_order_relaxed);
    }

    void Scheduler::noteDispatchFailure()
    {
        // 计数排在告警之前：一条每次都抛的投递不该因为日志通路出问题而丢账
        const std::size_t ordinal = m_failedDispatchCount.fetch_add(1, std::memory_order_relaxed);
        if (ordinal != 0)
        {
            // 只取上升沿：抛出源可能每批都命中，逐条打等于把日志交给那个坏投递方刷
            return;
        }

        // 本函数只在 catch 块里被调用，因此 current_exception() 就是刚被收下的那一条
        try
        {
            if (const std::exception_ptr failure = std::current_exception(); failure != nullptr)
            {
                std::rethrow_exception(failure);
            }
            LOG_ERROR("Scheduler: 一条投递在执行中抛出异常，已被派发级守卫就地收下、循环继续（非标准异常）。此后同类失败只计数不再逐条告警");
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("Scheduler: 一条投递在执行中抛出异常，已被派发级守卫就地收下、循环继续（首个原因：{}）。此后同类失败只计数不再逐条告警", failure.what());
        } catch (...)
        {
            // 日志通路自己失败时只吞掉这条告警：计数已经落定，把异常重新放回调用栈反而会让它
            // 沿 runAll() 穿到循环的泵上，正是本函数要消灭的那条路径
        }
    }

} // namespace AsynGyanis::Core
