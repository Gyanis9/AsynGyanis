/**
 * @file TestConnection.cpp
 * @brief Connection 单元测试：构造、关闭、取消传播与基类协程启动
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/Connection.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"

#include <gtest/gtest.h>

#include <chrono>
#include <utility>

namespace AsynGyanis::Core
{
    /**
     * @brief 构造后连接立即处于存活态并持有传入的 socket（描述符原样保留，不做替换）
     */
    TEST(Connection, ConstructionKeepsSocketAndAliveFlag)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1)); // 描述符 -1 的哑 socket，仅验证对象生命周期

        EXPECT_TRUE(connection.isAlive());
        EXPECT_EQ(connection.socket().fileDescriptor(), -1);
    }

    /**
     * @brief close() 把存活标志置为 false：后续查询据此拒绝继续读写的调用方
     */
    TEST(Connection, CloseMarksConnectionNotAlive)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.close();

        EXPECT_FALSE(connection.isAlive());
    }

    /**
     * @brief close() 同时向自身的取消对象广播停止请求：关闭即取消在途操作，不需调用方单独取消
     */
    TEST(Connection, CloseRequestsStopOnCancelable)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.close();

        EXPECT_TRUE(connection.cancelable().isStopRequested());
    }

    /**
     * @brief 基类 start() 不引入额外挂起点：单次 resume 即完成，不会吊住事件循环
     */
    TEST(Connection, BaseStartCompletesImmediately)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        auto task = connection.start();
        task.handle().resume();
        EXPECT_TRUE(task.isReady());
    }

    /**
     * @brief 移动构造带走关闭状态：已关闭的源移动后，新对象仍是「不存活」
     */
    TEST(Connection, MoveConstructionPreservesAliveState)
    {
        EventLoop loop;
        Connection connection1(AsyncSocket(loop, -1));
        connection1.close();

        Connection connection2(std::move(connection1));

        EXPECT_FALSE(connection2.isAlive());
    }

    /**
     * @brief 移动赋值以源的状态覆盖目标：不会把一条已关闭的连接「复活」成存活
     */
    TEST(Connection, MoveAssignmentPreservesAliveState)
    {
        EventLoop loop;
        Connection connection1(AsyncSocket(loop, -1));
        Connection connection2(AsyncSocket(loop, -1));

        connection1.close();
        connection2 = std::move(connection1);

        EXPECT_FALSE(connection2.isAlive());
    }

    /**
     * @brief cancelable() 暴露内部真实取消对象（引用）：外部请求停止与其状态查询保持同步
     */
    TEST(Connection, CancelableReflectsStopRequest)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        Cancelable &cancelable = connection.cancelable();
        ASSERT_FALSE(cancelable.isStopRequested());

        EXPECT_TRUE(cancelable.requestStop());
        EXPECT_TRUE(cancelable.isStopRequested());
    }

    /**
     * @brief 未设置截止时间的连接不参与空闲清扫：非 HTTP 会话不该被超时误伤
     */
    TEST(Connection, WithoutIdleDeadlineItNeverReportsExpired)
    {
        EventLoop  loop;
        Connection connection(AsyncSocket(loop, -1));

        // 构造后没有截止时间，任何「现在」都不算超期
        const auto farFuture = std::chrono::steady_clock::now() + std::chrono::hours(24);
        EXPECT_FALSE(connection.isIdleExpired(std::chrono::steady_clock::now()));
        EXPECT_FALSE(connection.isIdleExpired(farFuture));
    }

    /**
     * @brief refreshIdleDeadline() 按传入时限重新计时：到点前不超期，到点后才算超期
     */
    TEST(Connection, RefreshIdleDeadlineMarksExpiryAfterTimeout)
    {
        EventLoop  loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.refreshIdleDeadline(std::chrono::milliseconds{50});

        const auto now = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.isIdleExpired(now)) << "刚刷新的截止时间不该立刻到期";
        EXPECT_TRUE(connection.isIdleExpired(now + std::chrono::milliseconds{60})) << "超过时限后应判定为超期";

        // 时限为 0 表示关闭本项保护：不是「立即到期」
        connection.refreshIdleDeadline(std::chrono::milliseconds{0});
        EXPECT_FALSE(connection.isIdleExpired(now + std::chrono::hours(1))) << "0 应当清除截止时间而不是设成立即到期";

        // 负数一律按清除处理，避免设出一个已经过去的截止时间
        connection.refreshIdleDeadline(std::chrono::milliseconds{-5});
        EXPECT_FALSE(connection.isIdleExpired(now + std::chrono::hours(1)));
    }

    /**
     * @brief clearIdleDeadline() 撤销超时约束：清扫协程此后不会再判定它超期
     */
    TEST(Connection, ClearIdleDeadlineRemovesExpiry)
    {
        EventLoop  loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.refreshIdleDeadline(std::chrono::milliseconds{1});
        connection.clearIdleDeadline();

        EXPECT_FALSE(connection.isIdleExpired(std::chrono::steady_clock::now() + std::chrono::hours(1)));
    }

    /**
     * @brief 移动构造带走截止时间：移动后的新对象仍然受同一份超时约束
     */
    TEST(Connection, MoveConstructionPreservesIdleDeadline)
    {
        EventLoop  loop;
        Connection connection1(AsyncSocket(loop, -1));
        connection1.refreshIdleDeadline(std::chrono::milliseconds{1});

        Connection connection2(std::move(connection1));

        EXPECT_TRUE(connection2.isIdleExpired(std::chrono::steady_clock::now() + std::chrono::hours(1)))
                << "移动后截止时间丢失：这条连接会被空闲清扫漏掉";
    }
}
