#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/EventLoop/TimerQueue.h"

namespace AsynGyanis::Core
{
    EventLoop::EventLoop() :
        m_timerQueue(*this)
    {
        // 唤醒描述符挂载一个固定哨兵指针：run() 靠 data.ptr 是否等于它来区分
        //「唤醒通知」与「IoWatcher 的 I/O 事件」，因此两者不能共用同一个用户数据槽
        m_scheduler.setWakeupNotifier(&m_wakeup);
        m_epoll.addFileDescriptor(m_wakeup.readDescriptor(), EPOLLIN, &m_wakeupSentinel);
    }

    EventLoop::~EventLoop()
    {
        if (m_running.load(std::memory_order_acquire))
            stop();

        // 成员销毁顺序是 m_wakeup 早于 m_epoll，若不先摘除注册，唤醒 socket 会在仍属于
        // epoll 集合时被 closesocket，wepoll 内部线程会继续访问这个失效句柄并破坏堆
        m_epoll.delFileDescriptor(m_wakeup.readDescriptor());
    }

    void EventLoop::run()
    {
        // 这里刻意**不**清除 m_stopRequested，即停止请求是粘性的：它一旦被置位，
        // 之后每次 run() 都会立刻返回。原因不是忘了重置，而是这样才保证
        // 「stop() 先于 run() 到达」不会被丢掉——start() 之后立刻 stop() 是常见写法，
        // 若在 run() 开头清除标志，那次停止请求就会被吞掉，工作线程将永远阻塞在
        // epoll_wait 上，join 随之卡死（实测过）。需要重新运行请新建 EventLoop 实例
        m_running.store(true, std::memory_order_release);

        while (true)
        {
            // 投进来的可调用体/协程抛异常时不能让异常无声地逃出去：本函数通常跑在线程入口上，
            // 逃出去就是 std::terminate（整个进程带走），而且末尾的 m_running 复位会被跳过，
            // isRunning() 永远停在 true。这里就地记一条 ERROR 并复位状态，然后**照旧重抛**——
            // 失败语义不变（快速失败，由 WorkerSupervisor 重启 worker），但现场有据可查；
            // 自行捕获 run() 的调用方也不会再看到一个「仍在运行」的假状态
            try
            {
                m_scheduler.runAll();

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                // 有就绪协程时用 0 超时轮询，否则无限阻塞等待 epoll 事件
                const int timeoutMs = m_scheduler.hasWork() ? 0 : -1;
                for (auto events = m_epoll.wait(timeoutMs); const auto &ev: events)
                {
                    if (ev.data.ptr == &m_wakeupSentinel)
                    {
                        m_wakeup.drain();
                        continue;
                    }

                    if (ev.data.ptr)
                    {
                        // 挂载在 data.ptr 上的只可能是唤醒哨兵或某个 IoWatcher 的地址：
                        // 常驻注册写进去的是注册对象自己的地址，因此这里把事件交给它分发
                        //（它再决定是恢复等待中的协程，还是把就绪记下来留给下一次等待）
                        static_cast<IoWatcher *>(ev.data.ptr)->handleEvents(ev.events);
                    }
                }

                m_scheduler.runAll();
            } catch (const std::exception &loopError)
            {
                LOG_ERROR_FMT("EventLoop: 事件循环里逃出的异常已就地收口（循环停止）：{}", loopError.what());
                m_running.store(false, std::memory_order_release);
                throw;
            } catch (...)
            {
                LOG_ERROR_FMT("EventLoop: 事件循环里逃出的非标准异常已就地收口（循环停止）");
                m_running.store(false, std::memory_order_release);
                throw;
            }
        }

        m_running.store(false, std::memory_order_release);
    }

    void EventLoop::stop()
    {
        // 必须先置位再唤醒：唤醒只负责让阻塞中的 epoll_wait 立刻返回并重读标志。
        // 若顺序反过来，工作线程可能在标志写入前被唤醒并重新阻塞，而 stop() 不会再有
        // 第二次唤醒，run() 便永远等不到停止请求；release 语义则保证这次写入
        // 对随唤醒而恢复的线程可见，不会被重排到 notify 之后
        m_stopRequested.store(true, std::memory_order_release);
        wake();
    }

    void EventLoop::wake() const
    {
        m_wakeup.notify();
    }

    Epoll &EventLoop::epoll() noexcept
    {
        return m_epoll;
    }

    Scheduler &EventLoop::scheduler() noexcept
    {
        return m_scheduler;
    }

    TimerQueue &EventLoop::timerQueue() noexcept
    {
        return m_timerQueue;
    }

    bool EventLoop::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

}
