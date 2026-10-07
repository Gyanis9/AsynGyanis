#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/EventLoop/TimerQueue.h"

#include <string_view>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 观测槽位表的上限
         * @details 一槽一条循环，取 1024 是「比任何真实部署都宽」的整数：这张表只为观测存在，
         *          占满了既不拦构造也不丢循环，只是那几条循环不出现在快照里（差额由
         *          unregisteredEventLoopCount() 报出来）。
         */
        constexpr std::size_t kMaximumObservedEventLoops = 1024;

        /// 常量初始化、无析构：进程退出时不会出现「循环比登记表活得久」那种顺序问题
        std::atomic<const EventLoop *> g_observationSlots[kMaximumObservedEventLoops];
        std::atomic<std::size_t>       g_unregisteredLoopCount{0};

        /// 把时刻折成 steady 纪元的纳秒整数，好塞进原子量（MSVC 的 steady 刻度是 100 ns，折出来仍是整纳秒）
        [[nodiscard]] std::int64_t steadyNanos(const std::chrono::steady_clock::time_point moment) noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(moment.time_since_epoch()).count();
        }

        /// steadyNanos() 的反向换算
        [[nodiscard]] std::chrono::steady_clock::time_point steadyMoment(const std::int64_t nanos) noexcept
        {
            return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{nanos}};
        }

        /**
         * @brief 登记一条循环：占到一个空槽就把指针发布出去
         * @param loop 目标循环（非拥有）
         * @note 槽位满时只记一笔差额（由 unregisteredEventLoopCount() 报出），不拦构造——
         *        观测面少一条记录，总好过让一条能跑的循环建不起来
         */
        void attachObservation(const EventLoop &loop) noexcept
        {
            for (std::atomic<const EventLoop *> &slot: g_observationSlots)
            {
                const EventLoop *expected = nullptr;
                // CAS 而不是直接写：两条线程可以同时在构造各自的循环，先到先得即可，槽位本身没有语义。
                // release 配对读侧的 acquire：看到指针的一方同时看得到构造期写好的每一个成员
                if (slot.compare_exchange_strong(expected, &loop, std::memory_order_release, std::memory_order_relaxed))
                {
                    return;
                }
            }
            // 满表：差额要能被数出来，否则「快照少几条」与「真的只有这几条」在外部看起来一模一样
            g_unregisteredLoopCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 摘除一条循环的登记
         * @param loop 目标循环
         * @return true 表里本来就有它，已摘掉；false 表里没有，说明当初就没占上槽位
         */
        bool detachObservation(const EventLoop &loop) noexcept
        {
            for (std::atomic<const EventLoop *> &slot: g_observationSlots)
            {
                const EventLoop *expected = &loop;
                if (slot.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed))
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    EventLoop::EventLoop() : m_timerQueue(*this)
    {
        // 唤醒描述符挂载一个固定哨兵指针：run() 靠 data.ptr 是否等于它来区分
        // 「唤醒通知」与「IoWatcher 的 I/O 事件」，因此两者不能共用同一个用户数据槽
        m_scheduler.setWakeupNotifier(&m_wakeup);

        // 唤醒描述符建不起来（fd 耗尽等）：stop() 再也唤不醒阻塞在 epoll_wait 上的线程，
        // 收尾时的 join 会永久挂住，跨线程投递也永远不执行——启动期就当场失败，不要留一个
        // 「能跑但停不下来」的循环（与 TimerQueue 对描述符失败的处理同一口径）
        if (!m_wakeup.isValid())
        {
            throw Base::SystemException("创建事件循环的唤醒描述符失败：跨线程投递与 stop() 都将无法工作");
        }
        // 建起来了还要注册成功才算数：注册失败（epoll 实例的 watch 配额打满等）时写入唤醒描述符
        // 不会被监听，stop() 唤不醒阻塞中的 wait()，收尾的 join 仍会永久挂住——与上面同一口径当场抛
        if (!m_epoll.addFileDescriptor(m_wakeup.readDescriptor(), EPOLLIN, &m_wakeupSentinel))
        {
            throw Base::SystemException("把唤醒描述符注册进事件后端失败：跨线程投递与 stop() 都将无法工作");
        }

        // 登记放在构造的最后一步：指针一发布，别的线程就能立刻读这一条循环的成员，
        // 因此必须等所有成员都建好之后才交出去
        // 登记放在构造的最后一步：指针一发布，别的线程就能立刻读这一条循环的成员，
        // 因此必须等所有成员都建好之后才交出去
        attachObservation(*this);
    }

    EventLoop::~EventLoop()
    {
        // 摘不到就是当初没占上槽位（表满），那笔差额要还回去，否则未登记计数只涨不落。
        // 饱和减：这条计数只用于「快照可能不全」的提示，减过头绕回极大值比停在 0 更误导
        if (!detachObservation(*this))
        {
            std::size_t seen = g_unregisteredLoopCount.load(std::memory_order_relaxed);
            while (seen > 0 && !g_unregisteredLoopCount.compare_exchange_weak(seen, seen - 1, std::memory_order_relaxed))
            {
                // compare_exchange_weak 失败时已把最新值写回 seen，循环重试即可
            }
        }

        if (m_running.load(std::memory_order_acquire))
            stop();

        // 成员销毁顺序是 m_wakeup 早于 m_epoll，若不先摘除注册，唤醒 socket 会在仍属于
        // epoll 集合时被 closesocket，wepoll 内部线程会继续访问这个失效句柄并破坏堆
        m_epoll.delFileDescriptor(m_wakeup.readDescriptor());
    }

    void EventLoop::run()
    {
        // 这里刻意**不**清除 m_stopRequested，即停止请求是粘性的：它一旦被置位，
        // 之后每次 run() 都会立刻返回。原因不是忘了重置，而是这样才保证
        // 「stop() 先于 run() 到达」不会被丢掉——start() 之后立刻 stop() 是常见写法，
        // 若在 run() 开头清除标志，那次停止请求就会被吞掉，工作线程将永远阻塞在
        // epoll_wait 上，join 随之卡死（实测过）。需要重新运行请新建 EventLoop 实例
        m_running.store(true, std::memory_order_release);

        // 自观测：先记下跑这条循环的线程，再以「工作相」开场——第一条工作段的起点就是这里
        m_ownerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        m_phaseStartedAtNanos.store(steadyNanos(std::chrono::steady_clock::now()), std::memory_order_relaxed);
        m_phase.store(LoopPhase::Working, std::memory_order_release);

        while (true)
        {
            // 投进来的可调用体/协程抛异常时不能让异常无声地逃出去：本函数通常跑在线程入口上，
            // 逃出去就是 std::terminate（整个进程带走），而且末尾的 m_running 复位会被跳过，
            // isRunning() 永远停在 true。
            //
            // 这里就地收下、复位状态、置 stoppedByFailure 后**正常返回**，不再重抛。重抛的旧写法
            // 指望「由 WorkerSupervisor 重启 worker」来兜，但那台 supervisor 编排的是 worker **进程**
            // （fork+exec、盯进程退出），而 ThreadPool 的工作线程是一条没有重启函数的 jthread：
            // 它接住重抛后只是记一条日志再让线程体返回，于是 threadCount() 照样报原数、而那条循环
            // 再也不驱动任何东西——它上面的连接仍挂在活的 epoll 里，连收空闲连接的清扫协程也在同一条
            // 死循环上。那是「看着在跑其实已经停」的形态，比崩溃更难查。
            //
            // 走到这一层意味着是循环自身的设施出问题（后端 wait 失败这类），停是停对了；
            // 派发级的异常更早就被 Scheduler::runGuarded 与 IoWatcher 逐条收下了。
            try
            {
                m_scheduler.runAll();

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                // 有就绪协程时用 0 超时轮询，否则无限阻塞等待 epoll 事件
                const int timeoutMs = m_scheduler.hasWork() ? 0 : -1;
                // 进等待相：这条工作段到此为止，它的时长与是否超阈值都在 enterPhase 里记账
                enterPhase(LoopPhase::WaitingForEvents);
                auto events = m_epoll.wait(timeoutMs);
                // 醒过来即回到工作相。等待那一段单独记成一相，不算进工作段耗时——空闲不是停顿
                enterPhase(LoopPhase::Working);
                for (const auto &ev: events)
                {
                    if (ev.data.ptr == &m_wakeupSentinel)
                    {
                        m_wakeup.drain();
                        continue;
                    }

                    if (ev.data.ptr)
                    {
                        // 挂载在 data.ptr 上的只可能是唤醒哨兵或某个 IoWatcher 的地址：
                        // 常驻注册写进去的是注册对象自己的地址，因此这里把事件交给它分发
                        // （它再决定是恢复等待中的协程，还是把就绪记下来留给下一次等待）。
                        // **派发前先确认对象还活着**：这一批是批量取回来的，先前处理的那条事件
                        // 可能已经把它所属的连接关掉（会话收口就是这么做的），此时再派发就是
                        // 往已释放对象里写成员。登记表正是为这一种情形而设。
                        // 查表与派发之间**不持锁**：handleEvents() 里的业务会关连接、销毁
                        // IoWatcher，而注销登记要拿同一把非递归锁——持锁派发就是自死锁。
                        // 因此这道检查只覆盖「同一批内已被销毁」；跨线程的销毁不在线程契约内
                        // （循环对象只在所属循环上构造/销毁，见 EventLoop 的类说明）
                        auto *const watcher = static_cast<IoWatcher *>(ev.data.ptr);
                        if (isWatcherAlive(watcher))
                        {
                            watcher->handleEvents(ev.events);
                        }
                    }
                }

                m_scheduler.runAll();
            } catch (const std::exception &loopError)
            {
                LOG_ERROR_EXCEPTION(loopError, "EventLoop: 事件循环里逃出的异常已就地收口（本条循环停止，进程继续）：{}", loopError.what());
                m_stoppedByFailure.store(true, std::memory_order_release);
                m_running.store(false, std::memory_order_release);
                // 收下之后必须真的离开循环：这两个原子量是全仓唯一的「这条循环还在不在跑」判据
                // （见 EventLoop.h 对 isRunning()/stoppedByFailure 的说明），落回 while 的开头就变成
                // 「读数说已经停了，线程还在派发事件、恢复协程」；而析构那一步按 m_running==false 跳过
                // stop()，紧接着在仍会去等事件的线程底下销毁后端与唤醒套接字。持续失败的那一格还会退化成
                // 每轮一条 ERROR 日志的满核空转。重抛那一支早已被否掉（见本函数开头那段），所以这里是退出
                break;
            } catch (...)
            {
                LOG_ERROR_FMT("EventLoop: 事件循环里逃出的非标准异常已就地收口（本条循环停止，进程继续）");
                m_stoppedByFailure.store(true, std::memory_order_release);
                m_running.store(false, std::memory_order_release);
                break; // 同上：非标准异常这一支同样要真的停下
            }
        }

        m_running.store(false, std::memory_order_release);
    }

    void EventLoop::stop()
    {
        // 必须先置位再唤醒：唤醒只负责让阻塞中的 epoll_wait 立刻返回并重读标志。
        // 若顺序反过来，工作线程可能在标志写入前被唤醒并重新阻塞，而 stop() 不会再有
        // 第二次唤醒，run() 便永远等不到停止请求；release 语义则保证这次写入
        // 对随唤醒而恢复的线程可见，不会被重排到 notify 之后
        m_stopRequested.store(true, std::memory_order_release);
        wake();
    }

    void EventLoop::wake() const
    {
        m_wakeup.notify();
    }

    Epoll &EventLoop::epoll() noexcept
    {
        return m_epoll;
    }

    Scheduler &EventLoop::scheduler() noexcept
    {
        return m_scheduler;
    }

    void EventLoop::registerWatcher(const IoWatcher *const watcher)
    {
        const std::lock_guard lock(m_liveWatcherMutex);
        m_liveWatchers.insert(watcher);
    }

    void EventLoop::unregisterWatcher(const IoWatcher *const watcher)
    {
        const std::lock_guard lock(m_liveWatcherMutex);
        m_liveWatchers.erase(watcher);
    }

    bool EventLoop::isWatcherAlive(const IoWatcher *const watcher) const
    {
        const std::lock_guard lock(m_liveWatcherMutex);
        return m_liveWatchers.contains(watcher);
    }

    TimerQueue &EventLoop::timerQueue() noexcept
    {
        return m_timerQueue;
    }

    bool EventLoop::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    void EventLoop::enterPhase(const LoopPhase phase) noexcept
    {
        const auto nowMoment     = std::chrono::steady_clock::now();
        const auto previousPhase = m_phase.load(std::memory_order_relaxed);
        const auto previousStart = steadyMoment(m_phaseStartedAtNanos.load(std::memory_order_relaxed));
        // 刚结束那一相的时长：本函数每相只被调一次，因此一次读数就够（一轮两条相，各记各的）
        const auto segmentMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(nowMoment - previousStart);

        if (previousPhase == LoopPhase::Working)
        {
            m_completedWorkingSegments.fetch_add(1, std::memory_order_relaxed);
            // 高水位只有本循环的线程会写，读侧拿到稍旧的值也只是少报一次峰值，故不做 CAS
            if (segmentMicroseconds > std::chrono::microseconds{m_slowestWorkingSegmentMicros.load(std::memory_order_relaxed)})
            {
                m_slowestWorkingSegmentMicros.store(segmentMicroseconds.count(), std::memory_order_relaxed);
            }

            // 一条工作段吃掉阈值这么多，说明有处理器把循环线程占死了：这段时间里同一条循环上的
            // 其余连接一个事件都收不到。每段各报一条，不按调用点合并——八条循环一起卡的时候，
            // 压成一行就看不出卡的是哪条（正文里的时长与条数条条不同，正是 LogThrottle 的适用面之外）
            if (segmentMicroseconds > kSlowWorkingSegmentAlertThreshold)
            {
                LOG_ERROR_FMT("EventLoop: 一条工作段耗时 {} 毫秒，超过 {} 毫秒的告警阈值：这时长里循环线程被占住，"
                              "同一条循环上的其余连接都在等它（累计工作段 {} 条）",
                              std::chrono::duration_cast<std::chrono::milliseconds>(segmentMicroseconds).count(), kSlowWorkingSegmentAlertThreshold.count(),
                              m_completedWorkingSegments.load(std::memory_order_relaxed));
            }
        }

        // 先写起点再换相位：读侧看到新相位时一定也看得到这一相的起点（配 snapshot() 的 acquire 读）
        m_phaseStartedAtNanos.store(steadyNanos(nowMoment), std::memory_order_relaxed);
        m_phase.store(phase, std::memory_order_release);
    }

    EventLoopSnapshot EventLoop::snapshot() const noexcept
    {
        const auto phase = m_phase.load(std::memory_order_acquire);
        return EventLoopSnapshot{
                .ownerThread              = m_ownerThread.load(std::memory_order_relaxed),
                .isRunning                = m_running.load(std::memory_order_acquire),
                .phase                    = phase,
                .phaseStartedAt           = steadyMoment(m_phaseStartedAtNanos.load(std::memory_order_relaxed)),
                .completedWorkingSegments = m_completedWorkingSegments.load(std::memory_order_relaxed),
                .slowestWorkingSegment    = std::chrono::microseconds{m_slowestWorkingSegmentMicros.load(std::memory_order_relaxed)},
                .remotePendingCount       = m_scheduler.remotePendingCount(),
                .stoppedByFailure         = m_stoppedByFailure.load(std::memory_order_acquire),
                .failedDispatchCount      = m_scheduler.failedDispatchCount(),
        };
    }

    std::vector<ObservedEventLoop> eventLoopSnapshots()
    {
        std::vector<ObservedEventLoop> report;
        for (std::size_t index = 0; index < kMaximumObservedEventLoops; ++index)
        {
            // acquire 配对构造末尾那次 release 登记：拿到指针就等于拿到建好的循环
            const auto *loop = g_observationSlots[index].load(std::memory_order_acquire);
            if (loop == nullptr)
            {
                continue;
            }
            // 槽位号由读表的一方填：循环自己不知道自己在表里的位置，构造期也就不存在
            // 「指针已发布、号还没写好」那种半截状态
            report.push_back(ObservedEventLoop{.serialNumber = static_cast<std::uint64_t>(index) + 1U, .snapshot = loop->snapshot()});
        }
        return report;
    }

    std::size_t unregisteredEventLoopCount() noexcept
    {
        return g_unregisteredLoopCount.load(std::memory_order_relaxed);
    }

} // namespace AsynGyanis::Core
