#include "Database/Pool/ConnectionPool.h"

#include "Core/EventLoop/EventLoop.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Database
{

    // ========================================================================
    // 构造 / 析构
    // ========================================================================

    ConnectionPool::ConnectionPool(std::function<std::unique_ptr<DatabaseConnection>()> factory, const PoolConfig &config) :
        m_factory(std::move(factory))
        , m_config(config)
        , m_healthThread([this](std::stop_token stopToken)
        {
            healthCheckLoop(std::move(stopToken));
        })
    {
    }

    ConnectionPool::~ConnectionPool()
    {
        // 置假必须在令牌锁里做：此刻起归还路径要么已经持锁进来（本析构会等它走完），
        // 要么看到「池已停摆」直接关掉连接——池已析构就不能再被取消引用。
        // **锁只覆盖这一次赋值**：析构末尾会就地恢复等待者，被恢复的协程可能归还
        // PooledConnection，那条路径要锁同一把**非递归**令牌锁；持锁跨越恢复就是同线程
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
        // **恢复必须在锁外做**：协程恢复后可能立刻再触池（重试 acquire、归还连接），
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
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(m_config.acquireTimeoutMilliseconds);

        std::unique_ptr<DatabaseConnection> connection;
        {
            std::unique_lock lock(m_mutex);
            m_syncWaitingCount.fetch_add(1);

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

            m_syncWaitingCount.fetch_sub(1);
            // 池析构可能在等最后一位同步等待者离开（它睡在同一把 m_idleCondition 上等计数归零）
            m_idleCondition.notify_all();
        }

        if (!connection)
        {
            // 超时或池已停摆：拿不到连接就交出空的包装，让调用方看见「没拿到」而不是异常。
            // 只有前者记账：停摆期空手是正常收尾，混进来会让这个容量指标在每次优雅停机时虚涨
            if (!m_isShuttingDown.load(std::memory_order_acquire))
            {
                m_borrowTimeoutCount.fetch_add(1, std::memory_order_relaxed);
            }
            return {};
        }

        m_activeCount.fetch_add(1);
        return PooledConnection(std::move(connection), this);
    }

    // ========================================================================
    // acquireAsync — 协程异步获取
    // ========================================================================

    Core::Task<PooledConnection> ConnectionPool::acquireAsync(Core::EventLoop &loop)
    {
        // 截止时刻在这里定一次，重挂的每一轮共用它：被叫醒却没拿到连接不会把等待上限往后推
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(m_config.acquireTimeoutMilliseconds);

        while (true)
        {
            AcquireAwaiter   awaiter(this, &loop, deadline);
            PooledConnection result = co_await awaiter;

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
        // 再试一次：在 await_ready 和 await_suspend 之间可能已有连接归还
        m_result = m_pool->tryAcquireOrCreateInternal();
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

        // 仍无可用连接：加入等待列表。**这次判定必须与入表同锁**：唤醒方（归还路径的「交接或入栈」）拿的
        // 也是 m_asyncMutex，两者若不同锁，「再试失败」到「入表」之间归还的连接会被
        // 归还侧判成「没人等」而躺回空闲栈，本协程此后再也等不到唤醒。
        {
            std::lock_guard lock(m_pool->m_asyncMutex);
            // 锁里**只从空闲栈摘一条**，不建连：建连要跑工厂 + connect（秒级），握着 m_asyncMutex
            // 会让归还路径、后台超时唤醒与健康检查全排在它后面。池未满时的建连已经在上面
            // 那次无锁尝试里做过了，这里还是空栈就说明确实没有立即可用的连接
            // （摘到的那条若不可用，其断开已在 m_mutex 之外；它仍落在本把锁内，因为等待表与
            //   空闲栈的这笔交换必须同锁完成，否则会被判成「没人等」而漏唤醒）
            m_result = m_pool->tryAcquireInternal();
            if (m_result)
            {
                return false;
            }
            // 票据与入表同锁创建：唤醒方持锁读它，放锁之后再建会让唤醒方读到空票据。
            // 截止时刻不在这儿定：它由 acquireAsync() 造出本等待体时给一次，重挂的每轮共用，
            // 因此「反复失败的重试把超时无限顺延」这条路根本不存在；它在入表之前就已写好，
            // 后台线程持同一把锁读它，看见的只会是已写定的值（不是默认的时钟纪元）
            m_resumeTicket = std::make_shared<ResumeTicket>();
            m_resumeTicket->handle.store(handle, std::memory_order_release);
            m_pool->m_asyncWaiters.push_back(this);
            m_inList = true;
        }

        return true;
    }

    PooledConnection ConnectionPool::AcquireAwaiter::await_resume() noexcept
    {
        if (m_result)
        {
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
        // **建连不在锁内**：工厂要跑 TCP 握手或开文件（秒级），持着 m_mutex 会让所有取出、
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

    void ConnectionPool::returnConnection(std::unique_ptr<DatabaseConnection> connection)
    {
        // 先验空再动账：这是个公有入口，空指针在这里减一次活跃计数会把计数打到回绕，
        // 而它永远不会再被加回来（activeCount() 变成天文数字，totalCount() 与容量判定随之失真）
        if (!connection)
        {
            return;
        }

        m_activeCount.fetch_sub(1);

        // 会话状态复位必须早于「放回空闲栈」与「直接交给等待者」两条去向：
        // 上一个借用者留下的会话级状态（Redis 的未发送管道、临时表等）不能串给下一个借用者
        connection->resetSessionState();

        // ---- 判失联与判存活期：都排在两条去向之前 ----
        // 两条去向（直接交给等待者 / 放回空闲栈）必须拿到同一条「还活着且还在存活期内」的连接：
        // 空闲栈一侧的取出路径早就在判这两条，而直接交接此前只判了失联——把一条已过存活期的连接
        // 从后门塞给协程，等于绕过 maximumLifetimeSeconds 的轮换约定（对端已单方面掐线的连接同理）
        // 判定只读这条连接自己的建立时刻与池配置，不涉及共享状态，因此不必进 m_mutex
        const auto returnedAt = std::chrono::steady_clock::now();
        if (!isConnectionHealthy(connection.get()) || isPastMaximumLifetime(*connection, returnedAt))
        {
            // 丢弃并退还名额（断开留在锁外，与 healthCheckLoop 同一条纪律），
            // 再叫醒等待者：空闲栈没变多，但名额确实空了出来
            discardConnection(std::move(connection));
            wakeWaitersForFreedSlot();
            return;
        }

        // ---- 交给排队的异步等待者，没人等就入空闲栈 ----
        // 两件事必须在同一段 m_asyncMutex 之内决定：等待者的 await_suspend 是「持着这把锁先摘一次
        // 空闲栈，摘不到才把自己挂进等待表」的形状。判定与入栈若分处两段锁，「判没人等 → 等待者入表
        // → 连接入栈」这条交错会让连接躺在栈里、等待者睡到超时，而两侧各自的复检都拦不住它。
        // 锁序沿用既定方向（m_asyncMutex → m_mutex，反向嵌套就是 AB-BA）；恢复动作照纪律挪到锁外投递。
        std::shared_ptr<AcquireAwaiter::ResumeTicket> resumeTicket;
        Core::EventLoop *                             completionLoop = nullptr;
        {
            const std::lock_guard asyncLock(m_asyncMutex);

            if (!m_asyncWaiters.empty())
            {
                // 队首优先：先等的先拿到连接（FIFO 公平）
                AcquireAwaiter *const waiter = m_asyncWaiters.front();
                m_asyncWaiters.pop_front();

                waiter->m_result = std::move(connection);
                waiter->m_inList = false;
                resumeTicket     = waiter->m_resumeTicket;
                completionLoop   = waiter->m_completionLoop;
            }
            else
            {
                const std::lock_guard lock(m_mutex);

                IdleEntry entry;
                entry.connection   = std::move(connection);
                entry.returnedTime = returnedAt;
                m_idleStack.push_back(std::move(entry));
                // 通知留在锁内：等待侧回锁后会先复检空闲栈再睡，锁内提交保证两者之间不再插入别的归还
                m_idleCondition.notify_one();
            }
        }

        // 恢复投回等待者自己的事件循环，而不是就地跑：归还可能发生在任意线程（工作线程、
        // 另一个事件循环），就地恢复会让协程的后续代码落到那个线程上，与调用方「回调在自己的
        // 循环线程」的写法相悖。放在锁外还有一条理由：postRemote 要拿目标循环的锁，
        // 握着 m_asyncMutex 等它就等于把归还路径排在一个陌生锁后面
        if (resumeTicket != nullptr)
        {
            completionLoop->scheduler().postRemote(
                    [resumeTicket]()
                    {
                        resumeTicket->resumeOnce();
                    });
        }
    }

    void ConnectionPool::wakeWaitersForFreedSlot() noexcept
    {
        // 异步侧先叫醒一位：取走票据要持 m_asyncMutex，而恢复动作必须在锁外投
        // （就地恢复等于让协程的后续代码跑到本次归还的线程上，与「回调在事件循环线程」的约定相悖）
        std::shared_ptr<AcquireAwaiter::ResumeTicket> ticket;
        Core::EventLoop *                             completionLoop = nullptr;
        {
            const std::lock_guard asyncLock(m_asyncMutex);
            if (!m_asyncWaiters.empty())
            {
                AcquireAwaiter *const waiter = m_asyncWaiters.front();
                m_asyncWaiters.pop_front();
                waiter->m_inList = false;   // 结果留空：这次叫醒只说「有名额了」，拿到拿不到由它自己再试
                ticket         = waiter->m_resumeTicket;
                completionLoop = waiter->m_completionLoop;
            }
        }
        // 两把锁只顺序取、不嵌套：await_suspend 里是「m_asyncMutex → m_mutex」，反过来嵌套就是 AB-BA
        if (ticket != nullptr)
        {
            completionLoop->scheduler().postRemote(
                    [ticket]()
                    {
                        ticket->resumeOnce();
                    });
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
        // 建连的占位与回退都记在 m_totalCreated 上（那里是唯一改它的两处），这里只给读数出口，
        // 因此不碰 m_mutex：统计读取排在锁后会把取出路径一起堵住
        return m_totalCreated.load(std::memory_order_relaxed);
    }

    std::size_t ConnectionPool::borrowTimeoutCount() const noexcept
    {
        return m_borrowTimeoutCount.load(std::memory_order_relaxed);
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

        return std::move(entry.connection);
    }

    std::unique_ptr<DatabaseConnection> ConnectionPool::createNewConnection() noexcept
    {
        try
        {
            std::unique_ptr<DatabaseConnection> connection = m_factory();
            if (!connection)
            {
                return nullptr;
            }

            if (!connection->connect())
            {
                return nullptr;
            }

            // 建立成功的时刻记在连接自己身上：池在借出/归还之间没有任何地方能存这份信息，
            // 另建一张按裸指针索引的表反而多一把锁、多一份分配，还留下地址复用后的错配空间
            connection->markEstablishedAt(std::chrono::steady_clock::now());

            return connection;
        } catch (...)
        {
            // 工厂或 connect 抛异常时返回空指针
            return nullptr;
        }
    }

    bool ConnectionPool::isPastMaximumLifetime(const DatabaseConnection &connection,
                                               const std::chrono::steady_clock::time_point now) const noexcept
    {
        // 0 视为「立即过期」：连接一归还就被丢弃，永不进空闲栈
        if (m_config.maximumLifetimeSeconds == 0)
        {
            return true;
        }

        const auto lifetimeSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(now - connection.establishedAt()).count();
        return static_cast<std::size_t>(lifetimeSeconds) >= m_config.maximumLifetimeSeconds;
    }

    bool ConnectionPool::isEntryExpired(const IdleEntry &entry) const noexcept
    {
        const auto now = std::chrono::steady_clock::now();

        if (isPastMaximumLifetime(*entry.connection, now))
        {
            return true;
        }

        // 空闲超时检查：0 表示不设空闲超时限制
        if (m_config.idleTimeoutSeconds > 0)
        {
            const auto idleSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(now - entry.returnedTime).count();
            if (static_cast<std::size_t>(idleSeconds) >= m_config.idleTimeoutSeconds)
            {
                return true;
            }
        }

        return false;
    }

    bool ConnectionPool::isConnectionHealthy(DatabaseConnection *connection) noexcept
    {
        if (connection == nullptr)
        {
            return false;
        }

        // 纯状态查询：各驱动的 isConnected() 都只读内部状态标志与句柄非空，
        // 刻意不发任何网络请求（无探活往返），因此在获取与归还路径上直接调用是安全的
        try
        {
            return connection->isConnected();
        } catch (...)
        {
            return false;
        }
    }

    void ConnectionPool::healthCheckLoop(const std::stop_token &stopToken)
    {
        const auto interval = std::max(m_config.healthCheckIntervalSeconds, std::size_t{1});

        // 停止请求直接把本线程从等待里叫醒：jthread 的 join 因此不必等满当前那个 1 秒分片。
        // 回调随本函数返回而解除，所以它引用的 this 一直在有效期内
        const std::stop_callback wakeupOnStop(stopToken, [this]
        {
            m_healthWakeCondition.notify_all();
        });

        while (!stopToken.stop_requested())
        {
            // 分段睡眠，每 1 秒醒一次：既检查停止标志，也推进异步等待者的截止时刻
            const auto            totalSleepMilliseconds  = interval * 1000;
            constexpr std::size_t kSleepChunkMilliseconds = 1000;

            auto remainingMilliseconds = static_cast<int64_t>(totalSleepMilliseconds);
            while (remainingMilliseconds > 0 && !stopToken.stop_requested())
            {
                const auto chunk = std::min(static_cast<int64_t>(kSleepChunkMilliseconds), remainingMilliseconds);
                {
                    std::unique_lock lock(m_healthWakeMutex);
                    // 谓词判定与 notify 共用这把锁，因此停止请求落在「刚要进 wait_for 之前」也不会漏唤醒
                    m_healthWakeCondition.wait_for(lock, std::chrono::milliseconds(chunk),
                                                   [&stopToken]
                                                   {
                                                       return stopToken.stop_requested();
                                                   });
                }

                if (stopToken.stop_requested())
                {
                    break;
                }
                remainingMilliseconds -= chunk;

                // 每秒一次：把等到截止时刻的异步等待者以「空连接」唤醒。
                // 放在这里而不是等下一个健康检查周期，是为了让异步超时的粒度与同步一致（秒级以内）
                expireTimedOutWaiters();
            }

            if (stopToken.stop_requested())
            {
                break;
            }

            // ---- 遍历空闲栈，驱逐过期连接 ----
            // 锁内只做「摘出与计数」；disconnect 可能走网络/系统调用，必须留到锁外，
            // 否则一次慢断开会让所有取出路径一起等在这把锁上
            std::vector<std::unique_ptr<DatabaseConnection>> expiredConnections;
            {
                std::lock_guard lock(m_mutex);

                auto removeBegin = std::ranges::remove_if(m_idleStack,
                                                          [this, &expiredConnections](IdleEntry &entry) -> bool
                                                          {
                                                              if (isEntryExpired(entry))
                                                              {
                                                                  expiredConnections.push_back(std::move(entry.connection));
                                                                  m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
                                                                  return true;
                                                              }
                                                              return false;
                                                          }).begin();

                if (removeBegin != m_idleStack.end())
                {
                    m_idleStack.erase(removeBegin, m_idleStack.end());
                }
            }
            // 锁外断开：析构 unique_ptr 即关闭底层连接
            expiredConnections.clear();
        }
    }

    void ConnectionPool::removeAsyncWaiterLocked(AcquireAwaiter *waiter) noexcept
    {
        if (const auto it = std::ranges::find(m_asyncWaiters, waiter); it != m_asyncWaiters.end())
        {
            m_asyncWaiters.erase(it);
            waiter->m_inList = false;
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
    }

    bool ConnectionPool::returnConnectionIfAlive(std::unique_ptr<DatabaseConnection> &connection, const std::shared_ptr<PoolLiveness> &liveness,
                                                 const bool countAsActive) noexcept
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

        // 交接发生在归还路径减过活跃计数之后，等待器取出时要把它补回去
        if (countAsActive)
        {
            m_activeCount.fetch_add(1);
        }
        returnConnection(std::move(connection));
        return true;
    }

    void ConnectionPool::expireTimedOutWaiters() noexcept
    {
        const auto                                 now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<AcquireAwaiter::ResumeTicket>> timedOutTickets;
        std::vector<Core::EventLoop *>             completionLoops;
        {
            std::lock_guard lock(m_asyncMutex);
            std::vector<AcquireAwaiter *> timedOutWaiters;
            std::erase_if(m_asyncWaiters,
                          [&timedOutWaiters, now](AcquireAwaiter *const waiter)
                          {
                              // 尚未到点的留着；到点的摘出列表，结果保持空（等价的「超时返回空」）
                              if (waiter->m_deadline > now)
                              {
                                  return false;
                              }
                              timedOutWaiters.push_back(waiter);
                              return true;
                          });
            for (AcquireAwaiter *const waiter: timedOutWaiters)
            {
                waiter->m_inList = false;
                if (waiter->m_resumeTicket != nullptr)
                {
                    timedOutTickets.push_back(waiter->m_resumeTicket);
                    completionLoops.push_back(waiter->m_completionLoop);
                }
            }
        }

        // 恢复投回各自的事件循环：本函数跑在后台线程上，就地恢复会把协程的后续代码
        // 跑到这个线程上，而调用方是按「回调都在自己的事件循环线程上」写代码的。
        // 与交接路径同样经票据投递：投出去之后调用方可能立刻销毁 Task，裸句柄会 resume 已释放的帧
        for (std::size_t index = 0; index < timedOutTickets.size(); ++index)
        {
            const std::shared_ptr<AcquireAwaiter::ResumeTicket> ticket = timedOutTickets[index];
            completionLoops[index]->scheduler().postRemote(
                    [ticket]()
                    {
                        ticket->resumeOnce();
                    });
        }
    }

} // namespace AsynGyanis::Database
