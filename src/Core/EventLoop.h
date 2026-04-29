/**
 * @file EventLoop.h
 * @brief Per-thread event loop — epoll-driven, coroutine scheduling core
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_EVENTLOOP_H
#define CORE_EVENTLOOP_H

#include "Epoll.h"
#include "Scheduler.h"

#include <atomic>

namespace Core
{
    class EventLoop
    {
    public:
        EventLoop();

        ~EventLoop();

        EventLoop(const EventLoop &)            = delete;
        EventLoop &operator=(const EventLoop &) = delete;

        void run();
        void stop();
        void wake() const;

        [[nodiscard]] Epoll &epoll() noexcept;
        [[nodiscard]] Scheduler &scheduler() noexcept;
        [[nodiscard]] bool isRunning() const noexcept;

    private:
        Epoll             m_epoll;
        Scheduler         m_scheduler;
        int               m_wakeupFd{-1};
        int               m_wakeupSentinel{0};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopRequested{false};
    };

} // namespace Core

#endif
