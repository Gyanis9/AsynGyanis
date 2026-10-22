#include "Database/Pool/ConnectionPool.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/EventLoop/EventLoop.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Database
{
    namespace
    {
        /// 把一次恢复投递包进 noexcept 边界：投递要分配队列结点，失败只该丢这一条唤醒
        template<typename PostOperation>
        void postRemoteWithLog(PostOperation &&postOperation, const std::string_view description) noexcept
        {
            try
            {
                postOperation();
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("ConnectionPool: {}的投递未能入队，这条等待将落空（原因：{}）", description, failure.what());
            } catch (...)
            {
                LOG_ERROR_FMT("ConnectionPool: {}的投递未能入队，这条等待将落空（非标准异常）", description);
            }
        }
    } // namespace

    // ========================================================================
    // 构造 / 析构
    // ========================================================================

    ConnectionPool::ConnectionPool(std::function<std::unique_ptr<DatabaseConnection>()> factory, const PoolConfig &config) :
        // 配置先在线程起来之前过一遍判据：m_config 与 m_healthThread 按声明顺序初始化，
        // 把校验放在这一段里就等于「要么先拒、要么根本还没启动」，不会出现抛出去之后
        // 留下一条已经在跑的后台线程
        m_factory(std::move(factory)), m_config(validateConfiguration(config)), m_healthThread([this](std::stop_token stopToken) { healthCheckLoop(std::move(stopToken)); })
    {

        // 登记的十一条都是原子量，抓取时不碰池的锁（理由见头文件里那段的注释）。其中
        // `connections_created_total` 是 `connections_held` 的旧名别名：那个名字随 v2.4.0 发布过，
        // 而它其实不是历史累计（丢弃会减回去，它同时是上限的占位分母）——改名等于掐断既有面板，
        // 所以旧名留着、值与 help 都按真实语义走，新面板取 held
        m_metricHandles = {
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_active_connections", "此刻被取出未归还的数据库连接数（进程内各池相加）", Core::ProcessMetricKind::Gauge,
                                                             Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_activeCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric(
                        "asyn_db_pool_waiting_requests", "正在等一条空闲连接的取出请求数（同步等待与协程等待相加，与 ConnectionPool::waitingCount() 同一个数）",
                        Core::ProcessMetricKind::Gauge, Core::ProcessMetricMerge::Sum,
                        [this] { return static_cast<std::uint64_t>(m_syncWaitingCount.load(std::memory_order_relaxed) + m_asyncWaitingCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_connections_held", "池当下记在账上的连接数（创建 +，丢弃或建连回退 −）：与 maximumPoolSize 同一本账",
                                                             Core::ProcessMetricKind::Gauge, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_totalCreated.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_connections_created_total",
                                                             "同 asyn_db_pool_connections_held（旧名，随 v2.4.0 发布过：留着不让既有面板断线，新面板请取 held）；"
                                                             "本 `_total` 是 Gauge（池收缩时会降），勿用 rate()",
                                                             Core::ProcessMetricKind::Gauge, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_totalCreated.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_connections_discarded_total",
                                                             "被丢弃的连接总数：凡走丢弃出口的去向都计入——失联、超过存活期、会话没复位干净、"
                                                             "调用方判定会话状态不可信而主动丢弃，以及归还时那一格空闲栈没分配出来（只增不减）。"
                                                             "「建了就丢」以前只能从两个数推断，现在直接读得到",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_totalDiscarded.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_borrow_timeouts_total", "等到截止时刻仍没拿到连接的次数（停摆期与不等待的取用不计）",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_borrowTimeoutCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_waiting_rejections_total",
                                                             "等待名额已满（maximumWaitingRequests）而不再排队的借出次数（同步与协程合并，当场空手、不等待）。"
                                                             "它与 borrow_timeouts_total 分开：这里根本没等，混进去会把「池不够用」这个容量信号打成假的",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_waitingRejectedCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_oldest_borrow_start_seconds",
                                                             "当下在借连接里最久的一条的借出时刻（Unix 秒，0 = 没有在借）。`time() - 本值` 就是最久持有期："
                                                             "它贴着 acquireTimeout 的量级不降，说明有人拖住连接不放——配合池上限与等待数对着读，可分诊「池太小」与「借用者太慢」",
                                                             Core::ProcessMetricKind::Gauge, Core::ProcessMetricMerge::MinNonZero,
                                                             [this] { return oldestBorrowStartEpochSeconds(); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_session_reset_failures_total",
                                                             "归还时驱动报告「会话没复位干净」而被丢弃的连接数：它是 connections_discarded_total 的一个子集，"
                                                             "专指「驱动清不掉上一个借用者留下的状态」那一条去向。其余去向各有成因——对端掐线与到轮换期属正常生命周期，"
                                                             "调用方主动丢弃说的是业务语句在驱动侧失败，而归还时那一格没分配出来说的是内存吃紧。"
                                                             "这条在涨说明有借用者留下了清不掉的会话状态，或那条连接已经不能对话",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_sessionResetFailures.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_connection_probes_total",
                                                             "借出前发出的有效性探针数（PoolConfig::probeBeforeAcquire 打开时才增，关闭时恒为 0）：每一格都是"
                                                             "一次真实的往返，读侧要能看见自己为它付了多少。与 connections_discarded_total 对着读：探针判死的"
                                                             "那些连接会从丢弃数里现形",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_probeCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_db_pool_connect_retries_total",
                                                             "建连失败后重试的次数（只重驱动报「连不上」那一类错误码；鉴权错、库名错不计）。它涨而借用仍成功，"
                                                             "说的是服务端在重启或网络在抖；它涨且借用开始空手，才是重试预算也不够用",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_connectRetryCount.load(std::memory_order_relaxed)); }),
        };
    }

    ConnectionPool::~ConnectionPool()
    {
        // 置假必须在令牌锁里做：此刻起归还路径要么已经持锁进来（本析构会等它走完），
        // 要么看到「池已停摆」直接关掉连接——池已析构就不能再被取消引用。
        // 锁只覆盖这一次赋值：析构末尾会就地恢复等待者，被恢复的协程可能归还
        // PooledConnection，那条路径要锁同一把非递归令牌锁；持锁跨越恢复就是同线程
        // 二次加锁的自死锁（与下面「恢复必须在 m_asyncMutex 锁外做」是同一条纪律）
        {
            const std::lock_guard livenessLock(m_liveness->mutex);
            m_liveness->isAlive = false;
        }

        m_healthThread.request_stop();
        if (m_healthThread.joinable())
        {
            m_healthThread.join();
        }

        std::vector<IdleEntry> doomedConnections;
        {
            std::unique_lock lock(m_mutex);

            // 停摆标志：同步等待者的谓词据此成立，醒来后返回空连接而不是继续睡在 m_idleCondition 上
            m_isShuttingDown.store(true, std::memory_order_release);

            // 整栈先换出来，关闭留到锁外：断开是一次会阻塞的系统调用（SQLite 关文件句柄、
            // MySQL / Redis 关 socket），握着 m_mutex 逐条关会让统计读取和同步等待者的退出都排在
            // 整批关闭之后。本池的纪律一直是「断开不进 m_mutex」（取出与归还两条丢弃出口都如此），
            // 析构不该例外
            doomedConnections.swap(m_idleStack);

            // 唤醒所有剩余的同步等待者：它们醒来会看到停摆标志、返回空连接并自减计数
            m_idleCondition.notify_all();

            // 等最后一位同步等待者真的离开等待。不等的话本析构返回后它还睡在 m_idleCondition 上——而 m_idleCondition
            // 已随对象销毁（等待者的退出路径也会 notify_all，因此这里的等待不会漏唤醒）
            m_idleCondition.wait(lock, [this] { return m_syncWaitingCount.load(std::memory_order_acquire) == 0; });
        }

        // 池正在析构，名额与统计都不再有意义，只做关闭；成员销毁前必须把它们全部释放完
        for (auto &entry: doomedConnections)
        {
            closeTrackedConnection(std::move(entry.connection));
        }

        // 唤醒所有异步等待者：给它们空连接。
        // 这一次刻意「就地恢复」而不是投回各自的事件循环——池已经停摆，投递进循环的任务
        // 很可能永远不会被执行（循环也可能正在停止），那会让等待的协程永久挂起；
        // 就地恢复至少能让它们拿到空连接、继续走完自己的错误分支。
        // 恢复必须在锁外做：协程恢复后可能立刻再触池（重试 acquire、归还连接），
        // 那些路径都要拿 m_asyncMutex，持锁恢复就是同线程二次加锁的自死锁
        // 收集票据而不是裸指针：票据是共享所有权，等待器随帧析构时我们手里的这一份仍然有效，
        // 而裸指针在锁外就已经可能悬垂（调用方可以随时销毁那个 Task）
        std::vector<std::shared_ptr<AcquireAwaiter::ResumeTicket>> abandonedTickets;
        {
            std::lock_guard lock(m_asyncMutex);
            abandonedTickets.reserve(m_asyncWaiters.size());
            for (AcquireAwaiter *waiter: m_asyncWaiters)
            {
                waiter->m_result = nullptr;
                waiter->m_inList = false;
                if (waiter->m_resumeTicket != nullptr)
                {
                    abandonedTickets.push_back(waiter->m_resumeTicket);
                }
            }
            m_asyncWaiters.clear();
            refreshAsyncWaitingCount();
        }
        for (const std::shared_ptr<AcquireAwaiter::ResumeTicket> &ticket: abandonedTickets)
        {
            ticket->resumeOnce();
        }
    }

    std::shared_ptr<PoolLiveness> ConnectionPool::livenessToken() const noexcept
    {
        return m_liveness;
    }

    // ========================================================================
    // acquire — 阻塞获取
    // ========================================================================

    PooledConnection ConnectionPool::acquire()
    {
        // ---- 第一段：快速路径 ----
        // 尝试从空闲栈弹出（无锁区之后、加锁弹出）
        {
            if (std::unique_ptr<DatabaseConnection> connection = tryAcquireInternal())
            {
                m_activeCount.fetch_add(1);
                return PooledConnection(std::move(connection), this);
            }
        }

        // ---- 第二段：尝试创建新连接 ----
        // 未达上限则懒惰创建（避免上线就建满）。建连（工厂 + connect）在锁外跑：
        // 一次秒级的 TCP 握手不该把所有取出、统计与后台驱逐一起堵在 m_mutex 上
        if (std::unique_ptr<DatabaseConnection> connection = tryAcquireOrCreateInternal())
        {
            m_activeCount.fetch_add(1);
            return PooledConnection(std::move(connection), this);
        }

        // ---- 第三段：等待路径 ----
        // 等待要能被「任何一次腾出名额」救活：归还的那条若被判失联或过存活期，它是被丢弃而不是入栈的，
        // 此刻空闲栈仍然为空、名额却确实空了出来。只盯着空闲栈的写法会让这位借用者白等满
        // acquireTimeoutMilliseconds 再拿一个空连接回去，而它完全可以自己补一条。
        // 取连接统一走 tryAcquireOrCreateInternal：名额判定、过期判定与失联判定因此和另外三条取出路径
        // 同一口径（本函数此前只判了失联，睡在栈里过了存活期的那条会直接被交给刚被唤醒的借用者）
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_config.acquireTimeoutMilliseconds);

        // 等待名额判据（见 PoolConfig::maximumWaitingRequests）：两次乐观尝试都没拿到、且合计等待数
        // 已到上限时，本次借出不再排队、当场空手返回——宁可快速失败让调用方降级，也不排一条
        // 只会等满 acquireTimeout 的队。合计判据带一次瞬时误差：异步那份计数在 m_asyncMutex 下
        // 维护，两把锁按约定不互相嵌套（次序见 wakeWaitersForFreedSlot），因此两个原子读数之间
        // 必然差着此刻正在入表/摘表的那一两位；安全阀方向取「宁可多放进几个」——误放只多一点内存，
        // 误杀会白白掐掉一个本来能等到连接的业务请求
        bool isWaitingRefused = false;
        {
            const std::lock_guard lock(m_mutex);
            isWaitingRefused = m_syncWaitingCount.load(std::memory_order_relaxed) + m_asyncWaitingCount.load(std::memory_order_relaxed) >= m_config.maximumWaitingRequests;
        }
        if (isWaitingRefused)
        {
            // 计数与告警放在锁外：日志通路可能阻塞，不能带进 m_mutex
            noteWaitingRefusal();
            return {};
        }

        std::unique_ptr<DatabaseConnection> connection = waitForConnectionUnderDeadline(deadline);
        if (!connection)
        {
            // 超时或池已停摆：拿不到连接就交出空的包装，让调用方看见「没拿到」而不是异常。
            // 账已在等待段记过，这里只交出空的包装
            return {};
        }

        return PooledConnection(std::move(connection), this);
    }

    std::unique_ptr<DatabaseConnection> ConnectionPool::waitForConnectionUnderDeadline(const std::chrono::steady_clock::time_point deadline)
    {
        std::unique_ptr<DatabaseConnection> connection;
        std::unique_lock                    lock(m_mutex);
        m_syncWaitingCount.fetch_add(1, std::memory_order_seq_cst);

        while (!m_isShuttingDown.load(std::memory_order_acquire))
        {
            // 出锁再取：tryAcquireOrCreateInternal 自己会拿 m_mutex（握着它再拿一次就是同线程二次
            // 加锁），而它内部的建连与丢弃都是会阻塞的调用
            lock.unlock();
            connection = tryAcquireOrCreateInternal();
            lock.lock();
            if (connection || m_isShuttingDown.load(std::memory_order_acquire))
            {
                break;
            }
            // 复检一次空闲栈再决定睡不睡：出锁试一轮的空档里可能已经有人归还入栈，而那一次
            // notify_one 正好落在这位借用者「还没睡下」的时刻——条件变量不会补发，通知就此丢掉。
            // 只把通知挪进锁内堵不住这个窗口（等待侧本来就不在队列里），必须两侧配成一对
            if (!m_idleStack.empty())
            {
                continue;
            }
            // 每次醒来重试一轮：唤醒源是「有人归还入栈」「有人丢弃腾出名额」与池停摆三处
            if (m_idleCondition.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                break;
            }
        }

        // 记账必须排在「同步等待者计数减一」之前且留在锁内：池析构等的就是这条计数归零，计数一降
        // 它随时可能把本对象释放掉，此后再碰任何成员都是踩已释放内存（TSan 实测报出过一次）。
        // 取出主流程的第一、二段各记自己那一次，这里只负责等待段的出口
        if (connection)
        {
            m_activeCount.fetch_add(1);
        } else if (!m_isShuttingDown.load(std::memory_order_acquire))
        {
            // 只记「等到截止时刻仍空手」：停摆期空手是正常收尾，混进来会让这道容量指标在每次
            // 优雅停机时虚涨
            m_borrowTimeoutCount.fetch_add(1, std::memory_order_relaxed);
        }

        m_syncWaitingCount.fetch_sub(1);
        // 池析构可能在等最后一位同步等待者离开（它睡在同一把 m_idleCondition 上等计数归零）
        m_idleCondition.notify_all();
        return connection;
    }

    // ========================================================================
    // acquireAsync — 协程异步获取
    // ========================================================================

    Core::Task<PooledConnection> ConnectionPool::acquireAsync(Core::EventLoop &loop)
    {
        // 截止时刻在这里定一次，重挂的每一轮共用它：被叫醒却没拿到连接不会把等待上限往后推
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_config.acquireTimeoutMilliseconds);

        // 「上一轮是不是被腾名额唤醒的」存在帧里跨轮存活：等待体每轮都是新造的，唤醒方写的标志
        // 落在旧实例上，只写在那里的话下一轮的新实例看不到它——豁免形同虚设（用例实测被二次拒绝）
        bool isReturnedFromWake = false;
        while (true)
        {
            AcquireAwaiter   awaiter(this, &loop, deadline, isReturnedFromWake);
            PooledConnection result = co_await awaiter;

            // 交接后的恢复可能晚于池析构：下面几行读的都是池的成员，判活必须排在任何一次读之前。
            // 判死就地收尾，重挂与记账都没有对象可碰；连接若已在恢复时交出，随 result 一起关闭
            bool isPoolAlive = false;
            {
                const std::lock_guard livenessLock(awaiter.m_liveness->mutex);
                isPoolAlive = awaiter.m_liveness->isAlive;
            }
            if (!isPoolAlive)
            {
                co_return std::move(result);
            }

            // 消费掉唤醒来源再走下一轮：本轮被唤醒后若重挂入表，await_suspend 已把标志清零，
            // 这里读回的就是常态——豁免只保「被唤醒后的第一次重挂」
            isReturnedFromWake = awaiter.m_returnedFromWake;

            // 排队这一步分配不出来：不再重挂。内存持续吃紧时重挂就是「造票据→抛出→再造」的空转，
            // 一直转到截止时刻为止，而这条协程正跑在调用方的线程上；就地按空手结账，让调用方自己
            // 决定退避还是放弃。这一格不记进超时账：截止时刻根本没到，记成超时会把「池不够用」
            // 这个容量信号打成假的，成因只在这一行 ERROR 里出声。
            if (awaiter.m_isQueueingFailed)
            {
                LOG_ERROR("ConnectionPool: 异步借出排队时分配失败（恢复票据或等待表建不出来），本次借出按空手收尾，不计入超时");
                co_return std::move(result);
            }

            // 等待名额已满：不再重挂（重挂只会立刻再撞一次同一道判据），按空手结账并落账。
            // 同样不计入超时账：截止时刻根本没到
            if (awaiter.m_isWaitingCapReached)
            {
                noteWaitingRefusal();
                co_return std::move(result);
            }

            // 三种情况收尾：拿到了连接；到了截止时刻；池正在停摆（停摆中以空连接就地唤醒，
            // 再挂一轮也等不到东西，而且等待表马上要随池一起销毁）
            const bool isShuttingDown = m_isShuttingDown.load(std::memory_order_acquire);
            if (result || isShuttingDown || std::chrono::steady_clock::now() >= deadline)
            {
                // 停摆标志只读一次：两侧两次读之间它若翻假→真，同一次借出会被判成两种收尾。
                // 记账口径与同步 acquire() 完全一致——空手且不是停摆造成的才算超时，
                // 否则「异步借出超时」在这个指标上是个黑洞
                if (!result && !isShuttingDown)
                {
                    m_borrowTimeoutCount.fetch_add(1, std::memory_order_relaxed);
                }
                co_return std::move(result);
            }
        }
    }

    // ========================================================================
    // AcquireAwaiter 实现
    // ========================================================================

    ConnectionPool::AcquireAwaiter::~AcquireAwaiter()
    {
        // 令牌锁覆盖「碰池」的全部区段，且判活排在任何取消引用之前：调用方可能把这具帧留到池析构
        // 之后才销毁，那时连取 m_pool->m_asyncMutex 都已经是释放后使用。反过来，只要在此持锁读到
        // isAlive 为真，池析构就还卡在置假那一步，本区段内碰池都是安全的
        const std::lock_guard livenessLock(m_liveness->mutex);
        if (m_liveness->isAlive)
        {
            // 摘表与「取走交接结果」必须在唤醒方那把锁里做：returnConnection() 的交接段 / expireTimedOutWaiters()
            // 都是持 m_asyncMutex 写 m_result 与 m_inList 的，而本析构可能跑在任意线程、与它们没有任何
            // happens-before。无锁读的后果不只是摘表漏一条：读不到刚交接进来的连接就会把它随帧一起销毁，
            // 而池的总创建数不降、空闲栈也拿不回它——反复几次之后所有 acquire 都卡在「池已满」上。
            std::unique_ptr<DatabaseConnection> handedOverConnection;
            {
                const std::lock_guard asyncLock(m_pool->m_asyncMutex);
                if (m_inList)
                {
                    // 在锁内摘表：唤醒方写列表也持这把锁，摘表与交接因此不会交错
                    m_pool->removeAsyncWaiterLocked(this);
                    m_inList = false;
                }
                handedOverConnection = std::move(m_result);
            }

            // 已经交到手上、却来不及被取走的连接按「取出后立刻归还」结账：交接那一刻归还路径
            // 已经减过活跃计数，这次取出则从未被记上（记在 await_resume 里），因此先补记再归还。
            // 归还在 m_asyncMutex 之外做：returnConnection() 还要拿那把锁去唤醒别的等待者，
            // 持锁进入就是同线程二次加锁的自死锁
            if (handedOverConnection)
            {
                m_pool->m_activeCount.fetch_add(1);
                m_pool->returnConnection(std::move(handedOverConnection));
            }
        }
        // 池已停摆时什么都不做：等待表随池一起销毁，手里的连接随本帧析构关闭

        // 票据里的句柄一并清空：池可能已经把「恢复这次等待」投回了事件循环（交接连接那一刻），
        // 而本帧眼下就要析构。投递那边执行时看到空句柄会直接跳过，不会 resume 已释放的帧。
        // 这里只读 m_resumeTicket 再原子清句柄，不搬走那份 shared_ptr——搬走要在 m_asyncMutex 里做，
        // 而停摆分支不能碰池的锁；票据里的句柄本身是原子量，池一侧的读者只读不写，因此无竞争
        if (m_resumeTicket != nullptr)
        {
            m_resumeTicket->handle.store(nullptr, std::memory_order_release);
        }
    }

    bool ConnectionPool::AcquireAwaiter::await_ready() noexcept
    {
        // 尝试非阻塞获取：空闲栈为空但未达上限时会新建，与同步 acquire() 的口径一致
        m_result = m_pool->tryAcquireOrCreateInternal();
        return m_result != nullptr;
    }

    bool ConnectionPool::AcquireAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept
    {
        // 注册窗口的 RAII 计数：入表之前归还/丢弃路径只读等待表，看不见「正在注册」的这一位。
        // 计数在第一次尝试之前就加上，因此窗口内每一次腾名额都会在 m_freedSlotEpoch 上留痕；
        // 本函数返回时才减，入表那一侧已经先把 m_asyncWaitingCount 发布出去，快路径不会漏看。
        m_pool->m_pendingRegistrations.fetch_add(1, std::memory_order_seq_cst);
        struct PendingRegistrationGuard
        {
            std::atomic<std::size_t> &counter;
            ~PendingRegistrationGuard()
            {
                counter.fetch_sub(1, std::memory_order_seq_cst);
            }
        } pendingRegistrationGuard{m_pool->m_pendingRegistrations};

        m_handle = handle;
        return needsToSuspendAfterPostQueueRetry();
    }

    bool ConnectionPool::AcquireAwaiter::needsToSuspendAfterPostQueueRetry() noexcept
    {
        while (true)
        {
            // 再试一次：在 await_ready 和 await_suspend 之间可能已有连接归还。
            // 世代号在尝试之前取：这次尝试期间若有人腾出名额，下面的锁内核对会看见变化并重走一轮。
            // 这样就不必在入表之后再补一次取连接——那次补取会把「已经可被唤醒」的状态暴露给
            // 归还/超时路径，却在协程真正挂起之前就把它 resume 掉（TSan 实测到的帧释放竞争）
            const std::uint64_t slotEpochBefore = m_pool->m_freedSlotEpoch.load(std::memory_order_seq_cst);
            m_result                            = m_pool->tryAcquireOrCreateInternal();
            if (m_result)
            {
                return false;
            }

            // 不必再等的两种情形就地放行：截止时刻已过（等后台线程的下一拍才发现只会白睡一拍，
            // 而调用方拿到空连接的收尾与超时同解）、池正在停摆（停摆中不会再有东西可交接，
            // 入表反而会把这条协程留在正在销毁的池的等待表里）
            if (std::chrono::steady_clock::now() >= m_deadline || m_pool->m_isShuttingDown.load(std::memory_order_acquire))
            {
                return false;
            }

            // 仍无可用连接：加入等待列表。这次判定必须与入表同锁：唤醒方（归还路径的「交接或入栈」）拿的
            // 也是 m_asyncMutex，两者若不同锁，「再试失败」到「入表」之间归还的连接会被
            // 归还侧判成「没人等」而躺回空闲栈，本协程此后再也等不到唤醒。
            {
                std::lock_guard lock(m_pool->m_asyncMutex);
                // 锁里只从空闲栈摘一条，不建连：建连要跑工厂 + connect（秒级），握着 m_asyncMutex
                // 会让归还路径、后台超时唤醒与健康检查全排在它后面。池未满时的建连已经在上面
                // 那次无锁尝试里做过了，这里还是空栈就说明确实没有立即可用的连接
                // （摘到的那条若不可用，其断开已在 m_mutex 之外；它仍落在本把锁内，因为等待表与
                //   空闲栈的这笔交换必须同锁完成，否则会被判成「没人等」而漏唤醒）
                m_result = m_pool->tryAcquireInternal();
                if (m_result)
                {
                    return false;
                }

                // 本次尝试期间有人腾出名额：那一格可能是刚被丢弃的连接（空闲栈没变多）或别的等待者
                // 没接住的交接。释放锁后重走一轮，让刚腾出的名额参与本次判断，而不是带着它入表去睡。
                if (m_pool->m_freedSlotEpoch.load(std::memory_order_seq_cst) != slotEpochBefore)
                {
                    continue;
                }

                // 停摆可能发生在第一次判定之后：停摆方先置标志、再持同一把锁收等待表；这里在锁内
                // 再核一次，避免把刚入表的这一位留给一个已经收完表的析构过程
                if (m_pool->m_isShuttingDown.load(std::memory_order_acquire))
                {
                    return false;
                }

                // 等待名额判据（与同步 acquire() 同一条、同一笔计数）：合计等待数已到上限就不再入表，
                // 置标记按空手收尾——acquireAsync() 看到标记会直接结账，不会另造等待体重走这一步。
                // 合计判据带一次瞬时误差（同步那份计数在 m_mutex 下维护，两把锁按约定不互相嵌套），
                // 安全阀方向取「宁可多放进几个」：误放只多一点内存，误杀会白白掐掉一个本可等到连接的请求
                if (!m_returnedFromWake && m_pool->m_asyncWaiters.size() + m_pool->m_syncWaitingCount.load(std::memory_order_relaxed) >= m_pool->m_config.maximumWaitingRequests)
                {
                    m_isWaitingCapReached = true;
                    return false;
                }
                m_returnedFromWake = false; // 只保被唤醒后的第一次重挂：入表即消费（acquireAsync() 读回的就是清零后的值）
                // 票据与入表同锁创建：唤醒方持锁读它，放锁后再建会让它读到空票据。三步都要分配，
                // 而本函数是 noexcept：分配失败置 m_isQueueingFailed、回 false，由 acquireAsync()
                // 按「这一轮排不上队」就地结账，不再重挂空转。票据一并收回，避免带着指向本帧的票据
                // 走出函数。注入点见 AllocationFailureWhileQueueingYieldsEmptyConnectionAndLeavesNoWaiter
                try
                {
                    m_resumeTicket = std::make_shared<ResumeTicket>();
                    m_resumeTicket->handle.store(m_handle, std::memory_order_release);
                    m_pool->m_asyncWaiters.push_back(this);
                    m_inList = true;
                    m_pool->refreshAsyncWaitingCount();
                } catch (...)
                {
                    // 不必摘表：deque::push_back 的强保证是「分配不出来时容器不变」，而 m_inList 排在它之后
                    // 才置真，所以抛到这里时这一条根本还没进表。只把票据收回——留着会让等待体带着一张
                    // 指向自己帧的票据走出本函数
                    m_resumeTicket.reset();
                    m_isQueueingFailed = true;
                    return false;
                }

                return true;
            }
        }
    }

    PooledConnection ConnectionPool::AcquireAwaiter::await_resume() noexcept
    {
        if (m_result)
        {
            // 与析构同一条纪律：判活与碰池必须在同一段令牌锁内。池已停摆时这次取出从未开始，
            // 连接随本帧关闭，交回空包装让调用方按「没拿到」收尾
            const std::lock_guard livenessLock(m_liveness->mutex);
            if (!m_liveness->isAlive)
            {
                m_result.reset();
                return {};
            }
            m_pool->m_activeCount.fetch_add(1);
            return PooledConnection(std::move(m_result), m_pool);
        }
        return {};
    }

    // ========================================================================
    // tryAcquire — 非阻塞获取
    // ========================================================================

    PooledConnection ConnectionPool::tryAcquire() noexcept
    {
        std::unique_ptr<DatabaseConnection> connection = tryAcquireOrCreateInternal();
        if (!connection)
        {
            return {};
        }

        m_activeCount.fetch_add(1);
        return PooledConnection(std::move(connection), this);
    }

    std::unique_ptr<DatabaseConnection> ConnectionPool::tryAcquireOrCreateInternal() noexcept
    {
        // 优先从空闲栈取（LIFO：最新归还的连接最可能还在热点缓存里）
        if (std::unique_ptr<DatabaseConnection> connection = tryAcquireInternal(); connection)
        {
            return connection;
        }

        // 空闲栈为空：未达上限就新建一条，达上限才算「无可用连接」。
        // 同步的 acquire() 与异步的 acquireAsync() 共用这一份判定——若异步路径只从空闲栈取，
        // 一个刚建好的池上所有异步获取都会先挂起（尽管池完全有能力建连），
        // 而同步获取却能立刻建连，两条路径给出相反的行为。
        // 建连不在锁内：工厂要跑 TCP 握手或开文件（秒级），持着 m_mutex 会让所有取出、
        // 统计与后台驱逐一起排队；先用「占位」把额度定下来，再放锁执行建连
        {
            std::lock_guard lock(m_mutex);
            // 停摆中不再建连：析构已经置起这个标志（与本判定同一把锁），此时补出来的那条
            // 只会变成一个没人认领的连接。等待路径每轮醒来都会走到这里，因此这条闸门
            // 也挡住了「池正在销毁时等待者自己补建」那种浪费
            if (m_isShuttingDown.load(std::memory_order_acquire))
            {
                return nullptr;
            }
            // 上限闸门判的是 m_totalCreated（= createdCount()），不是 active+idle 的 totalCount()：
            // 占位加在建连之前，所以在途那条会让两个数差一——分诊「池太小」时要对的是这一个
            if (m_totalCreated.load(std::memory_order_relaxed) >= m_config.maximumPoolSize)
            {
                return nullptr;
            }
            // 先占位再加锁外建连，否则两个线程会同时看到「未达上限」而各建一条，突破上限
            m_totalCreated.fetch_add(1, std::memory_order_relaxed);
        }

        std::unique_ptr<DatabaseConnection> newConnection = createNewConnection();
        if (!newConnection)
        {
            // 建连失败：把占位还回去，否则空闲额度会被永久占住
            m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
        }

        return newConnection;
    }

    // ========================================================================
    // returnConnection — 归还连接（由 PooledConnection 调用）
    // ========================================================================

    void ConnectionPool::returnConnection(std::unique_ptr<DatabaseConnection> connection, const bool isDiscard)
    {
        // 先验空再动账：这是个公有入口，空指针在这里减一次活跃计数会把计数打到回绕，
        // 而它永远不会再被加回来（activeCount() 变成天文数字，totalCount() 与容量判定随之失真）
        if (!connection)
        {
            return;
        }

        m_activeCount.fetch_sub(1);
        if (isDiscard)
        {
            // 调用方判定了这条连接的会话状态不可信（语句在驱动侧失败、事务半路出错那一类）：
            // 不复位、不交接、不入栈，直接关掉并叫醒等待者——名额确实空了出来。
            // 跳过会话复位是有意的：没有下一个借用者要保护，那条往返是白付的
            discardAndWakeReturnedConnection(std::move(connection));
            return;
        }

        // ---- 先判失联与存活期：这两条命中时连接直接丢弃，不必再做会话复位 ----
        // 两条去向（直接交给等待者 / 放回空闲栈）必须拿到同一条「还活着且还在存活期内」的连接：
        // 空闲栈一侧的取出路径早就在判这两条，而直接交接此前只判了失联——把一条已过存活期的连接
        // 从后门塞给协程，等于绕过 maximumLifetimeSeconds 的轮换约定（对端已单方面掐线的连接同理）
        // 判定只读这条连接自己的建立时刻与池配置，不涉及共享状态，因此不必进 m_mutex
        const auto returnedAt = std::chrono::steady_clock::now();
        if (!isConnectionHealthy(connection.get()) || isPastMaximumLifetime(*connection, returnedAt))
        {
            // 丢弃并退还名额（断开留在锁外，与 healthCheckLoop 同一条纪律），
            // 再叫醒等待者：空闲栈没变多，但名额确实空了出来
            discardAndWakeReturnedConnection(std::move(connection));
            return;
        }

        // 会话状态复位必须早于「放回空闲栈」与「直接交给等待者」两条去向：
        // 上一个借用者留下的会话级状态（Redis 的未发送管道、临时表等）不能串给下一个借用者。
        // 复位没干净意味着上一个借用者的 MULTI/事务还挂在服务端，这种连接交出去就是
        // 「会话状态串给下一位」最贵的那一种——不报错、语句照收、锁照握
        if (!connection->resetSessionState())
        {
            // 「复位没干净」这一条单独记数：它说的是「驱动清不掉上一个借用者留下的状态」，
            // 与失联、过期那两条正常生命周期不是一回事，混进丢弃总数里就分不出「轮换到了」与「有人在还脏连接」
            m_sessionResetFailures.fetch_add(1, std::memory_order_relaxed);

            // 丢弃并退还名额（断开留在锁外，与 healthCheckLoop 同一条纪律），
            // 再叫醒等待者：空闲栈没变多，但名额确实空了出来
            discardAndWakeReturnedConnection(std::move(connection));
            return;
        }

        // 借用者可能在借用期间改过命令超时，而连接对象是池里那一份：不退回基线就是把上一位的超时口径
        // 串给下一位——设短了下一位的正常查询莫名超时，设长了下一位失控的查询失去那道界，两边都不报错。
        // 排在丢弃判定之后（这条连接马上要关掉就不必白付一趟驱动往返），也排在 m_asyncMutex 之前
        // （MySQL 那一档要下发一条 SET SESSION，不能握着锁等一趟网络来回）
        connection->restoreQueryTimeoutBaseline();

        // ---- 交给排队的异步等待者，没人等就入空闲栈 ----
        // 判定与入栈必须同锁：等待者的 await_suspend 也是「持这把锁先摘空闲栈、摘不到才入表」，
        // 分处两段锁会让连接躺在栈里而等待者睡到超时，两侧各自复检都拦不住。
        // 锁序沿用 m_asyncMutex → m_mutex；恢复动作照纪律挪到锁外投递。
        std::shared_ptr<AcquireAwaiter::ResumeTicket> resumeTicket;
        Core::EventLoop                              *completionLoop = nullptr;
        {
            const std::lock_guard asyncLock(m_asyncMutex);

            noteSlotFreedForRegisteringWaiters();
            if (!m_asyncWaiters.empty())
            {
                // 队首优先：先等的先拿到连接（FIFO 公平）
                AcquireAwaiter *const waiter = m_asyncWaiters.front();
                m_asyncWaiters.pop_front();
                refreshAsyncWaitingCount();

                waiter->m_result = std::move(connection);
                waiter->m_inList = false;
                resumeTicket     = waiter->m_resumeTicket;
                completionLoop   = waiter->m_completionLoop;
            } else
            {
                const std::lock_guard lock(m_mutex);

                // 先把那一格分配出来、再把连接交进去：deque 的新块分配发生在 IdleEntry 被写入之前，
                // 因此分配失败时连接还完整握在本函数手里，能按既有的「丢弃」去向结账。
                // 本函数不是 noexcept，但它唯一的调用形态都在 noexcept 边界上：~PooledConnection
                // （隐式）走 returnConnectionIfAlive（显式）进来，穿出去就是 std::terminate
                try
                {
                    IdleEntry &slot   = m_idleStack.emplace_back();
                    slot.connection   = std::move(connection);
                    slot.returnedTime = returnedAt;
                    // 通知留在锁内：等待侧回锁后会先复检空闲栈再睡，锁内提交保证两者之间不再插入别的归还
                    m_idleCondition.notify_one();
                } catch (...)
                {
                    // 只在分配失败时走到：连接没进栈，出锁之后按丢弃结账
                }
            }
        }

        // 空闲栈那一格没分配出来（内存吃紧）：这条连接走「丢弃」那本账——名额退还与叫醒等待者
        // 都在既有出口里，不另写一遍算术；断开排在两把锁之外，与其他丢弃路径同纪律
        if (connection)
        {
            discardConnection(std::move(connection));
            wakeWaitersForFreedSlot();
        }

        // 恢复投回等待者自己的事件循环，而不是就地跑：归还可能发生在任意线程（工作线程、
        // 另一个事件循环），就地恢复会让协程的后续代码落到那个线程上，与调用方「回调在自己的
        // 循环线程」的写法相悖。放在锁外还有一条理由：postRemote 要拿目标循环的锁，
        // 握着 m_asyncMutex 等它就等于把归还路径排在一个陌生锁后面
        if (resumeTicket != nullptr)
        {
            postRemoteWithLog([&] { completionLoop->scheduler().postRemote([resumeTicket]() { resumeTicket->resumeOnce(); }); }, "交接唤醒");
        }
    }

    void ConnectionPool::wakeWaitersForFreedSlot() noexcept
    {
        // 无等待者时整段跳过两把锁：抖动形态每次归还都走丢弃出口，而绝大多数时刻池里没人排队。
        // 三个计数都用 seq_cst：异步侧在锁内发布等待表，同步侧在 m_mutex 内发布；注册中的那一位
        // 还没进等待表，但它会用自己的世代号核对看见这次腾出的名额，前提是这里不能把它漏过去。
        // 前两个为零、注册计数非零时仍要进来取锁：丢弃路径没有空闲连接可留，注册者等的是这一次
        // 「名额空出来了」的信号，不能因为表里暂时没人就跳过
        if (m_asyncWaitingCount.load(std::memory_order_seq_cst) == 0 && m_syncWaitingCount.load(std::memory_order_seq_cst) == 0 &&
            m_pendingRegistrations.load(std::memory_order_seq_cst) == 0)
        {
            return;
        }

        // 异步侧先叫醒一位：取走票据要持 m_asyncMutex，而恢复动作必须在锁外投
        // （就地恢复等于让协程的后续代码跑到本次归还的线程上，与「回调在事件循环线程」的约定相悖）
        std::shared_ptr<AcquireAwaiter::ResumeTicket> ticket;
        Core::EventLoop                              *completionLoop = nullptr;
        {
            const std::lock_guard asyncLock(m_asyncMutex);
            if (!m_asyncWaiters.empty())
            {
                AcquireAwaiter *const waiter = m_asyncWaiters.front();
                m_asyncWaiters.pop_front();
                refreshAsyncWaitingCount();
                waiter->m_inList           = false; // 结果留空：这次叫醒只说「有名额了」，拿到拿不到由它自己再试
                waiter->m_returnedFromWake = true;  // 重挂时上限判据对它放行（acquireAsync() 读回后喂给下一轮）：这一位正是为它腾的
                ticket                     = waiter->m_resumeTicket;
                completionLoop             = waiter->m_completionLoop;
            }
        }
        // 两把锁只顺序取、不嵌套：await_suspend 里是「m_asyncMutex → m_mutex」，反过来嵌套就是 AB-BA
        if (ticket != nullptr)
        {
            postRemoteWithLog([&] { completionLoop->scheduler().postRemote([ticket]() { ticket->resumeOnce(); }); }, "释放名额唤醒");
        }

        // 同步侧：条件变量的通知取在 m_mutex 之内，理由与归还入栈那一处相同
        {
            const std::lock_guard lock(m_mutex);
            m_idleCondition.notify_one();
        }
    }

    // ========================================================================
    // 统计查询
    // ========================================================================

    std::size_t ConnectionPool::activeCount() const noexcept
    {
        return m_activeCount.load(std::memory_order_relaxed);
    }

    std::size_t ConnectionPool::idleCount() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_idleStack.size();
    }

    std::size_t ConnectionPool::totalCount() const noexcept
    {
        const std::size_t active = m_activeCount.load(std::memory_order_relaxed);
        std::lock_guard   lock(m_mutex);
        return active + m_idleStack.size();
    }

    std::size_t ConnectionPool::waitingCount() const noexcept
    {
        const std::size_t syncWaiters = m_syncWaitingCount.load(std::memory_order_relaxed);
        std::lock_guard   lock(m_asyncMutex);
        return syncWaiters + m_asyncWaiters.size();
    }

    std::size_t ConnectionPool::createdCount() const noexcept
    {
        // 建连占位、丢弃与空闲过期回收都改的是 m_totalCreated（这本账同时是 maximumPoolSize 的分母），
        // 这里只给读数出口，因此不碰 m_mutex：统计读取排在锁后会把取出路径一起堵住
        return m_totalCreated.load(std::memory_order_relaxed);
    }

    std::size_t ConnectionPool::discardedCount() const noexcept
    {
        // 只增不减的历史数：与 createdCount() 那本「当下持有」的账分开，丢过多少不必再从两者的差里推
        return m_totalDiscarded.load(std::memory_order_relaxed);
    }

    std::size_t ConnectionPool::borrowTimeoutCount() const noexcept
    {
        return m_borrowTimeoutCount.load(std::memory_order_relaxed);
    }

    std::size_t ConnectionPool::waitingRejectedCount() const noexcept
    {
        return m_waitingRejectedCount.load(std::memory_order_relaxed);
    }

    std::uint64_t ConnectionPool::oldestBorrowStartEpochSeconds() const noexcept
    {
        std::int64_t oldestStartNanos = 0;
        for (const std::atomic<std::int64_t> &slot: m_liveness->borrowStartNanos)
        {
            const std::int64_t startNanos = slot.load(std::memory_order_relaxed);
            if (startNanos != 0 && (oldestStartNanos == 0 || startNanos < oldestStartNanos))
            {
                oldestStartNanos = startNanos;
            }
        }
        if (oldestStartNanos == 0)
        {
            return 0;
        }

        // 账本记的是 steady 起点，监控侧要的是 Unix 秒：在抓取这一刻用 (steady, system) 同时刻
        // 对齐折算成「借出时刻」，持有期内被 NTP 拨表也不会把这条读数弄歪。时钟异常回退时按 0 秒算
        const auto         nowSteady         = std::chrono::steady_clock::now();
        const std::int64_t nowSteadyNanos    = std::chrono::duration_cast<std::chrono::nanoseconds>(nowSteady.time_since_epoch()).count();
        const std::int64_t heldSeconds       = nowSteadyNanos > oldestStartNanos ? (nowSteadyNanos - oldestStartNanos) / 1000000000 : 0;
        const std::int64_t nowSystemSeconds  = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t startEpochSeconds = nowSystemSeconds - heldSeconds;
        if (startEpochSeconds <= 0)
        {
            // 0 在指标语义里是「没有在借」：算不到正数就报 0，宁可不报也不占用那个哨兵值
            return 0;
        }
        return static_cast<std::uint64_t>(startEpochSeconds);
    }

    void ConnectionPool::noteWaitingRefusal() noexcept
    {
        // 计数排在告警之前：持续涌入的请求方会条条命中，不该因为日志通路出问题而丢账
        const std::size_t ordinal = m_waitingRejectedCount.fetch_add(1, std::memory_order_relaxed);
        if (ordinal != 0)
        {
            // 只取上升沿：与调度器两条饱和计数同一口径
            return;
        }

        try
        {
            LOG_WARN_FMT("ConnectionPool: 等待借出的请求数已达上限（{}），本条借出不再排队、当场空手返回；"
                         "此后同类只计数不再逐条告警。命中它意味着池容量或下游耗时出了问题——要修的是那一头",
                         m_config.maximumWaitingRequests);
        } catch (...)
        {
            // 日志通路自己失败时只吞掉这条告警：计数已经落定
        }
    }

    // ========================================================================
    // 内部方法
    // ========================================================================

    std::unique_ptr<DatabaseConnection> ConnectionPool::tryAcquireInternal() noexcept
    {
        IdleEntry entry;
        {
            std::lock_guard lock(m_mutex);

            if (m_idleStack.empty())
            {
                return nullptr;
            }

            // LIFO：从栈顶取（最新归还的连接最可能还在热点缓存中）
            entry = std::move(m_idleStack.back());
            m_idleStack.pop_back();
        }

        // 摘出之后才知道这条能不能用，而「丢弃一条不能用的」要断开 socket——那是一次会阻塞的
        // 系统调用。整段判定与丢弃都必须在 m_mutex 之外：握着它做断开，等于让所有取出路径、
        // 统计读取与后台驱逐一起排在那次关闭后面（与 healthCheckLoop 的锁外断开是同一条纪律）
        if (isEntryExpired(entry))
        {
            // 过期连接直接关闭丢弃
            discardConnection(std::move(entry.connection));
            return nullptr;
        }

        if (!isConnectionHealthy(entry.connection.get()))
        {
            discardConnection(std::move(entry.connection));
            return nullptr;
        }

        // 借出前的探针：只在配置打开时花这一次往返。它挡的是「状态读起来还连着、对面其实已经
        // 重启过」那一格——不开探针的话这条连接要等下一个借用者的第一条语句报错才被丢掉
        if (m_config.probeBeforeAcquire && !probeConnection(*entry.connection))
        {
            discardConnection(std::move(entry.connection));
            return nullptr;
        }

        return std::move(entry.connection);
    }


    std::unique_ptr<DatabaseConnection> ConnectionPool::createNewConnection() noexcept
    {
        // 建连重试的预算：只重 isTransientConnectFailure() 认的那一类（服务端正在重启、连接被拒），
        // 间隔 5 毫秒起翻倍、单次封顶 100 毫秒，累计不超过 acquireTimeoutMilliseconds ——
        // 后者本来就是这次借用愿意等的量级，重试不许把它撑破。鉴权错、库名错这类重试也不会中，
        // 一次就空手
        const auto attemptDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_config.acquireTimeoutMilliseconds);
        auto       backoff         = std::chrono::milliseconds{5};

        for (std::size_t attempt = 1;; ++attempt)
        {
            // 返回的是 pair 的成员而不是结构化绑定的名字：绑定名不是对象，move 它只会退化成拷贝
            auto outcome = attemptCreateConnection();
            if (outcome.first)
            {
                return std::move(outcome.first);
            }
            const bool isRetryable = outcome.second;
            // 「能不能再试」由驱动报的那条形如「连不上」的错误码决定（见 attemptCreateConnection）；
            // 工厂抛异常、返回空与鉴权错都落在 isRetryable 为假的一侧，一次就空手
            const bool isLastAllowedAttempt = attempt >= m_config.maximumConnectAttempts;
            if (!isRetryable || isLastAllowedAttempt || std::chrono::steady_clock::now() >= attemptDeadline)
            {
                return nullptr;
            }
            // 睡这一觉是在借用者所在的线程上（建连本来就在那条线程上跑），因此它有单次封顶与
            // 总量上限，不是一句「多试几次」就无限等下去
            const auto sleepFor = std::min(backoff, std::chrono::milliseconds{100});
            std::this_thread::sleep_for(sleepFor);
            backoff *= 2;
            m_connectRetryCount.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG_FMT("ConnectionPool: 建连失败且属可重试的一类，第 {} 次重试前等 {} 毫秒", attempt, sleepFor.count());
        }
    }

    std::pair<std::unique_ptr<DatabaseConnection>, bool> ConnectionPool::attemptCreateConnection() noexcept
    {
        try
        {
            std::unique_ptr<DatabaseConnection> connection = m_factory();
            if (!connection)
            {
                return {nullptr, false};
            }

            if (!connection->connect())
            {
                // 只有驱动明确报「连不上」的那几条码才值得再试：分类表在
                // isTransientConnectFailure() 一处，本函数不重复一份
                const bool isRetryable = isTransientConnectFailure(*connection);
                if (isRetryable)
                {
                    // 把驱动报的原因留在日志里：重试成功之后这条连接的 lastError 会被下一次 connect
                    // 清掉，届时再也看不出「刚才重试的那几次里有一直在拒我们」
                    LOG_WARN_FMT("ConnectionPool: 建连失败且属可重试的一类，原因：{}", connection->lastError());
                }
                return {nullptr, isRetryable};
            }

            // 建立成功的时刻记在连接自己身上：池在借出/归还之间没有任何地方能存这份信息，
            // 另建一张按裸指针索引的表反而多一把锁、多一份分配，还留下地址复用后的错配空间
            connection->markEstablishedAt(std::chrono::steady_clock::now());
            // 同时把创建者留下的命令超时记成基线：工厂回调里设的那个值就是这条连接对每一位借用者
            // 承诺的口径，归还时按它退回（见 restoreQueryTimeoutBaseline）
            connection->markQueryTimeoutBaseline();

            return {std::move(connection), false};
        } catch (const std::exception &failure)
        {
            // 工厂或 connect 抛异常时返回空指针。原因必须出声：调用方从 nullptr 只能知道
            // 「拿不到连接」，而真正的原因（驱动报的鉴权失败、主机不可达、URL 写错）在这里
            // 是唯一持有过它的地方，丢了就查不到。建连接不在请求热路径上，逐条打不会淹
            LOG_ERROR_EXCEPTION(failure, "ConnectionPool: 创建连接失败，本次获取按「无连接可用」收场（原因：{}）", failure.what());
            return {nullptr, false};
        } catch (...)
        {
            LOG_ERROR("ConnectionPool: 创建连接失败（非标准异常），本次获取按「无连接可用」收场");
            return {nullptr, false};
        }
    }


    void ConnectionPool::refreshAsyncWaitingCount() noexcept
    {
        // 只在持有 m_asyncMutex 的段里、改动 m_asyncWaiters 之后调用。灌的是「当下有多少」而不是增减量：
        // 漏调一处只会让这一格少报，不会一路漂走，而用例把这份镜像与 waitingCount() 逐轮对齐。
        // 用 release 发布：无等待者的唤醒快路径以 acquire 读它，读到非零才去取等待表锁
        m_asyncWaitingCount.store(m_asyncWaiters.size(), std::memory_order_seq_cst);
    }

    void ConnectionPool::removeAsyncWaiterLocked(AcquireAwaiter *waiter) noexcept
    {
        if (const auto it = std::ranges::find(m_asyncWaiters, waiter); it != m_asyncWaiters.end())
        {
            m_asyncWaiters.erase(it);
            waiter->m_inList = false;
            refreshAsyncWaitingCount();
        }
    }

    void ConnectionPool::closeTrackedConnection(std::unique_ptr<DatabaseConnection> connection) noexcept
    {
        if (!connection)
        {
            return;
        }

        connection->disconnect();
        connection.reset();
    }

    void ConnectionPool::discardConnection(std::unique_ptr<DatabaseConnection> connection) noexcept
    {
        closeTrackedConnection(std::move(connection));
        m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
        m_totalDiscarded.fetch_add(1, std::memory_order_relaxed);
        // 丢弃同样退还了名额，但空闲栈没变多：正在注册的等待者只靠等待表看不见这一笔，
        // 必须借世代号告诉它「有东西腾出来了，重走一轮建连」
        noteSlotFreedForRegisteringWaiters();
    }

    void ConnectionPool::noteSlotFreedForRegisteringWaiters() noexcept
    {
        if (m_pendingRegistrations.load(std::memory_order_seq_cst) != 0)
        {
            m_freedSlotEpoch.fetch_add(1, std::memory_order_seq_cst);
        }
    }

    void ConnectionPool::discardAndWakeReturnedConnection(std::unique_ptr<DatabaseConnection> connection) noexcept
    {
        discardConnection(std::move(connection));
        wakeWaitersForFreedSlot();
    }

    bool ConnectionPool::returnConnectionIfAlive(std::unique_ptr<DatabaseConnection> &connection, const std::shared_ptr<PoolLiveness> &liveness, const bool isDiscard) noexcept
    {
        if (liveness == nullptr)
        {
            return false;
        }

        // 判活与归还必须在同一段令牌锁内：池析构全程持这把锁，两者因此不会交错
        const std::lock_guard livenessLock(liveness->mutex);
        if (!liveness->isAlive)
        {
            return false;
        }

        returnConnection(std::move(connection), isDiscard);
        return true;
    }

    void ConnectionPool::expireTimedOutWaiters() noexcept
    {
        // 一条到期等待就是「票据 + 要投回哪个循环」这一对，两者必须同生同灭。原先用两条平行向量
        // 分两次 push_back：中间那次分配失败会让两条向量长度错位，而下面的投递按 tickets.size()
        // 去索引 loops，那是越界读。合成一对之后，分配失败要么整条收不下、要么收全。
        struct ExpiredWaiter
        {
            std::shared_ptr<AcquireAwaiter::ResumeTicket> ticket;         ///< 唤醒票据（取走即失效，重复投递安全）
            Core::EventLoop                              *completionLoop; ///< 这次唤醒要投回的事件循环
        };

        const auto                 now = std::chrono::steady_clock::now();
        std::vector<ExpiredWaiter> expiredWaiters;
        try
        {
            std::lock_guard               lock(m_asyncMutex);
            std::vector<AcquireAwaiter *> timedOutWaiters;

            // 容量先要到手，再动这张表。这段里唯一可能失败的分配就是这两次 reserve，而它们发生在
            // 任何一条等待者被摘出链表之前——reserve 抛出来时表里一条没动，catch 里那句
            // 「其余留到下一轮」才是真的。原先的顺序是先 erase_if 摘链、再逐条收集，收集那次
            // push_back 一抛，剩下的几条已经离开 m_asyncWaiters 却没人唤醒：那条协程永远睡着，
            // 而下一轮扫描再也看不见它（它已不在表里），借用超时的读数也不涨——现场只剩「调用方不动了」
            const std::size_t timedOutCount = static_cast<std::size_t>(
                    std::count_if(m_asyncWaiters.begin(), m_asyncWaiters.end(), [now](const AcquireAwaiter *const waiter) { return waiter->m_deadline <= now; }));
            expiredWaiters.reserve(timedOutCount);
            timedOutWaiters.reserve(timedOutCount);

            std::erase_if(m_asyncWaiters,
                          [&timedOutWaiters, now](AcquireAwaiter *const waiter)
                          {
                              // 尚未到点的留着；到点的摘出列表，结果保持空（等价的「超时返回空」）
                              if (waiter->m_deadline > now)
                              {
                                  return false;
                              }
                              timedOutWaiters.push_back(waiter); // 容量已在上面要到手，这一步不再分配
                              return true;
                          });
            refreshAsyncWaitingCount();
            for (AcquireAwaiter *const waiter: timedOutWaiters)
            {
                if (waiter->m_resumeTicket == nullptr)
                {
                    waiter->m_inList = false;
                    continue;
                }

                // 先把这一对完整收下，再改 m_inList：顺序反过来，一次分配失败就会留下一个
                // 「已从链表摘出、m_inList 已清、却没人唤醒」的等待器，那条协程便永远睡着。
                // 容量已在摘链之前要到，这一步因此不再分配
                expiredWaiters.push_back(ExpiredWaiter{waiter->m_resumeTicket, waiter->m_completionLoop});
                waiter->m_inList = false;
            }
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("ConnectionPool: 驱逐超时等待者时分配失败，已收下的 {} 条照常唤醒，其余留到下一轮（原因：{}）", expiredWaiters.size(), failure.what());
        } catch (...)
        {
            LOG_ERROR_FMT("ConnectionPool: 驱逐超时等待者时分配失败（非标准异常），已收下的 {} 条照常唤醒，其余留到下一轮", expiredWaiters.size());
        }

        // 恢复投回各自的事件循环：本函数跑在后台线程上，就地恢复会把协程的后续代码
        // 跑到这个线程上，而调用方是按「回调都在自己的事件循环线程上」写代码的。
        // 与交接路径同样经票据投递：投出去之后调用方可能立刻销毁 Task，裸句柄会 resume 已释放的帧
        //
        // 逐条投递并各自兜底：postRemote 自己要分配队列结点与 std::function 包装，
        // 一批里某一条失败只该丢那一条的唤醒，不该让其余的停在半途。
        // 本函数是 noexcept，抛穿出去就是 terminate，而这里已经退出临界区、不留锁
        for (const ExpiredWaiter &expired: expiredWaiters)
        {
            const std::shared_ptr<AcquireAwaiter::ResumeTicket> ticket = expired.ticket;
            postRemoteWithLog([&expired, ticket] { expired.completionLoop->scheduler().postRemote([ticket]() { ticket->resumeOnce(); }); }, "超时唤醒");
        }
    }

} // namespace AsynGyanis::Database
