/**
 * @file EventLoop.h
 * @brief 每线程事件循环 — 基于 epoll 驱动，协程调度核心
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once


#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/Epoll.h"
#include "Core/EventLoop/TimerQueue.h"
#include "Platform/IO/EventNotifier.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 事件循环当前所处的那一相
     *
     * @details 「在等事件」与「在干活」必须分开：空闲的循环可以整分钟不完成一轮，那不是停顿；
     *          占住循环线程的只有工作段。观测面因此按相记账，读侧只对 Working 判停顿。
     */
    enum class LoopPhase : std::uint8_t
    {
        NotStarted,       ///< run() 还没进来过
        Working,          ///< 正在跑就绪协程、派发 I/O 事件或执行投递进来的代码
        WaitingForEvents, ///< 阻塞在事件后端的等待里，等唤醒或超时
    };

    /**
     * @brief 一个事件循环的自观测快照
     *
     * @details 字段全部来自循环自己写、任意线程读的原子量，取快照因此不需要把动作投递进那个循环
     *          ——被怀疑卡住的循环恰恰没法应答，能隔着线程读才有诊断价值。
     * @note 循环停下之后相位与相位起点都停在最后一次记账的值上，判「还在不在跑」要读 isRunning()，
     *       不能只看相位已经持续多久
     * @note stoppedByFailure 与 failedDispatchCount 一起回答「这条循环是干净停的还是被一次抛出
     *       带走的」：只读 isRunning() 看不出这两种的区别，而后者意味着还有别的循环在跑、
     *       threadCount() 照样报原数
     */
    struct EventLoopSnapshot
    {
        std::thread::id                       ownerThread{};                ///< run() 所在线程；未启动过则是默认值
        bool                                  isRunning{};                  ///< 是否正处于 run() 的循环体里
        LoopPhase                             phase{LoopPhase::NotStarted}; ///< 当前相
        std::chrono::steady_clock::time_point phaseStartedAt{};             ///< 当前这一相的起点
        std::uint64_t                         completedWorkingSegments{};   ///< 已跑完的工作段条数，一条等于「一轮里不含等待的那段」
        std::chrono::microseconds             slowestWorkingSegment{};      ///< 历史最慢的一条工作段（高水位，只升不降）
        std::size_t                           remotePendingCount{};         ///< 跨线程投递里还没被取走的件数（本地就绪队列不在内）
        bool                                  stoppedByFailure{};           ///< run() 是否因逃逸到循环层的异常而收口（区别于 stop() 的正常停止）
        std::size_t                           failedDispatchCount{};        ///< 本循环派发时被守卫就地收下的抛出条数（见 Scheduler::failedDispatchCount）
    };

    /**
     * @brief 观测表里的一行：槽位号 + 那条循环的快照
     *
     * @details 槽位号由读表的一方按登记位置填上（不是循环自己的成员），于是构造期的发布顺序里
     *          没有「标签还没写好、指针已发布」这种半截状态。
     */
    struct ObservedEventLoop
    {
        std::uint64_t     serialNumber{}; ///< 观测槽位号，从 1 起；循环销毁后该槽位可被后来的循环复用
        EventLoopSnapshot snapshot;       ///< 那条循环自己的快照
    };

    /// 一条工作段超过这个时长就落 ERROR：循环线程被占住这么久，同循环上的其余连接都在等它
    inline constexpr std::chrono::milliseconds kSlowWorkingSegmentAlertThreshold{100};

    /**
     * @brief 取回进程内**所有**登记在观测槽位上的事件循环的快照
     *
     * @details 按槽位号升序返回，同一台机器上两次抓取因此对得上行。一个进程里通常有多条循环
     *          （每工作线程一条、链路出口自己的那条、客户端临时起的），整表端点因此不必知道循环归谁。
     * @return std::vector<ObservedEventLoop> 观测表；没有循环活着时为空
     * @note 任意线程可调，且不会把动作投递进被观测的那条循环
     */
    [[nodiscard]] std::vector<ObservedEventLoop> eventLoopSnapshots();

    /**
     * @brief 因槽位已满而没被观测到的循环条数
     * @return std::size_t 未登记条数；非零时 eventLoopSnapshots() 只是不全，不是没有循环在跑
     */
    [[nodiscard]] std::size_t unregisteredEventLoopCount() noexcept;

    class IoWatcher;
    /**
     * @brief 每线程一个的事件循环，封装 epoll 事件监控与协程调度
     *
     * @note **一个实例一个运行生命周期**：stop() 置下的停止请求是粘性的，此后再调用 run() 都会
     *       立刻返回（不会阻塞、也不会重新开始跑）——这个取舍保证「先 stop() 后 run()」的时序
     *       不丢停止请求，而 start() 之后立刻 stop() 正是常见写法；需要重新运行请新建实例。
     */
    class IoWatcher;
    class EventLoop
    {
    public:
        /**
         * @brief 构造一个事件循环对象
         * @details 构造期要建两样东西：跨线程唤醒用的通知器，以及把它注册进事件后端。任一步失败
         *          都意味着这个循环永远醒不过来（`stop()` 与远端投递都靠那条描述符），因此按不可恢复
         *          处理而不是降级运行——静默返回一个「醒不来的循环」会比抛错难查得多。
         * @throws Base::SystemException 唤醒描述符创建失败，或它未能注册进事件后端（文件描述符耗尽
         *         是最现实的成因：每个 worker 线程一个循环，且每个循环还要占定时器与套接字描述符）
         */
        EventLoop();

        /**
         * @brief 销毁事件循环
         */
        ~EventLoop();

        EventLoop(const EventLoop &) = delete;

        EventLoop &operator=(const EventLoop &) = delete;

        /**
         * @brief 启动事件循环（阻塞当前线程）
         * @note 持续处理就绪事件与协程任务，直到 stop() 被调用或发生逃逸到循环层的异常
         * @note **本函数不向调用方抛异常**：它的调用点绝大多数是裸线程入口（线程池的工作线程、
         *       遥测导出线程、示例与测试夹具里的 std::thread），让异常从那里穿出去就是
         *       std::terminate，整个进程连同在途请求一起没。逃逸到这一层的异常就地收下、
         *       置 stoppedByFailure 后正常返回；派发级的异常更早就被 Scheduler 与 IoWatcher
         *       逐条收下了，走到这里说明是循环自身的设施出问题（例如后端 wait 失败）。
         */
        void run();

        /**
         * @brief 请求停止事件循环（非阻塞）
         * @note 可被任意线程调用，线程安全
         */
        void stop();

        /**
         * @brief 唤醒阻塞在 epoll_wait 上的事件循环（非阻塞）
         */
        void wake() const;

        /**
         * @brief 获取 epoll 监控器（非常量引用）
         * @return Epoll& 内部 epoll 对象，可用于注册或修改文件描述符的监听事件
         */
        [[nodiscard]] Epoll &epoll() noexcept;

        /**
         * @brief 获取协程调度器（非常量引用）
         * @return Scheduler& 内部调度器对象，用于提交和管理协程任务
         */
        [[nodiscard]] Scheduler &scheduler() noexcept;

        /**
         * @brief 获取定时器队列（非常量引用）
         * @return TimerQueue& 本循环唯一的定时器队列；Timer 只是它的轻量句柄
         */
        [[nodiscard]] TimerQueue &timerQueue() noexcept;

        /**
         * @brief 登记一个存活的事件注册对象（由 IoWatcher 的构造调用）
         * @param watcher 目标对象（非拥有）
         */
        void registerWatcher(const IoWatcher *watcher);

        /**
         * @brief 注销一个事件注册对象（由 IoWatcher 的析构调用）
         * @param watcher 目标对象（非拥有）
         */
        void unregisterWatcher(const IoWatcher *watcher);

        /**
         * @brief 该事件注册对象是否仍然存活
         * @param watcher 目标对象
         * @return true 仍在登记表里，可以安全派发
         * @note 按线程契约，登记与注销都发生在所属循环线程上（见 IoWatcher 的类注释与
         *       TcpServer::close() 的 @warning）。这把锁是防御性的：它保证派发判定与登记表不会
         *       读到彼此的中途状态，**不构成**「可以从外部线程关闭连接/销毁注册对象」的许可——
         *       事件后端自身的注册表并不带锁，那样做仍然是竞争
         * @details 事件是**批量**从内核取回来的：先处理的那条有可能销毁后一条事件所属的对象
         *          （会话收口时顺手关掉另一条连接就是这条路径），派发前必须确认接收对象还在，
         *          否则后一条事件就是往已释放对象里写成员并 resume 垃圾句柄。
         */
        [[nodiscard]] bool isWatcherAlive(const IoWatcher *watcher) const;

        /**
         * @brief 检查事件循环是否正在运行
         * @return true 表示正处于 run() 循环中，false 表示已停止或尚未启动
         */
        [[nodiscard]] bool isRunning() const noexcept;

        /**
         * @brief 取本循环自己的那一行自观测快照
         * @details 读的全是原子量，因此**任意线程可调**：不必把动作投进本循环再等它应答——真停顿的
         *          循环正是应答不了的那条。要拿进程内所有循环的整表，用 eventLoopSnapshots()。
         * @return EventLoopSnapshot 各字段的口径见该结构体说明
         */
        [[nodiscard]] EventLoopSnapshot snapshot() const noexcept;

    private:
        /**
         * @brief 换到给定的那一相，并给刚结束的那一相记账
         * @param phase 要进入的相
         * @note 每条工作段结束在此累计条数、高水位，并按 kSlowWorkingSegmentAlertThreshold 落 ERROR
         */
        void enterPhase(LoopPhase phase) noexcept;

        /// 存活登记表：IoWatcher 构造/析构时登记与注销，事件派发前据此确认接收对象还活着
        mutable std::mutex                    m_liveWatcherMutex; ///< 保护下面那张表的锁，跨线程注销也要用
        std::unordered_set<const IoWatcher *> m_liveWatchers;     ///< 当前还活着的 IoWatcher

        Epoll                   m_epoll;                   ///< epoll 事件管理器
        Scheduler               m_scheduler;               ///< 协程调度器，管理待运行的任务队列
        Platform::EventNotifier m_wakeup;                  ///< 跨线程唤醒器
        int                     m_wakeupSentinel;          ///< 唤醒哨兵值，用于识别唤醒事件（可选的内部标记）
        std::atomic<bool>       m_running;                 ///< 循环是否正在运行中（原子标记）
        std::atomic<bool>       m_stopRequested;           ///< 是否已请求停止（原子标记，线程安全）
        std::atomic<bool>       m_stoppedByFailure{false}; ///< run() 是否被逃逸到循环层的异常带走（任意线程可读，进快照）
        /// 自观测那一组量：只有本循环的线程写，任意线程读，因此全是原子量且不需要与登记表配合
        std::atomic<LoopPhase>       m_phase{LoopPhase::NotStarted};   ///< 当前相，最后发布（见 enterPhase）
        std::atomic<std::int64_t>    m_phaseStartedAtNanos{0};         ///< 当前相的起点：steady 纪元的纳秒
        std::atomic<std::uint64_t>   m_completedWorkingSegments{0};    ///< 已结束的工作段条数
        std::atomic<std::int64_t>    m_slowestWorkingSegmentMicros{0}; ///< 最慢工作段的高水位（微秒）
        std::atomic<std::thread::id> m_ownerThread{};                  ///< 跑 run() 的那条线程，进入时写
        /// 定时器队列。声明在最后 = 最先销毁：驱动协程与循环唯一的 timerfd 先于其余部件退出，
        /// 收尾时不会再向调度器投递等待者
        TimerQueue m_timerQueue;
    };
} // namespace AsynGyanis::Core
