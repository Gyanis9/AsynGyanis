/**
 * @file TestConnectionPoolAsync.cpp
 * @brief 连接池异步获取测试 —— 挂起/唤醒的线程归属与快慢两条路径
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这个文件盯住的是一条容易被忽略、却在真机上才会暴露的性质：**协程在哪个线程上恢复**。
 *          池满时 `acquireAsync()` 会把协程挂起，归还连接的一方再把恢复动作投递回
 *          `acquireAsync()` 给定的 `EventLoop`（`Scheduler::scheduleRemote`），
 *          因此恢复后的代码仍运行在那个事件循环线程上——调用方可以放心按
 *          「我的后续代码都在自己的循环线程上」来写。
 *
 *          修复前该形参在实现里根本没用（写作 `/*loop*​/`），恢复点在归还连接的那个线程上；
 *          本文件就是防止它退化的那道闸：断言恢复线程 == 事件循环线程，且**不等于**调用线程。
 *
 * 覆盖场景：
 * - AcquireAsyncCreatesConnectionImmediatelyWhenPoolNotFull（快路径：池未满时直接建连，不挂起）
 * - AcquireAsyncReusesIdleConnectionImmediately（快路径：复用空闲连接，不新建）
 * - AcquireAsyncResumesOnGivenEventLoop（慢路径：隔线程归还后，恢复发生在循环线程上）
 * - DestructorWakesWaitersWithEmptyConnection（池销毁时以空连接唤醒，不永久挂起）
 */
#include "Database/Common/DatabaseConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "DatabaseTestSupport.h"

#include "TestConnectionPool.h"

#include <gtest/gtest.h>

#include <atomic>
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
        using TestSupport::EventLoopThread;
        using TestSupport::waitForCondition;

        /// 一次异步获取的观测结果
        struct AcquireProbe
        {
            std::optional<PooledConnection> connection;    ///< 拿到的连接（恢复后写入）
            std::thread::id                 resumeThreadId; ///< 恢复本协程的线程 id
            std::atomic<bool>               finished{false}; ///< 完成标记，最后写入（release 语义）
        };

        /**
         * @brief 驱动协程：等待一次异步获取，并记录恢复发生在哪个线程上
         *
         * @details 记录线程 id 的那一行必须紧跟在 `co_await` 之后——它正是「恢复点」，
         *          也是本文件要验证的观察点。
         * @param pool 目标连接池
         * @param loop 恢复用的事件循环
         * @param probe 观测结果出参，由调用方持有并保证活到协程结束
         * @return Core::Task<void> 驱动协程
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

    TEST(ConnectionPoolAsync, AcquireAsyncCreatesConnectionImmediatelyWhenPoolNotFull)
    {
        ConnectionCounter counter;
        ConnectionPool   pool = makeSingleSlotPool(counter);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        AcquireProbe probe;
        const std::thread::id callerThreadId = std::this_thread::get_id();

        // 池是空的但未达上限：应当与同步 acquire() 同口径——立刻建连返回，不经过挂起。
        // 这条曾经不成立：异步路径只从空闲栈取，导致刚建好的池上所有异步获取都先挂起
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

    TEST(ConnectionPoolAsync, AcquireAsyncReusesIdleConnectionImmediately)
    {
        ConnectionCounter counter;
        ConnectionPool   pool = makeSingleSlotPool(counter);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        // 先取一条再归还：池里出现一条空闲连接，异步获取应当直接复用而不新建
        pool.acquire().release();

        AcquireProbe probe;
        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume();

        ASSERT_TRUE(probe.finished.load(std::memory_order_acquire));
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_TRUE(probe.connection.value());
        EXPECT_EQ(counter.totalCreated.load(), 1) << "应复用空闲连接而不是再建一条";

        loopThread.parkDriver(std::move(driver));
    }

    TEST(ConnectionPoolAsync, AcquireAsyncResumesOnGivenEventLoop)
    {
        ConnectionCounter counter;
        ConnectionPool   pool = makeSingleSlotPool(counter);

        EventLoopThread loopThread;
        ASSERT_TRUE(loopThread.waitUntilRunning());

        // 占满唯一的坑：此时再异步获取必然挂起
        PooledConnection occupyingConnection = pool.acquire();
        ASSERT_TRUE(occupyingConnection);

        AcquireProbe probe;
        const std::thread::id callerThreadId = std::this_thread::get_id();

        Core::Task<void> driver = probeAcquireAsync(pool, loopThread.loop(), probe);
        driver.handle().resume();

        // 无可用连接且已达上限：协程应停在挂起点上
        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));

        // 在**调用线程**上归还连接：唤醒动作由这里发起，但恢复必须发生在循环线程上
        occupyingConnection.release();

        ASSERT_TRUE(waitForCondition([&probe]()
        {
            return probe.finished.load(std::memory_order_acquire);
        }));

        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_TRUE(probe.connection.value()) << "被唤醒的等待者应拿到刚归还的连接";

        // 核心断言：恢复落在给定的事件循环线程上，而不是发起归还的调用线程
        EXPECT_NE(probe.resumeThreadId, callerThreadId)
            << "协程在归还连接的线程上被就地恢复，loop 形参形同虚设";
        EXPECT_EQ(probe.resumeThreadId, loopThread.threadId())
            << "恢复应发生在 acquireAsync() 给定的那个事件循环线程上";

        loopThread.parkDriver(std::move(driver));
    }

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

        AcquireProbe probe;
        Core::Task<void> driver = probeAcquireAsync(*pool, loopThread.loop(), probe);
        driver.handle().resume();

        ASSERT_FALSE(probe.finished.load(std::memory_order_acquire));
        EXPECT_EQ(pool->waitingCount(), 1U) << "没有可用连接时协程应挂在等待列表里";

        // 池在还有等待者时被销毁：析构必须唤醒它们并交回空连接。
        // 这一次是就地恢复（池已停摆，投回事件循环可能永远不会被执行），
        // 理由写在 ConnectionPool 析构的注释里，因此这里只断言「确实醒了、拿到的是空连接」
        pool.reset();

        ASSERT_TRUE(waitForCondition([&probe]()
        {
            return probe.finished.load(std::memory_order_acquire);
        }));
        ASSERT_TRUE(probe.connection.has_value());
        EXPECT_FALSE(probe.connection.value()) << "池停摆时被唤醒的等待者应拿到空连接";

        loopThread.parkDriver(std::move(driver));
    }

} // namespace AsynGyanis::Database
