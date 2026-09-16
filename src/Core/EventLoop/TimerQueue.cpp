#include "Core/EventLoop/TimerQueue.h"

#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
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
        m_duration(std::max(duration, std::chrono::milliseconds::zero()))
    {
    }

    TimerQueue::Awaiter::~Awaiter()
    {
        // 只有真正登记过的等待器才需要动队列；队列若已先一步销毁（析构里把标记清掉），这里也就什么都不做
        if (m_isQueued)
        {
            m_queue->remove(*this);
        }
        if (m_isPendingResume)
        {
            m_queue->cancelPendingResume(*this);
        }
    }

    bool TimerQueue::Awaiter::await_ready() const noexcept
    {
        return false;
    }

    bool TimerQueue::Awaiter::await_suspend(const std::coroutine_handle<> handle)
    {
        computeDeadline();
        m_handle = handle;
        return m_queue->insert(*this);
    }

    void TimerQueue::Awaiter::await_resume() const noexcept
    {
    }

    void TimerQueue::Awaiter::computeDeadline() noexcept
    {
        const auto now = std::chrono::steady_clock::now();

        // 上限必须在**毫秒域**里算：直接拿时长与「时间点最大值 - 此刻」比较，两侧单位不同，
        // 比较会先把毫秒折成时钟的滴答（Windows 上是 100ns），毫秒量级的大数在这一步就越界了，
        // 比较结果随之失真——正是要防的那种溢出。折到毫秒只做除法，不会溢出
        const auto maximumDuration =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - now);

        // 时长加不进去（例如 milliseconds::max()）时饱和到时间点最大值：宁可等得比要求更久，
        // 也不能因溢出把截止时间算成过去，那会让等待立刻完成
        m_deadline = (m_duration >= maximumDuration) ? std::chrono::steady_clock::time_point::max() : now + m_duration;
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
        // 残留的等待者先标记为「已不在队列里」：它们的协程帧由各自的持有者销毁，
        // 析构时不能再回来访问正在析构的本对象（两种登记形态各清各的标记）
        for (Awaiter *const awaiter: m_heap)
        {
            awaiter->m_isQueued = false;
        }
        for (Awaiter *const awaiter: m_expiredAwaiters)
        {
            awaiter->m_isPendingResume = false;
        }
        m_heap.clear();
        m_expiredAwaiters.clear();
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
            [[maybe_unused]] const bool isArmed = rearm();

            // 注册失效（描述符已坏，或队列与循环在收尾）时以「未就绪」结束，驱动随之退出。
            // 状态必须落到 Dead：只复位成 Idle 的话，后续登记会再把**已经跑完的**驱动帧投一次，
            // 那是对停在终结点上的协程 resume，属未定义行为
            if (!co_await m_watcher.waitReadable())
            {
                m_driverState = DriverState::Dead;
                abandonPendingTimers();
                LOG_WARN_FMT("TimerQueue: 定时器注册已失效（事件循环正在收尾），此后登记定时等待会立即完成");
                co_return;
            }

            // 读走过期计数，并如实记账：本次上报已经消耗掉上一次的武装
            m_timer.drain();
            m_armedDeadline.reset();

            dispatchExpired();
        }
    }

    bool TimerQueue::insert(Awaiter &awaiter)
    {
        // 队列已停摆：再挂起就是永远等不到人的等待，直接告诉调用方「立即完成」
        if (m_driverState == DriverState::Dead)
        {
            return false;
        }

        // 第一次有人等定时器才把驱动协程投出去：它跑到「等定时器可读」后长期挂在那里，
        // 此后每个定时等待都只是往堆里插一项（并由 rearm 决定要不要改内核的截止时间）
        if (m_driverState == DriverState::Idle)
        {
            m_driverState = DriverState::Running;
            m_loop.scheduler().schedule(m_driverTask.handle());
        }

        m_heap.push_back(&awaiter);
        std::push_heap(m_heap.begin(), m_heap.end(), &TimerQueue::isLaterThan);
        awaiter.m_isQueued = true;

        // 新登记的截止时间可能比已武装的更早（驱动正等着一个更晚的时刻）：重武装让内核
        // 改到最近的截止时间；否则什么都不做，一次系统调用都不付
        [[maybe_unused]] const bool isArmed = rearm();
        return true;
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
        [[maybe_unused]] const bool isArmed = rearm();
    }

    bool TimerQueue::rearm() noexcept
    {
        if (m_heap.empty())
        {
            if (!m_armedDeadline.has_value())
            {
                return true;
            }
            m_timer.cancel();
            m_armedDeadline.reset();
            return true;
        }

        const auto nearest = m_heap.front()->m_deadline;
        if (m_armedDeadline.has_value() && *m_armedDeadline == nearest)
        {
            return true;
        }
        if (!m_timer.arm(toArmedDuration(nearest)))
        {
            // 失败时不能记账：记成「已武装」会让同一截止时间从此不再重试，
            // 此后所有定时器都静默失效（等待者挂在那里，没有任何日志说明为什么）
            m_armedDeadline.reset();
            LOG_ERROR_FMT("TimerQueue: 武装定时器描述符失败，最近一个定时（{} 毫秒后到期）不会触发；"
                          "下一次队列变化时会重试",
                          toArmedDuration(nearest).count());
            return false;
        }
        m_armedDeadline = nearest;
        return true;
    }

    void TimerQueue::dispatchExpired()
    {
        const auto now = std::chrono::steady_clock::now();

        // 到期的等待器按截止时间先后收进待恢复表（堆顶即最早，故收集顺序天然升序）
        while (!m_heap.empty() && m_heap.front()->m_deadline <= now)
        {
            Awaiter *const awaiter = m_heap.front();
            std::pop_heap(m_heap.begin(), m_heap.end(), &TimerQueue::isLaterThan);
            m_heap.pop_back();
            awaiter->m_isQueued        = false;
            awaiter->m_isPendingResume = true;
            m_expiredAwaiters.push_back(awaiter);
        }

        [[maybe_unused]] const bool isArmed = rearm();

        // 恢复动作排给调度器，而不是在这里就地恢复、也不把裸句柄投出去：本协程还在推进队列，
        // 就地恢复会让别的协程在队列状态尚未收敛时插进来；而**裸句柄一旦投出去就摘不回来**——
        // 投出之后、恢复之前若等待者的帧被销毁（取消、连接收尾），循环会 resume 一块已释放的帧。
        // 排一段「稍后恢复」的代码就没这个问题：恢复前逐个从待恢复表里取，帧析构时会把表里的
        // 自己摘掉，被销毁的等待者因此根本不会被恢复（与连接池的恢复票据同一目的，见 resumeExpired）
        if (!m_expiredAwaiters.empty())
        {
            m_loop.scheduler().postLocal([this] { resumeExpired(); });
        }
    }

    void TimerQueue::resumeExpired()
    {
        while (!m_expiredAwaiters.empty())
        {
            Awaiter *const awaiter = m_expiredAwaiters.front();
            m_expiredAwaiters.erase(m_expiredAwaiters.begin());
            awaiter->m_isPendingResume = false;

            // 句柄先取走再恢复（与「取回事件后再统一跑」的约定一致）：被恢复的代码可能顺手
            // 销毁表中其它等待器的帧，那些等待器已把自己摘掉，本循环每轮重新取表头即可
            if (const std::coroutine_handle<> handle = std::exchange(awaiter->m_handle, nullptr); handle)
            {
                handle.resume();
            }
        }
    }

    void TimerQueue::cancelPendingResume(Awaiter &awaiter) noexcept
    {
        awaiter.m_isPendingResume = false;

        const auto position = std::find(m_expiredAwaiters.begin(), m_expiredAwaiters.end(), &awaiter);
        if (position != m_expiredAwaiters.end())
        {
            m_expiredAwaiters.erase(position);
        }
    }

    void TimerQueue::abandonPendingTimers() noexcept
    {
        // 队列停摆（描述符坏了或正在收尾）：堆里的等待者再没有谁会叫醒，按既有收尾口径
        // 把它们标记成「已不在队列里」，协程帧由各自的持有者销毁
        for (Awaiter *const awaiter: m_heap)
        {
            awaiter->m_isQueued = false;
        }
        for (Awaiter *const awaiter: m_expiredAwaiters)
        {
            // 待恢复表里的同理不再恢复：排进来的那次 resumeExpired 会看到空表
            awaiter->m_isPendingResume = false;
        }
        m_heap.clear();
        m_expiredAwaiters.clear();
        m_armedDeadline.reset();
    }

} // namespace AsynGyanis::Core
