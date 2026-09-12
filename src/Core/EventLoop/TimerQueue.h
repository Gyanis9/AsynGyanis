/**
 * @file TimerQueue.h
 * @brief 事件循环级的定时器队列：整个循环共用一个定时器描述符，按截止时间排序
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once


#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Platform/IO/TimerFileDescriptor.h"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 事件循环级定时器队列，所有定时等待共用循环的唯一定时器描述符
     *
     * @details 每个事件循环只持有一个定时器描述符（Linux 上是 timerfd，Windows 上是 loopback
     *          socketpair），全部等待按截止时间排在同一个最小堆里，代价因此是 1 个描述符加
     *          O(log N) 堆操作。驱动协程按堆顶截止时间武装描述符（复用 IoWatcher 的按需武装，
     *          空队列不武装），到期项投回调度器而非就地恢复，避免嵌套恢复别的协程。
     *
     * @note 线程约束：等待器的登记与取消都只发生在事件循环线程上（等待器活在协程帧里），
     *       因此队列内部不需要任何锁。
     */
    class TimerQueue
    {
    public:
        /**
         * @brief 一次定时等待：等待器本身就是队列里的登记项
         *
         * @details 登记项放在等待器里（即协程帧里），地址稳定，入堆的是它的地址；
         *          等待器析构即从队列摘除，因此提前销毁的等待不会留下悬空登记。
         */
        class Awaiter
        {
        public:
            /**
             * @brief 构造等待器并记下截止时间
             * @param queue 所属队列
             * @param duration 等待时长；非正数表示「尽快到期」（在下一个驱动周期内完成），
             *        而不是永不触发
             */
            Awaiter(TimerQueue &queue, std::chrono::milliseconds duration) noexcept;

            /**
             * @brief 析构时把自己从队列上摘除
             * @details 协程帧可能在等待期间被销毁（取消、异常展开）。此时等待器随帧析构，
             *          必须顺手取消登记，否则队列的堆里会留下指向已释放等待器的指针。
             */
            ~Awaiter();

            Awaiter(const Awaiter &) = delete;

            Awaiter &operator=(const Awaiter &) = delete;

            Awaiter(Awaiter &&) = delete;

            Awaiter &operator=(Awaiter &&) = delete;

            /**
             * @brief 定时等待从不立即就绪
             * @return false（到期判定由队列驱动协程统一做，这里不做重复判断）
             */
            [[nodiscard]] bool await_ready() const noexcept;

            /**
             * @brief 登记进队列并挂起协程
             * @param handle 当前协程句柄，到期时由队列投回调度器恢复
             */
            void await_suspend(std::coroutine_handle<> handle);

            /**
             * @brief 到期后的收尾（无返回值：等待只有「到期」一种结果）
             */
            void await_resume() const noexcept;

        private:
            friend class TimerQueue;

            TimerQueue *                            m_queue;    ///< 所属队列（非拥有）
            std::chrono::steady_clock::time_point   m_deadline; ///< 截止时间
            std::coroutine_handle<>                 m_handle{}; ///< 等待中的协程，空表示无人在等
            bool                                    m_isQueued{false}; ///< 是否仍在队列的堆里
        };

        /**
         * @brief 构造队列并启动驱动协程
         * @param loop 所属事件循环（提供定时器描述符、epoll 与调度器）
         * @throws Base::SystemException 定时器描述符创建失败（启动期不可恢复故障）
         */
        explicit TimerQueue(EventLoop &loop);

        /**
         * @brief 析构：销毁驱动协程、摘除描述符注册并关闭描述符
         * @details 堆里残留的等待者会被标记为「已不在队列里」，它们的协程帧随后由各自的持有者
         *          销毁，析构时不会再来访问本对象
         */
        ~TimerQueue();

        // 禁止拷贝与移动：驱动协程与注册对象都绑定在本对象的地址上
        TimerQueue(const TimerQueue &) = delete;

        TimerQueue &operator=(const TimerQueue &) = delete;

        TimerQueue(TimerQueue &&) = delete;

        TimerQueue &operator=(TimerQueue &&) = delete;

        /**
         * @brief 创建一个等待器
         * @param duration 等待时长；非正数表示尽快到期
         * @return Awaiter 等待器，可 co_await
         */
        [[nodiscard]] Awaiter waitFor(std::chrono::milliseconds duration);

        /**
         * @brief 查询仍在等待的定时器数量（监控/调试用）
         * @return std::size_t 队列里尚未到期的登记项个数
         */
        [[nodiscard]] std::size_t pendingCount() const noexcept;

    private:
        /**
         * @brief 最小堆的比较器：截止时间早的排前面
         * @param left 左操作数
         * @param right 右操作数
         * @return true 表示 left 应排在 right 之后
         * @note 必须是 TimerQueue 的成员（等待器的截止时间是私有的）
         */
        [[nodiscard]] static bool isLaterThan(const Awaiter *left, const Awaiter *right) noexcept;

        /**
         * @brief 队列驱动协程：武装描述符 → 等到可读 → 派发到期项
         * @return Task<> 驱动协程；描述符注册失效（队列收尾）时结束
         */
        Task<> drive();

        /**
         * @brief 把一个等待器登记进堆
         * @param awaiter 目标等待器（其截止时间与协程句柄须已就位）
         */
        void insert(Awaiter &awaiter);

        /**
         * @brief 从堆里摘除一个等待器（等待器析构时调用）
         * @param awaiter 目标等待器
         */
        void remove(Awaiter &awaiter) noexcept;

        /**
         * @brief 按堆顶截止时间重新武装（或解除武装）定时器描述符
         * @details 截止时间没变时不做任何系统调用；堆为空时解除武装，空闲循环因此零唤醒
         */
        void rearm() noexcept;

        /**
         * @brief 把已到期的等待者投回调度器，并按新的堆顶重新武装
         */
        void dispatchExpired();

        EventLoop                    &m_loop;         ///< 所属事件循环
        Platform::TimerFileDescriptor m_timer;        ///< 循环唯一的定时器描述符
        IoWatcher                     m_watcher;      ///< 它的常驻注册（等待时武装可读）
        std::vector<Awaiter *>        m_heap;         ///< 最小堆：按截止时间，早的在前
        std::optional<std::chrono::steady_clock::time_point> m_armedDeadline; ///< 已武装的截止时间
        bool                          m_isDriverStarted{false}; ///< 驱动协程是否已投递（首次登记时启动）
        Task<>                        m_driverTask;   ///< 驱动协程（最后声明，最先销毁）
    };

} // namespace AsynGyanis::Core
