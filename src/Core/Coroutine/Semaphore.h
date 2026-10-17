/**
 * @file Semaphore.h
 * @brief 协程计数信号量：同一条事件循环上的协程按申请先后排队取许可
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 一次「已到手但还没恢复」的等待凭证：将恢复动作与等待器是否还活着分开
     *
     * @details 放行许可的一方把「恢复谁」投回循环，而那一刻之后调用方随时可能销毁 Task（帧连同
     *          等待器一起析构）。投出去的若是裸协程句柄，循环那边就会 resume 一块已释放的帧——那是
     *          释放后使用，且裸句柄一旦投出去就摘不回来。凭证由等待器与投递方共享：等待器析构
     *          时把里面的句柄清空（`revoke()`），投递方执行时看到空句柄就什么都不做。
     * @note 与 `ConnectionPool` 内部的恢复票据同一目的；这里抽出来是因为信号量、读写锁、通道三类
     *       等待都需要「可撤销的唤醒」，逐处复造最容易漏的正是撤销那一步。
     */
    class ASYN_CORE_API CoroutineResumeTicket final
    {
    public:
        /**
         * @brief 登记要恢复的协程（等待器挂起时调用）
         * @param handle 挂在等待器上的协程句柄
         */
        void attach(const std::coroutine_handle<> handle) noexcept;

        /**
         * @brief 撤销唤醒：等待器析构（含协程帧被销毁、异常展开）时调用
         */
        void revoke() noexcept;

        /**
         * @brief 取走句柄并恢复协程；已被取走或已撤销时空操作
         * @details 同一个句柄只允许被恢复一次，因此这里一律「先 exchange 取走再恢复」，不留
         *          「先判后取」的窗口。协程体的异常按惯例由 `Task` 的 promise 存住、穿不出 `resume()`；
         *          剩下的窗口是帧分配失败与非 Task 的协程，那两种就地丢弃本次唤醒并出声，不让它
         *          带走整条循环（本函数由调度器执行，抛穿出去就是 terminate）。
         * @note 全程 `noexcept` 是对外契约而非修饰：它跑在别人的循环线程上，抛穿出去就是 terminate，
         *       而它带不走一次日志。
         * @note 不是 const：`exchange` 要改原子本身，常量版本既过不了 MSVC 也过不了 GCC，
         *       与连接池那张恢复票据同一条限制
         */
        void resumeOnce() noexcept;

    private:
        /**
         * @brief 待恢复的协程，撤销后为空
         * @details 必须是原子的：清空发生在等待器（可能在任意线程上被销毁的帧）的析构里，读取发生在
         *          投回循环的执行体里，两者之间除了共享指针的引用计数之外没有任何同步关系；非原子字段
         *          就是数据竞争，坏掉的现场是 resume 一块已释放的帧
         */
        std::atomic<std::coroutine_handle<>> m_handle{nullptr};
    };

    /**
     * @brief 计数信号量：把「同一时刻最多 N 个协程在做某事」写成一条可 `co_await` 的队
     *
     * @details 形状取自业界同类件，逐格对照（本仓为什么这么定，理由跟在每一格后面）：
     *          - 排队：严格先进先出，与 asio `async_semaphore`、seastar `semaphore`、tokio
     *            `Semaphore::acquire`（文档原文：用队列按申请顺序公平发放许可）一致。队头要 5 个而
     *            手上只有 4 个时，后面只要 1 个的等待者也必须排队（不接受「跳过队头先满足小的」）：
     *            吞吐换可预测，饿死与「大请求永远被小请求插队」在服务器负载下更难受。
     *          - 不越队抢：`acquire()` 的快速通道只在「队伍为空且额度够」时才当场取走；队伍非空时
     *            连额度都不看。`tryAcquire()` 与 tokio 的 `try_acquire` 同一条口径：它不参与公平性，
     *            手上有额度就拿走，哪怕前面排着人——调用方选这条口就是在要吞吐而不是要顺序。
     *          - 归还：许可由 `Permit` 的析构归还（与 asio 的 `guard_type`、tokio 的 permit `Drop`
     *            一致）。不另设 `Permit::release()`：`std::optional<Permit>::reset()` 已经是同一个动作，
     *            再开一口就是两个真源。
     *          - 唤醒时机：许可在放行那一刻就记账给队头（互斥性当场成立），恢复动作排进循环的
     *            下一拍而不是就地 `resume()`。就地恢复等于在别人的析构里跑别人的协程：`Permit` 的析构
     *            可能正夹在中间，被唤醒的一方若顺手销毁了持有本信号量的对象，返回时摸到的就是已释放的
     *            `this`。这条与 `TimerQueue` 的「到期项投回调度器而非就地恢复」同一目的。
     *          - 容量：容量等于初始许可数，之后不增长。放行超出容量是调用方多还了一次（重复释放），
     *            公开口 `release()` 当场抛，`Permit` 那条 noexcept 路径记错误日志并夹住——两种处置都
     *            比「悄悄把门多开一格」好查。
     *
     * @note 线程约束：本对象只归一条事件循环，登记、放行、析构都必须在所属循环线程上做，内部
     *       因此一把锁都不需要（与 `TimerQueue` 同一条口径）。外来线程要参与，先 `postRemote()` 投进
     *       那条循环再 `co_await`；判据命中时抛 `Base::LogicException` 而不是静默排队——静默会把
     *       「两个线程各排各的队」这种坏掉的状态留到运行时才炸。
     * @note 生命周期：本对象必须活得比它放出去的每一个 `Permit`、以及每一个还在等的协程久。
     *       销毁时还挂着等待者，本对象把每个等待器标记成「已脱离队列」并留下告警：它们再不会被人叫醒，
     *       协程帧交给各自的持有者销毁（与 `TimerQueue` 停摆后的收尾一致）。已经拿到许可却还没恢复的
     *       等待者醒来时拿到空结果，调用方按「这一轮没拿到许可」结账。
     * @see Mutex 用同一个许可类型实现的独占锁
     */
    class ASYN_CORE_API Semaphore
    {
    public:
        class ASYN_CORE_API Permit;
        class ASYN_CORE_API Awaiter;

        /**
         * @brief 构造一个容量为 initialPermitCount 的信号量
         * @param loop 所属事件循环：唤醒等待者要投回这条循环的调度器
         * @param initialPermitCount 许可总数（也就是同一时刻允许的并发数），此后不增长；0 合法，
         *        表示「一开始谁都拿不到」，由别的协程用 `release()` 往里放
         */
        Semaphore(EventLoop &loop, std::size_t initialPermitCount) noexcept;

        /**
         * @brief 析构：把还挂着的等待者标记成「已脱离队列」，并在许可没全回来时留下告警
         * @details 不叫醒任何人：等待中的协程由各自的持有者销毁，已经投递但尚未执行的唤醒凭证
         *          会在执行时发现句柄已被撤销（见 CoroutineResumeTicket）。
         */
        ~Semaphore();

        Semaphore(const Semaphore &)            = delete;
        Semaphore &operator=(const Semaphore &) = delete;
        Semaphore(Semaphore &&)                 = delete;
        Semaphore &operator=(Semaphore &&)      = delete;

        /**
         * @brief 取许可：拿不到就挂起，按申请先后被叫醒
         * @details 返回值是等待器，用法是 `auto permit = co_await semaphore.acquire(n);`。许可在
         *          放行那一刻就已记在本等待器账上，因此中间没有「被插队拿走」的窗口。
         * @param permitCount 一次要取几个许可；默认 1
         * @return Awaiter 可等待对象；`co_await` 的结果是 `std::optional<Permit>`，为空表示本信号量
         *         在等待期间被销毁了（调用方按「没拿到许可」收尾，不要继续往下做需要许可的事）
         * @throws Base::InvalidArgumentException permitCount 为 0（要表达什么不由本类猜）
         * @throws Base::LogicException 调用者不在所属循环线程上
         */
        [[nodiscard]] Awaiter acquire(std::size_t permitCount = 1);

        /**
         * @brief 取许可：拿不到就当场返回空，不挂起也不排队
         * @details 本口不参与公平性：队伍里排着人时，只要手上还有额度就照样拿走（与 tokio
         *          `try_acquire` 同一条口径）。要顺序就用 `acquire()`，要立即判定就用本口。
         * @param permitCount 一次要取几个许可；默认 1
         * @return std::optional<Permit> 拿到就有值（析构即归还），没拿到为空
         * @throws Base::InvalidArgumentException permitCount 为 0
         * @throws Base::LogicException 调用者不在所属循环线程上
         */
        [[nodiscard]] std::optional<Permit> tryAcquire(std::size_t permitCount = 1);

        /**
         * @brief 直接放回许可：不经过 `Permit` 的那条口，给「许可不在对象上而在计数上」的用法用
         * @details 常规用法是让 `Permit` 出作用域自动归还；本口留给没有 RAII 载体可挂的场景
         *          （如按字节额度记账的预算器），用了它就要自己保证不多放。
         * @param permitCount 放回几个许可；默认 1
         * @throws Base::InvalidArgumentException permitCount 为 0
         * @throws Base::LogicException 放回后总许可数会超过容量（多放了一次），或调用者不在所属循环线程上
         */
        void release(std::size_t permitCount = 1);

        /**
         * @brief 当前可立即发放的许可数
         * @details 已经放出去的（`Permit` 还在调用方手上）、以及已记给队头但还没恢复的都不算在这一格里，
         *          所以这一格回答的是「现在还能给谁」，不是「总共剩多少」。
         * @return std::size_t 可立即发放的许可数
         */
        [[nodiscard]] std::size_t availablePermitCount() const noexcept;

        /**
         * @brief 还在等的请求个数
         * @details 含「许可已记到账、正等下一拍恢复」的那几个（它们确实还没回到自己的协程里），
         *          不含已经取走许可的——取走的那一刻就离队了。
         * @return std::size_t 个数，0 表示队伍是空的
         */
        [[nodiscard]] std::size_t waitingCount() const noexcept;

    private:
        friend class Permit;
        friend class Awaiter;

        /**
         * @brief 放回许可这一动作来自哪条口：两条口的失败处置不同，判据只写一遍
         * @details 公开 `release()` 抛出（调用点不在析构路径上，抛得出去）；`Permit` 的归还走
         *          noexcept 的内部口——多还的账要出声，但不能让一次归还把进程 terminate。
         */
        enum class ReturnSource
        {
            PublicRelease, ///< 调用方直接调 release()
            PermitReturn   ///< Permit 析构或移动后交出
        };

        /**
         * @brief 把等待器挂到队尾（挂起这一步用）
         * @param awaiter 要挂上的等待器
         */
        void linkAwaiter(Awaiter &awaiter) noexcept;

        /**
         * @brief 把等待器从队上摘掉（醒来收尾与帧被销毁都用）
         * @param awaiter 要摘掉的等待器；没挂着时是空操作
         */
        void unlinkAwaiter(Awaiter &awaiter) noexcept;

        /**
         * @brief 从队头开始把额度记给等待者，并把要恢复的凭证投回循环下一拍
         * @details 严格按顺序：遇到第一个「满足不了的未授额等待者」就停，后面的即使要得少也不越过它。
         *          已经记过账的队头直接跳过（它的许可已经花掉了，不占额度）。
         */
        void grantFromHead();

        /**
         * @brief 归还许可并在放行之后的那一刻结算队伍
         * @param permitCount 归还的个数
         * @param source 归还来自哪条口（决定超容量时是抛还是记日志夹住）
         */
        void returnPermits(std::size_t permitCount, ReturnSource source);

        /**
         * @brief 判据：调用者必须在所属循环线程上，否则抛出
         * @param operation 调用点的操作名（只用于报错文本）
         */
        void assertOwnerThread(const char *operation) const;

        EventLoop  &m_loop;                 ///< 所属循环：唤醒投回它的调度器，并给出线程判据
        std::size_t m_capacity;             ///< 许可总数（构造时定下，不增长）
        std::size_t m_availablePermitCount; ///< 还没被记出去的许可数
        Awaiter    *m_waiterHead{nullptr};  ///< 队伍头（申请最早的排在前）
        Awaiter    *m_waiterTail{nullptr};  ///< 队伍尾
        std::size_t m_waitingCount{0};      ///< 队伍长度（含已记账待恢复的）
    };

    /**
     * @brief 一沓已经到手的许可：析构即放回信号量
     *
     * @details 移动语义照 `Task` 的写法：交出所有权的一侧不再放回（`m_permitCount` 清零），
     *          避免同一份许可被还两次。放回动作本身见 Semaphore::returnPermits()。
     * @note 本对象必须在所属信号量之前销毁——析构要摸回信号量记账，没有存活判据。
     */
    class ASYN_CORE_API Semaphore::Permit
    {
    public:
        Permit(const Permit &)            = delete;
        Permit &operator=(const Permit &) = delete;

        /**
         * @brief 交出许可的所有权（移动构造）
         * @param other 交出的一侧：之后不再持有任何许可
         */
        Permit(Permit &&other) noexcept;

        /**
         * @brief 交出许可的所有权（移动赋值）：先把手上的还给信号量再接管
         * @param other 交出的一侧
         * @return Permit& 接管后的本对象
         */
        Permit &operator=(Permit &&other) noexcept;

        /**
         * @brief 析构：把手上的许可放回信号量
         */
        ~Permit();

        /**
         * @brief 这份许可里有几个许可
         * @return std::size_t 恒大于 0（取许可时 0 已被拒绝）
         */
        [[nodiscard]] std::size_t permitCount() const noexcept;

    private:
        friend class Semaphore;

        /**
         * @brief 由信号量在记账给本方时构造
         * @param semaphore 来源信号量
         * @param permitCount 记下的个数
         */
        explicit Permit(Semaphore &semaphore, std::size_t permitCount) noexcept;

        Semaphore  *m_semaphore{nullptr}; ///< 来源信号量；交出所有权后置空，析构据此决定放不放回
        std::size_t m_permitCount{0};     ///< 手上的许可个数
    };

    /**
     * @brief 一次许可等待：等待器本身就是队列上的节点，因此挂起这一步不需要分配
     *
     * @details 节点放在协程帧里（地址稳定），入队挂的是它自己；帧若在等待期间被销毁，析构顺手把自己
     *          从队上摘掉，并把已记账却没来得及用的许可还回去——不还就是许可泄漏，队伍会永久堵死。
     * @note 与 Semaphore 同一条线程约束：登记与摘除只发生在所属循环线程。
     */
    class ASYN_CORE_API Semaphore::Awaiter
    {
    public:
        /**
         * @brief 析构：从队上摘除自己，并归还已记账未使用的许可
         */
        ~Awaiter();

        Awaiter(const Awaiter &)            = delete;
        Awaiter &operator=(const Awaiter &) = delete;
        Awaiter(Awaiter &&)                 = delete;
        Awaiter &operator=(Awaiter &&)      = delete;

        /**
         * @brief 能不能当场拿走
         * @details 队伍为空且额度够时直接记账给本方并返回 true（快速通道，省掉一次挂起）；
         *          队伍非空时一律返回 false——这就是「不越队抢」的落点，许可只能按申请顺序给。
         * @note 会改状态（可能当场记走许可），因此不是 const：语言允许 awaiter 的这条判据非常量，
         *       `TimerQueue` 的等待器是常量版本，因为那边只需要登记截止时间、不需要抢额度
         * @return true 许可已到手，不必挂起；false 需要挂起排队
         */
        [[nodiscard]] bool await_ready() noexcept;

        /**
         * @brief 挂起并把本等待器挂到队尾
         * @param handle 当前协程句柄
         * @return true 已挂起，等放行的一方按顺序把许可记到本方账上；false 凭证分配不出来，
         *         本次不挂起（`await_resume` 随即给出空结果，调用方按「没拿到许可」结账）
         * @note 本函数不检查线程：`acquire()` 已经查过，两次调用之间不可能换线程（同一条协程）
         */
        bool await_suspend(const std::coroutine_handle<> handle);

        /**
         * @brief 醒来取许可
         * @return std::optional<Permit> 拿到就有值；为空只有两种成因——本信号量在等待期间被销毁
         *         （许可随之作废），或凭证分配失败、本次根本没挂上队。调用方按「没拿到许可」收尾
         */
        [[nodiscard]] std::optional<Permit> await_resume();

    private:
        friend class Semaphore;

        /**
         * @brief 还挂在队上时把自己摘下来（析构用；没挂着时是空操作）
         */
        void unlinkIfLinked() noexcept;

        /**
         * @brief 由 Semaphore::acquire() 构造
         * @param semaphore 来源信号量（析构期被标记成空，见 Semaphore::~Semaphore）
         * @param permitCount 一次要取几个许可
         */
        Awaiter(Semaphore &semaphore, std::size_t permitCount) noexcept;

        Semaphore                             *m_semaphore;         ///< 来源信号量；脱离队列（本对象被析构方销毁）后为空
        std::size_t                            m_permitCount;       ///< 要取的个数
        std::size_t                            m_grantedCount{0};   ///< 已记到本方账上、还没被 await_resume 取走的个数；非 0 即「已授额」
        bool                                   m_isLinked{false};   ///< 是否还挂在队上，析构据此决定要不要摘链
        std::shared_ptr<CoroutineResumeTicket> m_ticket;            ///< 唤醒凭证：挂起时才分配，撤销由析构负责
        Awaiter                               *m_previous{nullptr}; ///< 队上前一项
        Awaiter                               *m_next{nullptr};     ///< 队上后一项
    };
} // namespace AsynGyanis::Core
