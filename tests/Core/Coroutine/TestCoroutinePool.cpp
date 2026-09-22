// CoroutinePool 单元测试：进程级单例、块内分配回收、大块回退与跨线程回收

#include "Core/Coroutine/CoroutinePool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <latch>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief instance() 是进程级单例：另一个线程取到的也是同一个池（协程帧跨线程换手，按线程拆池就无法回收）
     */
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

    /**
     * @brief 块大小以内的分配整块可写、可归还，且释放后同一请求还能再分配成功（帧内存可复用）
     */
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

    /**
     * @brief 小档装不下的帧由大档接手：仍然整段可写，且指针归属本池而不是一次性全局堆分配
     *
     * @details 池按帧大小分两档：小档 256 B 装得下的小帧不浪费，大档 2048 B 接住框架里
     *          路由、会话那类 1.2–2.2 KB 的帧。实测过：不分档时每请求有 5–7 个帧落到全局堆，
     *          占每请求分配字节数的大头。
     */
    TEST(CoroutinePool, ServesOversizedFramesFromTheLargeTier)
    {
        auto &pool = CoroutinePool::instance();

        // 两倍小档：小档装不下，应当由大档接手
        const size_t oversized = pool.blockSize() * 2;
        void        *pointer   = pool.allocate(oversized);
        ASSERT_NE(pointer, nullptr);
        EXPECT_TRUE(pool.owns(pointer)) << "大档接手的帧仍应属于本池，否则每请求都要向全局分配器要内存";

        std::memset(pointer, 0xEF, oversized);
        pool.deallocate(pointer, oversized);

        // 归还之后同一档应当能再取到同一块（缓存复用），仍然可写
        void *reused = pool.allocate(oversized);
        ASSERT_NE(reused, nullptr);
        EXPECT_TRUE(pool.owns(reused));
        std::memset(reused, 0xEF, oversized);
        pool.deallocate(reused, oversized);
    }

    /**
     * @brief 两档都装不下的请求回退到全局堆：仍然整段可写，归还也不报错（超大帧不因池的规格而不可用）
     */
    TEST(CoroutinePool, AllocateAboveEveryTierFallsBackToGlobalNew)
    {
        auto &pool = CoroutinePool::instance();

        // 十六倍小档（默认 4 KiB）：超过大档规格，只能向全局分配器要
        const size_t hugeSize = pool.blockSize() * 16;
        void        *pointer  = pool.allocate(hugeSize);
        ASSERT_NE(pointer, nullptr);
        EXPECT_FALSE(pool.owns(pointer)) << "超过最大档的帧应当来自全局堆";

        std::memset(pointer, 0xEF, hugeSize);
        pool.deallocate(pointer, hugeSize);
    }

    /**
     * @brief 连续 100 次分配后再全部归还不会失败：覆盖跨块扩展与批量回收路径
     */
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

    /**
     * @brief 进程级单例的块大小下界：至少 64 字节，保证放得下典型协程帧（不以 0 或极小值配置）
     */
    TEST(CoroutinePool, BlockSizeMeetsMinimumUsableSize)
    {
        // 进程级单例按默认 256 字节块构造，至少应容纳典型协程帧
        auto &pool = CoroutinePool::instance();

        EXPECT_GE(pool.blockSize(), 64u);
    }

    /**
     * @brief 跨线程归还的池内块必须回到池里（而非被当成外来指针交给全局 ::operator delete）：再分配能拿回同一地址
     *
     * @details 归还进的是**归还方所在线程**的本地缓存（缓存与块属于哪个线程无关），
     *          该线程退出时缓存会被整体归还全局池，因此主线程随后能拿到同一地址。
     *          若缓存没有在线程退出时归还，这个块就永久滞留在已结束的线程上，本用例会失败。
     */
    TEST(CoroutinePool, CrossThreadDeallocationReturnsBlockToSamePool)
    {
        auto &pool = CoroutinePool::instance();

        void *const pointer = pool.allocate(96);
        ASSERT_NE(pointer, nullptr);
        EXPECT_TRUE(pool.owns(pointer));

        // 在另一个线程回收：块必须回到池里，而不是被交给全局 ::operator delete。
        // 该线程只归还这一块、随即退出，退出时把缓存整体交还全局池
        std::thread worker(
                [&pool, pointer]
                {
                    pool.deallocate(pointer, 96);
                });
        worker.join();

        // 换一条**新线程**来重新分配：它的本地缓存是空的，因此必然从全局池取块。
        // 归还线程把那一块最后压入，它正在全局链表的头部，必然落进第一批搬运里；
        // 但「第一批里具体第几个被发出」取决于搬运时的头插顺序，所以断言写成
        // 「取一批后这一块在其中」，而不是断言第一次分配就拿回它
        constexpr size_t    kProbeCount = 128;
        std::vector<void *> probedPointers;
        probedPointers.reserve(kProbeCount);
        std::thread verifier(
                [&pool, &probedPointers]
                {
                    for (size_t probeIndex = 0; probeIndex < kProbeCount; ++probeIndex)
                    {
                        probedPointers.push_back(pool.allocate(96));
                    }
                });
        verifier.join();

        EXPECT_NE(std::find(probedPointers.begin(), probedPointers.end(), pointer), probedPointers.end())
            << "跨线程归还的池内块没有回到池里：可能被当成外来指针交给了全局 ::operator delete，"
               "或者在退出线程的缓存里被永久滞留";
        EXPECT_TRUE(pool.owns(pointer));

        // 归还探针取走的块：其中包含最初那一块
        for (void *probedPointer: probedPointers)
        {
            pool.deallocate(probedPointer, 96);
        }
    }

    /**
     * @brief 并发分配不得重发同一块：四线程同时持有的所有块地址互不相同（用 latch 保证「同时持有」后再归还）
     */
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

    /**
     * @brief 验证等量复用不触发扩容：一批块释放后再申请同样多，池不再向系统要内存
     *
     * @details 判据是 allocatedCount() 不变。先申请一大批并持有，把单例中已累积的空闲块
     *          一并消耗掉，于是第二轮的供给只能来自第一轮释放的块——任何一块在归还路径上
     *          丢失，第二轮都会因缺块而扩容、被这条断言抓住。
     *          协程帧的分配与释放正走在这条路径上（每次 I/O 都会新建并销毁一个帧）。
     */
    TEST(CoroutinePool, SteadyStateReuseDoesNotGrowThePool)
    {
        auto &pool = CoroutinePool::instance();

        constexpr size_t kBlockCount = 300;
        std::vector<void *> pointers;
        pointers.reserve(kBlockCount);

        // 第一轮：持有全部块，顺手把单例里既有的空闲块也一并占用
        for (size_t blockIndex = 0; blockIndex < kBlockCount; ++blockIndex)
        {
            pointers.push_back(pool.allocate(128));
        }
        for (void *pointer: pointers)
        {
            pool.deallocate(pointer, 128);
        }

        const size_t allocatedAfterFirstRound = pool.allocatedCount();

        // 第二轮等量申请：供给来自上一轮释放的块，池不该再向系统要内存
        pointers.clear();
        for (size_t blockIndex = 0; blockIndex < kBlockCount; ++blockIndex)
        {
            pointers.push_back(pool.allocate(128));
        }
        EXPECT_EQ(pool.allocatedCount(), allocatedAfterFirstRound)
            << "第二轮分配触发了扩容：说明有块在归还路径上丢失了";

        for (void *pointer: pointers)
        {
            pool.deallocate(pointer, 128);
        }
    }

    /**
     * @brief 验证同一线程内释放后再申请拿回同一地址（后进先出），这是空闲块复用的基本契约
     *
     * @details 本地缓存与全局空闲链表都是栈式结构，先释放的最后被取出；
     *          协程帧的「建了就用、用完就还」正依赖这条顺序拿到还在缓存里的热块。
     */
    TEST(CoroutinePool, AllocateAfterDeallocateReturnsTheSameBlockWithinAThread)
    {
        auto &pool = CoroutinePool::instance();

        void *const first  = pool.allocate(144);
        void *const second = pool.allocate(144);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_NE(first, second);

        // 后释放的先被取回
        pool.deallocate(first, 144);
        pool.deallocate(second, 144);

        EXPECT_EQ(pool.allocate(144), second);
        EXPECT_EQ(pool.allocate(144), first);

        pool.deallocate(second, 144);
        pool.deallocate(first, 144);
    }

    /**
     * @brief 一档的扩容步长只跟着本档历史走，不被另一档的高水位带大
     * @details 两档块规格差 8 倍，步长一旦按「两档合计的已切分块数」算，被小帧跑热过的池第一次
     *          碰到大帧就要一次切出与小帧同数的大块，既是一记长缺页停顿也吃满共享的块数预算。
     *          判据取本档相邻两次扩容的比值，因此共享单例先前喂过多少块都不影响结论
     */
    TEST(CoroutinePool, TierExpansionStepFollowsItsOwnHistoryNotTheOtherTiers)
    {
        auto &pool = CoroutinePool::instance();

        // 大档按自己的需求长到这一步：步长参照值从这里取
        constexpr size_t kLargeWarmupBlockCount        = 200;
        // 小档的高水位：合计口径的步长会被它带大，因此要明显大于大档的历史
        constexpr size_t kSmallHighWaterBlockCount     = 4000;
        // 逼出大档下一次扩容的尝试上限：留出足够次数，不让用例因为「没等到扩容」而空转
        constexpr size_t kLargeDemandAfterSmallGrowth  = 2000;

        const size_t smallRequestBytes = 48;                   // 落在小档
        const size_t largeRequestBytes = pool.blockSize() * 2; // 超过小档规格，因此必定落在大档
        ASSERT_GT(largeRequestBytes, pool.blockSize()) << "挑不到大档，本用例等于没测";

        std::vector<void *> heldLarge;

        // 第一段：让大档按自己的需求长起来，记下最近一次扩容的步长作为参照
        size_t previousLargeStep = 0;
        size_t previousCount     = pool.allocatedCount();
        for (size_t blockIndex = 0; blockIndex < kLargeWarmupBlockCount; ++blockIndex)
        {
            void *const memory = pool.allocate(largeRequestBytes);
            ASSERT_NE(memory, nullptr);
            ASSERT_TRUE(pool.owns(memory)) << "大档的块不是从池里来的：块数上限可能已被同进程的其它用例顶满";
            heldLarge.push_back(memory);

            const size_t currentCount = pool.allocatedCount();
            if (currentCount > previousCount)
            {
                previousLargeStep = currentCount - previousCount;
            }
            previousCount = currentCount;
        }
        ASSERT_GT(previousLargeStep, 0U) << "大档一次都没扩容，参照值没有意义";

        // 第二段：把小档长到高水位并全部持有，使「两档合计」远大于大档自己的历史
        std::vector<void *> heldSmall;
        heldSmall.reserve(kSmallHighWaterBlockCount);
        previousCount = pool.allocatedCount();
        for (size_t blockIndex = 0; blockIndex < kSmallHighWaterBlockCount; ++blockIndex)
        {
            void *const memory = pool.allocate(smallRequestBytes);
            ASSERT_NE(memory, nullptr);
            heldSmall.push_back(memory);
        }
        const size_t smallGrownBlocks = pool.allocatedCount() - previousCount;
        ASSERT_GE(smallGrownBlocks, kSmallHighWaterBlockCount / 2)
            << "小档没能长起来：块数上限已被同进程的其它用例顶满，后面的比值断言会是空转";

        // 第三段：继续向大档要块，取小档长高之后的第一次扩容步长。
        // 只看第一次：翻倍是本档的既定行为，第二次起本来就比第一次大
        size_t firstStepAfterSmallGrowth = 0;
        previousCount                    = pool.allocatedCount();
        for (size_t blockIndex = 0; blockIndex < kLargeDemandAfterSmallGrowth && firstStepAfterSmallGrowth == 0;
             ++blockIndex)
        {
            void *const memory = pool.allocate(largeRequestBytes);
            ASSERT_NE(memory, nullptr);
            heldLarge.push_back(memory);

            const size_t currentCount = pool.allocatedCount();
            if (currentCount > previousCount)
            {
                firstStepAfterSmallGrowth = currentCount - previousCount;
            }
            previousCount = currentCount;
        }
        ASSERT_GT(firstStepAfterSmallGrowth, 0U) << "小档长高之后大档再没扩容，比值断言是空转";

        // 本档翻倍最多让下一步等于上一步的两倍（实测比值 2），取 3 留出实现余量；
        // 按「两档合计」算时这一步会跳到小档的高水位（实测比值 4~32）
        EXPECT_LE(firstStepAfterSmallGrowth, previousLargeStep * 3)
            << "大档的扩容步长被小档的高水位带大了：这一步切了 " << firstStepAfterSmallGrowth
            << " 块大块，而大档上一步只有 " << previousLargeStep << " 块、小档本轮长到 "
            << smallGrownBlocks << " 块";

        // 单次扩容不该一口吃下池内块数的四分之一：这是「提前吃满共享预算」的直接判据，
        // 与两档各自的规模无关
        const size_t blocksBeforeLastExpansion = previousCount - firstStepAfterSmallGrowth;
        EXPECT_LE(firstStepAfterSmallGrowth, blocksBeforeLastExpansion / 4)
            << "大档一次扩容要了 " << firstStepAfterSmallGrowth << " 块，而当时池内总共只有 "
            << blocksBeforeLastExpansion << " 块";

        for (void *const memory: heldSmall)
        {
            pool.deallocate(memory, smallRequestBytes);
        }
        for (void *const memory: heldLarge)
        {
            pool.deallocate(memory, largeRequestBytes);
        }
    }

    /**
     * @brief 池到达块数上限后不崩溃也不静默失败：改由全局堆承担且 owns() 返回 false；归还后池内块仍可复用
     * @details 池的块数上限是私有常量，用例不硬编码它，而是一路分配到出现「不属于本池」的块为止。
     *          本用例会把共享单例顶到上限，因此排在本文件最后：直接跑整个可执行体时，排在它后面的
     *          用例再也扩不出容（CTest 给每条用例起独立进程，顺序不影响各自的结论）
     */
    TEST(CoroutinePool, AllocationBeyondBlockCeilingFallsBackToGlobalHeap)
    {
        auto &pool = CoroutinePool::instance();

        // 池的块数上限是私有常量，这里不硬编码它：一路分配到出现「不属于本池」的块为止
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

        // 越过上限的块由全局堆承载，且归还时也必须走全局堆释放路径
        // （deallocate 靠 isOwnedBlock() 判定归属，因此判定分支与 allocate 天然一致）
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
