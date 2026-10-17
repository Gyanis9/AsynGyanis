// 日志上下文与协程的接线：作用域跨真实挂起点仍在、挂起期间线程上不残留、跨线程恢复也找得回、
// 子协程继承父协程的上下文

#include "Base/Log/LogContext.h"
#include "Core/Coroutine/Task.h"

#include <gtest/gtest.h>

#include <coroutine>
#include <string>
#include <string_view>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 手动放行的挂起点：把恢复时刻完全交给用例，且可以从另一条线程恢复
         */
        class ManualGate
        {
        public:
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> handle) noexcept
            {
                m_handle = handle;
            }

            void await_resume() const noexcept
            {
            }

            /// 恢复挂在该门上的协程（可在任意线程调用）
            void resume() const
            {
                m_handle.resume();
            }

        private:
            std::coroutine_handle<> m_handle{};
        };

        /// 取当前日志上下文里的一个字段；没有上下文或没有该键时返回空串
        std::string contextField(const std::string_view key)
        {
            const Base::LogContextPtr &context = Base::currentLogContext();
            if (context == nullptr)
            {
                return {};
            }
            const auto iterator = context->fields().find(std::string(key));
            return iterator == context->fields().end() ? std::string{} : iterator->second;
        }

        /// 在作用域里取一次、过挂起点后再取一次
        Task<void> suspendWithContext(ManualGate &gate, std::string &before, std::string &after)
        {
            const Base::LogContextScope scope({{"trace_id", "4bf92f3577b34da6a3ce929d0e0e4736"}});
            before = contextField("trace_id");
            co_await gate;
            after = contextField("trace_id");
            co_return;
        }

        /// 子协程：只读父协程留下的上下文
        Task<void> readContextInChild(std::string &seen)
        {
            seen = contextField("trace_id");
            co_return;
        }

        /// 父协程：建作用域后 co_await 子协程（对称转移进子协程）
        Task<void> awaitChildWithContext(std::string &seen)
        {
            const Base::LogContextScope scope({{"trace_id", "child-inherits"}});
            co_await readContextInChild(seen);
            co_return;
        }
    } // namespace

    /**
     * @brief 作用域跨挂起点仍在；挂起期间线程上清场；跨线程恢复后仍找得回
     */
    TEST(LogContextCoroutine, ScopeSurvivesSuspensionAndCrossThreadResume)
    {
        ManualGate  gate;
        std::string beforeSuspension;
        std::string afterResume;

        Task<void> task = suspendWithContext(gate, beforeSuspension, afterResume);
        task.handle().resume();

        EXPECT_EQ(beforeSuspension, "4bf92f3577b34da6a3ce929d0e0e4736") << "进入作用域后读不到 trace_id";
        EXPECT_EQ(Base::currentLogContext(), nullptr) << "协程挂起后线程上仍留着上下文：别的协程会读到上一条请求的 trace_id";

        // 在另一条线程上恢复：纯 thread_local 在这里必然丢上下文
        std::thread resumer([&gate] { gate.resume(); });
        resumer.join();

        EXPECT_TRUE(task.isReady());
        task.handle().promise().result();
        EXPECT_EQ(afterResume, "4bf92f3577b34da6a3ce929d0e0e4736") << "跨挂起点/跨线程恢复后上下文丢了";
        EXPECT_EQ(Base::currentLogContext(), nullptr) << "协程结束后上下文应清场";
    }

    /**
     * @brief 子协程在首次挂起之前就应当读到父协程的上下文（对称转移期间不清场）
     */
    TEST(LogContextCoroutine, ChildTaskInheritsContextBeforeItsFirstSuspension)
    {
        std::string seenInChild;
        Task<void>  task = awaitChildWithContext(seenInChild);
        task.handle().resume();

        EXPECT_TRUE(task.isReady());
        task.handle().promise().result();
        EXPECT_EQ(seenInChild, "child-inherits") << "子协程没继承父协程的日志上下文";
    }
} // namespace AsynGyanis::Core
