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
    TEST(Connection, ConstructionKeepsSocketAndAliveFlag)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1)); // 描述符 -1 的哑 socket，仅验证对象生命周期

        EXPECT_TRUE(connection.isAlive());
        EXPECT_EQ(connection.socket().fileDescriptor(), -1);
    }

    TEST(Connection, CloseMarksConnectionNotAlive)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.close();

        EXPECT_FALSE(connection.isAlive());
    }

    TEST(Connection, CloseRequestsStopOnCancelable)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.close();

        EXPECT_TRUE(connection.cancelable().isStopRequested());
    }

    TEST(Connection, BaseStartCompletesImmediately)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        auto task = connection.start();
        task.handle().resume();
        EXPECT_TRUE(task.isReady());
    }

    TEST(Connection, MoveConstructionPreservesAliveState)
    {
        EventLoop loop;
        Connection connection1(AsyncSocket(loop, -1));
        connection1.close();

        Connection connection2(std::move(connection1));

        EXPECT_FALSE(connection2.isAlive());
    }

    TEST(Connection, MoveAssignmentPreservesAliveState)
    {
        EventLoop loop;
        Connection connection1(AsyncSocket(loop, -1));
        Connection connection2(AsyncSocket(loop, -1));

        connection1.close();
        connection2 = std::move(connection1);

        EXPECT_FALSE(connection2.isAlive());
    }

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
