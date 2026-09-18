/**
 * @file ConnectionDistributor.h
 * @brief 接收分发器：一个循环接受连接，轮流交给若干工作循环接手
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 把已接受的连接轮流交给若干工作循环的接收分发器
     *
     * @note distribute() 只允许在**接受循环线程**上调用（内部游标不是原子的）；从其它线程发起时
     *       经 `Scheduler::postRemote()` 投递过来，投递本身是线程安全的。
     * @note 工作循环若在回调执行前就退出，投递过去的回调会被丢弃——因此描述符被包在「没人接手
     *       就关闭」的句柄里一起投递，丢弃也不会漏描述符。
     * @details 内核分摊（SO_REUSEPORT）只在 Linux 上存在，Windows 需要一个用户态入口：接受循环
     *          只做 accept 与派发，连接建立与生命周期都发生在工作循环上，接受因此不会成为瓶颈
     *          或单点。取轮转而不是「挑当前连接最少的」：后者要跨循环读在途计数，引入同步还要
     *          处理陈旧值；轮转不需要任何跨循环状态，偏差由「连接被服务时才占资源」兜住。
     */
    class ConnectionDistributor
    {
    public:
        /**
         * @brief 工作循环接手一条连接的入口
         * @param fileDescriptor 已接受的连接描述符，所有权随回调转移；接手方必须立即接管
         */
        using Adopter = std::function<void(int fileDescriptor)>;

        ConnectionDistributor() = default;

        ConnectionDistributor(const ConnectionDistributor &) = delete;

        ConnectionDistributor &operator=(const ConnectionDistributor &) = delete;

        /**
         * @brief 登记一个工作循环及其接手动作
         * @param loop 工作循环，必须比本分发器活得久
         * @param adopter 接手动作，在 loop 所在线程上执行；空对象会被忽略
         */
        void addWorker(EventLoop &loop, Adopter adopter);

        /**
         * @brief 取已登记的工作循环数量
         * @return std::size_t 数量；为 0 时 distribute() 一律返回 false
         */
        [[nodiscard]] std::size_t workerCount() const noexcept;

        /**
         * @brief 把一条已接受的连接交给下一个工作循环
         * @param fileDescriptor 已接受的连接描述符
         * @return true 已投递给某个工作循环，描述符所有权随之转移，调用方不得再碰它
         * @return false 没有可用的工作循环、描述符本身无效，或交接句柄分配失败——
         *         这三种情形下描述符仍归调用方，需自行关闭
         * @note 一旦交接句柄建成，所有权就算交出去了：此后即便投递排不上队也返回 true
         *       （那条连接由句柄关闭），调用方不会对同一个号关第二次
         */
        [[nodiscard]] bool distribute(int fileDescriptor) noexcept;

        /**
         * @brief 取累计派发成功的连接数（用于观测与用例断言）
         * @return std::size_t 累计条数
         */
        [[nodiscard]] std::size_t distributedCount() const noexcept;

    private:
        /// 一个工作循环与它的接手动作
        struct Worker
        {
            EventLoop *loop{nullptr}; ///< 目标循环（非拥有）
            Adopter    adopter;       ///< 在该循环上执行一次，参数是连接描述符
        };

        std::vector<Worker> m_workers;           ///< 已登记的工作循环，轮转顺序即登记顺序
        std::size_t         m_nextWorkerIndex{0}; ///< 下一次派发给哪个工作循环
        std::size_t         m_distributedCount{0}; ///< 累计派发成功的连接数
    };
} // namespace AsynGyanis::Core
