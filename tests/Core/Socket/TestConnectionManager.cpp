/**
 * @file TestConnectionManager.cpp
 * @brief ConnectionManager 单元测试：连接增删、优雅关闭与 waitAll 阻塞语义
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/ConnectionManager.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/Connection.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 有界轮询统一使用的超时上限（毫秒），避免固定 sleep 硬等
        constexpr int kWaitTimeoutMilliseconds = 2000;

        /// 有界轮询的步进间隔（毫秒）
        constexpr int kPollIntervalMilliseconds = 10;

        /**
         * @brief 构造一个挂在指定事件循环上的哑连接（描述符 -1，仅用于管理器增删）
         * @param loop 关联的事件循环
         * @return 哑连接的共享指针
         */
        std::shared_ptr<Connection> makeDummyConnection(EventLoop &loop)
        {
            return std::make_shared<Connection>(AsyncSocket(loop, -1));
        }

        /**
         * @brief 在 2 秒超时窗口内轮询等待原子标志置位
         * @param flag 待轮询的原子标志
         * @return 超时前置位返回 true，否则返回最后一次读取结果
         */
        bool waitForFlag(const std::atomic<bool> &flag)
        {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds{kWaitTimeoutMilliseconds};
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (flag.load())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{kPollIntervalMilliseconds});
            }
            return flag.load();
        }
    }

    TEST(ConnectionManager, AddIncrementsActiveCount)
    {
        EventLoop loop;
        ConnectionManager manager;
        ASSERT_EQ(manager.activeCount(), 0);

        manager.add(makeDummyConnection(loop));
        EXPECT_EQ(manager.activeCount(), 1);
    }

    TEST(ConnectionManager, RemoveByPointerDecrementsActiveCount)
    {
        EventLoop loop;
        ConnectionManager manager;
        std::shared_ptr<Connection> connection = makeDummyConnection(loop);

        manager.add(connection);
        ASSERT_EQ(manager.activeCount(), 1);

        manager.remove(connection.get());
        EXPECT_EQ(manager.activeCount(), 0);
    }

    TEST(ConnectionManager, RemoveNullPointerIsNoOp)
    {
        EventLoop loop;
        ConnectionManager manager;

        EXPECT_NO_THROW(manager.remove(nullptr));
        EXPECT_EQ(manager.activeCount(), 0);
    }

    TEST(ConnectionManager, AddNullPointerIsIgnored)
    {
        EventLoop loop;
        ConnectionManager manager;

        manager.add(nullptr);
        EXPECT_EQ(manager.activeCount(), 0);
    }

    TEST(ConnectionManager, TracksMultipleConnections)
    {
        EventLoop loop;
        ConnectionManager manager;
        const auto connection1 = makeDummyConnection(loop);
        const auto connection2 = makeDummyConnection(loop);
        const auto connection3 = makeDummyConnection(loop);

        manager.add(connection1);
        manager.add(connection2);
        manager.add(connection3);
        ASSERT_EQ(manager.activeCount(), 3);

        manager.remove(connection2.get());
        EXPECT_EQ(manager.activeCount(), 2);
    }

    TEST(ConnectionManager, ShutdownRequestsStopOnAllConnections)
    {
        EventLoop loop;
        ConnectionManager manager;
        const auto connection1 = makeDummyConnection(loop);
        const auto connection2 = makeDummyConnection(loop);

        manager.add(connection1);
        manager.add(connection2);

        manager.shutdown();

        EXPECT_TRUE(connection1->cancelable().isStopRequested());
        EXPECT_TRUE(connection2->cancelable().isStopRequested());
    }

    TEST(ConnectionManager, WaitAllReturnsOnceAllConnectionsRemoved)
    {
        EventLoop loop;
        ConnectionManager manager;
        std::shared_ptr<Connection> connection = makeDummyConnection(loop);
        manager.add(connection);

        std::atomic<bool> finished{false};
        std::thread waiter([&manager, &finished]()
        {
            manager.waitAll();
            finished.store(true);
        });

        // 集合非空时 waitAll 必然不会返回（不依赖线程调度时序，确定成立）
        ASSERT_FALSE(finished.load());

        manager.remove(connection.get());
        EXPECT_TRUE(waitForFlag(finished));

        waiter.join();
        EXPECT_TRUE(finished.load());
    }

    TEST(ConnectionManager, RemoveUnknownPointerIsNoOp)
    {
        EventLoop loop;
        ConnectionManager manager;
        std::shared_ptr<Connection> connection = makeDummyConnection(loop);
        manager.add(connection);

        const auto other = makeDummyConnection(loop);
        manager.remove(other.get()); // 不同的指针，不应影响已加入的连接
        EXPECT_EQ(manager.activeCount(), 1);
    }
}
