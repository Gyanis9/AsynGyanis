/**
 * @file TestAsyncSocket.cpp
 * @brief AsyncSocket 单元测试：创建、移动语义、bind/listen 生命周期与地址查询
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/AsyncSocket.h"

#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"

#include <gtest/gtest.h>

namespace AsynGyanis::Core
{
    TEST(AsyncSocket, CreateReturnsValidDescriptor)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);

        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        asyncSocket.close();
    }

    TEST(AsyncSocket, MoveConstructionTransfersDescriptor)
    {
        EventLoop loop;
        AsyncSocket asyncSocket1 = AsyncSocket::create(loop);
        const int fileDescriptor1 = asyncSocket1.fileDescriptor();
        ASSERT_GE(fileDescriptor1, 0);

        const AsyncSocket asyncSocket2(std::move(asyncSocket1));

        EXPECT_EQ(asyncSocket2.fileDescriptor(), fileDescriptor1);
        EXPECT_EQ(asyncSocket1.fileDescriptor(), -1);
    }

    TEST(AsyncSocket, MoveAssignmentTransfersDescriptor)
    {
        EventLoop loop;
        AsyncSocket asyncSocket1 = AsyncSocket::create(loop);
        AsyncSocket asyncSocket2 = AsyncSocket::create(loop);
        const int fileDescriptor1 = asyncSocket1.fileDescriptor();
        ASSERT_GE(fileDescriptor1, 0);

        asyncSocket2 = std::move(asyncSocket1);

        EXPECT_EQ(asyncSocket2.fileDescriptor(), fileDescriptor1);
        EXPECT_EQ(asyncSocket1.fileDescriptor(), -1);

        asyncSocket2.close();
    }

    TEST(AsyncSocket, CloseResetsDescriptorToInvalid)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        asyncSocket.close();

        EXPECT_EQ(asyncSocket.fileDescriptor(), -1);
    }

    TEST(AsyncSocket, DoubleCloseIsSafe)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);
        asyncSocket.close();

        EXPECT_NO_THROW(asyncSocket.close());
        EXPECT_EQ(asyncSocket.fileDescriptor(), -1);
    }

    TEST(AsyncSocket, BindToLoopbackEphemeralPortSucceeds)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        const InetAddress address = InetAddress::localhost(0);

        ASSERT_TRUE(asyncSocket.bind(address));

        asyncSocket.close();
    }

    TEST(AsyncSocket, ListenAfterBindSucceeds)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        const InetAddress address = InetAddress::localhost(0);

        ASSERT_TRUE(asyncSocket.bind(address));
        ASSERT_TRUE(asyncSocket.listen());

        asyncSocket.close();
    }

    TEST(AsyncSocket, SetSockOptEnablesAddressReuse)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);

        int optionValue = 1;
        ASSERT_TRUE(asyncSocket.setSockOpt(SOL_SOCKET, SO_REUSEADDR, &optionValue, sizeof(optionValue)));

        asyncSocket.close();
    }

    TEST(AsyncSocket, BindListenCloseLifecycleReportsEphemeralPort)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        const InetAddress address = InetAddress::localhost(0);

        ASSERT_TRUE(asyncSocket.bind(address));
        ASSERT_TRUE(asyncSocket.listen());

        const InetAddress localAddress = asyncSocket.localAddress();
        EXPECT_NE(localAddress.port(), 0);

        asyncSocket.close();
        EXPECT_EQ(asyncSocket.fileDescriptor(), -1);
    }

    TEST(AsyncSocket, LocalAddressMatchesBoundLoopbackAddress)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_TRUE(asyncSocket.bind(InetAddress::localhost(0)));

        const InetAddress localAddress = asyncSocket.localAddress();
        EXPECT_EQ(localAddress.ip(), "127.0.0.1");
        EXPECT_NE(localAddress.port(), 0);

        asyncSocket.close();
    }

    TEST(AsyncSocket, RemoteAddressThrowsOnUnconnectedSocket)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        EXPECT_THROW(asyncSocket.remoteAddress(), Base::SystemException);

        asyncSocket.close();
    }
}
