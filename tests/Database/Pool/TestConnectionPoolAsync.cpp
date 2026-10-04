// 连接池异步获取测试 —— 挂起/唤醒的线程归属与快慢两条路径。
// 盯住一条容易被忽略、却在真机上才会暴露的性质：**协程在哪个线程上恢复**。池满时 `acquireAsync()` 挂起，
// 归还连接的一方把恢复动作投递回 `acquireAsync()` 给定的 `EventLoop`（`Scheduler::scheduleRemote`），
// 因此恢复后的代码仍运行在那个事件循环线程上；本文件断言恢复线程 == 事件循环线程，且**不等于**调用线程。
// 覆盖场景：
// - AcquireAsyncCreatesConnectionImmediatelyWhenPoolNotFull（快路径：池未满时直接建连，不挂起）
// - AcquireAsyncReusesIdleConnectionImmediately（快路径：复用空闲连接，不新建）
// - AcquireAsyncResumesOnGivenEventLoop（慢路径：隔线程归还后，恢复发生在循环线程上）
// - DestructorWakesWaitersWithEmptyConnection（池销毁时以空连接唤醒，不永久挂起）
// - DestructorDoesNotDeadlockWhenResumedWaiterReturnsConnection（析构期唤醒的协程归还连接，不得同线程死锁）
// - DiscardingTaskAfterHandoffDoesNotResumeFreedFrame（交接后销毁 Task 不得 resume 已释放帧）
// - DiscardedTaskAfterHandoffReturnsConnectionToPool（丢弃已交接的帧要把连接与配额还回池）
// - WaiterRebuildsInsteadOfTakingExpiredHandover（过期连接不直接交接，协程被腾出的名额救活后另建一条）
// - AsyncBorrowTimeoutSharesTheCounter（异步空手收尾与同步共用同一份借出超时计数）
// - WaitingMetricMatchesWaitingCountWhileCoroutinesQueue（协程排队要在 /metrics 上看得见，与 waitingCount() 同一个数）
// - FrameOutlivingDestroyedPoolDoesNotTouchIt（帧活过池析构时不再碰已析构的池，连接随帧关闭）
// - AllocationFailureProbeInjectsExactlyOneFailurePerArm（「第 N 次分配失败」开关自证：掐一次、随后恢复、计数可查）
// - AllocationFailureWhileQueueingYieldsEmptyConnectionAndLeavesNoWaiter（排队途中分配失败：当场空手收尾、等待表不留半条）
// - AllocationFailureWhileReturningDiscardsConnectionInsteadOfLosingIt（归还入栈分配失败：那条连接走丢弃那本账）

#include "Database/Common/DatabaseConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "DatabaseTestSupport.h"
#include "MetricsTestSupport.h"
#include "AllocationProbe.h"

#include "TestConnectionPool.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace AsynGyanis::Database
{
    namespace
    {
        using namespace TestPoolSupport;
        using AsynGyanis::TestSupport::registryValue; // 指标助手在 AsynGyanis::TestSupport，不是本模块那一个 TestSupport
        using TestSupport::EventLoopThread;
        using TestSupport::waitForCondition;

        /// 全局那份 TestSupport 的别名：本文件在 AsynGyanis::Database 里，裸写 TestSupport:: 会撞到本模块的同名空间
        namespace SharedTestSupport = ::AsynGyanis::TestSupport;

        /// 一次异步获取的观测结果
        struct AcquireProbe
        {
            std::optional<PooledConnection> connection;      ///< 拿到的连接（恢复后写入）
            std::thread::id                 resumeThreadId;  ///< 恢复本协程的线程 id
            std::atomic<bool>               finished{false}; ///< 完成标记，最后写入（release 语义）
        };

        /**
         * @brief 驱动协程：等待一次异步获取，并记录恢复发生在哪个线程上
         *
         * @details 记录线程 id 的那一行必须紧跟在 `co_await` 之后：它正是本文件要验证的恢复点。
         */
        Core::Task<void> probeAcquireAsync(ConnectionPool &pool, Core::EventLoop &loop, AcquireProbe &probe)
        {
            probe.connection.emplace(co_await pool.acquireAsync(loop));
            probe.resumeThreadId = std::this_thread::get_id();
            probe.finished.store(true, std::memory_order_release);
        }

        /**
         * @brief 构造一个池上限为 1 的连接池（配合 MockConnection 工厂）
         * @param counter 连接计数器，用例据此断言连接是否被复用/新建
         * @return ConnectionPool 上限 1 的池
         */
        ConnectionPool makeSingleSlotPool(ConnectionCounter &counter)
        {
            PoolConfig configuration;
            configuration.maximumPoolSize = 1;
            return ConnectionPool(makeMockFactory(counter), configuration);
        }

    } // namespace

    /** @brief 钉住快路径：池未满时异步获取直接建连返回，不挂起、不切换线程 */
    TEST(ConnectionPoolAsync, AcquireAsyncCreatesConnectionImmediatelyWhenPoolNotFull)
    {
        ConnectionCounter counter;
        ConnectionPool    pool = makeSingleSlotPool(counter);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        AcquireProbe          probe;
        const std::thread::id callerThreadId = std::this_thread::get_id();

        // 池是空的但未达上限：应当与同步 acquire() 同口径——立刻建连返回，不经过挂起。
        // 池未满时必须直接新建连接：若只从空闲栈取，刚建好的池上所有异步获取都会先挂起
        ASSERT_EQ(counter.totalCreated.load(), 0) << "构造池时不应预创建连接";

        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume();

        ASSERT_TRUE(probe.finished.load(std::memory_order_acquire));
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_TRUE(probe.connection.value()) << "池未满时异步获取应直接建连并返回";
        EXPECT_EQ(counter.totalCreated.load(), 1) << "应当为此新建一条连接";

        // 未挂起意味着没有线程切换：恢复点就在调用线程上
        EXPECT_EQ(probe.resumeThreadId, callerThreadId);

        loopThread.parkDriver(std::move(driver));
    }

    /** @brief 钉住快路径：有空闲连接时直接复用而不是再建一条 */
    TEST(ConnectionPoolAsync, AcquireAsyncReusesIdleConnectionImmediately)
    {
        ConnectionCounter counter;
        ConnectionPool    pool = makeSingleSlotPool(counter);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        // 先取一条再归还：池里出现一条空闲连接，异步获取应当直接复用而不新建
        pool.acquire().release();

        AcquireProbe     probe;
        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume();

        ASSERT_TRUE(probe.finished.load(std::memory_order_acquire));
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_TRUE(probe.connection.value());
        EXPECT_EQ(counter.totalCreated.load(), 1) << "应复用空闲连接而不是再建一条";

        loopThread.parkDriver(std::move(driver));
    }

    /** @brief 钉住慢路径的线程归属：隔线程归还后恢复发生在 acquireAsync() 指定的事件循环线程上，而非归还线程 */
    TEST(ConnectionPoolAsync, AcquireAsyncResumesOnGivenEventLoop)
    {
        ConnectionCounter counter;
        ConnectionPool    pool = makeSingleSlotPool(counter);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        // 占满唯一的坑：此时再异步获取必然挂起
        PooledConnection occupyingConnection = pool.acquire();
        ASSERT_TRUE(occupyingConnection);

        AcquireProbe          probe;
        const std::thread::id callerThreadId = std::this_thread::get_id();

        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume();

        // 无可用连接且已达上限：协程应停在挂起点上
        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));

        // 在**调用线程**上归还连接：唤醒动作由这里发起，但恢复必须发生在循环线程上
        occupyingConnection.release();

        ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); }));

        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_TRUE(probe.connection.value()) << "被唤醒的等待者应拿到刚归还的连接";

        // 核心断言：恢复落在给定的事件循环线程上，而不是发起归还的调用线程
        EXPECT_NE(probe.resumeThreadId, callerThreadId) << "协程在归还连接的线程上被就地恢复，loop 形参形同虚设";
        EXPECT_EQ(probe.resumeThreadId, loopThread.threadId()) << "恢复应发生在 acquireAsync() 给定的那个事件循环线程上";

        loopThread.parkDriver(std::move(driver));
    }

    /**
     * @brief 归还的连接被当场丢弃时，挂起的协程要被腾出的名额救活，且不能接过那条过期的连接
     * @details 钉住两处：① 过期的连接不走「直接交接」——协程拿着一条服务端可能已单方面掐线的连接
     *          去跑，等于从后门绕过 maximumLifetimeSeconds；② 丢弃出口的那次叫醒有效——协程被
     *          「空唤醒」后会重挂一轮并自己补建一条，而不是干等到超时拿空连接
     */
    TEST(ConnectionPoolAsync, WaiterRebuildsInsteadOfTakingExpiredHandover)
    {
        ConnectionCounter counter;
        PoolConfig        configuration;
        configuration.maximumPoolSize            = 1;
        configuration.maximumLifetimeSeconds     = 3600;
        configuration.idleTimeoutSeconds         = 3600;
        configuration.healthCheckIntervalSeconds = 3600; // 后台驱逐不参与本用例的时序
        configuration.acquireTimeoutMilliseconds = 5000; // 没被救活时，协程会等满这里才拿空连接收尾
        ConnectionPool pool(makeMockFactory(counter), configuration);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        PooledConnection occupying = pool.acquire();
        ASSERT_TRUE(occupying);
        // 建立时刻挪到存活期之外：归还时它必然被判过期，只能丢弃而不是交给协程
        occupying->markEstablishedAt(std::chrono::steady_clock::now() - std::chrono::hours(2));

        AcquireProbe     probe;
        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume(); // 池满：挂到等待列表
        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));
        ASSERT_EQ(pool.waitingCount(), 1U) << "协程没有挂起：用例前提不成立";

        occupying.release(); // 丢弃过期的那条，并叫醒等待者

        ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); })) << "协程没被腾出的名额救活：它只在等交接或等超时";

        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_TRUE(probe.connection.value()) << "被叫醒的协程应重挂一轮并自己补建一条";
        EXPECT_EQ(counter.totalCreated.load(), 2) << "过期那条不该被交给协程，得另建一条";
        EXPECT_EQ(counter.totalDestroyed.load(), 1) << "过期那条要被丢弃，不能留在池里";
        EXPECT_EQ(pool.totalCount(), 1U);

        loopThread.parkDriver(std::move(driver));
    }

    /**
     * @brief 钉住 /metrics 上的「正在等待的请求数」与 `waitingCount()` 是同一个数：协程排队也要看得见
     * @details 异步取出才是服务主线（与 AsyncBorrowTimeoutSharesTheCounter 同一个理由），而这条 gauge
     *          此前只报同步等待者：协程在等空闲连接时，`asyn_db_pool_waiting_requests` 恒为 0，按它做
     *          排队告警的面板永远不会响。指标抓取不许去拿池的锁，所以这一格配了一份原子镜像；本用例把
     *          「挂着一条协程等待时两处相等且不为零」与「收口之后一起归零」按两拍核对——镜像漏刷任何一处，
     *          这两条里必有一条红。
     */
    TEST(ConnectionPoolAsync, WaitingMetricMatchesWaitingCountWhileCoroutinesQueue)
    {
        ConnectionCounter counter;
        PoolConfig        configuration;
        configuration.maximumPoolSize            = 1;
        configuration.maximumLifetimeSeconds     = 3600;
        configuration.idleTimeoutSeconds         = 3600;
        configuration.healthCheckIntervalSeconds = 3600;
        configuration.acquireTimeoutMilliseconds = 5000;
        ConnectionPool pool(makeMockFactory(counter), configuration);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        PooledConnection occupying = pool.acquire();
        ASSERT_TRUE(occupying);

        AcquireProbe     probe;
        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume(); // 池满：协程挂进等待列表
        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));
        ASSERT_EQ(pool.waitingCount(), 1U) << "协程没有挂起：用例前提不成立";

        ASSERT_TRUE(AsynGyanis::TestSupport::hasRegistrySample("asyn_db_pool_waiting_requests")) << "这条 gauge 根本没登记";
        EXPECT_EQ(registryValue("asyn_db_pool_waiting_requests"), static_cast<std::uint64_t>(pool.waitingCount())) << "协程在排队而 /metrics 上说没人等：这一格只数了同步等待者";

        occupying.release();
        ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); }));
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_EQ(pool.waitingCount(), 0U) << "等待收口之后锁内真值没归零";
        EXPECT_EQ(registryValue("asyn_db_pool_waiting_requests"), 0U) << "等待收口之后镜像没归零：摘出等待表的那几处漏刷了";

        loopThread.parkDriver(std::move(driver));
    }

    /**
     * @brief 钉住异步侧的账：等到截止时刻空手收尾要记进与同步同一份借出超时计数
     *
     * @details 异步获取是服务里更常用的那条线，若只有同步侧记账，池耗尽在这条线上依旧不可见。
     *          空手收尾同时还不该留下「建过一条新连接」的痕迹——它只是等了一场，什么也没拿到。
     */
    TEST(ConnectionPoolAsync, AsyncBorrowTimeoutSharesTheCounter)
    {
        ConnectionCounter counter;

        PoolConfig configuration;
        configuration.maximumPoolSize            = 1;
        configuration.idleTimeoutSeconds         = 3600;
        configuration.maximumLifetimeSeconds     = 3600;
        configuration.healthCheckIntervalSeconds = 3600;
        configuration.acquireTimeoutMilliseconds = 50; // 到点即空手；宣布到点的是后台那一拍，因此最迟一秒内

        ConnectionPool pool(makeMockFactory(counter), configuration);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        PooledConnection occupying = pool.acquire();
        ASSERT_TRUE(occupying);
        EXPECT_EQ(pool.borrowTimeoutCount(), 0U) << "成功的借出也被记成超时";

        AcquireProbe     probe;
        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume();
        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire)) << "占着唯一额度时不该立即完成";

        // 没有人归还：这条协程只能靠截止时刻到点收场
        ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); })) << "等待者没被超时叫醒：下面的计数没有对照";

        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_FALSE(static_cast<bool>(probe.connection.value())) << "占着唯一额度时异步借出应当空手";
        EXPECT_EQ(pool.borrowTimeoutCount(), 1U) << "异步空手没收进与同步共用的那份超时账";
        EXPECT_EQ(pool.createdCount(), 1U) << "空手收尾不该再建一条连接";

        loopThread.parkDriver(std::move(driver));
    }

    /** @brief 钉住池析构就地唤醒等待者并交回空连接，不把它永久挂在等待列表上 */
    TEST(ConnectionPoolAsync, DestructorWakesWaitersWithEmptyConnection)
    {
        ConnectionCounter counter;

        // 上限 0：任何连接都不允许创建，因此异步获取必然挂起——这样用例不需要「先占住一条
        // 连接」，也就不会出现「池销毁后还持有连接去归还」这种悬垂场景
        PoolConfig configuration;
        configuration.maximumPoolSize = 0;

        auto pool = std::make_unique<ConnectionPool>(makeMockFactory(counter), configuration);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        AcquireProbe     probe;
        Core::Task<void> driver = probeAcquireAsync(*pool, loopThread.loop(), probe);
        driver.handle().resume();

        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));
        EXPECT_EQ(pool->waitingCount(), 1U) << "没有可用连接时协程应挂在等待列表里";

        // 池在还有等待者时被销毁：析构必须唤醒它们并交回空连接。
        // 这一次是就地恢复（池已停摆，投回事件循环可能永远不会被执行），
        // 理由写在 ConnectionPool 析构的注释里，因此这里只断言「确实醒了、拿到的是空连接」
        pool.reset();

        ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); }));
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_FALSE(probe.connection.value()) << "池停摆时被唤醒的等待者应拿到空连接";

        loopThread.parkDriver(std::move(driver));
    }

    /**
     * @brief 驱动协程：先占住唯一一条连接，再等第二条（池已满，必然挂起），恢复后把第一条还回去
     *
     * @details 「占着连接等第二条」正是析构期就地恢复最危险的组合：析构会锁着存活令牌锁恢复等待者，
     *          而本协程恢复后立刻归还连接——归还路径要锁**同一把非递归锁**；析构若不把锁收到置假
     *          那一步为止，本协程就会在同线程上二次加锁、永久挂住。
     */
    Core::Task<void> probeHoldFirstThenWaitSecond(ConnectionPool &pool, Core::EventLoop &loop, AcquireProbe &probe)
    {
        PooledConnection first = co_await pool.acquireAsync(loop);
        probe.connection.emplace(std::move(first));
        probe.resumeThreadId = std::this_thread::get_id();

        // 池上限为 1：这一句必然挂起，等待者就这样留在池的列表里
        PooledConnection second = co_await pool.acquireAsync(loop);
        probe.finished.store(true, std::memory_order_release);

        // 恢复路径上归还连接：这一步必须能在池析构期间跑完
        probe.connection->release();
    }

    /**
     * @brief 钉住：池析构时唤醒的等待者可以安全地归还手里的连接（不得同线程死锁）
     * @details 归还路径「判活 + 调池」都在存活令牌锁内完成，析构则在同一把锁下置假——
     *          因此那把锁**只能覆盖置假这一步**。一旦它跨到析构末尾的就地恢复点上，
     *          被恢复的协程在归还连接时就会对同一把非递归锁二次加锁：本用例会直接挂死。
     */
    TEST(ConnectionPoolAsync, DestructorDoesNotDeadlockWhenResumedWaiterReturnsConnection)
    {
        ConnectionCounter counter;
        PoolConfig        configuration;
        configuration.maximumPoolSize = 1;
        auto pool                     = std::make_unique<ConnectionPool>(makeMockFactory(counter), configuration);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        AcquireProbe     probe;
        Core::Task<void> driver = probeHoldFirstThenWaitSecond(*pool, loopThread.loop(), probe);
        driver.handle().resume();

        ASSERT_TRUE(probe.connection.has_value()) << "第一条连接应当立刻建好返回";
        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire)) << "第二条应当挂在等待列表里";
        ASSERT_EQ(pool->waitingCount(), 1U);

        // 析构会就地恢复等待者；被恢复的协程紧接着归还手里的连接。
        // 若析构把存活令牌锁持过了恢复点，这一句永远不返回（用例挂死即回归）
        pool.reset();

        EXPECT_TRUE(probe.finished.load(std::memory_order_acquire)) << "等待者没有被唤醒";
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_FALSE(probe.connection.value()) << "池停摆后归还的连接应被关闭（包装器随之置空）而不是留在池里";

        loopThread.parkDriver(std::move(driver));
    }

    /**
     * @brief 钉住：连接已交接、恢复尚未执行时销毁 Task，不得 resume 已释放的协程帧
     * @details 交接把「恢复这次等待」投回事件循环，而调用方在那之后随时可能销毁 Task——协程帧连同
     *          等待器一起析构，恢复动作必须只经持票句柄执行，不能拿裸句柄去 resume 已释放的内存。
     *          用例用一个**不启动**的循环把这次恢复留在队列里，销毁 Task 之后再手动排空队列。
     */
    TEST(ConnectionPoolAsync, DiscardingTaskAfterHandoffDoesNotResumeFreedFrame)
    {
        ConnectionCounter counter;
        PoolConfig        configuration;
        configuration.maximumPoolSize = 1;
        ConnectionPool pool(makeMockFactory(counter), configuration);

        // 不启动的循环：交接只会把恢复动作排进它的队列，不会有人执行
        Core::EventLoop loop;

        PooledConnection occupying = pool.acquire();
        ASSERT_TRUE(occupying);

        AcquireProbe probe;
        {
            Core::Task<void> driver = probeAcquireAsync(pool, loop, probe);
            driver.handle().resume(); // 池满：挂到等待列表
            ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));
            ASSERT_EQ(pool.waitingCount(), 1U);

            occupying.release(); // 交接：恢复动作排进 loop 的队列
        } // driver 在这里析构 → 协程帧连同等待器一起销毁

        // 队列里的那次恢复现在才执行：帧已经没了，它必须什么都不做
        loop.scheduler().runAll();
        EXPECT_FALSE(probe.finished.load(std::memory_order_acquire)) << "帧已销毁，这次恢复不该跑任何代码";
    }

    /**
     * @brief 交接之后丢弃 Task：连接要回到池里，池的配额也要跟着回退
     * @details 交接那一刻连接已经记在等待者名下。帧被销毁时这次「取出」从未真正开始，
     *          若不按归还结账，池就永远少一个位置：连接随帧一起消失，而总创建数不降、
     *          空闲栈也拿不回它——反复丢弃几次之后，所有 acquire 都会卡在「池已满」上直到超时
     */
    TEST(ConnectionPoolAsync, DiscardedTaskAfterHandoffReturnsConnectionToPool)
    {
        ConnectionCounter counter;
        PoolConfig        configuration;
        configuration.maximumPoolSize = 1;
        ConnectionPool pool(makeMockFactory(counter), configuration);

        // 不启动的循环：交接只会把恢复动作排进它的队列，不会有人执行
        Core::EventLoop loop;

        PooledConnection occupying = pool.acquire();
        ASSERT_TRUE(occupying);
        ASSERT_EQ(pool.activeCount(), 1U);

        AcquireProbe probe;
        {
            Core::Task<void> driver = probeAcquireAsync(pool, loop, probe);
            driver.handle().resume(); // 池满：挂到等待列表
            ASSERT_EQ(pool.waitingCount(), 1U);

            occupying.release(); // 交接：连接转给等待者，恢复动作排进 loop 的队列
        } // driver 在这里析构 → 帧连同等待器一起销毁

        loop.scheduler().runAll(); // 那次恢复此刻执行：空操作

        // 连接回到了空闲栈，活跃计数回零——这次丢弃没有吃掉池的配额
        EXPECT_EQ(pool.activeCount(), 0U);
        EXPECT_EQ(pool.idleCount(), 1U);
        EXPECT_EQ(pool.waitingCount(), 0U);

        // 池没满、连接也在：再取一次必须立刻成功，而不是等到超时
        const PooledConnection reacquired = pool.tryAcquire();
        EXPECT_TRUE(reacquired) << "丢弃一次之后池再也取不出连接：配额被这次丢弃吃掉了";
    }

    /**
     * @brief 帧活过池的析构：销毁时不能再碰池（连取它的锁都不行），连接随帧关闭
     * @details 判活必须排在任何对池的取消引用之前：归还动作经存活令牌判活（与
     *          PooledConnection::doReturnToPool 同一处置），而「先解引用再判活」的写法
     *          在池已析构时本身就是一次释放后使用——判活的那一步已经踩空了
     */
    TEST(ConnectionPoolAsync, FrameOutlivingDestroyedPoolDoesNotTouchIt)
    {
        ConnectionCounter counter;
        PoolConfig        configuration;
        configuration.maximumPoolSize = 1;

        // 不启动的循环：交接只会把恢复动作排进它的队列，不会有人执行
        Core::EventLoop loop;

        // 帧比池活得久：池先析构，帧随后才销毁（Task 由用例自己持有）。
        // 池刻意放在堆上而不是栈上：栈内存析构后不会被立刻复用，误踩只表现为「恰好没崩」，
        // 用例就退化成一次运气测试；堆上的释放后使用才是 ASan 一定报得出的形态
        std::unique_ptr<ConnectionPool>   pool;
        std::unique_ptr<Core::Task<void>> driver;
        AcquireProbe                      probe;
        {
            pool = std::make_unique<ConnectionPool>(makeMockFactory(counter), configuration);

            PooledConnection occupying = pool->acquire();
            ASSERT_TRUE(occupying);

            driver = std::make_unique<Core::Task<void>>(probeAcquireAsync(*pool, loop, probe));
            driver->handle().resume(); // 池满：挂到等待列表
            ASSERT_EQ(pool->waitingCount(), 1U);

            occupying.release(); // 交接：连接转给等待者，恢复动作排进 loop 的队列
            pool.reset();        // 池随后析构——连接此刻记在帧名下，等待表里已经没有这一条
        }

        // 帧销毁：手里的等待器不能再去碰已经析构的池，连接随帧一起关掉。
        // 队列里那次恢复刻意还没执行（循环不推进），所以这一次销毁走的正是「既等过、
        // 手里又有连接」的归还路径——池已停摆，这条路径必须原地收手
        driver.reset();
        EXPECT_EQ(counter.totalCreated.load(), 1);
        EXPECT_EQ(counter.totalDestroyed.load(), 1) << "连接没有随帧销毁：归还路径踩到了已析构的池";

        // 帧没了，队列里那次恢复此刻执行：票据句柄已随帧清空，它什么都不做
        loop.scheduler().runAll();
        EXPECT_FALSE(probe.finished.load(std::memory_order_acquire));
    }

    /**
     * @brief 钉住「第 N 次分配失败」开关本身：只掐一次、之后恢复正常、并且真的掐过
     * @details 下一例靠它注入 bad_alloc，而「注入其实没发生」会让那一例变成假的「没崩=通过」。
     *          三格各自钉一件事：① 挂上后下一次分配确实抛；② 抛过一次之后自动解除
     *          （被测体的兜底分支自己也要分配内存，一直失败会把它打成另一种形状）；
     *          ③ injectedAllocationFailureCount() 涨了一格——凡是靠它注入的用例都以此自证。
     */
    TEST(ConnectionPoolAsync, AllocationFailureProbeInjectsExactlyOneFailurePerArm)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        SharedTestSupport::resetInjectedAllocationFailureCount();
        bool          didThrow      = false;
        bool          recoveredOk   = true;
        std::uint64_t injectedAtArm = 0U;
        {
            const SharedTestSupport::AllocationFailureGuard guard(1U);
            try
            {
                const std::unique_ptr<std::array<char, 64>> first = std::make_unique<std::array<char, 64>>();
                static_cast<void>(first);
            } catch (const std::bad_alloc &)
            {
                didThrow = true;
            }
            injectedAtArm = SharedTestSupport::injectedAllocationFailureCount();
            try
            {
                const std::unique_ptr<std::array<char, 64>> second = std::make_unique<std::array<char, 64>>();
                static_cast<void>(second);
            } catch (...)
            {
                recoveredOk = false;
            }
        }

        EXPECT_TRUE(didThrow) << "开关没掐掉任何一次分配：靠它注入的用例全是空的";
        EXPECT_EQ(injectedAtArm, 1U) << "注入计数没涨，用例无从自证失败真的发生过";
        EXPECT_TRUE(recoveredOk) << "开关用完一次没解除：兜底分支自己的分配也会被掐掉";

        // 作用域结束后不得继续影响分配：漏着的开关会把后面每一条用例都打成随机失败
        bool throwsAfterScope = false;
        try
        {
            const std::unique_ptr<std::array<char, 64>> third = std::make_unique<std::array<char, 64>>();
            static_cast<void>(third);
        } catch (...)
        {
            throwsAfterScope = true;
        }
        EXPECT_FALSE(throwsAfterScope) << "开关漏在作用域之外还挂着";

        // 尺寸档同一套自证：挂上 16..31 这一档后，第一次落在该档的分配失败、之后恢复
        bool threwInWindow = false;
        {
            const SharedTestSupport::AllocationFailureGuard sizeGuard = SharedTestSupport::AllocationFailureGuard::forNextAllocationBetween(16U, 31U);
            try
            {
                const std::unique_ptr<std::array<char, 24>> inWindow = std::make_unique<std::array<char, 24>>();
                static_cast<void>(inWindow);
            } catch (const std::bad_alloc &)
            {
                threwInWindow = true;
            }
            bool threwAgain = false;
            try
            {
                const std::unique_ptr<std::array<char, 24>> again = std::make_unique<std::array<char, 24>>();
                static_cast<void>(again);
            } catch (...)
            {
                threwAgain = true;
            }
            EXPECT_TRUE(threwInWindow) << "尺寸档没掐到该档的分配";
            EXPECT_FALSE(threwAgain) << "尺寸档用完一次没解除";
        }
        EXPECT_EQ(SharedTestSupport::injectedAllocationFailureCount(), 2U) << "两档各掐一次应当累计到 2";
    }

    /**
     * @brief 钉住「分配序列追踪」这一档：挂上期间本线程每次分配的序号与字节数读得回来，收手后不再记
     * @details 这一档是给「第 N 次分配失败」找命中点用的：连接池 `await_suspend` 里票据与入表那两步的
     *          序号就是靠它量出来的（本平台实测挂上之后四次分配依次为 416 / 24 / 64 / 16 字节，
     *          依次是 `acquireAsync` 的协程帧、票据、入表、收尾）。
     *          判据只钉形状不钉绝对值：挂上之前本线程已经分配过多少条与用例顺序有关，而每一次申请的
     *          字节数由标准库实现决定（两家标准库的 deque 分块大小本来就不同），写死任何一条都会让
     *          用例在换平台时假红或假绿。
     * @note 三格：① 期间确有分配就确有记录（否则「按量出来的序号掐」这一步是空的）；
     *       ② 序号严格递增且尺寸非零（注入按序号定目标，尺寸按桶定区间，两格任一失真就掐错分配）；
     *       ③ `endAllocationTrace()` 之后继续分配不得再往里记（漏着的追踪会让后面的用例读到一份
     *          永远读不完的缓冲，且记满 64 条即停，越界的记录会被静默丢掉）。
     */
    TEST(ConnectionPoolAsync, AllocationTraceSnapshotLocatesInjectionCoordinates)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        SharedTestSupport::beginAllocationTrace();
        const auto                                       ticketLike = std::make_shared<std::atomic<std::uint64_t>>();
        const auto                                       blockLike  = std::make_unique<std::array<char, 64>>();
        const SharedTestSupport::AllocationTraceSnapshot during     = SharedTestSupport::endAllocationTrace();
        static_cast<void>(ticketLike);
        static_cast<void>(blockLike);

        ASSERT_GE(during.count, 2U) << "这两次分配没被记下：靠序号找命中点的用例全是空的";
        ASSERT_LE(during.count, SharedTestSupport::kAllocationTraceCapacity);
        for (std::uint64_t index = 1U; index < during.count; ++index)
        {
            EXPECT_GT(during.records[index].ordinal, during.records[index - 1U].ordinal) << "序号不单调：按它掐会掐到别的分配";
            EXPECT_GT(during.records[index].bytes, 0U) << "尺寸记成 0：按尺寸区间掐就命中不了任何分配";
        }

        // 收手判据：end 之后再分配，下一次 end 的快照应当一条都不多
        SharedTestSupport::beginAllocationTrace();
        const SharedTestSupport::AllocationTraceSnapshot reopened = SharedTestSupport::endAllocationTrace();
        EXPECT_EQ(reopened.count, 0U) << "开手到收手之间没有分配却记了条目";
        const auto third = std::make_unique<std::array<char, 64>>();
        static_cast<void>(third);
        const SharedTestSupport::AllocationTraceSnapshot after = SharedTestSupport::endAllocationTrace();
        EXPECT_EQ(after.count, 0U) << "追踪没在 end 处收手：后面的用例会读到一份漏着的缓冲";
    }

    /**
     * @brief 钉住「排队那一步分配不出来」的出口：当场空手收尾，既不重挂也不在等待表里留半条
     * @details `AcquireAwaiter::await_suspend` 按协程语言是 `noexcept`，而把自己挂进异步等待表要付
     *          不止一次分配：票据的 `make_shared`、deque 首次插入的块。两家的标准库在这一步给的次数
     *          与字节数都不同（MSVC Debug 实测挂起窗口四笔，libstdc++ 连四笔都不到），所以坐标不写死：
     *          先用探针的追踪档量出「挂上开关到 resume 返回」这段窗口里本线程碰了几次堆，再逐档各掐一遍。
     * @details 这一例真正钉的是**标记那一格**：`acquireAsync()` 外面套着 `while(true)` 重挂，兜底只回
     *          `false` 而不留标记时，调用方会立刻另造一个等待体重走这一步——修复前实测掐中票据那一次
     *          之后等待表里仍然出现一条等待体，也就是内存持续吃紧时它在调用方的线程上空转到截止时刻。
     *          留了标记之后 `acquireAsync()` 按空手结账，并且不记进超时账（截止时刻根本没到，记成
     *          超时会把「池不够用」这个容量信号打成假的）。
     * @note 判据不是「没崩」，也不是「掐中了第几次」。逐档跑下来断的是同一条不变量：**掐到分配却没
     *       留下等待体**——`掐到了 + 表里有一条 + 调用方没往下走` 正是「兜底之后被重挂」的形状，出现即红。
     *       掐在协程帧那一步时异常由驱动协程的 promise 收下（当场既没有等待体也没完成），那是本例
     *       管不到的另一件事，不算违例。落在 try 里的那些档给出更强的读数：当场完成、手里是空连接、
     *       没记进超时账、借出与建连两格都不动。收尾那一格自证真的掐进了 try——否则本用例在
     *       「这一步不在本线程上」的构建里会全绿而什么都没钉住。
     * @note 证伪：把 `acquireAsync()` 里读标记那三行摘掉（退回「只回 false」），落在 try 里的每一档
     *       都红在不变量那一格，收尾的自证也一起红；把 `await_suspend` 的 try/catch 整段摘掉
     *       （退回修复前那几行），本用例当场带走进程（`RUN` 打得出来而结果行没有）。
     */
    TEST(ConnectionPoolAsync, AllocationFailureWhileQueueingYieldsEmptyConnectionAndLeavesNoWaiter)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        // 量窗口：这段路径在本线程上碰几次堆，两家的标准库不一样，所以逐档掐而不是把序号写死
        std::uint64_t windowAllocationCount = 0U;
        {
            ConnectionCounter counter;
            ConnectionPool    pool = makeSingleSlotPool(counter);

            EventLoopThread loopThread;
            ASSERT_TRUE(loopThread.waitUntilRunning());

            PooledConnection occupying = pool.acquire();
            ASSERT_TRUE(occupying);

            AcquireProbe     probe;
            Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);

            SharedTestSupport::beginAllocationTrace();
            driver.handle().resume(); // 池满：本线程走到 await_suspend 并挂起
            windowAllocationCount = SharedTestSupport::endAllocationTrace().count;
            ASSERT_GE(windowAllocationCount, 1U) << "挂起窗口里本线程一次堆都不碰：票据那一步不在这里，本用例的坐标失效";

            // 这一趟没掐开关，等待表里留着一条：把名额还回去叫醒它，别带着挂着的协程离开这一段
            occupying.release();
            ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); })) << "归还之后等待者没被叫醒：下面那一圈的前提不成立";
            loopThread.parkDriver(std::move(driver));
        }

        bool settledAtLeastOnce = false;
        for (std::uint64_t ordinal = 1U; ordinal <= windowAllocationCount; ++ordinal)
        {
            SCOPED_TRACE(ordinal);

            ConnectionCounter counter;
            ConnectionPool    pool = makeSingleSlotPool(counter);

            EventLoopThread loopThread;
            ASSERT_TRUE(loopThread.waitUntilRunning());

            // 占住唯一的名额：异步获取只能排队，于是必然走到 await_suspend 里的入表那几步
            PooledConnection occupying = pool.acquire();
            ASSERT_TRUE(occupying);

            AcquireProbe     probe;
            Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);

            SharedTestSupport::resetInjectedAllocationFailureCount();
            {
                const SharedTestSupport::AllocationFailureGuard guard(ordinal);
                driver.handle().resume();
            }

            const bool injected   = SharedTestSupport::injectedAllocationFailureCount() == 1U;
            const bool finished   = probe.finished.load(std::memory_order_acquire);
            const bool leftWaiter = pool.waitingCount() != 0U;

            EXPECT_FALSE(injected && leftWaiter && !finished) << "掐到了一次分配，等待表里却留着一条等待体而调用方没往下走：兜底之后被那一圈 while 重挂了一次";

            if (injected && finished && !leftWaiter)
            {
                settledAtLeastOnce = true;
                ASSERT_TRUE(probe.connection.has_value());
                EXPECT_FALSE(static_cast<bool>(probe.connection.value())) << "排队失败却交出了一条连接";
                EXPECT_EQ(pool.activeCount(), 1U) << "空手收尾被记成一次借出";
                EXPECT_EQ(pool.borrowTimeoutCount(), 0U) << "分配失败被记成了借出超时：这一格的成因是内存而不是截止时刻";
                EXPECT_EQ(pool.createdCount(), 1U) << "排队失败不该再建一条连接";
            }

            occupying.release();
            if (leftWaiter)
            {
                // 表里那条还挂着：叫醒它再离开这一段，免得池析构时带着等待体
                ASSERT_TRUE(waitForCondition([&probe]() { return probe.finished.load(std::memory_order_acquire); })) << "归还之后等待者没被叫醒：本用例的收尾前提不成立";
            }
            loopThread.parkDriver(std::move(driver));
        }

        EXPECT_TRUE(settledAtLeastOnce) << "窗口里没有任何一次分配落在 await_suspend 的 try 里：本平台这一步不在本线程上，本用例钉不住东西";
    }


    /**
     * @brief 钉住「归还入栈那一步分配不出来」的出口：这条连接走丢弃那本账，而不是凭空少一条
     * @details `returnConnection()` 不是 `noexcept`，但它唯一的调用形态都挂在 `noexcept` 边界上
     *          （`~PooledConnection` 与 `PooledConnection::release()`），所以入栈那一步的 `bad_alloc`
     *          既不能穿出去（进程带走），也不能就地吞掉（那条连接没人再持有，池的名额却还记着它——
     *          于是 `createdCount()` 一路贴着 `maximumPoolSize` 而实际无连可用）。兜底的形状是
     *          出锁之后按既有的「丢弃」去向结账；本例钉的就是那笔结账真的对得上。
     * @details 坐标沿用排队那一例的配方：先用追踪档量出「挂上开关到 release 返回」这段窗口里本线程
     *          碰了几次堆，再逐档掐。两家的标准库给的次数不同，序号与尺寸都不写死。
     * @note 每档断的是三条与平台无关的事：① 归还之后本线程不再握着名额（`activeCount()==0`）；
     *       ② 池的三本账自洽（`totalCount()==idleCount()`，活跃既已归零就不该再算进总数）；
     *       ③ 掐到分配时这条连接要么确实在栈里（入栈成功），要么确实在丢弃那本账上
     *       （`discardedCount()==1` 且 `createdCount()` 把名额还回去）——「既不在栈里也没记账」
     *       就是凭空少一条，出现即红。收尾那一格自证真的有一档落进了那一步。
     * @note 证伪：把出锁之后那句 `discardConnection` 摘掉（兜底只吞不还账），落在 try 里的那些档
     *       红在「丢弃那本账没涨」与 `createdCount()` 两格；把整段 try/catch 摘掉，`release()`
     *       带着 `bad_alloc` 穿过 `noexcept` 边界，本用例当场带走进程。
     */
    TEST(ConnectionPoolAsync, AllocationFailureWhileReturningDiscardsConnectionInsteadOfLosingIt)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        std::uint64_t windowAllocationCount = 0U;
        {
            ConnectionCounter counter;
            ConnectionPool    pool = makeSingleSlotPool(counter);

            PooledConnection borrowed = pool.acquire();
            ASSERT_TRUE(borrowed);

            SharedTestSupport::beginAllocationTrace();
            borrowed.release(); // 没人排队：这一趟走的就是「入空闲栈」那一支
            windowAllocationCount = SharedTestSupport::endAllocationTrace().count;
            ASSERT_GE(windowAllocationCount, 1U) << "归还窗口里本线程一次堆都不碰：那一步不在本线程上，本用例的坐标失效";
            ASSERT_EQ(pool.idleCount(), 1U) << "量窗口这一趟没掐开关，连接本该躺在栈里";
        }

        bool discardedAtLeastOnce = false;
        for (std::uint64_t ordinal = 1U; ordinal <= windowAllocationCount; ++ordinal)
        {
            SCOPED_TRACE(ordinal);

            ConnectionCounter counter;
            ConnectionPool    pool = makeSingleSlotPool(counter);

            PooledConnection borrowed = pool.acquire();
            ASSERT_TRUE(borrowed);

            SharedTestSupport::resetInjectedAllocationFailureCount();
            {
                const SharedTestSupport::AllocationFailureGuard guard(ordinal);
                borrowed.release();
            }

            const bool injected = SharedTestSupport::injectedAllocationFailureCount() == 1U;

            EXPECT_EQ(pool.activeCount(), 0U) << "归还之后还被记成一次借出";
            EXPECT_EQ(pool.totalCount(), pool.idleCount()) << "三本账不自洽：活跃已归零，总数却还带着那一条";

            if (!injected)
            {
                continue;
            }

            if (pool.idleCount() == 0U)
            {
                // 那条连接没进栈：必须走丢弃那本账，名额一并还回去，否则池会一直以为自己满员
                discardedAtLeastOnce = true;
                EXPECT_EQ(pool.discardedCount(), 1U) << "入栈失败却没记进丢弃那本账：这条连接凭空少了，谁都不会再碰它";
                EXPECT_EQ(pool.createdCount(), 0U) << "丢弃没把名额退还：createdCount 一直贴着上限而实际无连可用";
            } else
            {
                EXPECT_EQ(pool.discardedCount(), 0U) << "连接既在栈里又被记成丢弃：同一笔账结了两回";
                EXPECT_EQ(pool.createdCount(), 1U) << "连接在栈里却被减了名额";
            }
        }

        EXPECT_TRUE(discardedAtLeastOnce) << "窗口里没有任何一档让这条连接走丢弃：本平台的入栈那一步没被掐到，本用例钉不住东西";
    }

} // namespace AsynGyanis::Database
