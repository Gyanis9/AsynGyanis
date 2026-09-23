// ConnectionDistributor 单元测试：轮转派发、回调落在目标循环线程上、无可派人选时不接管描述符
//
// 末尾的 HandoffAllocationProfile 是派发这条路的分配台账（共用 AllocationProbe）：
//   · 一千次「派发 + 取走」：MSVC 2002 块 / 112128 字节，libstdc++ 2062 块 / 103744 字节，
//     即每交一条连接两块（交接句柄一块、投进队列的回调载荷一块）；
//   · 对照组（只造一个描述符再关掉，完全不碰派发）实测 0 块，所以上面两块都归派发本身，
//     不是造描述符的开销冒充的；
//   · 已试过并**被读数否决**的优化：把接手动作改成登记时共享持有、闭包只捕获一个指针——
//     一千次仍是 2062 块（字节数从 103744 降到 87744），也就是说第二块并不是「复制 Adopter」
//     带来的，改法只是把那块换小了一点；
//   · 要降到一块得给 Scheduler 加一条「载荷指针 + 平凡函数指针」的投递通道（std::function
//     一旦捕获非平凡可复制的东西就必然进堆）。收益是每连接省一次分配，相对 accept 自身的
//     微秒级开销不划算 —— 记为刻意不做，别再为它动结构。
//
// 用例跑真实的 EventLoop（各占一个线程）与真实的套接字描述符：跨循环这件事的坑
// （唤醒丢失、描述符归属错）只有在真循环上才暴露得出来。

#include "Core/EventLoop/ConnectionDistributor.h"
#include "Core/EventLoop/EventLoop.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"

#include "CoreTestSupport.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <fcntl.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::waitForCondition;

        /**
         * @brief 造一个未连接的 TCP 描述符
         * @details 分发器只负责把描述符交出去，不在这条连接上做 I/O，因此未连接套接字足够；
         *          它是有效的系统描述符，能覆盖「谁负责关闭」这条契约。
         * @return int 描述符；无效时返回无效值
         */
        int makeDetachedSocketDescriptor()
        {
            return static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        }

        /**
         * @brief 一条被接手记录
         */
        struct HandoffRecord
        {
            int             fileDescriptor{Platform::FileDescriptor::kInvalid}; ///< 接手的描述符
            std::thread::id handledOn{};                                       ///< 接手动作跑在哪个线程上
        };

        /**
         * @brief 等工作循环攒够指定条数的接手记录
         * @param mutex 保护记录的互斥锁
         * @param records 接手记录
         * @param expectedCount 期望条数
         * @return true 时限内攒够
         */
        bool waitForHandoffCount(std::mutex &mutex, const std::vector<HandoffRecord> &records, const std::size_t expectedCount)
        {
            return waitForCondition(
                    [&mutex, &records, expectedCount]
                    {
                        const std::lock_guard lock(mutex);
                        return records.size() >= expectedCount;
                    });
        }
    } // namespace

    /**
     * @brief 轮转派发：连派四条，两条落在 A、两条落在 B，且每条都跑在对应循环的线程上
     */
    TEST(ConnectionDistributorTest, DistributesRoundRobinAndRunsOnTargetLoopThread)
    {
        EventLoop workerA;
        EventLoop workerB;
        std::thread threadA([&workerA]()
        {
            workerA.run();
        });
        std::thread threadB([&workerB]()
        {
            workerB.run();
        });

        std::mutex                     recordsMutex;
        std::vector<HandoffRecord>     records;
        std::vector<std::thread::id>   workerThreadIds;

        const auto recordHandoff = [&recordsMutex, &records](const int fileDescriptor)
        {
            {
                const std::lock_guard lock(recordsMutex);
                records.push_back(HandoffRecord{fileDescriptor, std::this_thread::get_id()});
            }
            // 接手方负责关闭：契约是「distribute() 返回 true 即所有权转移」
            Platform::FileDescriptor::close(fileDescriptor);
        };

        ConnectionDistributor distributor;
        distributor.addWorker(workerA, recordHandoff);
        distributor.addWorker(workerB, recordHandoff);
        ASSERT_EQ(distributor.workerCount(), 2U);

        // 本线程扮演接受循环：它不属于任何工作循环，因此回调跑在本线程上就说明没跨过去
        constexpr std::size_t kConnectionCount = 4;

        for (std::size_t index = 0; index < kConnectionCount; ++index)
        {
            ASSERT_TRUE(distributor.distribute(makeDetachedSocketDescriptor())) << "第 " << index << " 条连接没有派出去";
        }
        EXPECT_EQ(distributor.distributedCount(), kConnectionCount);

        ASSERT_TRUE(waitForHandoffCount(recordsMutex, records, kConnectionCount)) << "接手回调没有在时限内全部执行";

        {
            const std::lock_guard lock(recordsMutex);
            ASSERT_EQ(records.size(), kConnectionCount);
            for (const HandoffRecord &record : records)
            {
                EXPECT_NE(record.handledOn, std::this_thread::get_id()) << "回调跑在了派发线程上，连接没有真的换循环";
                EXPECT_NE(record.fileDescriptor, Platform::FileDescriptor::kInvalid);
            }
        }

        workerA.stop();
        workerB.stop();
        threadA.join();
        threadB.join();
    }

    /**
     * @brief 没有登记工作循环时，描述符不被接管：调用方保留所有权
     */
    TEST(ConnectionDistributorTest, RejectsDispatchWithoutWorkers)
    {
        ConnectionDistributor distributor;
        EXPECT_EQ(distributor.workerCount(), 0U);

        const int fileDescriptor = makeDetachedSocketDescriptor();
        EXPECT_FALSE(distributor.distribute(fileDescriptor)) << "没有工作循环却宣称接管了描述符";
        EXPECT_EQ(distributor.distributedCount(), 0U);

        // 描述符仍归调用方，必须由调用方关闭（若分发器偷偷接管，这里就是双重关闭）
        Platform::FileDescriptor::close(fileDescriptor);
    }

    /**
     * @brief 无效描述符不被接管：避免把 -1 当成一条连接投出去
     */
    TEST(ConnectionDistributorTest, RejectsInvalidDescriptor)
    {
        EventLoop worker;
        std::thread workerThread([&worker]()
        {
            worker.run();
        });

        ConnectionDistributor distributor;
        distributor.addWorker(worker, [](int)
        {
        });

        EXPECT_FALSE(distributor.distribute(Platform::FileDescriptor::kInvalid));
        EXPECT_EQ(distributor.distributedCount(), 0U);

        worker.stop();
        workerThread.join();
    }

    /**
     * @brief 空接手动作被忽略：不会留下一个会在目标线程上抛异常的工作循环
     */
    TEST(ConnectionDistributorTest, IgnoresWorkerWithoutAdopter)
    {
        EventLoop           worker;
        ConnectionDistributor distributor;
        distributor.addWorker(worker, {});

        EXPECT_EQ(distributor.workerCount(), 0U);
        EXPECT_FALSE(distributor.distribute(makeDetachedSocketDescriptor()));
    }

    namespace
    {
        /**
         * @brief 描述符是否还开着
         * @details 未连接的 TCP 套接字问名字报「参数无效」，已被关掉的报「不是套接字」，
         *          两者足以区分「开着但没绑定」与「已经关了」；POSIX 上直接问 F_GETFD
         * @return true 仍开着
         */
        bool isDescriptorStillOpen(const int fileDescriptor)
        {
#if ASYN_PLATFORM_WIN32
            sockaddr_storage name{};
            socklen_t        nameLength = sizeof(name);
            if (::getsockname(fileDescriptor, reinterpret_cast<sockaddr *>(&name), &nameLength) != SOCKET_ERROR)
            {
                return true;
            }
            return ::WSAGetLastError() != WSAENOTSOCK;
#else
            return ::fcntl(fileDescriptor, F_GETFD) != -1;
#endif
        }
    } // namespace

    /**
     * @brief 接手动作抛出时描述符当场被关闭：不留「已交出却没人管」的野描述符
     * @details 契约是「句柄一旦建成所有权就算交出，distribute() 一律返回 true」，调用方因此
     *          不会再关它。投递没被执行时由交接句柄的析构兜住，但「执行了、adopter 抛出」
     *          原先没人负责：take() 已把所有权摘走，抛出来就是每失败一次漏一个句柄——
     *          连接风暴叠加会话构造失败会把进程的文件描述符配额耗光。
     *          抛出会穿过批次处理继续把那条工作循环停掉，这是既定的快速失败口径，
     *          本用例只钉「句柄被关掉」这一条副作用
     */
    TEST(ConnectionDistributorTest, ThrowingAdopterDoesNotLeakTheDescriptor)
    {
        TestSupport::EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        std::atomic<bool> isAdopterEntered{false};
        ConnectionDistributor distributor;
        distributor.addWorker(runner.loop(), [&isAdopterEntered](int)
        {
            isAdopterEntered.store(true, std::memory_order_release);
            throw std::runtime_error("用例设定的接手失败");
        });

        const int fileDescriptor = makeDetachedSocketDescriptor();
        ASSERT_NE(fileDescriptor, static_cast<int>(Platform::FileDescriptor::kInvalid));
        EXPECT_TRUE(distributor.distribute(fileDescriptor)) << "句柄已建成，按契约应当报「已接管」";

        ASSERT_TRUE(waitForCondition([&isAdopterEntered]
                                    {
                                        return isAdopterEntered.load(std::memory_order_acquire);
                                    }))
                << "接手动作根本没跑起来，这条路径没被走到";
        const bool isClosedEventually = waitForCondition(
                [fileDescriptor]
                {
                    return !isDescriptorStillOpen(fileDescriptor);
                });
        EXPECT_TRUE(isClosedEventually)
                << "adopter 抛出后描述符 " << fileDescriptor << " 仍开着：每次接手失败都会漏一个句柄";

        if (isDescriptorStillOpen(fileDescriptor))
        {
            Platform::FileDescriptor::close(fileDescriptor);
        }
        runner.join();
    }

    /**
     * @brief 目标循环先退出、投递从未被执行时，描述符由交接句柄关回去
     * @details 交接句柄（HandoffDescriptor）存在的唯一理由就是这一条：投出去的回调可能永远没人取，
     *          而调用方按契约已经交出了所有权、不会再关它。少这一步，每丢一条待接手的连接就漏一个
     *          描述符，连接风暴会把进程的文件描述符配额耗光。
     *          用例刻意不起后台线程：循环从未 run()，队列里的投递只能随调度器析构一起被丢弃，
     *          「没被执行」因此是构造出来的确定状态，而不是撞上时序运气。
     * @note 同时钉住 distributedCount() 的口径：它记的是「已交出所有权」，不是「已被接手」——
     *       这一条连接没人接手，计数仍然加它
     */
    TEST(ConnectionDistributorTest, DiscardedHandoffClosesTheDescriptorWhenTheLoopStopsFirst)
    {
        const int fileDescriptor = makeDetachedSocketDescriptor();
        ASSERT_NE(fileDescriptor, static_cast<int>(Platform::FileDescriptor::kInvalid));

        std::atomic<bool> isAdopterEntered{false};
        std::size_t       distributedCountBeforeScopeEnd = 0;
        {
            EventLoop             loop; // 不启动：没有人会来取这条投递
            ConnectionDistributor distributor;
            distributor.addWorker(loop, [&isAdopterEntered](int)
            {
                isAdopterEntered.store(true, std::memory_order_release);
            });

            ASSERT_TRUE(distributor.distribute(fileDescriptor)) << "句柄已建成，按契约应当报「已接管」";
            distributedCountBeforeScopeEnd = distributor.distributedCount();
            EXPECT_TRUE(isDescriptorStillOpen(fileDescriptor))
                    << "投递还没被执行就把描述符关掉，等于掐掉一条本可以接手成功的连接";

            // 出作用域：distributor 先销毁（投递自带 adopter 副本，不依赖它），随后 loop 销毁
            // → 调度器成员析构 → 队列里那条投递连同交接句柄一起被丢掉
        }

        EXPECT_EQ(distributedCountBeforeScopeEnd, 1U) << "计数应当把这条「已交出所有权」的连接算进去";
        EXPECT_FALSE(isAdopterEntered.load(std::memory_order_acquire)) << "循环没跑过，接手动作不该执行";
        EXPECT_FALSE(isDescriptorStillOpen(fileDescriptor))
                << "目标循环退出后没被执行的那条投递没有把描述符关回去：每丢一条就漏一个句柄";
    }

    /**
     * @brief 派发一条连接的分配画像：交接句柄与回调载荷各自一块堆
     * @details 量的是「投一条 + 那条被取走」这一整趟，跑在单线程上（循环不起线程，由本用例
     *          直接 runOne() 取用），否则读数里会混进后台线程自己的动作。判据只在 Release 下钉，
     *          Debug 仍跑同样的形状并打出直方图供对照（口径同 tests/Net/Http 那套台账）
     */
    TEST(ConnectionDistributor, HandoffAllocationProfile)
    {
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

        EventLoop             loop;
        ConnectionDistributor distributor;
        std::atomic<int>      handledCount{0};
        distributor.addWorker(loop,
                              [&handledCount](const int fileDescriptor)
                              {
                                  Platform::FileDescriptor::close(fileDescriptor);
                                  handledCount.fetch_add(1, std::memory_order_relaxed);
                              });

        AsynGyanis::TestSupport::resetAllocationHistogram();
        const auto profile = measurePerOperation(
                [&]
                {
                    const int fileDescriptor = makeDetachedSocketDescriptor();
                    if (!distributor.distribute(fileDescriptor))
                    {
                        Platform::FileDescriptor::close(fileDescriptor);
                        return 0U;
                    }
                    return loop.scheduler().runOne() ? 1U : 0U;
                });

        // 直方图要在对照组之前取：对照窗口会把它清零，两个窗口共用一份快照就打不出归属了
        const auto handoffHistogram = AsynGyanis::TestSupport::snapshotAllocationHistogram();

        // 对照组：只造一个描述符再关掉，完全不碰派发链路。造描述符这一步在被测形状里也在做，
        // 不把库自己的分配减出去，就会记到交接的头上
        AsynGyanis::TestSupport::resetAllocationHistogram();
        const auto baseline = measurePerOperation(
                []
                {
                    const int fileDescriptor = makeDetachedSocketDescriptor();
                    Platform::FileDescriptor::close(fileDescriptor);
                    return 1U;
                });
        const auto baselineHistogram = AsynGyanis::TestSupport::snapshotAllocationHistogram();

        std::printf("distributor-handoff total=%llu bytes=%llu\n", static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        std::printf("distributor-baseline total=%llu bytes=%llu\n", static_cast<unsigned long long>(baseline.totalAllocations),
                    static_cast<unsigned long long>(baseline.totalBytes));
        for (std::size_t bucket = 0; bucket < handoffHistogram.size(); ++bucket)
        {
            if (handoffHistogram[bucket] != 0)
            {
                std::printf("   handoff bucket=%zu bytes=%zu count=%llu\n", bucket,
                            bucket * AsynGyanis::TestSupport::kAllocationHistogramBucketBytes,
                            static_cast<unsigned long long>(handoffHistogram[bucket]));
            }
        }
        // 对照组的分布正常情况下一个桶都不该亮；亮了就说明造描述符自己也在碰堆，
        // 上面那句「两块都归派发」的结论要重算
        for (std::size_t bucket = 0; bucket < baselineHistogram.size(); ++bucket)
        {
            if (baselineHistogram[bucket] != 0)
            {
                std::printf("   baseline bucket=%zu bytes=%zu count=%llu\n", bucket,
                            bucket * AsynGyanis::TestSupport::kAllocationHistogramBucketBytes,
                            static_cast<unsigned long long>(baselineHistogram[bucket]));
            }
        }

        // 结构判据：一千次派发确实都被接手动作跑完，读数不是空转出来的
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "派发没有被取走，读数没有意义";
        EXPECT_EQ(handledCount.load(std::memory_order_relaxed), static_cast<int>(kMeasurementIterations));
        // 对照组必须显著小于被测组：否则「每连接两块」就是拿造描述符的开销冒充派发本身的开销。
        // 实测两侧读数为 2002 与 0（MSVC 与 libstdc++ 同向）
        EXPECT_LE(baseline.totalAllocations, profile.totalAllocations / 2U)
                << "对照组的分配已接近派发全程：台账把库自己的开销记到了交接头上";

#ifdef NDEBUG
        // 一千次派发实测 2002 块（MSVC）/ 2062 块（libstdc++）：每交一条连接两块，一块是交接句柄
        // （「没人接手就关闭」要求载荷可复制，只能共享持有），一块是投进队列的回调载荷。上界取
        // 「每连接至多三块」——换 STL 与队列分块差异都落在里面，而真多出一块时立刻报红
        EXPECT_LE(profile.totalAllocations, kMeasurementIterations * 3U)
                << "每交一条连接的堆块数越界：交接这条路上多半又多了一次分配";
#endif
    }
} // namespace AsynGyanis::Core
