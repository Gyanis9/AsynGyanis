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
#include <deque>
#include <optional>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 事件循环级定时器队列，所有定时等待共用循环的唯一定时器描述符
     *
     * @note 线程约束：等待器的登记与取消都只发生在事件循环线程上（等待器活在协程帧里），
     *       队列内部因此不需要任何锁。
     * @details 每个循环只持有一个定时器描述符（Linux 上是 timerfd，Windows 上是 loopback
     *          socketpair），全部等待按截止时间排在同一个最小堆里，代价是 1 个描述符加 O(log N)
     *          堆操作；驱动协程按堆顶截止时间武装描述符（空队列不武装），到期项投回调度器而非
     *          就地恢复，避免嵌套恢复别的协程。
     */
    namespace detail
    {
        /**
         * @brief 把绝对截止时间换算成描述符要武装的时长
         * @details 一律**向上**取整：描述符只认毫秒，向下取整会让定时器最多提前 1 毫秒醒来，
         *          那一刻没有任何到期项，驱动只能再武装一次——每次到期白多一发
         *          timerfd_settime 与一次循环唤醒，而定时器密集（每连接一个 tick）时这是
         *          成倍的事件循环开销。向上取整最迟晚 1 毫秒触发，且不再有空转的一拍。
         * @param deadline 目标截止时间
         * @param now 换算时刻（由调用方给出，使这条换算能被确定性地测出来）
         * @return std::chrono::milliseconds 要武装的时长；已到期时返回 1 毫秒（0 会被描述符当成解除武装）
         */
        inline std::chrono::milliseconds armedDurationFor(const std::chrono::steady_clock::time_point &deadline, const std::chrono::steady_clock::time_point &now) noexcept
        {
            const auto remaining = deadline - now;
            const auto floored   = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            // 只比「被截掉的那一段」的正负，不构造 now + floored：截止时间允许饱和到
            // time_point::max()，加一个毫秒会把它加溢出
            const auto truncated = remaining - floored;
            const auto roundedUp = floored + (truncated > decltype(truncated)::zero() ? std::chrono::milliseconds(1) : std::chrono::milliseconds(0));
            return roundedUp > std::chrono::milliseconds(0) ? roundedUp : std::chrono::milliseconds(1);
        }
    } // namespace detail

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
             * @brief 构造等待器并记下等待时长
             * @param queue 所属队列
             * @param duration 等待时长；非正数表示「尽快到期」（在下一个驱动周期内完成），
             *        而不是永不触发
             * @note 截止时间在**真正挂起时**（await_suspend）才按此刻 + 时长算出，
             *       因此 `auto awaiter = timer.waitFor(1s); …先干别的…; co_await awaiter;`
             *       等的是「挂起时刻起 1 秒」，而不是构造时刻起 1 秒
             */
            Awaiter(TimerQueue &queue, std::chrono::milliseconds duration) noexcept;

            /**
             * @brief 析构时把自己从队列上摘除
             * @details 协程帧可能在等待期间被销毁（取消、异常展开）。此时等待器随帧析构，
             *          必须顺手取消登记：堆里残留的是指向已释放等待器的指针，而已经收集待恢复
             *          的那一份则会变成一次对已释放帧的 resume——两者都是释放后使用。
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
             * @param handle 当前协程句柄，到期时由队列恢复
             * @return true 已登记，可以挂起
             * @return false 队列已停摆（定时器描述符坏了，再不会有到期通知），不挂起、
             *         立即以「已到期」结束等待，避免业务永远卡在一次不会到来的定时上
             */
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

            /**
             * @brief 到期后的收尾（无返回值：等待只有「到期」一种结果）
             */
            void await_resume() const noexcept;

        private:
            friend class TimerQueue;

            /**
             * @brief 按「此刻 + 时长」算出截止时间，溢出方向取远
             * @details 时长可能大得加不进时间点（例如 milliseconds::max()），直接相加会溢出成
             *          一个过去的时刻，等待于是立刻完成——与调用方「等很久」的本意相反。
             *          这里把结果饱和到时间点最大值，等待因此按「很久」处理
             */
            void computeDeadline() noexcept;

            TimerQueue                           *m_queue;                  ///< 所属队列（非拥有）
            std::chrono::milliseconds             m_duration{};             ///< 等待时长（挂起时才折算成截止时间）
            std::chrono::steady_clock::time_point m_deadline{};             ///< 截止时间（await_suspend 时算出）
            std::coroutine_handle<>               m_handle{};               ///< 等待中的协程，空表示无人在等
            bool                                  m_isQueued{false};        ///< 是否仍在队列的堆里
            bool                                  m_isPendingResume{false}; ///< 已到期、尚待恢复（在队列的待恢复表里）
            std::size_t                           m_heapIndex{0};           ///< 在堆数组里的下标，仅 m_isQueued 为真时有效
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
         * @brief 交换堆中两个下标上的等待器，并回写它们的堆下标
         * @param leftIndex 左侧下标
         * @param rightIndex 右侧下标
         * @note 堆内指针只能经此函数交换：漏回写下标会让后续取消定位到错误的槽位
         */
        void swapHeapAt(std::size_t leftIndex, std::size_t rightIndex) noexcept;

        /**
         * @brief 让下标处的等待器上浮到正确位置（新登记与取消补位后用）
         * @param index 起始下标
         * @return std::size_t 上浮结束后的下标
         */
        std::size_t siftAwaiterUp(std::size_t index) noexcept;

        /**
         * @brief 让下标处的等待器下沉到正确位置（取堆顶与取消补位后用）
         * @param index 起始下标
         * @return std::size_t 下沉结束后的下标
         */
        std::size_t siftAwaiterDown(std::size_t index) noexcept;

        /**
         * @brief 摘走堆顶等待器并维持堆序，同时把它的「仍在堆里」标记清掉
         * @return Awaiter * 原堆顶（调用方负责接着处理）
         * @note 前置条件是堆非空
         */
        [[nodiscard]] Awaiter *takeHeapTop() noexcept;

        /**
         * @brief 队列驱动协程：武装描述符 → 等到可读 → 派发到期项
         * @return Task<> 驱动协程；描述符注册失效（队列收尾）时结束
         */
        Task<> drive();

        /**
         * @brief 驱动协程的三态
         */
        enum class DriverState
        {
            Idle,    ///< 尚未启动（还没人用过定时器）
            Running, ///< 已投递并在跑
            Dead     ///< 注册已失效，再不会有到期通知（此后登记一律立即完成）
        };

        /**
         * @brief 把一个等待器登记进堆
         * @param awaiter 目标等待器（其截止时间与协程句柄须已就位）
         * @return true 已登记；false 队列已停摆，调用方不应挂起
         */
        [[nodiscard]] bool insert(Awaiter &awaiter);

        /**
         * @brief 从堆里摘除一个等待器（等待器析构时调用）
         * @param awaiter 目标等待器
         */
        void remove(Awaiter &awaiter) noexcept;

        /**
         * @brief 把一个等待器从「待恢复表」里摘除（等待器析构时调用）
         * @param awaiter 目标等待器
         */
        void cancelPendingResume(Awaiter &awaiter) noexcept;

        /**
         * @brief 按堆顶截止时间重新武装（或解除武装）定时器描述符
         * @details 截止时间没变时不做任何系统调用；堆为空时解除武装，空闲循环因此零唤醒
         * @return true 内核状态已按当前队列需要到位；false 武装失败（描述符坏了），
         *         本次不记账，下一次 rearm 会重试
         */
        [[nodiscard]] bool rearm() noexcept;

        /**
         * @brief 把已到期的等待者收进待恢复表，并按新的堆顶重新武装
         */
        void dispatchExpired();

        /**
         * @brief 恢复所有已到期、且仍然活着的等待者
         * @details 由调度器在本轮清空里执行（见 dispatchExpired 的说明）。逐个从待恢复表摘下
         *          再就地恢复：被恢复的代码可能销毁表中其它等待器的帧，那些等待器析构时会
         *          把自己从表里摘掉，因此每轮都重新取表头、绝不缓存表内地址
         */
        void resumeExpired();

        /**
         * @brief 队列停摆后的收尾：堆里的等待者不再有人叫醒，交给各自持有者销毁
         */
        void abandonPendingTimers() noexcept;

        EventLoop                                           &m_loop;                           ///< 所属事件循环
        Platform::TimerFileDescriptor                        m_timer;                          ///< 循环唯一的定时器描述符
        IoWatcher                                            m_watcher;                        ///< 它的常驻注册（等待时武装可读）
        std::vector<Awaiter *>                               m_heap;                           ///< 最小堆：按截止时间，早的在前
        std::deque<Awaiter *>                                m_expiredAwaiters;                ///< 已到期待恢复：按截止时间升序，恢复在下一拍做
        std::optional<std::chrono::steady_clock::time_point> m_armedDeadline;                  ///< 已武装的截止时间
        DriverState                                          m_driverState{DriverState::Idle}; ///< 驱动协程状态（首次登记时启动）
        Task<>                                               m_driverTask;                     ///< 驱动协程（最后声明，最先销毁）
    };

} // namespace AsynGyanis::Core
