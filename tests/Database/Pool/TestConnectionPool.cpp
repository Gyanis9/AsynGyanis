// 连接池单元测试 —— 用 TestConnectionPool.h 里的 MockConnection 驱动，全程不触碰真实数据库。
// 覆盖场景：
// - AcquireReleaseReusesConnection：取一条，归还，再取，应得同一连接
// - AcquireBlocksThenSucceeds / AcquireTimeoutReturnsEmpty / TryAcquireReturnsEmptyWhenExhausted：阻塞、超时与非阻塞获取
// - ExcessLifetimeConnectionIsDiscarded / UnhealthyConnectionIsDiscarded：过期与不健康连接都在归还时丢弃
// - ConcurrentAcquireReleaseStress：多线程并发获取/归还，统计自洽
// - StatisticsAreConsistent / PooledConnectionMoveSemantics：统计方法与包装器移动语义
// - ResetsSessionStateOnBothReturnPaths / DestructorWakesBlockedSyncWaiters：两条归还去向都复位会话状态；析构叫醒同步等待者
// - ReturnPathDoesNotHoldTheIdleStackLockAcrossDisconnect / AcquirePathDoesNotHoldTheIdleStackLockAcrossDisconnect：
//   两条丢弃出口都不握着 m_mutex 做 disconnect
// - EstablishedTimeSurvivesRepeatedBorrowAndReturn：连接的建立时刻跟着连接本身，反复借用不重新盖戳

#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Common/DatabaseConnection.h"

#include "TestConnectionPool.h"

#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {

        using namespace TestPoolSupport;

        // ========================================================================
        // AcquireReleaseReusesConnection
        // ========================================================================

        /**
         * @brief 取一条连接，归还，再取 — 第二次应当拿到同一个 MockConnection
         *
         * @details LIFO 栈顶是最新归还的连接。取两次应得到相同指针，
         *          证明连接被复用而非每次都新建。
         */
        TEST(ConnectionPool, AcquireReleaseReusesConnection)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize = 1;

            ConnectionPool pool(factory, configuration);

            PooledConnection first = pool.acquire();
            ASSERT_TRUE(first) << "首次获取应成功";

            DatabaseConnection *firstPointer = first.operator->();

            first.release();
            ASSERT_FALSE(first) << "release 后应为空";

            PooledConnection second = pool.acquire();
            ASSERT_TRUE(second) << "第二次获取应成功";

            DatabaseConnection *secondPointer = second.operator->();

            EXPECT_EQ(firstPointer, secondPointer)
                << "第二次应得到与第一次相同的连接指针";
        }

        // ========================================================================
        // AcquireBlocksThenSucceeds
        // ========================================================================

        /**
         * @brief 池容量 1，拿走连接后另一线程在超时前归还，阻塞的获取应拿到
         */
        TEST(ConnectionPool, AcquireBlocksThenSucceeds)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize    = 1;
            configuration.acquireTimeoutMilliseconds = 5000; // 足够长

            ConnectionPool pool(factory, configuration);

            // 拿掉唯一的连接
            PooledConnection taken = pool.acquire();
            ASSERT_TRUE(taken);

            // 另一线程尝试获取（将阻塞）
            std::atomic<bool> secondGotConnection{false};
            std::thread secondThread([&]()
            {
                PooledConnection second = pool.acquire();
                if (second)
                {
                    secondGotConnection.store(true);
                }
            });

            // 等到第二个线程真的挂在等待列表上再归还：固定 sleep 只能缩小「还没开始等」的窗口，
            // 慢机器上下面那条断言就退化成赌调度（同文件其它用例已统一改成这种有界轮询）
            const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (pool.waitingCount() == 0 && std::chrono::steady_clock::now() < waitDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ASSERT_EQ(pool.waitingCount(), 1U) << "等待者没有挂上：用例前提不成立";

            // 归还连接 — 应唤醒等待者
            taken.release();

            secondThread.join();

            EXPECT_TRUE(secondGotConnection.load())
                << "归还连接后，阻塞的线程应成功获取连接";
        }

        // ========================================================================
        // AcquireTimeout
        // ========================================================================

        /**
         * @brief 容量为 0（工厂可创建但池不让建），超时内拿不到，返回空
         *
         * @details maximumPoolSize = 0 意味着不允许创建任何连接，
         *          同时空闲栈为空，acquire() 等待 acquireTimeoutMs 后应返回空。
         */
        TEST(ConnectionPool, AcquireTimeoutReturnsEmpty)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize    = 0;          // 不允许创建任何连接
            configuration.acquireTimeoutMilliseconds = 100; // 短超时

            ConnectionPool pool(factory, configuration);

            PooledConnection connection = pool.acquire();

            EXPECT_FALSE(connection)
                << "容量为 0 时 acquire 应超时并返回空连接";
        }

        // ========================================================================
        // tryAcquireReturnsEmptyWhenExhausted
        // ========================================================================

        /**
         * @brief 非阻塞获取：容量 1，拿走连接后 tryAcquire 应立刻返回空
         */
        TEST(ConnectionPool, TryAcquireReturnsEmptyWhenExhausted)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize = 1;

            ConnectionPool pool(factory, configuration);

            PooledConnection first = pool.tryAcquire();
            ASSERT_TRUE(first) << "首次 tryAcquire 应成功";

            PooledConnection second = pool.tryAcquire();
            EXPECT_FALSE(second) << "池已空，tryAcquire 应立刻返回空";

            first.release();

            PooledConnection third = pool.tryAcquire();
            EXPECT_TRUE(third) << "归还后 tryAcquire 应成功";
        }

        // ========================================================================
        // ExcessLifetimeConnectionIsDiscarded
        // ========================================================================

        /**
         * @brief 把 maxLifetimeSeconds 设成 0（立即过期），归还的连接应被丢弃、下次获取拿到新连接
         */
        TEST(ConnectionPool, ExcessLifetimeConnectionIsDiscarded)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize       = 2;
            configuration.maximumLifetimeSeconds = 0; // 立即过期

            ConnectionPool pool(factory, configuration);

            PooledConnection first = pool.acquire();
            ASSERT_TRUE(first);
            MockConnection *firstMock = static_cast<MockConnection *>(first.operator->());
            const std::int64_t firstId = firstMock->id();

            // 归还：应被丢弃（maxLifetimeSeconds == 0）
            first.release();

            PooledConnection second = pool.acquire();
            ASSERT_TRUE(second);
            MockConnection *secondMock = static_cast<MockConnection *>(second.operator->());
            const std::int64_t secondId = secondMock->id();

            EXPECT_NE(firstId, secondId)
                << "过期连接被丢弃后，应创建一个新连接";
        }

        // ========================================================================
        // UnhealthyConnectionIsDiscarded
        // ========================================================================

        /**
         * @brief 不健康的连接在归还时被丢弃，下次获取得到新连接
         */
        TEST(ConnectionPool, UnhealthyConnectionIsDiscarded)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize = 1;

            ConnectionPool pool(factory, configuration);

            PooledConnection first = pool.acquire();
            ASSERT_TRUE(first);
            MockConnection *firstMock = static_cast<MockConnection *>(first.operator->());
            const std::int64_t firstId = firstMock->id();

            // 模拟连接不健康
            firstMock->setHealthOk(false);

            // 归还：不健康的连接应被丢弃
            first.release();

            PooledConnection second = pool.acquire();
            ASSERT_TRUE(second);
            MockConnection *secondMock = static_cast<MockConnection *>(second.operator->());
            const std::int64_t secondId = secondMock->id();

            EXPECT_NE(firstId, secondId)
                << "不健康的连接被丢弃后，应创建一个新连接";
        }

        // ========================================================================
        // ConcurrentAcquireReleaseStress
        // ========================================================================

        /**
         * @brief 多线程 × N 轮获取/归还，校验统计计数自洽、不断言失败
         *
         * @details 8 个线程各执行 100 次 acquire+release 循环，
         *          池容量为 4。完成后检查 totalCount == active + idle，
         *          且没有线程拿到空连接。
         */
        TEST(ConnectionPool, ConcurrentAcquireReleaseStress)
        {
            constexpr std::size_t kThreadCount = 8;
            constexpr std::size_t kIterationsPerThread = 100;
            constexpr std::size_t kPoolSize = 4;

            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize    = kPoolSize;
            configuration.acquireTimeoutMilliseconds = 10000; // 足够长

            ConnectionPool pool(factory, configuration);

            std::atomic<std::size_t> emptyAcquires{0};
            std::vector<std::thread> threads;

            for (std::size_t t = 0; t < kThreadCount; ++t)
            {
                threads.emplace_back([&]()
                {
                    for (std::size_t i = 0; i < kIterationsPerThread; ++i)
                    {
                        PooledConnection connection = pool.acquire();
                        if (!connection)
                        {
                            emptyAcquires.fetch_add(1);
                            continue;
                        }

                        // 模拟使用连接
                        std::this_thread::yield();

                        connection.release();
                    }
                });
            }

            for (auto &thread : threads)
            {
                thread.join();
            }

            EXPECT_EQ(emptyAcquires.load(), 0)
                << "所有线程都应成功获取到连接";

            const std::size_t active = pool.activeCount();
            const std::size_t idle   = pool.idleCount();
            const std::size_t total  = pool.totalCount();
            EXPECT_EQ(total, active + idle)
                << "总连接数应等于活跃数 + 空闲数";
        }

        // ========================================================================
        // StatisticsAreConsistent
        // ========================================================================

        /**
         * @brief 验证统计方法返回合理值
         */
        TEST(ConnectionPool, StatisticsAreConsistent)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize = 3;

            ConnectionPool pool(factory, configuration);

            EXPECT_EQ(pool.activeCount(), 0);
            EXPECT_EQ(pool.idleCount(), 0);
            EXPECT_EQ(pool.totalCount(), 0);

            PooledConnection conn1 = pool.acquire();
            ASSERT_TRUE(conn1);

            EXPECT_EQ(pool.activeCount(), 1);
            EXPECT_EQ(pool.totalCount(), 1);

            PooledConnection conn2 = pool.acquire();
            ASSERT_TRUE(conn2);

            EXPECT_EQ(pool.activeCount(), 2);
            EXPECT_EQ(pool.totalCount(), 2);

            conn1.release();

            EXPECT_EQ(pool.activeCount(), 1);
            EXPECT_EQ(pool.idleCount(), 1);
            EXPECT_EQ(pool.totalCount(), 2);

            conn2.release();

            EXPECT_EQ(pool.activeCount(), 0);
            EXPECT_EQ(pool.idleCount(), 2);
            EXPECT_EQ(pool.totalCount(), 2);

            // 再次取出，空闲数应降
            PooledConnection conn3 = pool.acquire();
            ASSERT_TRUE(conn3);
            EXPECT_EQ(pool.activeCount(), 1);
            EXPECT_EQ(pool.idleCount(), 1);
            EXPECT_EQ(pool.totalCount(), 2);
        }

        // ========================================================================
        // MoveSemantics
        // ========================================================================

        /**
         * @brief 验证 PooledConnection 移动语义正确
         */
        TEST(ConnectionPool, PooledConnectionMoveSemantics)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize = 2;

            ConnectionPool pool(factory, configuration);

            PooledConnection original = pool.acquire();
            ASSERT_TRUE(original);

            PooledConnection moved(std::move(original));
            EXPECT_TRUE(moved) << "移动后目标应持有连接";
            EXPECT_FALSE(original) << "移动后源应为空";

            // 归还 moved，释放连接回池
            moved.release();

            PooledConnection assigned = pool.acquire();
            ASSERT_TRUE(assigned);
            PooledConnection another = pool.acquire();
            ASSERT_TRUE(another);

            assigned = std::move(another);
            EXPECT_TRUE(assigned) << "移动赋值后应持有连接";
            EXPECT_FALSE(another) << "移动赋值后源应为空";
        }

        // ========================================================================
        // ResetSessionStateIsCalledOnReturn
        // ========================================================================

        /**
         * @brief 钉住：归还连接时池会复位会话状态，且两条去向都复位
         * @details 会话级状态（Redis 的未发送管道、临时表……）不能串给下一个借用者。
         *          池有两条交接路径——放回空闲栈、直接交给异步等待者——两条都必须先复位
         */
        TEST(ConnectionPool, ResetsSessionStateOnBothReturnPaths)
        {
            ConnectionCounter counter;
            auto              factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize = 1;

            ConnectionPool pool(factory, configuration);

            // 路径一：归还进空闲栈
            {
                PooledConnection connection = pool.acquire();
                ASSERT_TRUE(connection);
                EXPECT_EQ(counter.sessionResetCount.load(), 0) << "还没归还就不该复位";
            }
            EXPECT_EQ(counter.sessionResetCount.load(), 1) << "归还进空闲栈之前必须复位一次";

            // 路径二：归还时正有等待者，连接直接转给它（不入空闲栈）
            PooledConnection held = pool.acquire();
            ASSERT_TRUE(held);

            std::atomic<bool> waiterFinished{false};
            PooledConnection  assignedToWaiter;
            std::thread       waiter([&pool, &assignedToWaiter, &waiterFinished]()
            {
                assignedToWaiter = pool.acquire(); // 池满：阻塞等待
                waiterFinished.store(true, std::memory_order_release);
            });

            // 等它真的挂在等待列表上再归还，保证走的是「直接交给等待者」那条路
            const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (pool.waitingCount() == 0 && std::chrono::steady_clock::now() < waitDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ASSERT_EQ(pool.waitingCount(), 1U) << "等待者没有挂上：用例前提不成立";

            held.release();
            waiter.join();

            EXPECT_TRUE(waiterFinished.load(std::memory_order_acquire));
            EXPECT_TRUE(assignedToWaiter) << "等待者应当拿到刚归还的连接";
            EXPECT_EQ(counter.sessionResetCount.load(), 2) << "交给等待者之前同样必须复位";
        }

        // ========================================================================
        // DestructorWakesBlockedSyncWaiters
        // ========================================================================

        /**
         * @brief 池析构时仍阻塞在 acquire() 上的同步等待者要被叫醒，而不是等满自己的超时
         * @details 析构停摆时等待谓词必须成立：析构做的第一件事就是清空空闲栈，若等待者只看
         *          「空闲栈非空」，它就只能等满自己的超时，而它依赖的条件变量此刻已随对象销毁。
         *          判据：析构叫醒它之后，它自己带着空连接回来（不是被那 30 秒超时叫醒的）。
         */
        TEST(ConnectionPool, DestructorWakesBlockedSyncWaiters)
        {
            ConnectionCounter counter;

            // 上限 0：任何连接都不允许创建，同步 acquire() 必然走等待路径
            PoolConfig configuration;
            configuration.maximumPoolSize            = 0;
            configuration.acquireTimeoutMilliseconds = 30000;

            auto pool = std::make_unique<ConnectionPool>(makeMockFactory(counter), configuration);

            // 工作线程持裸指针：捕获 unique_ptr 的话，主线程下面那次 reset() 会与线程里的
            // 解引用构成对指针本身的数据竞争（TSan 实测抓到过），而池对象的成员访问另有
            // 锁与停摆标志兜住
            ConnectionPool *const poolPointer = pool.get();

            std::atomic<bool>          isWaiterReturned{false};
            std::atomic<bool>          isWaiterGotConnection{true};
            std::atomic<std::int64_t>  waiterElapsedMilliseconds{0};
            std::thread                waiter(
                    [poolPointer, &isWaiterReturned, &isWaiterGotConnection, &waiterElapsedMilliseconds]
                    {
                        const auto startedAt = std::chrono::steady_clock::now();
                        const PooledConnection connection = poolPointer->acquire();
                        waiterElapsedMilliseconds.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                std::chrono::steady_clock::now() - startedAt)
                                                                .count(),
                                                        std::memory_order_relaxed);
                        isWaiterGotConnection.store(static_cast<bool>(connection));
                        isWaiterReturned.store(true, std::memory_order_release);
                    });

            // 等它真的挂进等待列表再析构：否则考的是「析构之后才来等」，不是这条用例要钉的场景
            const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (pool->waitingCount() == 0 && std::chrono::steady_clock::now() < waitDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ASSERT_GT(pool->waitingCount(), 0U) << "等待者没有挂上：用例前提不成立";

            pool.reset();

            // 析构只是把等待者从条件变量上叫醒，它未必来得及在 reset() 返回前跑完 acquire()，
            // 直接断言就是赌调度（Linux 满载的 CI 上实测红过两次）；先等它回来再断言形态——
            // 析构真没叫醒它时这里只会等满时限，用例照样红
            const auto returnDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!isWaiterReturned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < returnDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            EXPECT_TRUE(isWaiterReturned.load(std::memory_order_acquire))
                    << "析构返回后同步等待者还睡着：它只能等满 30 秒超时，而条件变量已经随对象销毁";
            EXPECT_FALSE(isWaiterGotConnection.load()) << "池停摆时被唤醒的同步等待者应拿到空连接";
            EXPECT_LT(waiterElapsedMilliseconds.load(), 10000)
                    << "等待者是被自己的 30 秒超时叫醒的，而不是被析构叫醒的（等待了 "
                    << waiterElapsedMilliseconds.load() << "ms）";

            waiter.join();
        }

        // ========================================================================
        // 丢弃连接的出口不握着 m_mutex 做断开
        // ========================================================================

        /**
         * @brief 归还路径丢弃过期连接时，断开必须落在 m_mutex 之外
         * @details disconnect() 是一次会阻塞的系统调用（驱动里还要发一条 Quit 并等它走完）。留在
         *          锁内时，一次慢关闭会把所有取出路径、统计读取与后台驱逐一起堵在那把锁上——而
         *          maximumLifetimeSeconds 为 0（「一归还就过期」）时这条出口就是每次归还的主路径。
         *          重叠条件是用例自己造的：观察者只在门闩确认「归还线程确实停在 disconnect 里」之后
         *          才去碰那把锁，因此它要么立刻拿到锁（断开已在锁外），要么一直等不到（锁被那次
         *          关闭占着）。判据不依赖调度运气。
         */
        TEST(ConnectionPool, ReturnPathDoesNotHoldTheIdleStackLockAcrossDisconnect)
        {
            ConnectionCounter counter;
            DisconnectGate    gate;
            counter.disconnectGate = &gate;

            PoolConfig configuration;
            configuration.maximumPoolSize            = 2;
            configuration.maximumLifetimeSeconds     = 0;      // 一归还就过期：必然走丢弃出口
            configuration.healthCheckIntervalSeconds = 3600;   // 后台驱逐不参与本用例的时序

            ConnectionPool pool(makeMockFactory(counter), configuration);

            PooledConnection connection = pool.acquire();
            ASSERT_TRUE(static_cast<bool>(connection)) << "连接没拿到：用例前提不成立";

            gate.arm();
            std::thread returner([&connection]()
            {
                connection.release();
            });

            ASSERT_TRUE(TestSupport::waitForCondition([&gate]()
            {
                return gate.hasArrived();
            }, 2000)) << "归还没有发生断开：用例没有量到它要量的那条出口";

            std::atomic<bool> isObserverDone{false};
            std::thread       observer([&pool, &isObserverDone]()
            {
                static_cast<void>(pool.idleCount());
                isObserverDone.store(true, std::memory_order_release);
            });

            // 一次互斥量交接用不了 200ms：读不到只能是那把锁还被占着
            const bool observerGotThrough = TestSupport::waitForCondition([&isObserverDone]()
            {
                return isObserverDone.load(std::memory_order_acquire);
            }, 200);

            gate.release();
            returner.join();
            observer.join();

            EXPECT_TRUE(observerGotThrough)
                    << "归还路径握着 m_mutex 做 disconnect：一次慢关闭会把取出路径与统计读取一起堵住";
        }

        /**
         * @brief 取出路径丢弃失联连接时，断开同样必须落在 m_mutex 之外
         * @details 与上一条同一条纪律，另一处出口：连接在入栈之后才失联（空闲期被对端掐断），只有
         *          取出时的健康检查能发现它。这个时序按单实例设置够不着，因此用全体存活开关造
         *          「归还时健康、取出时失联」——不去睡过存活期，也就不引入时钟上的赌注。
         */
        TEST(ConnectionPool, AcquirePathDoesNotHoldTheIdleStackLockAcrossDisconnect)
        {
            ConnectionCounter counter;
            DisconnectGate    gate;
            counter.disconnectGate = &gate;

            PoolConfig configuration;
            configuration.maximumPoolSize            = 2;
            configuration.idleTimeoutSeconds         = 3600;
            configuration.maximumLifetimeSeconds     = 3600;
            configuration.healthCheckIntervalSeconds = 3600;

            ConnectionPool pool(makeMockFactory(counter), configuration);

            PooledConnection first = pool.acquire();
            ASSERT_TRUE(static_cast<bool>(first)) << "连接没拿到：用例前提不成立";
            first.release();
            ASSERT_EQ(pool.idleCount(), 1U) << "第一条没有入栈：用例前提不成立";

            counter.connectionsHealthy.store(false); // 空闲期失联：下一次取出才发现
            gate.arm();

            // 取出的那条必然被丢弃，丢弃后要补建一条（池未满），因此这里拿到的是新连接
            std::thread acquirer([&pool]()
            {
                const PooledConnection replacement = pool.acquire();
                static_cast<void>(replacement);
            });

            ASSERT_TRUE(TestSupport::waitForCondition([&gate]()
            {
                return gate.hasArrived();
            }, 2000)) << "取出没有发生断开：用例没有量到它要量的那条出口";

            std::atomic<bool> isObserverDone{false};
            std::thread       observer([&pool, &isObserverDone]()
            {
                static_cast<void>(pool.totalCount());
                isObserverDone.store(true, std::memory_order_release);
            });

            const bool observerGotThrough = TestSupport::waitForCondition([&isObserverDone]()
            {
                return isObserverDone.load(std::memory_order_acquire);
            }, 200);

            counter.connectionsHealthy.store(true);
            gate.release();
            acquirer.join();
            observer.join();

            EXPECT_TRUE(observerGotThrough)
                    << "取出路径握着 m_mutex 做 disconnect：过期与健康判定都留在锁内时，统计读取要等那次关闭";
        }

        /**
         * @brief 连接的建立时刻跟着连接本身走，反复借用不会被重新盖戳
         * @details 存活期过期判的是「这条连接建立了多久」，而这条依据只有在借用/归还的整段途中
         *          都不漂移时才成立：若每次借出都重记一次，`maximumLifetimeSeconds` 就永远不会到，
         *          一条连接可以被无限期地续下去。
         */
        TEST(ConnectionPool, EstablishedTimeSurvivesRepeatedBorrowAndReturn)
        {
            ConnectionCounter counter;

            PoolConfig configuration;
            configuration.maximumPoolSize            = 2;
            configuration.idleTimeoutSeconds         = 0;
            configuration.maximumLifetimeSeconds     = 3600;
            configuration.healthCheckIntervalSeconds = 3600;

            ConnectionPool pool(makeMockFactory(counter), configuration);

            const std::chrono::steady_clock::time_point firstEstablishedAt = [&pool]
            {
                const PooledConnection connection = pool.acquire();
                return connection->establishedAt();
            }();

            PooledConnection second = pool.acquire();
            ASSERT_TRUE(static_cast<bool>(second)) << "第二条没拿到：用例前提不成立";
            const std::chrono::steady_clock::time_point reusedEstablishedAt = second->establishedAt();
            second.release();

            // 池里只有一条连接（上限 2、只建过一条），因此第二次借用拿到的就是同一条
            EXPECT_EQ(reusedEstablishedAt, firstEstablishedAt)
                    << "借用/归还会给连接重新盖建立时刻，存活期上限因此永远到不了";
        }

    } // namespace
} // namespace AsynGyanis::Database