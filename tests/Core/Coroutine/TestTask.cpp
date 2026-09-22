// Task 单元测试：返回值、异常传播（含交给等待方与没人接手两条出路）、移动语义与等待器接口

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Core/Coroutine/Task.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 测试协程：返回固定值 42
         * @return Task<int>
         */
        Task<int> simpleValueTask()
        {
            co_return 42;
        }

        /**
         * @brief 测试协程：无返回值直接结束
         * @return Task<void>
         */
        Task<void> simpleVoidTask()
        {
            co_return;
        }

        /**
         * @brief 测试协程：抛出运行时异常
         * @return Task<int>
         */
        Task<int> throwingTask()
        {
            throw std::runtime_error("test error");
            co_return 0;
        }

        /**
         * @brief 测试协程：返回一个只可移动的载荷
         * @return Task<std::unique_ptr<int>> 指向 7 的唯一指针
         */
        Task<std::unique_ptr<int>> moveOnlyPayloadTask()
        {
            co_return std::make_unique<int>(7);
        }

        /**
         * @brief 手动放行的挂起点：把恢复权完全交给测试代码
         * @details 用来制造「任务已启动、挂在内部等待上」这一中间状态——这正是
         *          「co_await 一个已经在跑的任务」要面对的场景，定时器之类的真实等待点
         *          无法在单测里精确控制放行时刻。
         */
        class ManualGate
        {
        public:
            [[nodiscard]] bool await_ready() const noexcept { return false; }

            void await_suspend(const std::coroutine_handle<> handle) noexcept { m_handle = handle; }

            void await_resume() const noexcept {}

            /// 放行：恢复挂在门上的协程（门未被等待时为空操作）
            void open() const noexcept
            {
                if (m_handle)
                {
                    m_handle.resume();
                }
            }

        private:
            std::coroutine_handle<> m_handle{}; ///< 挂在门上的协程
        };

        /**
         * @brief 一条被记录下来的日志事件（只留断言要用的两项）
         */
        struct RecordedEntry
        {
            Base::LogLevel level{};     ///< 级别
            std::string    message;     ///< 正文
        };

        /// 用例与 Sink 共享的记录容器：Sink 被日志器接管后，用例仍能从这份引用读回事件
        struct RecordCollector
        {
            std::mutex                 mutex;   ///< 保护 entries
            std::vector<RecordedEntry> entries; ///< 按到达顺序记录的日志事件
        };

        /**
         * @brief 把事件收进内存的记录型 Sink
         */
        class RecordingSink final : public Base::LogSink
        {
        public:
            /**
             * @brief 构造记录型 Sink
             * @param collector 与用例共享的记录容器
             */
            explicit RecordingSink(std::shared_ptr<RecordCollector> collector) :
                m_collector(std::move(collector))
            {
            }

            /**
             * @brief 记下一条事件的级别与正文
             * @param event 日志事件
             */
            void write(const Base::LogEvent &event) override
            {
                const std::lock_guard lock(m_collector->mutex);
                m_collector->entries.push_back(RecordedEntry{event.level, event.message});
            }

            /**
             * @brief 内容全在内存里，没有缓冲要落盘，故为空实现
             */
            void flush() override
            {
            }

        private:
            std::shared_ptr<RecordCollector> m_collector; ///< 与用例共享的记录容器
        };

        /**
         * @brief 摘掉挂在 root 日志器上的 Sink
         * @details root 默认不带任何 Sink（由使用方配置），故清空即恢复用例前的原样
         */
        class RootSinkGuard
        {
        public:
            /// 显式默认构造：本类的用途是「离开作用域时清掉 Sink」，构造本身无事可做
            RootSinkGuard() = default;

            ~RootSinkGuard()
            {
                Base::LoggerRegistry::instance().getRootLogger().clearSinks();
            }

            RootSinkGuard(const RootSinkGuard &) = delete;
            RootSinkGuard &operator=(const RootSinkGuard &) = delete;
        };

        /**
         * @brief 取记录快照（把锁内的副本交出来，断言不再碰容器）
         * @param collector 记录容器
         * @return std::vector<RecordedEntry> 事件副本
         */
        std::vector<RecordedEntry> snapshotOf(const std::shared_ptr<RecordCollector> &collector)
        {
            const std::lock_guard lock(collector->mutex);
            return collector->entries;
        }
    }

    /**
     * @brief 惰性启动：resume 之前不算完成，resume 到终结点后 result() 交出 co_return 的值
     */
    TEST(Task, SimpleValueReturnIsAvailableAfterResume)
    {
        auto task = simpleValueTask();
        ASSERT_FALSE(task.isReady());

        // 恢复协程至最终挂起点
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        const int result = task.handle().promise().result();
        EXPECT_EQ(result, 42);
    }

    /**
     * @brief 第二次读结果必须报错，而不是静默交回一份重复或已被搬空的载荷
     * @details 结果是一次性的（按 move 交出）。老实现里第二次读既不是崩溃也不是异常：
     *          `std::optional` 在 move 走一个 int 之后仍然是「有值」的，于是同一份值被交出两次；
     *          而 T 是 move-only（如 `unique_ptr`）时，第二次交出的是 nullptr——一个看着合法的
     *          空值会顺着响应与状态一路往下传。抛 `InvalidArgumentException`（`std::logic_error`
     *          分支）而不是框架的运行期故障基类，因为这是调用方用错了对象，不该被当成可重试的故障
     */
    TEST(Task, SecondResultReadThrowsInsteadOfReturningGarbage)
    {
        auto task = simpleValueTask();
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        EXPECT_EQ(task.handle().promise().result(), 42);

        EXPECT_THROW(static_cast<void>(task.handle().promise().result()), Base::InvalidArgumentException)
                << "第二次读取结果应当被拒绝，而不是返回一个值";
        // 用法错误必须落在 std::logic_error 分支，不能被「可重试的运行期故障」那一侧的 catch 兜住
        EXPECT_THROW(static_cast<void>(task.handle().promise().result()), std::logic_error)
                << "报错类型不在 logic_error 分支上，调用方会把它当成可重试的故障";

        try
        {
            static_cast<void>(task.handle().promise().result());
        } catch (const Base::InvalidArgumentException &error)
        {
            // 文案要说清「已经被取走一次」并给出替代做法，只报「无值可读」等于没报
            EXPECT_NE(std::string(error.what()).find("已经被取走一次"), std::string::npos)
                    << "报错文案没说明原因：" << error.what();
        }
    }

    /**
     * @brief 只可移动的载荷把后果说得更清楚：第二次读交出的是空指针
     * @details 老实现里这一步不报错也不崩，只是把已被搬空的 `unique_ptr` 再交出去一次；
     *          调用方拿到「非空的返回值」却解引用到空指针，是最难归位的一类故障
     */
    TEST(Task, SecondResultReadOfMoveOnlyPayloadIsRejected)
    {
        auto task = moveOnlyPayloadTask();
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        const std::unique_ptr<int> firstPayload = task.handle().promise().result();
        ASSERT_NE(firstPayload, nullptr) << "第一次读取本该拿到值";
        EXPECT_EQ(*firstPayload, 7);

        EXPECT_THROW(static_cast<void>(task.handle().promise().result()), Base::InvalidArgumentException)
                << "第二次读取交出的是被搬空的指针：必须报错，不能静默给一个空值";
    }

    /**
     * @brief Task<void> 正常跑到终结点时 isReady() 为 true，且 result() 不抛异常
     */
    TEST(Task, VoidTaskCompletesWithoutThrow)
    {
        auto task = simpleVoidTask();
        ASSERT_FALSE(task.isReady());

        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        // 正常结束的 void 协程取结果不应抛出异常
        EXPECT_NO_THROW(task.handle().promise().result());
    }

    /**
     * @brief 协程体抛出的异常被 promise 捕获存下，在 result() 处按原类型重新抛出，不会丢失在协程帧里
     */
    TEST(Task, ExceptionIsCapturedAndRethrownByResult)
    {
        auto task = throwingTask();
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        EXPECT_THROW(task.handle().promise().result(), std::runtime_error);
    }

    /**
     * @brief 没人接手的协程抛出异常时，错误要落到日志而不是静静消失
     * @details 服务起不来正是这个形态：start()/listen() 协程被 schedule 出去、没人 await，
     *          异常存进 promise 之后再没有谁去取。这里不必启事件循环——直接把句柄 resume 到
     *          跑完，走的是调度器投递后同一条终结路径
     */
    TEST(Task, DetachedTaskReportsUnhandledExceptionToLogger)
    {
        const std::shared_ptr<RecordCollector> collector = std::make_shared<RecordCollector>();
        RootSinkGuard                        sinkGuard;
        Base::LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<RecordingSink>(collector));

        auto task = throwingTask();
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        // 只报一条：多报会让日志变成噪声，少报就等于没修
        const std::vector<RecordedEntry> entries = snapshotOf(collector);
        ASSERT_EQ(entries.size(), 1U) << "分离协程的异常没有被唯一地报出来";
        EXPECT_EQ(entries.front().level, Base::LogLevel::Error) << "没人接手的异常不该按低于错误的级别记";
        EXPECT_NE(entries.front().message.find("test error"), std::string::npos)
                << "报出来的正文里没有异常文本，运维无从定位：" << entries.front().message;
    }

    /**
     * @brief 有人 await 的任务不再另记这条日志：异常交回等待方，不该同时留一份噪声
     */
    TEST(Task, AwaitedTaskRethrowsWithoutReportingToLogger)
    {
        const std::shared_ptr<RecordCollector> collector = std::make_shared<RecordCollector>();
        RootSinkGuard                        sinkGuard;
        Base::LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<RecordingSink>(collector));

        bool isCaughtByAwaiter = false;
        // 子任务在父协程 await_suspend 时登记了 continuation，因此它终结时异常是有主的
        auto parentBody = [&isCaughtByAwaiter]() -> Task<>
        {
            try
            {
                co_await throwingTask();
            } catch (const std::runtime_error &)
            {
                isCaughtByAwaiter = true;
            }
        };
        auto parent = parentBody();
        parent.handle().resume();

        EXPECT_TRUE(isCaughtByAwaiter) << "父协程没接到子任务的异常，本例的对照前提就不成立了";
        EXPECT_TRUE(snapshotOf(collector).empty()) << "异常已交给等待方，却又被当成没人接手报了一次";
    }

    /**
     * @brief 移动构造把句柄整体交给新对象：新对象持有的句柄与源原先持有的完全相同
     */
    TEST(Task, MoveConstructionTransfersHandle)
    {
        auto first = simpleValueTask();
        const auto originalHandle = first.handle();

        Task<int> second(std::move(first));

        EXPECT_EQ(second.handle(), originalHandle);
    }

    /**
     * @brief 移动赋值同样转移句柄所有权：目标接管新帧并销毁旧帧，不会出现两个 Task 共用一个帧
     */
    TEST(Task, MoveAssignmentTransfersHandle)
    {
        auto first = simpleValueTask();
        auto second = simpleValueTask();

        const auto originalHandle = first.handle();
        second = std::move(first);

        EXPECT_EQ(second.handle(), originalHandle);
    }

    /**
     * @brief isReady() 以协程是否到达终结点为准：未 resume 过的惰性协程不算完成
     */
    TEST(Task, IsReadyReturnsFalseBeforeResume)
    {
        // 惰性启动：协程创建后处于挂起状态，未恢复前未完成
        auto task = simpleValueTask();

        EXPECT_FALSE(task.isReady());
    }

    /**
     * @brief 跑到终结点后 isReady() 翻转为 true，调用方据此决定「直接取结果」还是「继续等待」
     */
    TEST(Task, IsReadyReturnsTrueAfterCompletion)
    {
        auto task = simpleValueTask();
        task.handle().resume();

        EXPECT_TRUE(task.isReady());
    }

    /**
     * @brief await_resume() 本身就能取出协程结果，等待器接口可脱离 co_await 单独使用
     */
    TEST(Task, AwaitResumeReturnsCoroutineValue)
    {
        auto task = simpleValueTask();
        task.handle().resume();

        const int value = task.await_resume();
        EXPECT_EQ(value, 42);
    }

    /**
     * @brief await_ready() 与完成状态一致：未完成返回 false（需挂起等待），完成后返回 true（可直接取结果）
     */
    TEST(Task, AwaitReadyReflectsDoneState)
    {
        auto task = simpleValueTask();
        EXPECT_FALSE(task.await_ready());

        task.handle().resume();
        EXPECT_TRUE(task.await_ready());
    }

    /**
     * @brief 被移动走的 Task 句柄为空：源对象不再持有协程帧，其析构不会重复销毁
     */
    TEST(Task, MovedFromTaskHasNullHandle)
    {
        auto first = simpleValueTask();
        Task<int> second(std::move(first));

        EXPECT_EQ(first.handle(), nullptr);
    }

    /**
     * @brief co_await 一个已启动的任务是「等它结束」，不是「把它叫醒」
     *
     * @details 任务停在自身内部的等待上时，恢复它等于谎报「你等的那个操作完成了」：
     *          等待方会立刻越过 co_await 继续执行，而任务里的子等待依旧悬着，随后
     *          随帧析构一起被销毁。这里用一道手动门把这个差异钉死——等待方在子任务
     *          真正放行之前，一步都不许前进。
     */
    TEST(Task, AwaitStartedTaskWaitsInsteadOfResumingIt)
    {
        ManualGate gate;
        bool       isChildFinished = false;

        // 惰性 Task 的协程帧记住的是闭包对象的地址：闭包必须先落到具名变量上再调用，
        // 否则「构造后立即调用的临时闭包」在语句结束即销毁，恢复协程时读到的捕获已是死对象
        auto childBody = [&gate, &isChildFinished]() -> Task<>
        {
            co_await gate;
            isChildFinished = true;
        };
        auto child = childBody();

        // 先把子任务启动到内部等待点上：从这一刻起它已经「在跑」
        child.handle().resume();
        ASSERT_FALSE(isChildFinished);
        ASSERT_FALSE(child.isReady());

        int parentProgress = 0;
        auto parentBody    = [&child, &parentProgress]() -> Task<>
        {
            parentProgress = 1;
            co_await child;
            parentProgress = 2;
        };
        auto parent = parentBody();

        parent.handle().resume();
        EXPECT_EQ(parentProgress, 1) << "等待方越过 co_await 前进了：说明它把子任务的内部等待当成了已完成";
        EXPECT_FALSE(isChildFinished) << "子任务被等待方从自己的等待点上叫醒了";

        // 真正放行子任务：它跑到终结点后应当把等待方唤醒
        gate.open();
        EXPECT_TRUE(isChildFinished);
        EXPECT_EQ(parentProgress, 2) << "子任务结束后没有把等待方唤醒";
        EXPECT_TRUE(child.isReady());
        EXPECT_TRUE(parent.isReady());
    }

    /**
     * @brief co_await 一个惰性任务会就地启动它，而不是空等一个没人启动的协程
     */
    TEST(Task, AwaitLazyTaskStartsIt)
    {
        bool isChildFinished = false;

        auto childBody = [&isChildFinished]() -> Task<>
        {
            isChildFinished = true;
            co_return;
        };
        auto child = childBody();

        auto parentBody = [&child]() -> Task<>
        {
            co_await child;
        };
        auto parent = parentBody();

        parent.handle().resume();
        EXPECT_TRUE(isChildFinished) << "co_await 没有启动惰性任务";
        EXPECT_TRUE(child.isReady());
        EXPECT_TRUE(parent.isReady());
    }
} // namespace AsynGyanis::Core
