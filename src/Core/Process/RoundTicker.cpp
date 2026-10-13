#include "Core/Process/RoundTicker.h"

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include <mutex>
#include <utility>

namespace AsynGyanis::Core
{
    RoundTicker::RoundTicker(ThreadPool &pool, RoundCallback onRoundComplete) noexcept : m_pool(pool), m_onRoundComplete(std::move(onRoundComplete))
    {
    }

    RoundTicker::~RoundTicker()
    {
        // 拆的是拍协程的帧：调用方必须已经 stop() 过线程池（工作线程已 join），否则这里销毁的
        // 是一帧可能正被恢复的协程——Task 的析构是无条件 destroy()，没有「已投递未派发」的窗口保护
    }

    bool RoundTicker::completesRound(const std::vector<std::uint64_t> &progresses, const std::size_t index) noexcept
    {
        // 下标越界只可能是编号表被改坏（那条循环根本不存在）。这一格取「不回调」而不是抛：
        // 在一个认不出自己有几条循环的状态下把一轮算齐，报出去的是「整个运行时还在推进」这个假证
        if (index >= progresses.size())
        {
            return false;
        }

        // 本轮的编号就是「我刚走完第几拍」：每条循环都要至少走到这一拍，整轮才算齐
        const std::uint64_t round = progresses[index];
        for (const std::uint64_t progressed: progresses)
        {
            if (progressed < round)
            {
                return false;
            }
        }
        return true;
    }

    bool RoundTicker::arm(const std::chrono::milliseconds interval)
    {
        {
            // 重复调用是空操作：两条节拍会把同一轮回调两次，而「这条通道走过几轮」那笔账
            // 正是持有者与运维要看的
            const std::lock_guard lock(m_stateMutex);
            if (m_isArmed.load(std::memory_order_acquire))
            {
                return true;
            }
        }

        const std::size_t loopCount = m_pool.threadCount();
        {
            const std::lock_guard lock(m_stateMutex);
            m_progresses.assign(loopCount, 0);
            m_intervalMicroseconds.store(std::chrono::duration_cast<std::chrono::microseconds>(interval).count(), std::memory_order_relaxed);
        }

        // 先入表、后排度：交出去的是裸句柄，而本对象是这些帧唯一的持有者，
        // 入表这一步抛出就先留下一个没人持有却能被打发的句柄（见 Scheduler 那条帧归属注释）
        m_tickTasks.reserve(loopCount);
        for (std::size_t index = 0; index < loopCount; ++index)
        {
            m_tickTasks.emplace_back(tickLoop(index));
        }
        for (std::size_t index = 0; index < loopCount; ++index)
        {
            // 排度走 scheduleRemote()：arm() 常在主线程上调用，而那些循环正在别处跑，
            // 本地排队会被当场拒掉（Scheduler::schedule() 的线程判据）
            m_pool.scheduler(index).scheduleRemote(m_tickTasks[index].handle());
        }

        // 标记排在最后：拍数表、间隔与那批帧都落好了才对外说「已挂上」，
        // 别的线程读到 isArmed() 为真时不会撞上一个还在半路的装配现场
        m_isArmed.store(true, std::memory_order_release);
        return true;
    }

    Task<> RoundTicker::tickLoop(const std::size_t index)
    {
        // 本协程只碰自己那条循环的定时器：Timer 与它的等待器都按循环归属，跨循环用就是数据竞态
        Timer timer{m_pool.eventLoop(index)};

        // 向下取整：节拍只会比折出的值更早到，不会更晚——看门狗类的心跳怕的是晚到，早一毫秒不构成
        // 任何后果。窗口折不出至少 1 毫秒的节拍在 Platform 那一层就已经被拒了，这里不会睡成 0（那是忙等）
        const std::chrono::milliseconds interval =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds(m_intervalMicroseconds.load(std::memory_order_relaxed)));

        while (true)
        {
            co_await timer.waitFor(interval);

            {
                const std::lock_guard lock(m_stateMutex);
                ++m_progresses[index];
                // 醒来的这一刻这条循环确实在推进；只有把最后那一拍补齐的那条循环才回调，
                // 于是每轮恰好一次，而任何一条停摆都会让这一轮永远凑不齐
                if (completesRound(m_progresses, index))
                {
                    m_completedRoundCount.fetch_add(1, std::memory_order_relaxed);
                    if (m_onRoundComplete)
                    {
                        m_onRoundComplete();
                    }
                }
            }
        }
    }

    bool RoundTicker::isArmed() const noexcept
    {
        return m_isArmed.load(std::memory_order_acquire);
    }

    std::uint64_t RoundTicker::completedRoundCount() const noexcept
    {
        return m_completedRoundCount.load(std::memory_order_relaxed);
    }

    std::size_t RoundTicker::armedLoopCount() const noexcept
    {
        // 未挂上时那张表还是空的；挂上后它的条数就是「一轮要凑齐几条」
        return m_tickTasks.size();
    }
} // namespace AsynGyanis::Core
