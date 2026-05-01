/**
 * @file EpollAwaiter.h
 * @brief 将文件描述符注册到边沿触发的 epoll 并挂起直到就绪的等待器（Awaitable）
 * @copyright Copyright (c) 2026
 */

#ifndef CORE_EPOLLWAITER_H
#define CORE_EPOLLWAITER_H

#include "Epoll.h"

#include <coroutine>

namespace Core
{
    /**
     * @brief epoll 等待器，用于协程中异步等待 I/O 事件
     *
     * 实现 C++20 协程的 awaitable 接口，将当前 fd 以边沿触发（EPOLLET）模式注册到 epoll，
     * 当指定事件发生时恢复协程，并在恢复后自动从 epoll 中移除该 fd。
     *
     * RAII 安全保障：若协程帧在 await_resume() 之前被销毁（异常、取消等），
     * 析构函数自动从 epoll 中移除 fd 注册，防止悬空指针被 epoll 回调。
     *
     * 典型用法：
     * @code
     * co_await EpollAwaiter(epoll, fd, EPOLLIN);
     * @endcode
     *
     * @note 边沿触发模式要求用户循环读写直到 EAGAIN，否则可能丢失后续事件。
     */
    class EpollAwaiter
    {
    public:
        /**
         * @brief 构造一个针对特定 fd 和事件的等待器
         * @param epoll     epoll 实例引用
         * @param fd        要监听的文件描述符
         * @param eventMask 监听的事件掩码，例如 EPOLLIN、EPOLLOUT 等（内部会自动添加 EPOLLET）
         */
        EpollAwaiter(Epoll &epoll, int fd, uint32_t eventMask) :
            m_epoll(&epoll), m_fd(fd), m_eventMask(eventMask)
        {
        }

        /**
         * @brief RAII 析构：若协程帧被销毁而未正常恢复，自动清理 epoll 注册
         */
        ~EpollAwaiter()
        {
            if (m_registered)
            {
                [[maybe_unused]] auto _ = m_epoll->delFd(m_fd);
            }
        }

        // 禁止拷贝和移动（生命周期严格绑定到协程帧）
        EpollAwaiter(const EpollAwaiter &)            = delete;
        EpollAwaiter &operator=(const EpollAwaiter &) = delete;
        EpollAwaiter(EpollAwaiter &&)                 = delete;
        EpollAwaiter &operator=(EpollAwaiter &&)      = delete;

        /**
         * @brief 决定是否立即恢复（不挂起）
         * @return 始终返回 false，表示总是需要挂起协程，等待事件
         */
        bool await_ready() const noexcept
        {
            return false;
        }

        /**
         * @brief 挂起当前协程，并将 fd 注册到 epoll
         * @param handle 当前协程的句柄，将被保存到 epoll_event.data.ptr 中
         */
        void await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            m_epoll->addFd(m_fd, m_eventMask | EPOLLET, handle.address());
            m_registered = true;
        }

        /**
         * @brief 恢复时清除 epoll 注册，并标记已清理以避免析构函数重复删除
         */
        void await_resume() const noexcept
        {
            m_registered            = false;
            [[maybe_unused]] auto _ = m_epoll->delFd(m_fd);
        }

    private:
        Epoll *      m_epoll;             ///< epoll 实例指针（非拥有，生命周期由外部保证）
        int          m_fd;                ///< 被监听的文件描述符
        uint32_t     m_eventMask;         ///< 要监听的事件掩码（不包含 EPOLLET，内部会自动添加）
        mutable bool m_registered{false}; ///< 是否已注册到 epoll（用于 RAII 析构清理）
    };
}

#endif
