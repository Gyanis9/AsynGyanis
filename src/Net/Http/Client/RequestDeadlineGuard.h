/**
 * @file RequestDeadlineGuard.h
 * @brief 出站请求的时限看门狗：到点就把被监视对象关掉，让挂在收发上的协程立刻收口
 * @author Gyanis
 * @date 2026-09-25
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include <chrono>
#include <optional>
#include <utility>

namespace AsynGyanis::Net
{
    /**
     * @brief 到期就关掉被监视对象的看门狗协程
     * @details 收发协程挂在套接字的等待器上，而等待器没有取消接口；「关掉」是本框架里让它们立刻收尾的
     *          既定手段（与 stop()/close() 收尾走同一条路径）。被监视对象只要有 `close()` 就行：套接字、
     *          池化连接、HTTP/2 客户端连接都满足。
     * @tparam WatchedType 被监视对象
     * @param loop 所属事件循环（提供定时器）
     * @param watched 被监视对象（非拥有；生命周期由调用方保证长于本协程）
     * @param timeout 时限
     * @param isCancelled 请求已结束的标志；为真时看门狗醒来什么都不做
     * @param description 日志里写的对象名，便于现场分辨是哪一层掐断了
     */
    template<typename WatchedType>
    Core::Task<void> watchRequestDeadline(Core::EventLoop &loop, WatchedType &watched,
                                          const std::chrono::milliseconds timeout, const bool &isCancelled,
                                          const std::string_view description)
    {
        Core::Timer timer(loop);
        co_await timer.waitFor(timeout);
        if (isCancelled)
        {
            co_return;
        }
        // 超时是「对端不说话」这类外部状况，调用方只会拿到一个空结果，因此在这里留一条日志：否则排查
        // 现场时只能看到「请求没结果」，看不出是被时限掐断的
        LOG_WARN_FMT("{}: 超过 {} 毫秒仍未完成，已关闭连接", description, timeout.count());
        watched.close();
        co_return;
    }

    /**
     * @brief 看门狗的持有者：析构（含异常展开）时先撤销看门狗，被监视对象随后才销毁
     * @details **声明顺序即安全**：把它声明在被监视对象之后，任何返回路径都会先销毁本对象，绝不留下
     *          一个还在等时限、醒来却要关一个已销毁对象的协程帧。撤销即销毁协程帧——帧里等待中的定时器
     *          随帧析构一起注销，不留常驻等待。
     * @tparam WatchedType 被监视对象
     */
    template<typename WatchedType>
    class RequestDeadlineGuard
    {
    public:
        /**
         * @brief 启动看门狗
         * @param loop 所属事件循环
         * @param watched 被监视对象
         * @param timeout 时限
         * @param description 日志里写的对象名
         */
        RequestDeadlineGuard(Core::EventLoop &loop, WatchedType &watched, const std::chrono::milliseconds timeout,
                             const std::string_view description)
            : m_watchdog(watchRequestDeadline(loop, watched, timeout, m_isCancelled, description))
        {
            m_watchdog->handle().resume(); // 惰性协程：手动启动
        }

        ~RequestDeadlineGuard()
        {
            m_isCancelled = true;
            m_watchdog.reset();
        }

        RequestDeadlineGuard(const RequestDeadlineGuard &) = delete;
        RequestDeadlineGuard &operator=(const RequestDeadlineGuard &) = delete;
        RequestDeadlineGuard(RequestDeadlineGuard &&) = delete;
        RequestDeadlineGuard &operator=(RequestDeadlineGuard &&) = delete;

    private:
        bool m_isCancelled{false};             ///< 请求已结束（看门狗协程按引用持有）
        std::optional<Core::Task<>> m_watchdog; ///< 看门狗协程帧：置空即撤销
    };
} // namespace AsynGyanis::Net
