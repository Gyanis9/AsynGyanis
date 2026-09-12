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
#include <vector>

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

    /**
     * @brief 验证 add() 让活跃计数加一（计数是过载保护的判据，不能多算少算）
     */
    TEST(ConnectionManager, AddIncrementsActiveCount)
    {
        EventLoop loop;
        ConnectionManager manager;
        ASSERT_EQ(manager.activeCount(), 0);

        manager.add(makeDummyConnection(loop));
        EXPECT_EQ(manager.activeCount(), 1);
    }

    /**
     * @brief 验证按裸指针 remove() 能让活跃计数减一
     *
     * @details 管理器按裸指针索引（Connection 的属主是协程侧，管理器不共享所有权），
     *          因此这条是计数能归零的前提。
     */
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

    /**
     * @brief 验证 remove(nullptr) 是空操作：清理路径可能在指针已失效时无脑调用
     */
    TEST(ConnectionManager, RemoveNullPointerIsNoOp)
    {
        EventLoop loop;
        ConnectionManager manager;

        EXPECT_NO_THROW(manager.remove(nullptr));
        EXPECT_EQ(manager.activeCount(), 0);
    }

    /**
     * @brief 验证 add(nullptr) 被忽略而不是插入一个空条目（否则后续遍历会解空指针）
     */
    TEST(ConnectionManager, AddNullPointerIsIgnored)
    {
        EventLoop loop;
        ConnectionManager manager;

        manager.add(nullptr);
        EXPECT_EQ(manager.activeCount(), 0);
    }

    /**
     * @brief 验证多连接独立记账：移除其中一个不影响其余连接
     */
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

    /**
     * @brief 验证 shutdown() 给每条已登记连接都发出停止请求
     *
     * @details 这是「优雅关闭」的核心：漏掉任何一条，那条连接就会继续跑，
     *          收尾阶段等它退出会一直等不到。
     */
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

    /**
     * @brief 验证 shutdown() 之后挂上来的连接会被 add() 自己收尾
     *
     * @details shutdown() 只能遍历它调用那一刻的快照，晚到的连接不会被它看到；
     *          若 add() 不做补偿，这条连接会永远留在活跃表里——既不会被关闭，
     *          也会让 waitAll() 永远等不到集合变空。
     */
    TEST(ConnectionManager, AddAfterShutdownClosesNewcomerImmediately)
    {
        EventLoop loop;
        ConnectionManager manager;
        manager.shutdown();

        // shutdown() 已开始：这条连接在加入的那一刻就必须被收尾，而不是留在表里等下一次关闭
        const auto lateConnection = makeDummyConnection(loop);
        manager.add(lateConnection);

        EXPECT_TRUE(lateConnection->cancelable().isStopRequested()) << "晚到的连接没有收到停止请求";
        EXPECT_FALSE(lateConnection->isAlive()) << "晚到的连接应当已被 close() 收尾";

        // add() 只负责关闭，**不**代删表项：摘除由持有该连接的协程在收尾路径上调 remove()，
        // 与正常关闭路径一致（谁拥有谁摘除），所以此刻它仍在活跃表里
        EXPECT_EQ(manager.activeCount(), 1U);

        // 属主收尾后名额释放：若不释放，过载保护会因这些连接永久拒绝新连接
        manager.remove(lateConnection.get());
        EXPECT_EQ(manager.activeCount(), 0U);
    }

    /**
     * @brief 验证 waitAll() 阻塞到集合为空才返回
     *
     * @details 先断言「集合非空时等待线程必然未返回」（这一步不依赖调度时序，确定成立），
     *          再移除连接并等到线程结束——把「会等待」与「能醒来」两半都钉住，
     *          避免只测后者时阻塞实现退化成立即返回也照样通过。
     */
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

    /**
     * @brief 验证移除一个未登记的指针不会误伤已登记连接
     */
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

    /**
     * @brief 验证空管理器取快照得到空列表：清扫协程在空闲服务器上靠它安全地空转
     */
    TEST(ConnectionManager, SnapshotOfEmptyManagerIsEmpty)
    {
        ConnectionManager manager;

        EXPECT_TRUE(manager.snapshot().empty());
    }

    /**
     * @brief 验证快照是「取的那一刻」的独立副本：之后的增删改不动它，且原连接对象仍被持有
     *
     * @details 清扫协程要在锁外遍历快照并逐条关闭；若快照与内部集合共享同一份存储，
     *          遍历中途的 remove() 会让迭代器失效。这里还顺带钉住「快照持有 shared_ptr」——
     *          被移除的连接对象在快照析构前不会销毁，清除动作因此不会踩到已释放对象。
     */
    TEST(ConnectionManager, SnapshotIsIndependentCopyHoldingConnectionsAlive)
    {
        EventLoop loop;
        ConnectionManager manager;
        const auto connection1 = makeDummyConnection(loop);
        const auto connection2 = makeDummyConnection(loop);

        manager.add(connection1);
        manager.add(connection2);

        std::vector<std::shared_ptr<Connection> > snapshot = manager.snapshot();
        ASSERT_EQ(snapshot.size(), 2U);

        const std::weak_ptr<Connection> firstConnectionRef = connection1;
        manager.remove(connection1.get());

        // 快照是副本：管理器里的增删不会改变它；被移除的连接仍由快照持有，对象依然存活
        EXPECT_EQ(snapshot.size(), 2U);
        EXPECT_EQ(manager.activeCount(), 1U);
        EXPECT_FALSE(firstConnectionRef.expired()) << "快照没有持有连接对象：遍历期间该对象可能已销毁";
    }
}
