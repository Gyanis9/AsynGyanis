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
        // 启动后台健康检查线程，周期性清理过期空闲连接
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

        // 请求后台线程停止并等待其退出
        m_healthThread.request_stop();
        if (m_healthThread.joinable())
        {
            m_healthThread.join();
        }

        // 关闭所有空闲连接
        {
            std::lock_guard lock(m_mutex);
            for (auto &entry: m_idleStack)
            {
                if (entry.connection)
                {
                    // 从创建时间映射表中移除
                    {
                        std::lock_guard ctLock(m_ctMapMutex);
                        m_creationTimeMap.erase(entry.connection.get());
                    }
                    entry.connection->disconnect();
                    entry.connection.reset();
                }
            }
            m_idleStack.clear();
        }

        {
            std::lock_guard ctLock(m_ctMapMutex);
            m_creationTimeMap.clear();
        }

        // 唤醒所有剩余的同步等待者，让它们拿到空连接
        m_cv.notify_all();

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
            // 取走再恢复：句柄只能被恢复一次（另一侧可能同时把它置空）
            if (const std::coroutine_handle<> abandonedHandle = ticket->handle.exchange(nullptr); abandonedHandle != nullptr)
            {
                abandonedHandle.resume();
            }
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
        // 计算截止时间
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(m_config.acquireTimeoutMilliseconds);

        std::unique_lock lock(m_mutex);

        // 等待直到有空闲连接或超时
        m_syncWaitingCount.fetch_add(1);
        while (m_idleStack.empty())
        {
            // 等待条件变量，最多等到截止时间
            if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                // 超时：没有拿到连接，返回空
                m_syncWaitingCount.fetch_sub(1);
                return {};
            }
        }
        m_syncWaitingCount.fetch_sub(1);

        // 从 LIFO 栈顶弹出（最新归还的连接最可能还在热点缓存中）
        IdleEntry entry = std::move(m_idleStack.back());
        m_idleStack.pop_back();
        lock.unlock();

        // ---- 健康检查 ----
        // 出栈后检查连接健康状态：不健康则丢弃并重建
        if (!isConnectionHealthy(entry.connection.get()))
        {
            // 从创建时间映射表中移除
            {
                std::lock_guard ctLock(m_ctMapMutex);
                m_creationTimeMap.erase(entry.connection.get());
            }
            entry.connection->disconnect();
            entry.connection.reset();

            entry.connection = createNewConnection();
            if (!entry.connection)
            {
                // 重建失败：总创建数减一（把不健康的丢弃了但没能补上）
                m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
                // 返回空：调用方检查 operator bool()
                return {};
            }
        }

        m_activeCount.fetch_add(1);
        return PooledConnection(std::move(entry.connection), this);
    }

    // ========================================================================
    // acquireAsync — 协程异步获取
    // ========================================================================

    Core::Task<PooledConnection> ConnectionPool::acquireAsync(Core::EventLoop &loop)
    {
        AcquireAwaiter   awaiter(this, &loop);
        PooledConnection result = co_await awaiter;
        co_return std::move(result);
    }

    // ========================================================================
    // AcquireAwaiter 实现
    // ========================================================================

    ConnectionPool::AcquireAwaiter::~AcquireAwaiter()
    {
        // 如果还在等待列表中，自行移除（防悬挂）
        // 这发生在调用方提前销毁 Task 导致协程帧被释放的场景
        if (m_inList)
        {
            m_pool->removeAsyncWaiter(this);
        }

        // 已经交到手上、却来不及被取走的连接按「取出后立刻归还」结账：交接那一刻归还路径
        // 已经减过活跃计数，这次取出则从未被记上（记在 await_resume 里），因此先补记再归还。
        // 不结账的话池会永远少一个位置——连接随帧销毁，而总创建数不降、空闲栈也拿不回它，
        // 反复丢弃几次之后所有 acquire 都会卡在「池已满」上直到超时。
        // 判活与调用都在令牌锁内（与 PooledConnection::doReturnToPool 同一处置）：调用方可能
        // 把这具帧留到池析构之后才销毁，那时碰池的记账就是释放后使用
        if (m_result && m_liveness != nullptr)
        {
            const std::lock_guard livenessLock(m_liveness->mutex);
            if (m_liveness->isAlive)
            {
                m_pool->m_activeCount.fetch_add(1);
                m_pool->returnConnection(std::move(m_result));
            }
        }

        // 票据里的句柄一并清空：池可能已经把「恢复这次等待」投回了事件循环（交接连接那一刻），
        // 而本帧眼下就要析构。投递那边执行时看到空句柄会直接跳过，不会 resume 已释放的帧
        if (m_resumeTicket)
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
        this->m_handle = handle;

        // 再试一次：在 await_ready 和 await_suspend 之间可能已有连接归还
        m_result = m_pool->tryAcquireOrCreateInternal();
        if (m_result)
        {
            return false; // 获取到连接，不挂起
        }

        // 仍无可用连接：加入等待列表。**这次判定必须与入表同锁**：唤醒方（归还路径）拿的
        // 也是 m_asyncMutex，两者若不同锁，「再试失败」到「入表」之间归还的连接会被
        // notifyAsyncWaiter 判成「没人等」而躺回空闲栈，本协程此后再也等不到唤醒。
        // 截止时刻也必须在同一段锁里定下：后台线程是持锁读它的，放锁之后再写，
        // 它会在那个窗口里看到默认值（时钟纪元）→ 把刚入表的等待者判成「已超时」并投递恢复，
        // 而此刻协程还没挂起（轻则异步获取无故返回空连接，重则恢复尚未挂起的帧）
        {
            std::lock_guard lock(m_pool->m_asyncMutex);
            // 锁里**只取空闲栈**，不建连：建连要跑工厂 + connect（秒级），握着 m_asyncMutex
            // 会让归还路径、后台超时唤醒与健康检查全排在它后面。池未满时的建连已经在上面
            // 那次无锁尝试里做过了，这里还是空栈就说明确实没有立即可用的连接
            m_result = m_pool->tryAcquireInternal();
            if (m_result)
            {
                return false;
            }
            // 只在入表时定一次，不随每次尝试刷新——否则反复失败的重试会把超时无限顺延
            m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_pool->m_config.acquireTimeoutMilliseconds);
            // 票据与入表同锁创建：唤醒方持锁读它，放锁之后再建会让唤醒方读到空票据
            m_resumeTicket = std::make_shared<ResumeTicket>();
            m_resumeTicket->handle.store(handle, std::memory_order_release);
            m_pool->m_asyncWaiters.push_back(this);
            m_inList = true;
        }

        return true; // 挂起，等待归还路径唤醒
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
        // 减少活跃计数
        m_activeCount.fetch_sub(1);

        if (!connection)
        {
            return; // 空连接直接忽略
        }

        // 会话状态复位必须早于「放回空闲栈」与「直接交给等待者」两条去向：
        // 上一个借用者留下的会话级状态（Redis 的未发送管道、临时表等）不能串给下一个借用者
        connection->resetSessionState();

        // ---- 健康检查 ----
        if (!isConnectionHealthy(connection.get()))
        {
            // 不健康的连接直接丢弃
            {
                std::lock_guard ctLock(m_ctMapMutex);
                m_creationTimeMap.erase(connection.get());
            }
            connection->disconnect();
            connection.reset();
            m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
            return;
        }

        // ---- 优先尝试唤醒异步等待者 ----
        if (notifyAsyncWaiter(connection))
        {
            return; // 连接直接转给了等待者，不入空闲栈
        }

        // ---- 查创建时间并入空闲栈 ----
        std::chrono::steady_clock::time_point createdTime;
        {
            std::lock_guard ctLock(m_ctMapMutex);
            if (const auto it = m_creationTimeMap.find(connection.get()); it != m_creationTimeMap.end())
            {
                createdTime = it->second;
            } else
            {
                // 理论上不应走到这里，但若映射丢失则以当前时间为保守估计
                // 保守估计意味着 maxLifetime 检查会延后，但仍有 healthCheckLoop 兜底
                createdTime = std::chrono::steady_clock::now();
            }
        }

        {
            std::lock_guard lock(m_mutex);

            const auto now = std::chrono::steady_clock::now();

            IdleEntry entry;
            entry.connection   = std::move(connection);
            entry.createdTime  = createdTime;
            entry.returnedTime = now;

            // 惰性过期检查：归还时如果连接已超过最大存活时间，直接关闭不归还
            // maximumLifetimeSeconds == 0 视为「立即过期」，连接永不入空闲栈
            if (m_config.maximumLifetimeSeconds == 0)
            {
                // 最大存活时间为 0：立即过期，直接丢弃
                {
                    std::lock_guard ctLock(m_ctMapMutex);
                    m_creationTimeMap.erase(entry.connection.get());
                }
                entry.connection->disconnect();
                entry.connection.reset();
                m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
                return;
            }

            {
                const auto lifetimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - createdTime).count();
                if (static_cast<std::size_t>(lifetimeSeconds) >= m_config.maximumLifetimeSeconds)
                {
                    // 连接已超最大存活时间，丢弃
                    {
                        std::lock_guard ctLock(m_ctMapMutex);
                        m_creationTimeMap.erase(entry.connection.get());
                    }
                    entry.connection->disconnect();
                    entry.connection.reset();
                    m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
            }

            m_idleStack.push_back(std::move(entry));
        }

        // 通知一个等待者：有空闲连接了
        m_cv.notify_one();
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

    // ========================================================================
    // 内部方法
    // ========================================================================

    std::unique_ptr<DatabaseConnection> ConnectionPool::tryAcquireInternal() noexcept
    {
        std::lock_guard lock(m_mutex);

        if (m_idleStack.empty())
        {
            return nullptr;
        }

        // LIFO：从栈顶取（最新归还的连接最可能还在热点缓存中）
        IdleEntry entry = std::move(m_idleStack.back());
        m_idleStack.pop_back();

        // 锁在 lock 析构时释放，entry 已移出空闲栈归本函数所有

        if (isEntryExpired(entry))
        {
            // 过期连接直接关闭丢弃
            {
                std::lock_guard ctLock(m_ctMapMutex);
                m_creationTimeMap.erase(entry.connection.get());
            }
            entry.connection->disconnect();
            entry.connection.reset();
            m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        // 健康检查：不健康则丢弃
        if (!isConnectionHealthy(entry.connection.get()))
        {
            {
                std::lock_guard ctLock(m_ctMapMutex);
                m_creationTimeMap.erase(entry.connection.get());
            }
            entry.connection->disconnect();
            entry.connection.reset();
            m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
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

            // 调用 connect() 建立实际连接
            if (!connection->connect())
            {
                return nullptr;
            }

            {
                std::lock_guard ctLock(m_ctMapMutex);
                m_creationTimeMap[connection.get()] = std::chrono::steady_clock::now();
            }

            return connection;
        } catch (...)
        {
            // 工厂或 connect 抛异常时返回空指针
            return nullptr;
        }
    }

    bool ConnectionPool::isEntryExpired(const IdleEntry &entry) const noexcept
    {
        const auto now = std::chrono::steady_clock::now();

        // 最大存活时间检查：0 表示立即过期
        if (m_config.maximumLifetimeSeconds == 0)
        {
            return true;
        }

        {
            const auto lifetimeSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(now - entry.createdTime).count();
            if (static_cast<std::size_t>(lifetimeSeconds) >= m_config.maximumLifetimeSeconds)
            {
                return true;
            }
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

        // 轻量探活：调用 isConnected()
        // 对于大多数驱动（如 SqliteConnection），isConnected() 只检查内部状态标志，
        // 不触发任何 IO，是 O(1) 操作。
        // 对于 MySQL/Redis，isConnected() 可能会执行一次网络探活（如 mysql_ping），
        // 这会在获取路径上引入一次网络往返，但可以保证调用方拿到的是可用连接。
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
        // 健康检查间隔
        const auto interval = std::max(m_config.healthCheckIntervalSeconds, std::size_t{1});

        while (!stopToken.stop_requested())
        {
            // 分段睡眠，每 1 秒检查一次停止标志，使线程能及时响应停止请求
            const auto            totalSleepMs  = interval * 1000;
            constexpr std::size_t kSleepChunkMs = 1000;

            auto remainingMs = static_cast<int64_t>(totalSleepMs);
            while (remainingMs > 0 && !stopToken.stop_requested())
            {
                const auto chunk = std::min(static_cast<int64_t>(kSleepChunkMs), remainingMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                remainingMs -= chunk;

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

                // 使用 erase-remove_if 惯用法移除过期连接
                auto removeBegin = std::ranges::remove_if(m_idleStack,
                                                          [this, &expiredConnections](IdleEntry &entry) -> bool
                                                          {
                                                              if (isEntryExpired(entry))
                                                              {
                                                                  // 从创建时间映射表中移除
                                                                  {
                                                                      std::lock_guard ctLock(m_ctMapMutex);
                                                                      m_creationTimeMap.erase(entry.connection.get());
                                                                  }
                                                                  expiredConnections.push_back(std::move(entry.connection));
                                                                  m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
                                                                  return true; // 标记移除
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

    bool ConnectionPool::notifyAsyncWaiter(std::unique_ptr<DatabaseConnection> &connection)
    {
        std::lock_guard lock(m_asyncMutex);

        if (m_asyncWaiters.empty())
        {
            return false;
        }

        // 从队首取出一个等待者（FIFO 公平：先等的先拿到连接）
        AcquireAwaiter *waiter = m_asyncWaiters.front();
        m_asyncWaiters.pop_front();

        // 将连接交给等待者
        waiter->m_result = std::move(connection);
        waiter->m_inList = false;

        // 把恢复动作投递回等待者所属的事件循环，而不是就地恢复：归还连接可能发生在
        // 任意线程（工作线程、另一个事件循环），就地恢复会让协程的后续代码跑在那个线程上，
        // 而调用方是按「回调都在自己的事件循环线程上」来写代码的。
        // postRemote 内部持锁入队并唤醒目标循环，因此不存在丢唤醒的窗口。
        // **经票据投递**：这次投递之后调用方随时可能销毁 Task（帧连同等待器一起析构），
        // 投裸句柄会让循环那边 resume 一块已释放的帧——票据让那次恢复在帧没了之后变成空操作
        const auto resumeTicket = waiter->m_resumeTicket;
        if (resumeTicket != nullptr)
        {
            waiter->m_completionLoop->scheduler().postRemote(
                    [resumeTicket]()
                    {
                        // 取走再恢复：句柄只能被恢复一次
                        if (const std::coroutine_handle<> handedOverHandle = resumeTicket->handle.exchange(nullptr);
                            handedOverHandle != nullptr)
                        {
                            handedOverHandle.resume();
                        }
                    });
        }

        return true;
    }

    void ConnectionPool::removeAsyncWaiter(AcquireAwaiter *waiter) noexcept
    {
        std::lock_guard lock(m_asyncMutex);

        // 在列表中查找并移除
        if (const auto it = std::ranges::find(m_asyncWaiters, waiter); it != m_asyncWaiters.end())
        {
            m_asyncWaiters.erase(it);
            waiter->m_inList = false;
        }
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
                        if (const std::coroutine_handle<> timedOutHandle = ticket->handle.exchange(nullptr); timedOutHandle != nullptr)
                        {
                            timedOutHandle.resume();
                        }
                    });
        }
    }

} // namespace AsynGyanis::Database
