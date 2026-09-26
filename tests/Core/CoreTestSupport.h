/**
 * @file CoreTestSupport.h
 * @brief Core 模块单元测试辅助：有界等待、事件泵推进、就绪分发与后台事件循环运行器
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本文件是「驱动 Core::EventLoop 与 Core::Task」这类测试设施的唯一定义处：
 *          各模块（Core / Database / Net）的异步用例共用同一套夹具与纪律，
 *          下游模块通过 CMake 引入本目录后直接包含本头即可。
 */

#pragma once

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"
#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Core::TestSupport
{
    using AsynGyanis::TestSupport::kWaitTimeout;
    using AsynGyanis::TestSupport::waitForCondition;

    /// 事件泵的单步等待（毫秒）：够短到不拖慢用例，又够长到让刚发出的字节到达
    inline constexpr int kPumpStepMilliseconds = 5;

    /**
     * @brief 取回一批就绪事件并交给各自的注册对象
     * @details 与 EventLoop::run() 的分发那一步同构：事件里挂载的就是注册对象地址，
     *          用例自驱分发因此不引入事件循环线程，时序完全确定。
     * @param loop 事件循环
     * @param timeoutMilliseconds 等待就绪的超时（毫秒）；0 表示只取当前已就绪的
     * @return std::size_t 本次分发的事件数
     */
    inline std::size_t dispatchOnce(EventLoop &loop, const int timeoutMilliseconds = 1000)
    {
        std::size_t dispatchedCount = 0;
        for (const auto &event: loop.epoll().wait(timeoutMilliseconds))
        {
            if (event.data.ptr != nullptr)
            {
                static_cast<IoWatcher *>(event.data.ptr)->handleEvents(event.events);
                ++dispatchedCount;
            }
        }
        return dispatchedCount;
    }

    /**
     * @brief 推进循环一步：分发一批就绪事件，再清空就绪队列
     * @param loop 事件循环
     * @param timeoutMilliseconds 等待就绪的超时（毫秒），默认 kPumpStepMilliseconds
     */
    inline void stepLoopOnce(EventLoop &loop, const int timeoutMilliseconds = kPumpStepMilliseconds)
    {
        dispatchOnce(loop, timeoutMilliseconds);
        loop.scheduler().runAll();
    }

    /**
     * @brief 反复推进循环，直到条件成立或超出时限
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param loop 事件循环
     * @param predicate 待成立的条件
     * @param timeout 时间上限，默认 kWaitTimeout
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool advanceUntil(EventLoop &loop, Predicate predicate, const std::chrono::milliseconds timeout = kWaitTimeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            stepLoopOnce(loop);
        }
        return true;
    }

    /**
     * @brief 一次异步任务的观测结果
     * @tparam ResultType 任务结果类型
     */
    template<typename ResultType>
    struct CompletedTask
    {
        std::optional<ResultType> value;            ///< 任务返回值（成功时才有值）
        std::exception_ptr        error;            ///< 任务抛出的异常（失败时非空）
        bool                      finished = false; ///< 是否在时限内收到完成通知
    };

    /**
     * @brief 驱动协程：co_await 目标任务，把结果或异常搬进调用方提供的变量
     *
     * @details Task<T> 只能被协程 co_await，故先挂起在内层任务上、完成后在事件循环线程上恢复，
     *          再把结果写出去并最后置完成标记：标记由事件循环线程写入，调用线程读取前必须做
     *          acquire 语义的同步（见 std::atomic 内存序），这是结果得以按值搬出的前提。
     *
     * @tparam ResultType 内层任务的结果类型
     * @param inner 待等待的任务（按值接收，帧内持有它的生命周期）
     * @param value 出参：任务返回值
     * @param error 出参：任务抛出的异常
     * @param finished 出参：完成标记，写于所有其它出参之后
     * @return Task<void> 驱动协程
     */
    template<typename ResultType>
    Task<void> collectTask(Task<ResultType> inner,
                           std::optional<ResultType> &value,
                           std::exception_ptr &error,
                           std::atomic<bool> &finished)
    {
        try
        {
            value.emplace(co_await std::move(inner));
        }
        catch (...)
        {
            // 任务异常在此收敛：驱动协程本身不向上抛，调用线程只需看 error 是否被写入
            error = std::current_exception();
        }

        // release 语义：保证上面的写入对读取到本标记的线程可见
        finished.store(true, std::memory_order_release);
    }

    /**
     * @brief 后台事件循环运行器：在独立线程上跑 EventLoop::run()，析构先停循环再 join
     *
     * @details 默认构造内建并自持一个事件循环，传入循环引用则改为借用——两种模式都只在本对象
     *          存活期间驱动它。销毁纪律：**协程帧必须活到事件循环线程结束之后**——runToCompletion()
     *          产出的驱动帧一律留在 m_driverTasks，且它声明在 m_thread **之前**，逆序析构保证帧
     *          销毁晚于循环线程结束；调用方自持的驱动协程对象也须声明在本类之前。
     */
    class EventLoopThread
    {
    public:
        /// 自持模式：内建事件循环，随本对象销毁；适合「用例只关心一个后台循环」的场景
        EventLoopThread() :
            m_ownedLoop(std::make_unique<EventLoop>()), m_loop(m_ownedLoop.get()),
            m_thread([this]()
            {
                runLoopGuarded();
            })
        {
        }

        /**
         * @brief 借用模式：只驱动调用方给出的事件循环，不接管它的生命周期
         * @param loop 待驱动的事件循环；调用方必须保证它比本对象活得久
         */
        explicit EventLoopThread(EventLoop &loop) :
            m_loop(&loop), m_thread([this]()
            {
                runLoopGuarded();
            })
        {
        }

        ~EventLoopThread()
        {
            join();
        }

        EventLoopThread(const EventLoopThread &)            = delete;
        EventLoopThread &operator=(const EventLoopThread &) = delete;

        /**
         * @brief 停止事件循环并等待承载线程退出
         * @note 析构会自动调用；需要在销毁被测对象之前先让循环停手时可显式调用。幂等：重复调用无副作用
         */
        void join()
        {
            m_loop->stop();
            if (m_thread.joinable())
            {
                m_thread.join();
            }
        }

        /**
         * @brief 获取被驱动的事件循环
         * @return EventLoop& 事件循环，供提交协程、注册事件使用
         */
        [[nodiscard]] EventLoop &loop() noexcept
        {
            return *m_loop;
        }

        /**
         * @brief 事件循环的 run() 是否已经进入循环体
         * @return true 已进入；false 尚未启动或已停止
         */
        [[nodiscard]] bool isRunning() const noexcept
        {
            return m_loop->isRunning();
        }

        /**
         * @brief 取得后台循环线程的 id
         * @details 用于断言「协程恢复发生在事件循环线程上」这类线程归属性质：
         *          在驱动协程里记录 `std::this_thread::get_id()`，再与本方法比较即可。
         * @return std::thread::id 循环线程 id；线程尚未启动时为空 id
         */
        [[nodiscard]] std::thread::id threadId() const noexcept
        {
            return m_thread.get_id();
        }

        /**
         * @brief 自旋等待事件循环进入运行状态
         * @details 线程刚启动时 run() 可能还没读到运行标志，此时提交的协程虽然不会丢
         *          （调度器会先入队），但断言「已经跑起来」会让用例结论依赖调度时序。
         * @return true 在 kWaitTimeout 内进入运行状态
         */
        [[nodiscard]] bool waitUntilRunning() const
        {
            return waitForCondition([this]()
            {
                return m_loop->isRunning();
            });
        }

        /**
         * @brief 把协程投给事件循环线程执行
         * @param task 待执行的协程（跨线程安全：走调度器全局队列并唤醒循环）
         */
        void schedule(Task<void> &task)
        {
            m_loop->scheduler().scheduleRemote(task.handle());
        }

        /**
         * @brief 启动一个任务并等到它完成
         *
         * @details 首次恢复也交给事件循环线程做，**不在调用线程上 resume**：驱动协程会立刻把内层
         *          任务跑到它的第一个挂起点，而那条挂起路径上可能就地注册 IO 观察者
         *          （`AsyncSocket::ensureWatcher()` → 后端的 `addFileDescriptor`）。循环线程此刻
         *          正堵在 `wait()` 里，两份线程同时碰同一份后端状态就是数据竞争——Windows 上实测
         *          表现为事件循环后端的偶发堆破坏。完成标记是驱动协程的**最后一次**出参写入，之后
         *          只走 final_suspend 收尾，故按值搬出结果安全；帧仍留在 m_driverTasks 里活到 join 之后。
         *
         * @tparam ResultType 任务结果类型
         * @param task 待等待的异步任务
         * @return CompletedTask<ResultType> 结果、异常与完成情况
         */
        template<typename ResultType>
        [[nodiscard]] CompletedTask<ResultType> runToCompletion(Task<ResultType> task)
        {
            CompletedTask<ResultType> completed;
            std::atomic<bool>         finishedFlag{false};

            Task<void> driver = collectTask<ResultType>(std::move(task), completed.value, completed.error, finishedFlag);
            m_loop->scheduler().scheduleRemote(driver.handle());

            completed.finished = waitForCondition([&finishedFlag]()
            {
                return finishedFlag.load(std::memory_order_acquire);
            });

            // 帧的销毁推迟到事件循环线程 join 之后（见类注释的成员声明顺序）
            m_driverTasks.push_back(std::move(driver));
            return completed;
        }

        /**
         * @brief 把调用方自己的驱动协程交给运行器保管，直到循环线程结束
         *
         * @details 用于「提交后先断言尚未完成、放行后再等结果」这类用例：此时协程帧不能放在
         *          用例体的局部对象里（用例体先于夹具成员析构，帧会与 resume() 收尾竞态），
         *          交出来让运行器按成员声明顺序统一销毁即可。
         * @param driver 待保管的驱动协程（通常由 collectTask() 产出）
         */
        void parkDriver(Task<void> driver)
        {
            m_driverTasks.push_back(std::move(driver));
        }

    private:
        /**
         * @brief 在后台线程上驱动循环，并把逃出来的异常就地收口
         * @details run() 按契约把投递体的异常重抛给调用方，而这里就是那个调用方——线程入口
         *          不接就是 std::terminate：整个测试进程连同其它在跑的用例一起没，且没有任何
         *          现场。与 ThreadPool 工作线程体同一口径：记一条错误、让本线程体面退出，
         *          用例随后因等不到结果而报红（红得有信息，而不是把整条测试跑炸掉）
         */
        void runLoopGuarded()
        {
            try
            {
                m_loop->run();
            } catch (const std::exception &loopError)
            {
                GTEST_LOG_(ERROR) << "后台事件循环因异常退出：" << loopError.what();
            } catch (...)
            {
                GTEST_LOG_(ERROR) << "后台事件循环因非标准异常退出";
            }
        }

        std::unique_ptr<EventLoop> m_ownedLoop;     ///< 自持模式下的事件循环；借用模式下为空
        EventLoop *                m_loop{nullptr}; ///< 实际驱动的事件循环，恒非空
        std::vector<Task<void> >   m_driverTasks;   ///< 驱动协程：声明在 m_thread 之前，故晚于 join 销毁
        std::jthread               m_thread;        ///< 跑 m_loop->run() 的后台线程，析构自动 join
    };
} // namespace AsynGyanis::Core::TestSupport
