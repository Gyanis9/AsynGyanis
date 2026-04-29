/**
 * @file Task.h
 * @brief 协程返回类型 Task<T>，定义 promise_type 并集成调度器
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TASK_H
#define CORE_TASK_H

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace Core
{
    // 前向声明
    class Scheduler;

    // ============================================================================
    // Task<T> — 协程返回类型
    // ============================================================================

    /**
     * @brief 协程返回类型。
     *
     * 惰性启动（initial_suspend → suspend_always），
     * 完成后通过 FinalAwaiter 将协程句柄推入 Scheduler。
     *
     * @tparam T 协程返回值类型
     */
    template<typename T = void>
    class Task;

    // ============================================================================
    // FinalAwaiter — 协程结束时将 continuation 推入调度器
    // ============================================================================

    struct FinalAwaiter
    {
        bool await_ready() noexcept
        {
            return false;
        }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept
        {
            // 恢复 continuation（调用方在 co_await 时挂起的协程）
            if (auto &promise = handle.promise(); promise.m_continuation)
            {
                return promise.m_continuation;
            }
            return std::noop_coroutine();
        }

        void await_resume() noexcept
        {
        }
    };

    // ============================================================================
    // Task<T> 实现
    // ============================================================================

    template<typename T>
    class Task
    {
    public:
        struct promise_type;

        using Handle = std::coroutine_handle<promise_type>;

        explicit Task(Handle h) :
            m_handle(h)
        {
        }

        Task(const Task &)            = delete;
        Task &operator=(const Task &) = delete;

        Task(Task &&other) noexcept :
            m_handle(std::exchange(other.m_handle, nullptr))
        {
        }

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other)
            {
                if (m_handle)
                    m_handle.destroy();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        ~Task()
        {
            if (m_handle)
                m_handle.destroy();
        }

        // ========================================================================
        // Promise 类型
        // ========================================================================

        struct promise_type
        {
            Task get_return_object()
            {
                return Task(Handle::from_promise(*this));
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            FinalAwaiter final_suspend() noexcept
            {
                return {};
            }

            void unhandled_exception()
            {
                m_exception = std::current_exception();
            }

            template<typename U>
            void return_value(U &&value)
            {
                m_value = std::forward<U>(value);
            }

            T result()
            {
                if (m_exception)
                    std::rethrow_exception(m_exception);
                return std::move(*m_value);
            }

            std::optional<T>        m_value;
            std::exception_ptr      m_exception;
            std::coroutine_handle<> m_continuation{nullptr};
        };

        // ========================================================================
        // Awaitable 接口（支持嵌套 co_await Task<T>）
        // ========================================================================

        bool await_ready() const noexcept
        {
            return m_handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            m_handle.promise().m_continuation = continuation;
            return m_handle;
        }

        T await_resume()
        {
            return m_handle.promise().result();
        }

        // ========================================================================
        // 查询
        // ========================================================================

        [[nodiscard]] bool isReady() const noexcept
        {
            return m_handle.done();
        }

        Handle handle() const noexcept
        {
            return m_handle;
        }

    private:
        Handle m_handle;
    };

    // ============================================================================
    // Task<void> 特化
    // ============================================================================

    template<>
    class Task<void>
    {
    public:
        struct promise_type;

        using Handle = std::coroutine_handle<promise_type>;

        explicit Task(const Handle h) :
            m_handle(h)
        {
        }

        Task(const Task &)            = delete;
        Task &operator=(const Task &) = delete;

        Task(Task &&other) noexcept :
            m_handle(std::exchange(other.m_handle, nullptr))
        {
        }

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other)
            {
                if (m_handle)
                    m_handle.destroy();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        ~Task()
        {
            if (m_handle)
                m_handle.destroy();
        }

        struct promise_type
        {
            Task get_return_object()
            {
                return Task(Handle::from_promise(*this));
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            FinalAwaiter final_suspend() noexcept
            {
                return {};
            }

            void unhandled_exception()
            {
                m_exception = std::current_exception();
            }

            void return_void()
            {
            }

            void result()
            {
                if (m_exception)
                    std::rethrow_exception(m_exception);
            }

            std::exception_ptr      m_exception;
            std::coroutine_handle<> m_continuation{nullptr};
        };

        bool await_ready() const noexcept
        {
            return m_handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            m_handle.promise().m_continuation = continuation;
            return m_handle;
        }

        void await_resume()
        {
            m_handle.promise().result();
        }

        [[nodiscard]] bool isReady() const noexcept
        {
            return m_handle.done();
        }

        Handle handle() const noexcept
        {
            return m_handle;
        }

    private:
        Handle m_handle;
    };

}

#endif
