#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/SystemException.h"

namespace AsynGyanis::Core
{
    EventLoop::EventLoop()
    {
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
        m_running.store(true, std::memory_order_release);

        while (true)
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
                    const auto handle = std::coroutine_handle<>::from_address(ev.data.ptr);
                    m_scheduler.schedule(handle);
                }
            }

            m_scheduler.runAll();
        }

        m_running.store(false, std::memory_order_release);
    }

    void EventLoop::stop()
    {
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

    bool EventLoop::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

}
