/**
 * @file PerIpConnectionLimiter.h
 * @brief 按来源 IP 记账的并发连接限额：挡住「同一个来源开一堆连接」
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace AsynGyanis::Net
{
    /**
     * @brief 按来源 IP 记账的并发连接限额。
     *
     * @details 只记「当前有多少条来自该 IP 的连接还活着」，超限就拒；不做速率限制、不做黑白名单、
     *          不解析 IP 归属。它与 TcpServer::setMaxConnections 的全局上限互补：后者挡总量，本类挡单点。
     *
     * @par 为什么要求调用方显式共享同一份，而不是做成进程级单例
     *      内核按 SO_REUSEPORT 把新连接分摊给多个监听器（本框架是每循环一个），若每个服务器各自
     *      持一份计数，单 IP 的实际上限会乘上监听器数量，限额等于失效——所以本类设计成显式传入、
     *      由调用方在所有监听器之间共享同一份。不做进程级单例还有一个原因：本仓库的用例并行执行，
     *      全局可变状态会让互不相干的用例互相污染计数。
     *
     * @note 线程安全：多个循环线程会并发 tryAcquire/Lease 析构，内部一把互斥量保护计数表。
     *       取名额发生在 accept 之后、建连之前，不在每请求路径上，锁竞争与连接建立同量级。
     * @warning 判据是**TCP 对端地址**，因此同一台 NAT 后面的多个客户端会被算作一个来源——限额按
     *          「一个来源」给，取值要按这个前提定，不要当成「每个用户」。
     * @see TcpServer::setPerIpConnectionLimiter()
     */
    class PerIpConnectionLimiter
    {
    private:
        /// 计数表本体。与租约共享所有权：租约可能比限额对象活得更久（收尾期的连接协程晚于服务器析构）
        struct State
        {
            std::mutex                                   mutex;        ///< 保护下面这张表
            std::unordered_map<std::string, std::size_t> activeCounts; ///< 来源地址 → 当前活跃连接数（计数为 0 的条目即时删除）
        };

    public:
        /**
         * @brief 一次「已占名额」的凭据：析构即归还。
         *
         * @details 用它而不是让调用方自己配对 acquire/release，是为了让归还包括异常路径在内的
         *          每一条出口都成立；限额关闭时它是空壳（isTracking() 为 false），析构什么都不做。
         * @note 只可移动不可拷贝：拷贝会让两份凭据归还同一个名额，计数越还越少。
         */
        class Lease
        {
        public:
            /// 构造空壳凭据（不占任何名额），用于「限额关闭」这条路径
            Lease() = default;

            Lease(const Lease &) = delete;

            Lease &operator=(const Lease &) = delete;

            /**
             * @brief 移动构造：名额随凭据转移，被移走的一方变成空壳
             * @param other 交出名额的凭据
             */
            Lease(Lease &&other) noexcept;

            /**
             * @brief 移动赋值：先归还自己手里的名额，再接管对方的
             * @param other 交出名额的凭据
             * @return Lease& 本对象
             */
            Lease &operator=(Lease &&other) noexcept;

            /// @brief 析构：归还名额（空壳时什么都不做）
            ~Lease();

            /**
             * @brief 本凭据是否真的占着一个名额
             * @return true 占着（析构时会归还）；false 是限额关闭时拿到的空壳
             */
            [[nodiscard]] bool isTracking() const noexcept;

        private:
            friend class PerIpConnectionLimiter;

            /// 由限额对象构造出真正占名额的凭据
            Lease(std::shared_ptr<State> state, std::string ipKey);

            /// 归还并把自己置空（move 与析构共用的唯一出口，避免两处各写一遍）
            void releaseIfTracking() noexcept;

            std::shared_ptr<State> m_state; ///< 计数表；空表示本凭据没占名额
            std::string            m_ipKey; ///< 归还时用它找回计数条目
        };

        /**
         * @brief 构造限额
         * @param maximumConnectionsPerIp 单个来源允许同时存活的连接条数；**0 表示关闭该项保护**
         *        （与框架里其它限额字段的 0 语义一致：是「不设这项保护」而不是「一条都不许」）
         */
        explicit PerIpConnectionLimiter(std::size_t maximumConnectionsPerIp);

        PerIpConnectionLimiter(const PerIpConnectionLimiter &) = delete;

        PerIpConnectionLimiter &operator=(const PerIpConnectionLimiter &) = delete;

        /**
         * @brief 为某个来源占用一个名额
         * @param ipKey 来源标识，**只含地址、不含端口**（调用方传 InetAddress::ip()）。带上端口就等于
         *        按连接计数——每条连接的端口都不同，限额永远碰不到
         * @return std::optional<Lease> 成功时返回持有名额的凭据；该来源已达上限时返回空 optional；
         *         限额为 0 时返回的是不占名额的空壳凭据
         * @note 返回的凭据必须活得与连接一样久：它一析构，名额就还回去了
         */
        [[nodiscard]] std::optional<Lease> tryAcquire(std::string ipKey);

        /**
         * @brief 查某个来源当前的活跃连接数
         * @param ipKey 来源标识
         * @return std::size_t 活跃条数；没见过的来源为 0
         * @note 供观测与测试使用，不参与判定
         */
        [[nodiscard]] std::size_t activeCountFor(const std::string &ipKey) const;

    private:
        /**
         * @brief 归还一个名额（只由 Lease 调用）
         * @param state 计数表
         * @param ipKey 来源标识
         */
        static void release(State &state, const std::string &ipKey);

        std::shared_ptr<State> m_state;                    ///< 计数表，与租约共享
        std::size_t            m_maximumConnectionsPerIp;  ///< 单个来源的上限，0 表示关闭该项保护
    };

} // namespace AsynGyanis::Net
