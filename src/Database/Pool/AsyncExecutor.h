/**
 * @file AsyncExecutor.h
 * @brief 阻塞任务执行器 —— 把数据库这类阻塞调用挪出事件循环线程
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 数据库驱动（sqlite3 / libmysqlclient）的接口全是阻塞的：一次查询会占住调用线程
 *          直到服务端返回或超时。在服务器引擎里，事件循环线程一旦被占住，同一线程上所有
 *          连接的可读可写事件、定时器与已就绪协程都会一起停摆——这正是必须把阻塞调用
 *          挪出去的唯一原因。本类提供「固定线程数的工作线程 + 任务队列」，并把完成后的
 *          协程恢复投递回调用方指定的 EventLoop。
 *
 * ## 为什么不用 Core 既有的原语（读了 ThreadPool / IoContext / Scheduler 之后的结论）
 * - `Core::ThreadPool` 的每个线程都绑定了一个 EventLoop 并跑着 `EventLoop::run()`。往里塞
 *   「一个阻塞的数据库调用」只能以协程的形式投递，而那段协程会在该线程的 run() 里就地执行
 *   （协程在 runAll() 中被 resume），阻塞时长内这个线程的 epoll 等待、定时器与其它协程
 *   全部停摆——问题只是从「调用方的事件循环」搬到了「线程池的事件循环」，没有被解决；
 * - `Core::Scheduler::scheduleRemote()` 能把协程投到另一个 EventLoop 上运行，但它的语义是
 *   「在哪跑」而不是「跑多久不阻塞事件循环」；用它跑阻塞任务同样会占住目标线程；
 * - `Core::IoContext` 是应用级的运行时入口（持有 ThreadPool、run() 阻塞主线程），库代码
 *   不应假设它存在，更不该在内部启动它。
 * 因此需要一个「不绑定事件循环、只跑阻塞函数」的执行器：它就是本类。它与 Core 的关系是
 * 单向的——本类只借用 `EventLoop::scheduler().scheduleRemote()` 来恢复协程，Core 完全不知道
 * 本类的存在，两者不构成耦合。
 *
 * ## 完成回调如何恢复协程
 * 工作线程跑完任务后**不直接 resume 协程**：协程若在被调方的线程上继续执行，后续代码就会
 * 跑到工作线程上，调用方对线程的假设（例如「回调都在事件循环线程上」）立刻失效。
 * 因此这里统一走 `Scheduler::scheduleRemote(句柄)` 把恢复动作投回调用方给定的 EventLoop；
 * `scheduleRemote` 内部持锁入全局队列并通过 EventNotifier 唤醒目标循环，
 * 因此不需要再额外调用 `EventLoop::wake()`，也不会出现「任务入队时循环刚好判断完 hasWork()」
 * 这类丢唤醒的窗口。
 *
 * ## 生命周期
 * 工作线程用 `std::jthread` 管理：构造即全部启动，析构请求停止并等待退出（jthread 析构自动
 * join）。停止时**队列中已接收的任务会先跑完再退出**：直接丢弃它们会让等待结果的协程永远挂起
 * （协程帧还挂在事件循环的等待里，却再也没有人叫醒它）。
 * 本对象必须比所有借用它的协程活得久：协程挂起期间工作线程只持有一份堆上的共享状态
 * （见 SubmissionAwaiter），因此提前销毁 Task 不会让工作线程访问已释放的协程帧，
 * 但被销毁的协程永远不会被恢复（与 ConnectionPool::acquireAsync 的约定一致）。
 *
 * @code
 *   // 应用启动时创建一个（或用 AsyncExecutor::shared() 用进程级共享实例）
 *   AsyncExecutor executor(4);
 *
 *   Core::Task<int> task = executor.submit<int>(loop, []()
 *   {
 *       return expensiveBlockingCall();
 *   });
 *   int result = co_await task;   // 阻塞调用在工作线程上执行，恢复发生在 loop 所在线程
 * @endcode
 */
#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 阻塞任务执行器
     *
     * @details 固定数量的工作线程 + 一个任务队列，用于承载数据库这类阻塞调用。
     *          线程数在构造时确定，运行期不再增减：数据库连接的并行度受池上限约束，
     *          工作线程数多于池容量只会让多出来的线程排队等连接，反而增加上下文切换。
     *
     * @note 本类的线程安全由互斥锁与原子计数保证，submit() 可从任意线程调用；
     *       工作线程之间互不共享状态（每个任务自带它的全部输入与输出）。
     */
    class AsyncExecutor
    {
    public:
        /**
         * @brief 构造执行器并立刻启动全部工作线程
         *
         * @param workerCount 工作线程数；0 表示按 std::thread::hardware_concurrency() 自动确定
         *        （取不到时按 1 处理），且无论如何至少 1 个——否则提交的任务永远不会被执行
         * @note 构造期间工作线程进入等待队列的循环，不会执行任何用户代码
         */
        explicit AsyncExecutor(std::size_t workerCount = 0);

        /**
         * @brief 析构：请求停止全部工作线程，跑完队列中已接收的任务，然后等待线程退出
         *
         * @details 停止时不丢弃队列中的任务（丢弃会让等待它们的协程永远挂起）。
         *          析构返回后本对象的所有线程都已结束，因此不再允许提交新任务。
         */
        ~AsyncExecutor();

        // 工作线程与队列都是独占资源，拷贝与移动会让「谁负责停止线程」变得含糊
        AsyncExecutor(const AsyncExecutor &) = delete;

        AsyncExecutor &operator=(const AsyncExecutor &) = delete;

        AsyncExecutor(AsyncExecutor &&) = delete;

        AsyncExecutor &operator=(AsyncExecutor &&) = delete;

        /**
         * @brief 获取实际启动的工作线程数
         * @return std::size_t 工作线程个数，恒大于 0
         */
        [[nodiscard]] std::size_t workerCount() const noexcept
        {
            return m_workers.size();
        }

        /**
         * @brief 获取当前排队等待执行的任务数（不含正在执行的那个）
         * @return std::size_t 队列长度；主要用于监控与测试
         */
        [[nodiscard]] std::size_t pendingTaskCount() const noexcept
        {
            return m_pendingCount.load(std::memory_order_relaxed);
        }

        /**
         * @brief 取得进程级共享的执行器
         *
         * @details 函数内静态对象，首次调用时创建（线程安全），随进程退出销毁并 join 全部线程。
         *          适合「不想自己管线程池」的场景；需要控制线程数或生命周期时请自行构造实例。
         * @return AsyncExecutor& 进程级共享实例的引用
         */
        [[nodiscard]] static AsyncExecutor &shared();

        /**
         * @brief 提交一个阻塞任务，并在完成后回到指定事件循环
         *
         * @details 语义：把 work 交给某个工作线程执行，当前协程立即挂起；
         *          work 返回或抛出异常后，协程在 completionLoop 所在的线程上恢复。
         *          work 抛出的异常会在恢复处以原样重新抛出（穿过本层，不吞不包装）。
         *
         * @tparam ResultType 任务的返回值类型，不允许为 void（调用方总需要一个结果或异常）
         * @param completionLoop 恢复协程用的事件循环；调用方必须保证它在本任务完成前一直存活，
         *        且其 run() 正在（或即将）被执行——否则协程不会被恢复
         * @param work 在工作线程上执行的阻塞任务，其返回值必须可移动构造
         * @return Core::Task<ResultType> 惰性启动的协程；co_await 它即完成「提交 + 等待结果」
         * @throws 由 work 抛出的任意异常（在恢复处重新抛出）
         * @note work 里不要访问协程帧上的对象：它会在工作线程上执行，而协程帧属于调用方；
         *       需要的数据请在提交前拷贝进捕获列表
         */
        template<typename ResultType>
        [[nodiscard]] Core::Task<ResultType> submit(Core::EventLoop &completionLoop, std::function<ResultType()> work)
        {
            // void 返回值无法区分「完成」与「未完成」，本执行器只服务「需要一个结果」的场景
            static_assert(!std::is_void_v<ResultType>,
                          "AsyncExecutor::submit：ResultType 不能是 void，"
                          "需要无返回值的场景请让任务返回一个状态值或改用自定义 awaitable");

            // 协程体只有一句：把接续点交给 awaitable，挂起与恢复逻辑全部收敛在那里
            co_return co_await SubmissionAwaiter<ResultType>(*this, completionLoop, std::move(work));
        }

    private:
        /**
         * @brief 一次提交的共享状态
         *
         * @details 放在堆上由提交方与工作线程各持一份 shared_ptr：协程挂起期间工作线程
         *          只读写本结构，不触碰协程帧，因此调用方提前销毁 Task 也不会让工作线程
         *          访问到已释放的帧内对象。
         *
         * @tparam ResultType 任务返回值类型
         */
        template<typename ResultType>
        struct SubmissionState
        {
            std::function<ResultType()> work;                    ///< 待执行的阻塞任务
            std::optional<ResultType>   value;                   ///< 任务返回值（成功后才有值）
            std::exception_ptr          error;                   ///< 任务抛出的异常（失败时非空）
            std::coroutine_handle<>     continuation;            ///< 等待结果的协程句柄
            Core::EventLoop *           completionLoop{nullptr}; ///< 恢复该协程的事件循环
        };

        /**
         * @brief submit() 的等待体：挂起协程 → 入队 → 恢复时取结果
         *
         * @tparam ResultType 任务返回值类型
         */
        template<typename ResultType>
        class SubmissionAwaiter
        {
        public:
            /**
             * @brief 构造等待体并把任务搬到堆上的共享状态
             * @param executor 目标执行器
             * @param completionLoop 恢复协程用的事件循环
             * @param work 待执行的阻塞任务
             */
            SubmissionAwaiter(AsyncExecutor &executor, Core::EventLoop &completionLoop, std::function<ResultType()> work) :
                m_executor(&executor), m_state(std::make_shared<SubmissionState<ResultType> >())
            {
                // 任务与事件循环都放进堆状态：工作线程只会用到它们，不依赖协程帧的存活
                m_state->work           = std::move(work);
                m_state->completionLoop = &completionLoop;
            }

            /**
             * @brief 恒不就地完成：阻塞任务绝不能在调用线程上执行，否则本类就失去了意义
             * @return false
             */
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }

            /**
             * @brief 把任务投进队列并挂起当前协程
             *
             * @details 这里只做「入队」这一件不阻塞的事：真正耗时的 work 由工作线程执行。
             *          入队后立即返回，控制权交回调用方，因此 await_suspend 的耗时与 work
             *          的耗时无关——「提交后控制权立刻返回」正是由这一点保证的。
             * @param continuation 当前协程的句柄，任务完成后由工作线程投递回来恢复它
             */
            void await_suspend(std::coroutine_handle<> continuation)
            {
                // 复制一份 shared_ptr 进入队列：只要任务还在队列里或正在执行，堆状态就不会被销毁
                std::shared_ptr<SubmissionState<ResultType> > state = m_state;
                state->continuation                                 = continuation;

                m_executor->enqueue(
                        [state]()
                        {
                            try
                            {
                                state->value = state->work();
                            } catch (...)
                            {
                                // 异常在此暂存，恢复到事件循环线程后再原样抛出：
                                // 工作线程上抛异常没有接收者，只会 terminate
                                state->error = std::current_exception();
                            }

                            // 恢复动作投回事件循环线程（scheduleRemote 线程安全且自带唤醒），
                            // 因此协程的后续代码与调用方对线程的假设保持一致
                            state->completionLoop->scheduler().scheduleRemote(state->continuation);
                        });
            }

            /**
             * @brief 在事件循环线程上取出结果
             * @return ResultType 任务的返回值
             * @throws 任务执行期间抛出的原始异常（std::exception_ptr 原样重抛，不丢失类型）
             */
            [[nodiscard]] ResultType await_resume()
            {
                if (m_state->error != nullptr)
                {
                    std::rethrow_exception(m_state->error);
                }

                // 走到这里说明任务成功返回；value 必然有值（ResultType 不允许为 void），
                // 若为空只能是本类内部状态错乱，交给 optional 抛 bad_optional_access 而不是静默返回默认值
                return std::move(m_state->value.value());
            }

            // 允许外围类访问本等待体的私有成员（与其他嵌套 RAII 类的写法一致）
            friend class AsyncExecutor;

        private:
            AsyncExecutor *                               m_executor; ///< 目标执行器，生命周期由调用方保证
            std::shared_ptr<SubmissionState<ResultType> > m_state;    ///< 与工作线程共享的任务状态
        };

        /**
         * @brief 把任务放进队列并唤醒一个工作线程
         * @param task 待执行的闭包（已捕获好全部输入与输出位置）
         * @note 本方法只做入队，不执行任务：调用方（await_suspend）因此不会被阻塞
         */
        void enqueue(std::function<void()> task);

        /**
         * @brief 工作线程主循环
         * @param stopToken 停止令牌，由 std::jthread 在析构时请求停止
         */
        void workerLoop(const std::stop_token &stopToken);

        std::mutex                         m_mutex;           ///< 保护任务队列
        std::condition_variable            m_condition;       ///< 通知工作线程有新任务或收到停止请求
        std::deque<std::function<void()> > m_tasks;           ///< 待执行的阻塞任务（FIFO，先到先服务）
        std::atomic<std::size_t>           m_pendingCount{0}; ///< 队列长度（原子，供监控快速读取）

        // m_workers 必须声明在最后：成员按声明逆序销毁，最后声明的先销毁，
        // jthread 析构会 join，从而保证线程都结束了才会轮到上面的互斥锁与条件变量被销毁
        std::vector<std::jthread> m_workers; ///< 固定数量的工作线程，析构时自动 join
    };

} // namespace AsynGyanis::Database
