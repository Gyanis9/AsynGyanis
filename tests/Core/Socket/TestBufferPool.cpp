/**
 * @file TestBufferPool.cpp
 * @brief BufferPool 单元测试：配置属性、缓冲区获取/释放、数据写入与隔离性
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/BufferPool.h"

#include <gtest/gtest.h>

#include <cstring>

namespace AsynGyanis::Core
{
    TEST(BufferPool, ConstructionReportsConfiguredProperties)
    {
        const BufferPool pool(1024, 8);

        EXPECT_EQ(pool.bufferSize(), 1024u);
        EXPECT_EQ(pool.bufferCount(), 8u);
    }

    TEST(BufferPool, AcquireReturnsDistinctValidIndices)
    {
        BufferPool pool(256, 4);

        const int firstIndex = pool.acquire();
        const int secondIndex = pool.acquire();
        const int thirdIndex = pool.acquire();
        const int fourthIndex = pool.acquire();

        ASSERT_GE(firstIndex, 0);
        ASSERT_GE(secondIndex, 0);
        ASSERT_GE(thirdIndex, 0);
        ASSERT_GE(fourthIndex, 0);

        // 所有索引应互不相同
        EXPECT_NE(firstIndex, secondIndex);
        EXPECT_NE(firstIndex, thirdIndex);
        EXPECT_NE(firstIndex, fourthIndex);
        EXPECT_NE(secondIndex, thirdIndex);
        EXPECT_NE(secondIndex, fourthIndex);
        EXPECT_NE(thirdIndex, fourthIndex);
    }

    TEST(BufferPool, AcquireReturnsMinusOneWhenExhausted)
    {
        BufferPool pool(64, 2);

        EXPECT_GE(pool.acquire(), 0);
        EXPECT_GE(pool.acquire(), 0);
        // 缓冲区耗尽后应返回 -1
        EXPECT_EQ(pool.acquire(), -1);
    }

    TEST(BufferPool, ReleaseAllowsReacquire)
    {
        BufferPool pool(128, 2);

        const int firstIndex = pool.acquire();
        const int secondIndex = pool.acquire();
        ASSERT_GE(firstIndex, 0);
        ASSERT_GE(secondIndex, 0);
        ASSERT_EQ(pool.acquire(), -1);

        pool.release(firstIndex);

        // 释放后应能重新获取到缓冲区
        const int reacquiredIndex = pool.acquire();
        EXPECT_GE(reacquiredIndex, 0);
    }

    TEST(BufferPool, DataReturnsWritablePointer)
    {
        BufferPool pool(1024, 4);

        const int index = pool.acquire();
        ASSERT_GE(index, 0);

        void *pointer = pool.data(index);
        ASSERT_NE(pointer, nullptr);

        // 缓冲区应可完整写入
        std::memset(pointer, 0xAB, 1024);
    }

    TEST(BufferPool, DataIsIsolatedBetweenBuffers)
    {
        BufferPool pool(1024, 4);

        const int firstIndex = pool.acquire();
        const int secondIndex = pool.acquire();
        ASSERT_GE(firstIndex, 0);
        ASSERT_GE(secondIndex, 0);

        std::memset(pool.data(firstIndex), 0x11, 1024);
        std::memset(pool.data(secondIndex), 0x22, 1024);

        // 各缓冲区写入互不影响
        const auto *firstPointer = static_cast<const unsigned char *>(pool.data(firstIndex));
        const auto *secondPointer = static_cast<const unsigned char *>(pool.data(secondIndex));
        EXPECT_EQ(firstPointer[0], 0x11);
        EXPECT_EQ(secondPointer[0], 0x22);
    }

    TEST(BufferPool, ZeroBufferSizeConstructionIsTolerated)
    {
        // 边界用例：缓冲区大小为 0 时不崩溃，仍可按个数分配索引
        BufferPool pool(0, 4);

        EXPECT_EQ(pool.bufferSize(), 0u);
        EXPECT_EQ(pool.bufferCount(), 4u);

        const int index = pool.acquire();
        EXPECT_GE(index, 0);
    }
} // namespace AsynGyanis::Core
