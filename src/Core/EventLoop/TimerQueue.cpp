#include "Core/EventLoop/TimerQueue.h"

#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 到期时长换算成描述符需要的毫秒数；非正数（已到期）按 1ms 处理，避免被当成「解除武装」
        std::chrono::milliseconds toArmedDuration(const std::chrono::steady_clock::time_point deadline) noexcept
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            return remaining.count() > 0 ? remaining : std::chrono::milliseconds(1);
        }
    } // namespace

    bool TimerQueue::isLaterThan(const Awaiter *const left, const Awaiter *const right) noexcept
    {
        return left->m_deadline > right->m_deadline;
    }

    TimerQueue::Awaiter::Awaiter(TimerQueue &queue, const std::chrono::milliseconds duration) noexcept :
        m_queue(&queue),
        m_deadline(std::chrono::steady_clock::now() + std::max(duration, std::chrono::milliseconds::zero()))
    {
    }

    TimerQueue::Awaiter::~Awaiter()
    {
        // 只有真正登记过的等待器才需要动队列；队列若已先一步销毁，这里也必须是安全的
        if (m_isQueued)
        {
            m_queue->remove(*this);
        }
    }

    bool TimerQueue::Awaiter::await_ready() const noexcept
    {
        return false;
    }

    void TimerQueue::Awaiter::await_suspend(const std::coroutine_handle<> handle)
    {
        m_handle = handle;
        m_queue->insert(*this);
    }

    void TimerQueue::Awaiter::await_resume() const noexcept
    {
    }

    TimerQueue::TimerQueue(EventLoop &loop) :
        m_loop(loop), m_timer(), m_watcher(loop, m_timer.fileDescriptor()), m_driverTask(drive())
    {
        if (m_timer.fileDescriptor() < 0)
        {
            // 定时器描述符建不起来说明本平台不支持该机制（Linux 上为 timerfd）：
            // 这是不可恢复的启动期故障，带着 errno 抛出便于定位
            throw Base::SystemException("创建定时器描述符失败");
        }
        // 驱动协程在这里只是被创建（惰性协程，还没跑），投递发生在第一次登记定时等待时：
        // 没用过定时器的循环因此不会被塞进一个常驻任务，新建的循环仍是「零待办」
    }

    TimerQueue::~TimerQueue()
    {
        // 堆里残留的等待者先标记为「已不在队列里」：它们的协程帧由各自的持有者销毁，
        // 析构时不能再回来访问正在析构的本对象
        for (Awaiter *const awaiter: m_heap)
        {
            awaiter->m_isQueued = false;
        }
        m_heap.clear();
        m_armedDeadline.reset();

        // 成员析构顺序（逆声明序）：驱动协程帧 → 描述符注册 → 描述符。
        // 驱动帧销毁时它的等待器会自行摘除登记，注册对象析构时不再有等待者
    }

    TimerQueue::Awaiter TimerQueue::waitFor(const std::chrono::milliseconds duration)
    {
        return Awaiter(*this, duration);
    }

    std::size_t TimerQueue::pendingCount() const noexcept
    {
        return m_heap.size();
    }

    Task<> TimerQueue::drive()
    {
        while (true)
        {
            rearm();

            // 注册失效（队列或循环在收尾）时以「未就绪」结束，驱动随之退出。
            // 标志位一并复位：不清的话后续登记会以为驱动还在，而它已经退出，等待者永远等不到人叫醒
            if (!co_await m_watcher.waitReadable())
            {
                m_isDriverStarted = false;
                co_return;
            }

            // 读走过期计数，并如实记账：本次上报已经消耗掉上一次的武装
            m_timer.drain();
            m_armedDeadline.reset();

            dispatchExpired();
        }
    }

    void TimerQueue::insert(Awaiter &awaiter)
    {
        // 第一次有人等定时器才把驱动协程投出去：它跑到「等定时器可读」后长期挂在那里，
        // 此后每个定时等待都只是往堆里插一项（并由 rearm 决定要不要改内核的截止时间）
        if (!m_isDriverStarted)
        {
            m_isDriverStarted = true;
            m_loop.scheduler().schedule(m_driverTask.handle());
        }

        m_heap.push_back(&awaiter);
        std::push_heap(m_heap.begin(), m_heap.end(), &TimerQueue::isLaterThan);
        awaiter.m_isQueued = true;

        // 新登记的截止时间可能比已武装的更早（驱动正等着一个更晚的时刻）：重武装让内核
        // 改到最近的截止时间；否则什么都不做，一次系统调用都不付
        rearm();
    }

    void TimerQueue::remove(Awaiter &awaiter) noexcept
    {
        awaiter.m_isQueued = false;

        // 取消是 O(n)：定位后整堆重建。它只在等待器被提前销毁（连接被强关、协程被取消）时
        // 发生；正常路径（到期取堆顶）是 O(log n) 且不扫描
        const auto position = std::find(m_heap.begin(), m_heap.end(), &awaiter);
        if (position == m_heap.end())
        {
            return;
        }
        m_heap.erase(position);
        std::make_heap(m_heap.begin(), m_heap.end(), &TimerQueue::isLaterThan);
        rearm();
    }

    void TimerQueue::rearm() noexcept
    {
        if (m_heap.empty())
        {
            if (!m_armedDeadline.has_value())
            {
                return;
            }
            m_timer.cancel();
            m_armedDeadline.reset();
            return;
        }

        const auto nearest = m_heap.front()->m_deadline;
        if (m_armedDeadline.has_value() && *m_armedDeadline == nearest)
        {
            return;
        }
        m_timer.arm(toArmedDuration(nearest));
        m_armedDeadline = nearest;
    }

    void TimerQueue::dispatchExpired()
    {
        const auto now = std::chrono::steady_clock::now();
        while (!m_heap.empty() && m_heap.front()->m_deadline <= now)
        {
            Awaiter *const awaiter = m_heap.front();
            std::pop_heap(m_heap.begin(), m_heap.end(), &TimerQueue::isLaterThan);
            m_heap.pop_back();
            awaiter->m_isQueued = false;

            if (awaiter->m_handle)
            {
                // 投回调度器而不是就地恢复：本协程还在推进队列，就地恢复会让别的协程在
                // 队列状态尚未收敛时插进来；这也与事件分发「取回事件后再统一跑」的既有约定一致
                m_loop.scheduler().schedule(std::exchange(awaiter->m_handle, nullptr));
            }
        }
        rearm();
    }

} // namespace AsynGyanis::Core
