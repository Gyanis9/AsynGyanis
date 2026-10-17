/**
 * @file Task.h
 * @brief 协程返回类型 Task<T>，定义 promise_type 并集成调度器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogContext.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/CoroutinePool.h"

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Core
{
    class Scheduler;

    /**
     * @brief 记下「协程里跑出但没人接住」的异常：分离投递的任务唯一的报错出口
     * @details 取抛出物的文本只能重抛一次再接住；非 std::exception 的抛出物不猜类型、只报它存在
     * @param exception 待记录的异常
     */
    inline void reportUnhandledException(const std::exception_ptr &exception) noexcept
    {
        try
        {
            std::rethrow_exception(exception);
        } catch (const std::exception &unhandledException)
        {
            LOG_ERROR_EXCEPTION(unhandledException, "Task: 协程有异常没人接住，已记录并丢弃。原因：{}", unhandledException.what());
        } catch (...)
        {
            LOG_ERROR("Task: 协程抛出了非 std::exception 的抛出物，没人接住，已丢弃");
        }
    }

    // ============================================================================
    // Task<T> — 协程返回类型
    // ============================================================================

    /**
     * @brief 惰性启动的协程返回类型：完成后以对称转移就地恢复等待者，不经过调度器入队
     * @tparam T 协程返回值类型
     */
    template<typename T = void>
    class Task;

    // ============================================================================
    // FinalAwaiter — 协程结束时以对称转移恢复 continuation
    // ============================================================================

    /**
     * @brief 协程最终挂起点：返回 continuation 句柄实现对称转移，避免递归恢复
     */
    struct ASYN_CORE_API FinalAwaiter
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
         * @brief 协程挂起时，返回需要恢复的协程句柄（即 continuation）；没人接手的异常就地记日志
         * @details 分离投递（schedule 后无人 co_await）的协程抛出的异常只会被存进 promise 再没人取，
         *          处置与 std::execution 的 report_exception 一致：终结点上没有等待者就记一条错误。
         *          之后真有人 await 到本任务，异常照样重新抛出（日志可能重复，不丢则不划算）
         * @tparam Promise 协程 promise 类型
         * @param handle 当前协程的句柄
         * @return 需要恢复的协程句柄，若不存在则返回 noop_coroutine
         */
        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept
        {
            auto &promise = handle.promise();
            if (promise.m_exception && !promise.m_continuation)
            {
                reportUnhandledException(promise.m_exception);
            }

            if (promise.m_continuation)
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

    /**
     * @brief 惰性协程的初始挂起点
     *
     * @details 除了「创建后立即挂起」之外，它还在协程体第一次真正跑起来时给 promise 打上
     *          「已启动」标记。这个标记是 Task::await_suspend() 判断「该启动还是该等它结束」
     *          的唯一依据：只靠协程句柄无法区分「停在初始挂起点的惰性协程」与「已经在跑、
     *          只是挂在内部某个等待上的协程」，而两者要采取的动作正好相反。
     */
    struct ASYN_CORE_API InitialSuspendAwaiter
    {
        /**
         * @brief 异步起点必须挂起（惰性启动）。
         * @return false
         */
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        /**
         * @brief 挂起时无动作：协程由显式 resume 或对称转移启动。
         */
        void await_suspend(std::coroutine_handle<>) const noexcept
        {
        }

        /**
         * @brief 协程体开始执行，标记为已启动。
         */
        void await_resume() const noexcept
        {
            *m_isStarted = true;
        }

        bool *m_isStarted{nullptr}; ///< 指向 promise 中的「已启动」标记
    };

    namespace Detail
    {
        /**
         * @brief 把一次 co_await 包成「挂起前保存日志上下文、恢复后重新发布」
         *
         * @details 日志上下文原本是线程局部的：协程挂起后线程回到循环，别的协程会跑在上面；
         *          若不保存/清除，前一个请求的 trace_id 会串到后一个请求的日志上；若不恢复，
         *          协程在别的线程上恢复（或同一线程被别的协程污染过）后就再也找不到自己的上下文。
         *          本等待器在挂起前把当前上下文存进所属协程帧，并在恢复时重新发布——跨挂起点、
         *          跨线程都成立。无上下文时只做两次空 shared_ptr 赋值，零堆分配。
         *
         * @note 只持原始 awaiter 的指针：co_await 操作数的临时量活到整条 full-expression 结束
         *       （含挂起与恢复），这是语言保证的生存期，与编译器把 awaiter 放进协程帧是同一件事
         * @note 对称转移（await_suspend 返回句柄）时**保留**当前上下文：接手的通常是子协程，
         *       它应当继承父协程的上下文；只有交回 noop（等待目标已在跑、本协程真挂起）才清场
         */
        template<typename Awaiter, typename Promise>
        class LogContextAwaiter
        {
        public:
            LogContextAwaiter(Awaiter *awaiter, Promise *promise) noexcept : m_awaiter(awaiter), m_promise(promise)
            {
            }

            [[nodiscard]] bool await_ready()
            {
                return m_awaiter->await_ready();
            }

            template<typename Handle>
            auto await_suspend(Handle handle)
            {
                m_promise->m_logContext = Base::currentLogContext();
                using SuspendResult     = decltype(m_awaiter->await_suspend(handle));
                // 先置位再交给 inner：inner 的 await_suspend 一旦把续体发布出去，另一条线程就可能
                // 立刻恢复本协程并读这一位；写在发布之后就是数据竞争（TSan 实测报在 await_resume）
                m_isSuspended = true;
                if constexpr (std::is_void_v<SuspendResult>)
                {
                    m_awaiter->await_suspend(handle);
                    Base::publishLogContext(nullptr);
                } else if constexpr (std::is_same_v<SuspendResult, bool>)
                {
                    const bool isSuspended = m_awaiter->await_suspend(handle);
                    if (!isSuspended)
                    {
                        // 没有挂起：没有并发读者，可以就地改回 false
                        m_isSuspended = false;
                        return false;
                    }
                    Base::publishLogContext(nullptr);
                    return true;
                } else
                {
                    auto nextHandle = m_awaiter->await_suspend(handle);
                    if (nextHandle == std::noop_coroutine())
                    {
                        // 等待目标已经在别处跑：本协程真挂起了，线程要交还给循环，必须清场
                        Base::publishLogContext(nullptr);
                    }
                    return nextHandle;
                }
            }

            decltype(auto) await_resume()
            {
                if (m_isSuspended)
                {
                    Base::publishLogContext(m_promise->m_logContext);
                }
                return m_awaiter->await_resume();
            }

        private:
            Awaiter *m_awaiter;            ///< 被包装的等待器（由 co_await 操作数持有生存期）
            Promise *m_promise;            ///< 所属协程的 promise，用于跨挂起点暂存上下文
            bool     m_isSuspended{false}; ///< 本次是否真的挂起过（await_ready 直接放行时不恢复）
        };
    } // namespace Detail

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
         * @param handle 协程句柄
         */
        explicit Task(const Handle handle) : m_handle(handle)
        {
        }

        Task(const Task &) = delete;

        Task &operator=(const Task &) = delete;

        /**
         * @brief 移动构造函数。
         * @param other 要移动的 Task 对象
         */
        Task(Task &&other) noexcept : m_handle(std::exchange(other.m_handle, nullptr))
        {
            // 接管句柄的同时把源对象置空：协程帧只能被销毁一次，
            // 源对象随后析构时若仍持有同一句柄，就会对同一帧重复 destroy
        }

        /**
         * @brief 移动赋值运算符。
         * @param other 要移动的 Task 对象
         * @return 当前对象的引用
         */
        Task &operator=(Task &&other) noexcept
        {
            // 自赋值保护：*this 与 other 是同一对象时，交换会把同一个句柄写回自身，
            // old 仍是这个要继续用的帧，随后 destroy 即销毁了活着的句柄，析构时再销毁一次
            if (this != &other)
            {
                // 先取出旧句柄再接管新句柄：m_handle 被覆盖后旧帧就再无别的引用，
                // 而协程帧不会自行释放，只能在这里补 destroy，否则未跑完的帧永久泄漏
                const auto old = m_handle;
                m_handle       = std::exchange(other.m_handle, nullptr);
                // 判空后再销毁：句柄可能已被移动走，对空句柄调用 destroy 是未定义行为
                if (old)
                {
                    old.destroy();
                }
            }
            return *this;
        }

        /**
         * @brief 析构函数，销毁协程句柄（如果有效）。
         * @note 把一个 Task 的 `handle()` 交给调度器排队之后，本对象必须活到那次派发之后：析构拆的是
         *       **帧本身**，与协程停在哪个挂起点无关，排进队列的句柄因此会指到一块已经没了的内存
         *       （归属契约见 Scheduler 的类注释）。
         */
        ~Task()
        {
            // 判空是因为句柄可能已被移动走（移动构造/赋值会把源对象置空），
            // 对空句柄调用 destroy 是未定义行为；非空时这里是协程帧的最后回收点：
            // 帧若停在挂起点上被销毁，帧内尚未结束的局部对象会随帧正常析构
            if (m_handle)
            {
                m_handle.destroy();
            }
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
             * @brief 经 CoroutinePool.h 的帧内存总口申请协程帧内存（检测构建下直送全局堆）。
             */
            static void *operator new(const size_t size)
            {
                return allocateCoroutineFrame(size);
            }

            /**
             * @brief 将协程帧内存归还给进程级 CoroutinePool。
             *
             * @details 协程帧可能在线程之间迁移，池本身是进程级单例并由互斥锁保护，
             *          因此任何线程回收自己看到的帧都只会把块压回同一个空闲列表。
             */
            static void operator delete(void *const memory, const size_t size) noexcept
            {
                deallocateCoroutineFrame(memory, size);
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
             * @brief 初始挂起点：协程创建后立即挂起（惰性启动），并在首次恢复时标记已启动。
             * @return InitialSuspendAwaiter
             */
            InitialSuspendAwaiter initial_suspend() noexcept
            {
                return InitialSuspendAwaiter{&m_isStarted};
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
             * @brief 保存协程返回值（非 void 版本）
             * @tparam U 返回值类型（应可转换为 T）
             * @param value 返回值
             * @note T 为 std::optional<U2> 时，`co_return std::nullopt` 会被 optional 的
             *       nullopt_t 赋值重载解释成「清空容器」，读取侧随后解引用空 optional 会崩溃。
             *       这里特判该情形，让它落成「有值的空 optional」，符合调用方的直觉语义。
             */
            template<typename U>
            void return_value(U &&value)
            {
                if constexpr (std::is_same_v<std::remove_cvref_t<U>, std::nullopt_t>)
                {
                    m_value = T{};
                } else
                {
                    m_value = std::forward<U>(value);
                }
            }

            /**
             * @brief 获取协程的结果（值或异常）。
             * @return T 类型的值
             * @throws Base::InvalidArgumentException 结果已被读取过第二次（同一个 Task 被读了两次）
             * @throw 如果协程抛出异常，则重新抛出
             */
            T result()
            {
                if (m_exception)
                    std::rethrow_exception(m_exception);

                // 结果是一次性的（按 move 交出）。判据不能看 optional 是否还有值：move 走一个 int
                // 之后 optional 仍然是「有值」的，第二次读因此既不是异常也不是崩溃，而是**静默**
                // 交回一份重复或已被搬空的载荷（move-only 的 T 会交出 nullptr）。这里显式记账，
                // 把「同一个 Task 被读第二次」这个调用方错误当场报出来
                if (m_isResultConsumed)
                {
                    throw Base::InvalidArgumentException("读取协程结果失败：这个 Task 的返回值已经被取走一次。"
                                                         "同一个 Task 只应被读取一次；要反复使用请把结果留在调用方，"
                                                         "或按每次使用重新起一个协程");
                }
                m_isResultConsumed = true;
                return std::move(*m_value);
            }

            /**
             * @brief 包装每一次 co_await：把日志上下文跨挂起点保存与恢复
             * @details 见 Detail::LogContextAwaiter 的说明。无上下文时只做空指针赋值，零堆分配
             * @tparam Awaiter 被等待对象（由编译器按 co_await 操作数推导）
             * @param awaiter 被等待对象
             * @return Detail::LogContextAwaiter 包装后的等待器
             */
            template<typename Awaiter>
            auto await_transform(Awaiter &&awaiter)
            {
                return Detail::LogContextAwaiter<std::remove_reference_t<Awaiter>, promise_type>{std::addressof(awaiter), this};
            }

            std::optional<T>        m_value;                   ///< 协程的返回值（若存在）
            std::exception_ptr      m_exception;               ///< 协程中发生的异常（若有）
            std::coroutine_handle<> m_continuation{nullptr};   ///< 等待该协程的父协程句柄
            bool                    m_isStarted{false};        ///< 协程体是否已开始执行（初始挂起点被恢复过）
            bool                    m_isResultConsumed{false}; ///< 结果是否已被取走（一次性的，第二次读要报出来）
            Base::LogContextPtr     m_logContext;              ///< 挂起前暂存的日志上下文（恢复时重新发布）
        };

        // ========================================================================
        // Awaitable 接口（支持嵌套 co_await Task<T>）
        // ========================================================================

        /**
         * @brief 判断协程是否已经完成，若完成则无需挂起。
         * @return true 表示协程已完成，可直接获取结果；false 表示需要挂起等待
         * @warning 移动后的 Task 句柄为空，此时调用本方法会解引用空句柄（未定义行为）；
         *          只对仍然持有句柄的对象调用（`handle()` 非空可作判据）
         */
        bool await_ready() const noexcept
        {
            return m_handle.done();
        }

        /**
         * @brief 挂起当前协程，并把 continuation 登记为「本任务结束后要恢复的协程」
         * @details 未启动的任务（停在初始挂起点）返回自身句柄就地启动；已启动的只登记 continuation
         *          后挂起，等它真正到达终结点由 FinalAwaiter 以对称转移恢复。**绝不能就地恢复已启动的
         *          任务**：那等于把它内部挂着的等待当成已完成，任务里真正挂着的子协程会失去唤醒者。
         * @param continuation 等待当前协程的父协程句柄
         * @return 需要恢复的协程句柄：未启动的任务返回自身句柄（启动它），已启动的返回 noop_coroutine
         */
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            m_handle.promise().m_continuation = continuation;
            if (m_handle.promise().m_isStarted)
            {
                return std::noop_coroutine();
            }
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
         * @warning 移动后的 Task 句柄为空，此时调用本方法会解引用空句柄（未定义行为）；
         *          只对仍然持有句柄的对象调用
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
         * @param handle 协程句柄
         */
        explicit Task(const Handle handle) : m_handle(handle)
        {
        }

        Task(const Task &) = delete;

        Task &operator=(const Task &) = delete;

        /**
         * @brief 移动构造函数。
         * @param other 要移动的 Task 对象
         */
        Task(Task &&other) noexcept : m_handle(std::exchange(other.m_handle, nullptr))
        {
            // 接管句柄的同时把源对象置空：协程帧只能被销毁一次，
            // 源对象随后析构时若仍持有同一句柄，就会对同一帧重复 destroy
        }

        /**
         * @brief 移动赋值运算符。
         * @param other 要移动的 Task 对象
         * @return 当前对象的引用
         */
        Task &operator=(Task &&other) noexcept
        {
            // 自赋值保护：*this 与 other 是同一对象时，交换会把同一个句柄写回自身，
            // old 仍是这个要继续用的帧，随后 destroy 即销毁了活着的句柄，析构时再销毁一次
            if (this != &other)
            {
                // 先取出旧句柄再接管新句柄：m_handle 被覆盖后旧帧就再无别的引用，
                // 而协程帧不会自行释放，只能在这里补 destroy，否则未跑完的帧永久泄漏
                const auto old = m_handle;
                m_handle       = std::exchange(other.m_handle, nullptr);
                // 判空后再销毁：句柄可能已被移动走，对空句柄调用 destroy 是未定义行为
                if (old)
                    old.destroy();
            }
            return *this;
        }

        /**
         * @brief 析构函数，销毁协程句柄（如果有效）。
         * @note 把一个 Task 的 `handle()` 交给调度器排队之后，本对象必须活到那次派发之后：析构拆的是
         *       **帧本身**，与协程停在哪个挂起点无关，排进队列的句柄因此会指到一块已经没了的内存
         *       （归属契约见 Scheduler 的类注释）。
         */
        ~Task()
        {
            // 判空是因为句柄可能已被移动走（移动构造/赋值会把源对象置空），
            // 对空句柄调用 destroy 是未定义行为；非空时这里是协程帧的最后回收点：
            // 帧若停在挂起点上被销毁，帧内尚未结束的局部对象会随帧正常析构
            if (m_handle)
                m_handle.destroy();
        }

        /**
         * @brief Task<void> 的 promise_type 特化。
         */
        struct promise_type
        {
            /**
             * @brief 经 CoroutinePool.h 的帧内存总口申请协程帧内存（检测构建下直送全局堆）。
             */
            static void *operator new(const size_t size)
            {
                return allocateCoroutineFrame(size);
            }

            /**
             * @brief 将协程帧内存归还给进程级 CoroutinePool。
             */
            static void operator delete(void *const memory, const size_t size) noexcept
            {
                deallocateCoroutineFrame(memory, size);
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
             * @brief 初始挂起点：协程创建后立即挂起，并在首次恢复时标记已启动。
             * @return InitialSuspendAwaiter
             */
            InitialSuspendAwaiter initial_suspend() noexcept
            {
                return InitialSuspendAwaiter{&m_isStarted};
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

            /**
             * @brief 包装每一次 co_await：把日志上下文跨挂起点保存与恢复（语义同主模板）
             * @tparam Awaiter 被等待对象
             * @param awaiter 被等待对象
             * @return Detail::LogContextAwaiter 包装后的等待器
             */
            template<typename Awaiter>
            auto await_transform(Awaiter &&awaiter)
            {
                return Detail::LogContextAwaiter<std::remove_reference_t<Awaiter>, promise_type>{std::addressof(awaiter), this};
            }

            std::exception_ptr      m_exception;             ///< 协程中发生的异常（若有）
            std::coroutine_handle<> m_continuation{nullptr}; ///< 等待该协程的父协程句柄
            bool                    m_isStarted{false};      ///< 协程体是否已开始执行（初始挂起点被恢复过）
            Base::LogContextPtr     m_logContext;            ///< 挂起前暂存的日志上下文（恢复时重新发布）
        };

        /**
         * @brief 判断协程是否已经完成。
         * @return true 表示已完成；false 表示尚未完成
         * @warning 移动后的 Task 句柄为空，此时调用本方法会解引用空句柄（未定义行为）；
         *          只对仍然持有句柄的对象调用
         */
        bool await_ready() const noexcept
        {
            return m_handle.done();
        }

        /**
         * @brief 挂起当前协程并保存 continuation，返回被等待的协程句柄。
         * @details 语义与主模板一致，见 Task<T>::await_suspend()：未启动的任务就地启动，
         *          已启动的任务只登记等待者后挂起（等它到达终结点再经 FinalAwaiter 转移回来）。
         * @param continuation 等待当前协程的父协程句柄
         * @return 需要恢复的协程句柄：未启动的任务返回自身句柄（启动它），已启动的返回 noop_coroutine
         */
        std::coroutine_handle<> await_suspend(const std::coroutine_handle<> continuation) const noexcept
        {
            m_handle.promise().m_continuation = continuation;
            if (m_handle.promise().m_isStarted)
            {
                return std::noop_coroutine();
            }
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
         * @warning 移动后的 Task 句柄为空，此时调用本方法会解引用空句柄（未定义行为）；
         *          只对仍然持有句柄的对象调用
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

} // namespace AsynGyanis::Core
