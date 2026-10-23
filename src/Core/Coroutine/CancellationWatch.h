/**
 * @file CancellationWatch.h
 * @brief 把 std::stop_token 的停止请求投到事件循环上的看门狗
 * @author Gyanis
 * @date 2026-10-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace AsynGyanis::Core
{
    /**
     * @brief 在所属循环上轮询停止令牌；请求停止时执行一次取消动作
     * @details 令牌可能由任意线程请求停止，而取消动作要碰连接、流与协议层，那些对象只归
     *          所属循环线程。看门狗因此跑在循环上，只轮询一个线程安全的读数。
     *          动作在析构（请求返回）时被撤销，不会留下一条醒来碰已销毁对象的协程。
     */
    class CancellationWatch
    {
    public:
        /// 取消动作：在所属循环线程上执行一次
        using CancelAction = std::function<Task<>(std::stop_token)>;

        /**
         * @brief 启动看门狗；令牌不可停止时什么都不做
         * @param loop 所属事件循环
         * @param token 调用方的停止令牌
         * @param action 请求停止时执行的动作
         */
        CancellationWatch(EventLoop &loop, const std::stop_token token, CancelAction action) : m_finished(std::make_shared<std::atomic<bool>>(false))
        {
            if (!token.stop_possible())
            {
                return;
            }
            m_task.emplace(watch(loop, token, m_finished, std::move(action)));
            loop.scheduler().schedule(m_task->handle());
        }

        /// 撤销看门狗：先置完成标志，再销毁帧（帧里的定时器随之注销）
        ~CancellationWatch()
        {
            if (m_finished != nullptr)
            {
                m_finished->store(true, std::memory_order_release);
            }
            m_task.reset();
        }

        CancellationWatch(const CancellationWatch &)            = delete;
        CancellationWatch &operator=(const CancellationWatch &) = delete;

    private:
        static Task<> watch(EventLoop &loop, const std::stop_token token, const std::shared_ptr<std::atomic<bool>> &finished, CancelAction action)
        {
            Timer timer(loop);
            while (!finished->load(std::memory_order_acquire))
            {
                if (token.stop_requested())
                {
                    co_await action(token);
                    co_return;
                }
                co_await timer.waitFor(std::chrono::milliseconds{5});
            }
        }

        std::shared_ptr<std::atomic<bool>> m_finished; ///< 请求已收尾的标志（看门狗按引用持有）
        std::optional<Task<>>              m_task;     ///< 看门狗协程帧：置空即撤销
    };
} // namespace AsynGyanis::Core
