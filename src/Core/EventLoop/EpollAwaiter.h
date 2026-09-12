/**
 * @file EpollAwaiter.h
 * @brief 将文件描述符注册到边沿触发的 epoll 并挂起直到就绪的等待器（Awaitable）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once


#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/Epoll.h"

#include <coroutine>

namespace AsynGyanis::Core
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
         * @param fileDescriptor        要监听的文件描述符
         * @param eventMask 监听的事件掩码，例如 EPOLLIN、EPOLLOUT 等（内部会自动添加 EPOLLET）
         */
        EpollAwaiter(Epoll &epoll, int fileDescriptor, uint32_t eventMask) :
            m_epoll(&epoll), m_fileDescriptor(fileDescriptor), m_eventMask(eventMask)
        {
        }

        /**
         * @brief RAII 析构：若协程帧被销毁而未正常恢复，自动清理 epoll 注册
         */
        ~EpollAwaiter()
        {
            if (m_registered)
            {
                [[maybe_unused]] auto _ = m_epoll->delFileDescriptor(m_fileDescriptor);
            }
        }

        // 禁止拷贝和移动（生命周期严格绑定到协程帧）
        EpollAwaiter(const EpollAwaiter &) = delete;

        EpollAwaiter &operator=(const EpollAwaiter &) = delete;

        EpollAwaiter(EpollAwaiter &&) = delete;

        EpollAwaiter &operator=(EpollAwaiter &&) = delete;

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
         * @throws Base::SystemException 注册失败（同一 fd 已被另一个等待器注册、或 fd 无效）。
         *         **失败必须抛而不是静默挂起**：await_suspend 抛出时协程会在 co_await 处
         *         被恢复并重抛该异常，调用方能就地处理；若在这里吞掉返回值，协程已经挂起
         *         却再无任何事件能唤醒它——表现为整个连接静默卡死，且没有任何错误线索
         */
        void await_suspend(std::coroutine_handle<> handle) const
        {
            if (!m_epoll->addFileDescriptor(m_fileDescriptor, m_eventMask | EPOLLET, handle.address()))
            {
                throw Base::SystemException("把文件描述符注册到 epoll 失败"
                                            "（该 fd 可能已被另一个等待器注册，或不是有效的描述符）");
            }
            // 注册成功才置位：失败路径上没有任何东西需要清理，析构函数据此不再尝试删除
            m_registered = true;
        }

        /**
         * @brief 恢复时清除 epoll 注册，并标记已清理以避免析构函数重复删除
         */
        void await_resume() const noexcept
        {
            m_registered            = false;
            [[maybe_unused]] auto _ = m_epoll->delFileDescriptor(m_fileDescriptor);
        }

    private:
        Epoll *      m_epoll;             ///< epoll 实例指针（非拥有，生命周期由外部保证）
        int          m_fileDescriptor;    ///< 被监听的文件描述符
        uint32_t     m_eventMask;         ///< 要监听的事件掩码（不包含 EPOLLET，内部会自动添加）
        mutable bool m_registered{false}; ///< 是否已注册到 epoll（用于 RAII 析构清理）
    };
}
