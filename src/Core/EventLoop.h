/**
 * @file EventLoop.h
 * @brief 每线程事件循环 — epoll + io_uring 混合驱动，协程调度核心
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_EVENTLOOP_H
#define CORE_EVENTLOOP_H

#include "Epoll.h"
#include "Scheduler.h"
#include "Uring.h"

#include <atomic>
#include <memory>

namespace Base
{
    class IOUringContext;
}

namespace Core
{
    class EventLoop
    {
    public:
        explicit EventLoop(unsigned queueDepth = 256, unsigned uringFlags = 0);

        ~EventLoop();

        EventLoop(const EventLoop &)            = delete;
        EventLoop &operator=(const EventLoop &) = delete;

        void run();
        void stop();
        void wake() const;

        [[nodiscard]] Epoll &epoll() noexcept;
        [[nodiscard]] Uring &uring() noexcept;
        [[nodiscard]] Scheduler &scheduler() noexcept;
        [[nodiscard]] bool isRunning() const noexcept;

    private:
        void harvestUringCompletions();

        Epoll                           m_epoll;
        Scheduler                       m_scheduler;
        int                             m_wakeupFd{-1};
        int                             m_uringEventFd{-1};
        int                             m_wakeupSentinel{0};
        int                             m_uringSentinel{0};
        std::unique_ptr<Base::IOUringContext> m_uringCtx;
        Uring                           m_uring;
        std::atomic<bool>               m_running{false};
        std::atomic<bool>               m_stopRequested{false};
    };

} // namespace Core

#endif
