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
    /**
     * @brief 构造只登记配置不按需分配：bufferSize/bufferCount 原样可查
     */
    TEST(BufferPool, ConstructionReportsConfiguredProperties)
    {
        const BufferPool pool(1024, 8);

        EXPECT_EQ(pool.bufferSize(), 1024u);
        EXPECT_EQ(pool.bufferCount(), 8u);
    }

    /**
     * @brief 连续 acquire() 交出互不相同的有效索引：池容量内绝不重复发同一块
     */
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

    /**
     * @brief 缓冲区耗尽后返回 -1 哨兵而不是 0——0 是合法索引，两者混同会让调用方把失败当成功
     */
    TEST(BufferPool, AcquireReturnsMinusOneWhenExhausted)
    {
        BufferPool pool(64, 2);

        EXPECT_GE(pool.acquire(), 0);
        EXPECT_GE(pool.acquire(), 0);
        // 缓冲区耗尽后应返回 -1
        EXPECT_EQ(pool.acquire(), -1);
    }

    /**
     * @brief 释放后的索引回到空闲栈并可被再次 acquire()，容量在归还后恢复
     */
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

    /**
     * @brief 验证重复释放不会让同一块缓冲被交给两个使用者
     *
     * @details 分配器若把重复释放照旧压栈，同一索引会在空闲栈里出现两次，
     *          之后两次 acquire() 会拿到同一个索引——两个使用者互相覆写同一块内存，
     *          而且不报任何错。release() 因此必须是幂等的。
     */
    TEST(BufferPool, RepeatedReleaseDoesNotAliasTheSameBuffer)
    {
        BufferPool pool(64, 2);

        const int firstIndex  = pool.acquire();
        const int secondIndex = pool.acquire();
        ASSERT_GE(firstIndex, 0);
        ASSERT_GE(secondIndex, 0);
        EXPECT_NE(firstIndex, secondIndex);

        // 同一个索引连释放三次，只能让它回到空闲栈一次
        pool.release(firstIndex);
        pool.release(firstIndex);
        pool.release(firstIndex);

        const int reacquired = pool.acquire();
        EXPECT_GE(reacquired, 0);
        // 池已满（两个索引都被占用），再次 acquire 必须是「没有可用缓冲」
        EXPECT_EQ(pool.acquire(), -1) << "重复释放让池凭空多出了缓冲，说明同一索引被压栈两次";
    }

    /**
     * @brief 验证释放「从未取出的索引」不会凭空多出可用缓冲
     */
    TEST(BufferPool, ReleaseOfInvalidOrUnacquiredIndexIsIgnored)
    {
        BufferPool pool(64, 2);

        // 只取出一个：另一个索引仍未被占用
        const int acquiredIndex  = pool.acquire();
        const int untouchedIndex = 1 - acquiredIndex; // 两个缓冲里没被取出的那个
        ASSERT_GE(acquiredIndex, 0);

        // 越界索引，以及那个「从没被取出」的索引，都不应改变可用缓冲数
        pool.release(-1);
        pool.release(2);
        pool.release(untouchedIndex);

        const int lastAvailable = pool.acquire();
        EXPECT_GE(lastAvailable, 0) << "池里应仍剩一个未取出的缓冲";
        EXPECT_NE(lastAvailable, acquiredIndex) << "不该把已占用的缓冲再发一次";
        EXPECT_EQ(pool.acquire(), -1) << "对未占用索引调用 release() 不应让池凭空多出缓冲";
    }

    /**
     * @brief data() 交出的是整块可写内存：容量即 bufferSize，全块写入不越界
     */
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

    /**
     * @brief 各缓冲区在内存上互不重叠：写满一块不会改动另一块的内容
     */
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

    /**
     * @brief 边界用例：bufferSize 为 0 仍可构造并按个数发索引，单个参数极端不使整个池不可用
     */
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
