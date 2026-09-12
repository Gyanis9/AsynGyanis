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
}
