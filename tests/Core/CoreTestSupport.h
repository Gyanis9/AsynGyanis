/**
 * @file CoreTestSupport.h
 * @brief Core 模块单元测试辅助：有界等待、事件泵推进与就绪分发
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"

#include <chrono>
#include <cstddef>
#include <thread>

namespace AsynGyanis::Core::TestSupport
{
    /// 等待类断言的统一上限：正常耗时都在毫秒级，给足余量但不许无界等待
    inline constexpr std::chrono::milliseconds kWaitTimeout{5000};

    /// 事件泵的单步等待（毫秒）：够短到不拖慢用例，又够长到让刚发出的字节到达
    inline constexpr int kPumpStepMilliseconds = 5;

    /**
     * @brief 在时限内轮询等待条件成立（避免固定 sleep 造成的偶发失败）
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 待轮询的条件
     * @param timeout 超时上限，默认 kWaitTimeout
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const std::chrono::milliseconds timeout = kWaitTimeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

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
} // namespace AsynGyanis::Core::TestSupport
