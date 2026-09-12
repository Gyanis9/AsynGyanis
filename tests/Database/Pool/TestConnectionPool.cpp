/**
 * @file TestConnectionPool.cpp
 * @brief 连接池单元测试
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 用 TestConnectionPool.h 里的 MockConnection 驱动连接池，覆盖取用/归还复用、容量满时的阻塞与超时、
 *          超寿命连接的丢弃、并发取还的统计自洽，全程不触碰真实数据库。
 */
// 覆盖场景：
// - AcquireReleaseReusesConnection：取一条，归还，再取，应得同一连接
// - AcquireBlocksThenSucceeds：容量1，另一线程等超时前归还，阻塞者应拿到
// - AcquireTimeout：容量0，超时内无法获取，返回空
// - ExcessLifetimeConnectionIsDiscarded：maxLifetimeSeconds=0，归还后丢弃，下次获取得到新连接
// - ConcurrentAcquireReleaseStress：多线程并发获取/归还，统计自洽

#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Common/DatabaseConnection.h"

#include "TestConnectionPool.h"

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

            // 第一次获取
            PooledConnection first = pool.acquire();
            ASSERT_TRUE(first) << "首次获取应成功";

            DatabaseConnection *firstPointer = first.operator->();

            // 归还
            first.release();
            ASSERT_FALSE(first) << "release 后应为空";

            // 第二次获取
            PooledConnection second = pool.acquire();
            ASSERT_TRUE(second) << "第二次获取应成功";

            DatabaseConnection *secondPointer = second.operator->();

            // 应当得到同一个连接（LIFO 栈顶）
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

            // 确保第二个线程已开始等待
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // 归还连接 — 应唤醒等待者
            taken.release();

            // 等待第二个线程完成
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

            // 第一次非阻塞获取应成功
            PooledConnection first = pool.tryAcquire();
            ASSERT_TRUE(first) << "首次 tryAcquire 应成功";

            // 第二次非阻塞获取应立刻失败
            PooledConnection second = pool.tryAcquire();
            EXPECT_FALSE(second) << "池已空，tryAcquire 应立刻返回空";

            // 归还后再次尝试应成功
            first.release();

            PooledConnection third = pool.tryAcquire();
            EXPECT_TRUE(third) << "归还后 tryAcquire 应成功";
        }

        // ========================================================================
        // ExcessLifetimeConnectionIsDiscarded
        // ========================================================================

        /**
         * @brief 把 maxLifetimeSeconds 设成 0（立即过期），拿到的连接归还后应被丢弃，
         *        下次获取拿到的是新连接（不同 ID）。
         */
        TEST(ConnectionPool, ExcessLifetimeConnectionIsDiscarded)
        {
            ConnectionCounter counter;
            auto factory = makeMockFactory(counter);

            PoolConfig configuration;
            configuration.maximumPoolSize       = 2;
            configuration.maximumLifetimeSeconds = 0; // 立即过期

            ConnectionPool pool(factory, configuration);

            // 第一次获取
            PooledConnection first = pool.acquire();
            ASSERT_TRUE(first);
            MockConnection *firstMock = static_cast<MockConnection *>(first.operator->());
            const std::int64_t firstId = firstMock->id();

            // 归还：应被丢弃（maxLifetimeSeconds == 0）
            first.release();

            // 第二次获取：应得到新连接
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

            // 再次获取：应创建新连接
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

                        // 归还
                        connection.release();
                    }
                });
            }

            for (auto &thread : threads)
            {
                thread.join();
            }

            // 断言：没有线程拿到空连接
            EXPECT_EQ(emptyAcquires.load(), 0)
                << "所有线程都应成功获取到连接";

            // 统计自洽：total == active + idle
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

            // 移动构造：源对象应变为空，目标持有连接
            PooledConnection moved(std::move(original));
            EXPECT_TRUE(moved) << "移动后目标应持有连接";
            EXPECT_FALSE(original) << "移动后源应为空";

            // 归还 moved，释放连接回池
            moved.release();

            // 移动赋值：从池再取两条连接
            PooledConnection assigned = pool.acquire();
            ASSERT_TRUE(assigned);
            PooledConnection another = pool.acquire();
            ASSERT_TRUE(another);

            // 移动赋值：another 的内容转移到 assigned
            assigned = std::move(another);
            EXPECT_TRUE(assigned) << "移动赋值后应持有连接";
            EXPECT_FALSE(another) << "移动赋值后源应为空";
        }

    } // namespace
} // namespace AsynGyanis::Database