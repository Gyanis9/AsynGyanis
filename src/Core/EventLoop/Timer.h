/**
 * @file Timer.h
 * @brief 可 co_await 的定时器：事件循环级定时器队列的轻量句柄
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once


#include "Core/EventLoop/TimerQueue.h"

#include <chrono>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 定时器：`co_await timer.waitFor(duration)` 即可延迟指定时长
     *
     * @note 定时等待全部登记在所属循环的 `TimerQueue` 上，因此本对象构造与析构都不产生系统调用，
     *       可按连接、按请求放心创建；到期精度由队列统一的毫秒级节拍决定。
     * @details 循环停止后未到期的等待不会自行触发（与「循环停了就没有事件」一致），因此必须在
     *          事件循环运行期间等待；同刻度到期的多个定时器按截止时间顺序恢复。
     */
    class Timer
    {
    public:
        /**
         * @brief 定时等待的等待器（等价于队列的等待器）
         */
        using Awaiter = TimerQueue::Awaiter;

        /**
         * @brief 构造定时器
         * @param loop 事件循环，提供定时器队列
         */
        explicit Timer(EventLoop &loop) noexcept;

        /**
         * @brief 析构函数：无副作用（不持有描述符，也没有需要注销的注册）
         */
        ~Timer();

        /**
         * @brief 创建一个等待器，使得 co_await timer.waitFor(duration) 延迟指定时长
         * @param duration 需要等待的时长；非正数表示尽快到期（下一个驱动周期内）
         * @return Awaiter 对象，可用于 co_await
         */
        Awaiter waitFor(std::chrono::milliseconds duration);

    private:
        TimerQueue &m_queue; ///< 所属循环的定时器队列（非拥有，循环比定时器活得久）
    };

}
