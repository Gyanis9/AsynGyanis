// ConnectionDistributor 单元测试：轮转派发、回调落在目标循环线程上、无可派人选时不接管描述符
//
// 用例跑真实的 EventLoop（各占一个线程）与真实的套接字描述符：跨循环这件事的坑
// （唤醒丢失、描述符归属错）只有在真循环上才暴露得出来。

#include "Core/EventLoop/ConnectionDistributor.h"
#include "Core/EventLoop/EventLoop.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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
} // namespace AsynGyanis::Core
