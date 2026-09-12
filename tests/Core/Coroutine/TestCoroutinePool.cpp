/**
 * @file TestCoroutinePool.cpp
 * @brief CoroutinePool 单元测试：进程级单例、块内分配回收、大块回退与跨线程回收
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Coroutine/CoroutinePool.h"

#include <gtest/gtest.h>

#include <cstring>
#include <latch>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace AsynGyanis::Core
{
    TEST(CoroutinePool, InstanceReturnsProcessWideSingleton)
    {
        CoroutinePool &first  = CoroutinePool::instance();
        CoroutinePool &second = CoroutinePool::instance();

        EXPECT_EQ(&first, &second);

        // 工作线程取到的必须是同一个池：协程帧会跨线程换手，池按线程拆分即无法回收
        CoroutinePool *fromWorker = nullptr;
        std::thread    worker(
                [&fromWorker]
                {
                    fromWorker = &CoroutinePool::instance();
                });
        worker.join();
        EXPECT_EQ(fromWorker, &first);
    }

    TEST(CoroutinePool, AllocateWithinBlockSizeRoundTrips)
    {
        auto &pool = CoroutinePool::instance();

        void *pointer = pool.allocate(128);
        ASSERT_NE(pointer, nullptr);

        // 用特定模式填充以验证内存可写
        std::memset(pointer, 0xCD, 128);

        pool.deallocate(pointer, 128);

        // 释放后应能再次分配成功
        void *reallocated = pool.allocate(128);
        ASSERT_NE(reallocated, nullptr);
        pool.deallocate(reallocated, 128);
    }

    TEST(CoroutinePool, AllocateLargerThanBlockSizeFallsBackToGlobalNew)
    {
        auto &pool = CoroutinePool::instance();

        // 超过块大小的请求应回退到全局 ::operator new 且仍可用
        const size_t largeSize = pool.blockSize() * 2;
        void *pointer = pool.allocate(largeSize);
        ASSERT_NE(pointer, nullptr);

        std::memset(pointer, 0xEF, largeSize);
        pool.deallocate(pointer, largeSize);
    }

    TEST(CoroutinePool, MultipleAllocationsAndDeallocationsSucceed)
    {
        auto &pool = CoroutinePool::instance();
        std::vector<void *> pointers;

        for (int round = 0; round < 100; ++round)
        {
            void *pointer = pool.allocate(64);
            ASSERT_NE(pointer, nullptr);
            pointers.push_back(pointer);
        }

        for (void *pointer: pointers)
        {
            pool.deallocate(pointer, 64);
        }
    }

    TEST(CoroutinePool, BlockSizeMeetsMinimumUsableSize)
    {
        // 进程级单例按默认 256 字节块构造，至少应容纳典型协程帧
        auto &pool = CoroutinePool::instance();

        EXPECT_GE(pool.blockSize(), 64u);
    }

    TEST(CoroutinePool, CrossThreadDeallocationReturnsBlockToSamePool)
    {
        auto &pool = CoroutinePool::instance();

        void *const pointer = pool.allocate(96);
        ASSERT_NE(pointer, nullptr);
        EXPECT_TRUE(pool.owns(pointer));

        // 在另一个线程回收：块必须回到同一个池的空闲列表，而不是被交给全局 ::operator delete
        std::thread worker(
                [&pool, pointer]
                {
                    pool.deallocate(pointer, 96);
                });
        worker.join();

        // 回收后必须能被重新分配，且拿回的还是那块池内存
        void *const reallocated = pool.allocate(96);
        EXPECT_EQ(reallocated, pointer);
        pool.deallocate(reallocated, 96);
    }

    TEST(CoroutinePool, ConcurrentAllocationHandsOutDistinctBlocks)
    {
        auto &pool = CoroutinePool::instance();

        constexpr int    threadCount          = 4;
        constexpr int    allocationsPerThread = 200;

        std::vector<void *> collected;
        collected.reserve(threadCount * allocationsPerThread);
        std::mutex           collectedMutex;
        std::latch           allAllocated(threadCount);
        std::vector<std::thread> workers;

        for (int workerIndex = 0; workerIndex < threadCount; ++workerIndex)
        {
            workers.emplace_back(
                    [&pool, &collected, &collectedMutex, &allAllocated]
                    {
                        std::vector<void *> localPointers;
                        localPointers.reserve(allocationsPerThread);
                        for (int round = 0; round < allocationsPerThread; ++round)
                        {
                            localPointers.push_back(pool.allocate(48));
                        }
                        for (void *pointer: localPointers)
                        {
                            std::memset(pointer, 0x5A, 48);
                        }
                        {
                            const std::lock_guard lock(collectedMutex);
                            collected.insert(collected.end(), localPointers.begin(), localPointers.end());
                        }

                        // 等所有线程都完成分配后再归还：只有同时持有的块才不允许重复
                        allAllocated.arrive_and_wait();
                        for (void *pointer: localPointers)
                        {
                            pool.deallocate(pointer, 48);
                        }
                    });
        }
        for (auto &worker: workers)
        {
            worker.join();
        }

        const std::unordered_set<void *> uniquePointers(collected.begin(), collected.end());
        EXPECT_EQ(uniquePointers.size(), collected.size());
    }

    TEST(CoroutinePool, AllocationBeyondBlockCeilingFallsBackToGlobalHeap)
    {
        auto &pool = CoroutinePool::instance();

        // 池的块数上限是私有常量，这里不硬编码它：一路分配到出现「不属于本池」的块为止。
        // 修复前这一步会崩——空闲列表为空且 expand() 因到达上限而一块都不加时，
        // 旧实现仍对它调 back()，属于对空 vector 的越界访问
        std::vector<void *> blocks;
        void              *firstForeignBlock = nullptr;
        constexpr size_t   kAllocationAttemptCeiling = 100000;

        for (size_t attempt = 0; attempt < kAllocationAttemptCeiling && firstForeignBlock == nullptr; ++attempt)
        {
            void *block = pool.allocate(64);
            ASSERT_NE(block, nullptr) << "第 " << attempt << " 次分配失败：池到达上限后必须改走全局堆";
            blocks.push_back(block);

            if (!pool.owns(block))
            {
                firstForeignBlock = block;
            }
        }

        ASSERT_NE(firstForeignBlock, nullptr)
            << "在 " << kAllocationAttemptCeiling << " 次分配内没有观察到池上限：上限常量是否被调大了？";

        // 越过上限的块由全局堆承载，且 released 时也必须走全局堆释放路径
        // （deallocate 靠 ownsUnlocked() 判定归属，因此判定分支与 allocate 天然一致）
        for (void *block: blocks)
        {
            pool.deallocate(block, 64);
        }

        // 归还后池内块是可以复用的：再分配一次应当回到池里，而不是每次都新建
        void *reused = pool.allocate(64);
        EXPECT_TRUE(pool.owns(reused));
        pool.deallocate(reused, 64);
    }
} // namespace AsynGyanis::Core
