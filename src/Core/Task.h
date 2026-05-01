/**
 * @file Task.h
 * @brief 协程返回类型 Task<T>，定义 promise_type 并集成调度器
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TASK_H
#define CORE_TASK_H

#include "CoroutinePool.h"

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace Core
{
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

    /**
     * @brief 协程最终挂起点，负责将 continuation（等待该协程的父协程）恢复执行。
     *
     * 该等待体在协程即将结束时被调用，通过返回父协程的句柄来实现对称转移，
     * 避免不必要的递归恢复。
     */
    struct FinalAwaiter
    {
        /**
         * @brief 永远返回 false，协程终结点必须挂起。
         * @return false
         */
        bool await_ready() noexcept
        {
            return false;
        }

        /**
         * @brief 协程挂起时，返回需要恢复的协程句柄（即 continuation）。
         * @tparam Promise 协程 promise 类型
         * @param handle 当前协程的句柄
         * @return 需要恢复的协程句柄，若不存在则返回 noop_coroutine
         */
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

        /**
         * @brief 恢复后无操作（协程已结束）。
         */
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

        /**
         * @brief 从协程句柄构造 Task 对象。
         * @param h 协程句柄
         */
        explicit Task(Handle h) :
            m_handle(h)
        {
        }

        Task(const Task &)            = delete;
        Task &operator=(const Task &) = delete;

        /**
         * @brief 移动构造函数。
         * @param other 要移动的 Task 对象
         */
        Task(Task &&other) noexcept :
            m_handle(std::exchange(other.m_handle, nullptr))
        {
        }

        /**
         * @brief 移动赋值运算符。
         * @param other 要移动的 Task 对象
         * @return 当前对象的引用
         */
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

        /**
         * @brief 析构函数，销毁协程句柄（如果有效）。
         */
        ~Task()
        {
            if (m_handle)
                m_handle.destroy();
        }

        // ========================================================================
        // Promise 类型
        // ========================================================================

        /**
         * @brief Task 的 promise_type 结构，用于协程内部状态管理。
         */
        struct promise_type
        {
            /**
             * @brief 从线程局部 CoroutinePool 分配协程帧内存。
             *
             * 若请求大小超过池的块大小，自动回退到全局 ::operator new。
             * 这样确保大协程帧也能正确分配。
             */
            static void *operator new(const size_t size)
            {
                return CoroutinePool::instance().allocate(size);
            }

            /**
             * @brief 将协程帧内存归还给线程局部 CoroutinePool。
             *
             * 若指针不属于当前线程的池（跨线程析构场景），
             * 自动回退到全局 ::operator delete 保证安全。
             */
            static void operator delete(void *const ptr, const size_t size) noexcept
            {
                CoroutinePool::instance().deallocate(ptr, size);
            }

            /**
             * @brief 创建协程的返回对象（Task）。
             * @return Task 对象，持有当前 promise 对应的句柄
             */
            Task get_return_object()
            {
                return Task(Handle::from_promise(*this));
            }

            /**
             * @brief 初始挂起点：协程创建后立即挂起（惰性启动）。
             * @return std::suspend_always
             */
            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            /**
             * @brief 最终挂起点：协程执行完毕后挂起，通过 FinalAwaiter 调度父协程。
             * @return FinalAwaiter
             */
            FinalAwaiter final_suspend() noexcept
            {
                return {};
            }

            /**
             * @brief 处理协程中未捕获的异常。
             */
            void unhandled_exception()
            {
                m_exception = std::current_exception();
            }

            /**
             * @brief 保存协程返回值（非 void 版本）。
             * @tparam U 返回值类型（应可转换为 T）
             * @param value 返回值
             */
            template<typename U>
            void return_value(U &&value)
            {
                m_value = std::forward<U>(value);
            }

            /**
             * @brief 获取协程的结果（值或异常）。
             * @return T 类型的值
             * @throw 如果协程抛出异常，则重新抛出
             */
            T result()
            {
                if (m_exception)
                    std::rethrow_exception(m_exception);
                return std::move(*m_value);
            }

            std::optional<T>        m_value;                 ///< 协程的返回值（若存在）
            std::exception_ptr      m_exception;             ///< 协程中发生的异常（若有）
            std::coroutine_handle<> m_continuation{nullptr}; ///< 等待该协程的父协程句柄
        };

        // ========================================================================
        // Awaitable 接口（支持嵌套 co_await Task<T>）
        // ========================================================================

        /**
         * @brief 判断协程是否已经完成，若完成则无需挂起。
         * @return true 表示协程已完成，可直接获取结果；false 表示需要挂起等待
         */
        bool await_ready() const noexcept
        {
            return m_handle.done();
        }

        /**
         * @brief 挂起当前协程，并将当前协程的 continuation 保存到被等待的 Task 中，
         *        然后返回被等待的协程句柄以实现对称转移。
         * @param continuation 等待当前协程的父协程句柄
         * @return 需要恢复的协程句柄（即被等待的 Task 的内部协程）
         */
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            m_handle.promise().m_continuation = continuation;
            return m_handle;
        }

        /**
         * @brief 协程恢复后获取等待结果。
         * @return 协程的返回值
         */
        T await_resume()
        {
            return m_handle.promise().result();
        }

        // ========================================================================
        // 查询
        // ========================================================================

        /**
         * @brief 检查协程是否已经完成执行。
         * @return true 表示协程已完成；false 表示尚未完成
         */
        [[nodiscard]] bool isReady() const noexcept
        {
            return m_handle.done();
        }

        /**
         * @brief 获取内部的协程句柄。
         * @return 协程句柄
         */
        Handle handle() const noexcept
        {
            return m_handle;
        }

    private:
        Handle m_handle; ///< 协程句柄
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

        /**
         * @brief 从协程句柄构造 Task<void> 对象。
         * @param h 协程句柄
         */
        explicit Task(const Handle h) :
            m_handle(h)
        {
        }

        Task(const Task &)            = delete;
        Task &operator=(const Task &) = delete;

        /**
         * @brief 移动构造函数。
         * @param other 要移动的 Task 对象
         */
        Task(Task &&other) noexcept :
            m_handle(std::exchange(other.m_handle, nullptr))
        {
        }

        /**
         * @brief 移动赋值运算符。
         * @param other 要移动的 Task 对象
         * @return 当前对象的引用
         */
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

        /**
         * @brief 析构函数，销毁协程句柄（如果有效）。
         */
        ~Task()
        {
            if (m_handle)
                m_handle.destroy();
        }

        /**
         * @brief Task<void> 的 promise_type 特化。
         */
        struct promise_type
        {
            /**
             * @brief 从线程局部 CoroutinePool 分配协程帧内存。
             */
            static void *operator new(const size_t size)
            {
                return CoroutinePool::instance().allocate(size);
            }

            /**
             * @brief 将协程帧内存归还给线程局部 CoroutinePool。
             */
            static void operator delete(void *const ptr, const size_t size) noexcept
            {
                CoroutinePool::instance().deallocate(ptr, size);
            }

            /**
             * @brief 创建协程的返回对象（Task<void>）。
             * @return Task<void> 对象
             */
            Task get_return_object()
            {
                return Task(Handle::from_promise(*this));
            }

            /**
             * @brief 初始挂起点：协程创建后立即挂起。
             * @return std::suspend_always
             */
            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            /**
             * @brief 最终挂起点：通过 FinalAwaiter 调度父协程。
             * @return FinalAwaiter
             */
            FinalAwaiter final_suspend() noexcept
            {
                return {};
            }

            /**
             * @brief 处理协程中未捕获的异常。
             */
            void unhandled_exception()
            {
                m_exception = std::current_exception();
            }

            /**
             * @brief void 返回值，无动作。
             */
            void return_void()
            {
            }

            /**
             * @brief 获取协程的结果（检查异常）。
             * @throw 如果协程抛出异常，则重新抛出
             */
            void result()
            {
                if (m_exception)
                    std::rethrow_exception(m_exception);
            }

            std::exception_ptr      m_exception;             ///< 协程中发生的异常（若有）
            std::coroutine_handle<> m_continuation{nullptr}; ///< 等待该协程的父协程句柄
        };

        /**
         * @brief 判断协程是否已经完成。
         * @return true 表示已完成；false 表示尚未完成
         */
        bool await_ready() const noexcept
        {
            return m_handle.done();
        }

        /**
         * @brief 挂起当前协程并保存 continuation，返回被等待的协程句柄。
         * @param continuation 等待当前协程的父协程句柄
         * @return 需要恢复的协程句柄
         */
        std::coroutine_handle<> await_suspend(const std::coroutine_handle<> continuation) const noexcept
        {
            m_handle.promise().m_continuation = continuation;
            return m_handle;
        }

        /**
         * @brief 恢复后获取结果（可能抛出异常）。
         */
        void await_resume() const
        {
            m_handle.promise().result();
        }

        /**
         * @brief 检查协程是否已经完成执行。
         * @return true 表示协程已完成；false 表示尚未完成
         */
        [[nodiscard]] bool isReady() const noexcept
        {
            return m_handle.done();
        }

        /**
         * @brief 获取内部的协程句柄。
         * @return 协程句柄
         */
        Handle handle() const noexcept
        {
            return m_handle;
        }

    private:
        Handle m_handle; ///< 协程句柄
    };

}

#endif
