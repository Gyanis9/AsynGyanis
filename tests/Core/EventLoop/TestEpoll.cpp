// Epoll 单元测试：实例句柄、移动语义、事件注册/修改/移除与超时等待；
// Windows 侧另测完成端口的探针重投记账会不会把日志写成噪声

#include "Core/EventLoop/Epoll.h"
#include "Platform/Platform.h"
#include "Platform/IO/FileDescriptor.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#if ASYN_PLATFORM_WIN32
#include "Base/Exception/LogicException.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Platform/IO/Socket.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#endif

#if !ASYN_PLATFORM_WIN32
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 测试辅助：创建一个可触发的文件描述符
         * @details Linux 直接使用 eventfd（读写为同一描述符）；Windows 没有 eventfd，
         *          改用互相连通的 socket 描述符对（wepoll 可监听 socket）。
         */
        struct TestEventFd
        {
            int fileDescriptor{Platform::FileDescriptor::kInvalid};  ///< 注册到 epoll 的描述符（Windows 上为读端）
            int writeDescriptor{Platform::FileDescriptor::kInvalid}; ///< 触发事件时写入的描述符（Linux 上与 fileDescriptor 相同）

            TestEventFd()
            {
#if ASYN_PLATFORM_WIN32
                // Windows 用 loopback 描述符对承载 eventfd 的唤醒语义
                if (!Platform::FileDescriptor::createPair(fileDescriptor, writeDescriptor))
                {
                    return;
                }
                // createPair 内部已设置非阻塞，这里保留兜底设置以防实现回退
                Platform::FileDescriptor::setNonBlocking(fileDescriptor);
#else
                fileDescriptor      = static_cast<int>(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
                writeDescriptor = fileDescriptor;
#endif
            }

            ~TestEventFd()
            {
                // 读端始终需要关闭；Linux 上读写同描述符，仅关闭一次
                if (Platform::FileDescriptor::isValid(fileDescriptor))
                {
                    Platform::FileDescriptor::close(fileDescriptor);
                }
#if ASYN_PLATFORM_WIN32
                if (Platform::FileDescriptor::isValid(writeDescriptor) && writeDescriptor != fileDescriptor)
                {
                    Platform::FileDescriptor::close(writeDescriptor);
                }
#endif
            }

            TestEventFd(const TestEventFd &)            = delete;
            TestEventFd &operator=(const TestEventFd &) = delete;

            /**
             * @brief 向写端写入一次计数，触发读端可读事件
             * @return true 写入成功
             */
            bool trigger()
            {
                const std::uint64_t value = 1;
#if ASYN_PLATFORM_WIN32
                // Windows 上描述符实为 socket，写入走 Winsock send
                return ::send(writeDescriptor, reinterpret_cast<const char *>(&value), sizeof(value), 0)
                       == static_cast<int>(sizeof(value));
#else
                return ::write(writeDescriptor, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
#endif
            }
        };
    }

    /**
     * @brief 构造后立即持有可用的 epoll 句柄，而不是 kInvalidEpollHandle 占位
     */
    TEST(Epoll, ConstructionAllocatesValidHandle)
    {
        Epoll epoll;
        ASSERT_NE(epoll.fileDescriptor(), Platform::kInvalidEpollHandle);
    }

    /**
     * @brief 移动构造把 epoll 句柄整体交给新对象，不重建底层实例
     */
    TEST(Epoll, MoveConstructorTransfersHandle)
    {
        Epoll first;
        const Platform::EpollHandle originalHandle = first.fileDescriptor();

        Epoll second(std::move(first));

        EXPECT_EQ(second.fileDescriptor(), originalHandle);
    }

    /**
     * @brief 移动赋值同样转移句柄：源被掏空、目标持有原句柄，不泄漏也不产生两个所有者
     */
    TEST(Epoll, MoveAssignmentTransfersHandle)
    {
        Epoll first;
        Epoll second;
        const Platform::EpollHandle originalHandle = first.fileDescriptor();

        second = std::move(first);

        EXPECT_EQ(second.fileDescriptor(), originalHandle);
    }

    /**
     * @brief 注册 EPOLLIN 后触发可读即上报事件，且事件带回注册时挂载的用户数据指针（调用方靠它定位上下文）
     */
    TEST(Epoll, AddFileDescriptorDeliversReadableEvent)
    {
        Epoll epoll;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        int sentinel = 0;
        ASSERT_TRUE(epoll.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));

        // 写入计数使描述符变为可读
        ASSERT_TRUE(eventFd.trigger());

        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events[0].data.ptr, static_cast<void *>(&sentinel));
    }

    /**
     * @brief 注销后不再投递事件：即使描述符已可读也不产生就绪项（避免等待器被已摘除的 fd 唤醒）
     */
    TEST(Epoll, DelFileDescriptorStopsEventDelivery)
    {
        Epoll epoll;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        ASSERT_TRUE(epoll.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, nullptr));
        ASSERT_TRUE(epoll.delFileDescriptor(eventFd.fileDescriptor));

        // 移除后再触发不应产生任何就绪事件
        eventFd.trigger();

        const auto events = epoll.wait(10);
        EXPECT_TRUE(events.empty());
    }

    /**
     * @brief 无效描述符与重复注册都要当场被拒，而失败的那一次不许伤到已成立的那一次
     * @details 三个后端各用自己的办法实现这张表（内核报 EEXIST / 表里已有 / 提交失败）。这里盯的是
     *          共同的后果：把「拒重复」实现成「先摘旧的再建新的」，第二份注册对象就会悄悄顶掉第一份
     *          的归属——上层两份注册对象于是有一份永远收不到事件。
     */
    TEST(Epoll, RejectsInvalidAndDuplicateRegistrationWithoutHurtingTheFirst)
    {
        Epoll backend;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        EXPECT_FALSE(backend.addFileDescriptor(Platform::FileDescriptor::kInvalid, EPOLLIN, nullptr))
                << "无效描述符被收下：注册表里会留一条永远等不到通知的记录";

        int firstSentinel = 1;
        ASSERT_TRUE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &firstSentinel));

        int secondSentinel = 2;
        EXPECT_FALSE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &secondSentinel))
                << "同一个描述符注册了两次都报成功：两份注册对象会在同一个就绪上互相覆盖";

        // 被拒的那一次不许动第一次的归属：触发一次，事件仍要带着第一份用户数据回来
        ASSERT_TRUE(eventFd.trigger());
        const auto events = backend.wait(100);
        ASSERT_EQ(events.size(), 1U) << "重复注册被拒之后，第一次注册收不到事件了";
        EXPECT_EQ(events[0].data.ptr, static_cast<void *>(&firstSentinel)) << "事件带的是第二次的用户数据：归属被顶掉了";

        EXPECT_TRUE(backend.delFileDescriptor(eventFd.fileDescriptor));
    }

    /**
     * @brief 未注册描述符上的改与删、以及第二次删，都必须报「没做成」
     * @details 这三条都返回 bool，而调用方（IoWatcher）用它区分「内核状态未知」与「已经落好」：
     *          把失败报成成功，账本就会记成一个内核里并不存在的关注位。
     */
    TEST(Epoll, OperationsOnUnregisteredDescriptorsReportFailure)
    {
        Epoll backend;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        int sentinel = 1;
        EXPECT_FALSE(backend.modFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel))
                << "没注册过的描述符被当成改成功了";
        EXPECT_FALSE(backend.delFileDescriptor(eventFd.fileDescriptor)) << "没注册过的描述符被当成注销成功了";

        ASSERT_TRUE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));
        ASSERT_TRUE(backend.delFileDescriptor(eventFd.fileDescriptor));
        EXPECT_FALSE(backend.delFileDescriptor(eventFd.fileDescriptor))
                << "第二次注销也报成功：注销的返回值就成了不可靠信号，重复摘除会被当成一次真实收尾";
    }

    /**
     * @brief 关注位清零期间不许上报，改回来之后那份一直就绪的状态仍要能收到
     * @details 这是 IoWatcher「不关注必须显式写进内核」那一侧的后端义务：清零后若不把在途探针取消，
     *          恢复关注时那份旧就绪会被当成新事件交回来；而恢复之后不再补投，那个一直可读的描述符
     *          就再也报不上来（epoll 靠内核重取状态看不见这个缺口，只有另两个后端会露出来）。
     */
    TEST(Epoll, ClearedMaskStaysQuietAndRestoringItDeliversAgain)
    {
        Epoll backend;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        int sentinel = 1;
        ASSERT_TRUE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));
        ASSERT_TRUE(backend.modFileDescriptor(eventFd.fileDescriptor, 0, &sentinel));

        // 清零期间触发：没有任何关注位，这一份就绪不该出现在结果里
        ASSERT_TRUE(eventFd.trigger());
        EXPECT_TRUE(backend.wait(50).empty()) << "关注位已清零，后端还在上报这个描述符";

        ASSERT_TRUE(backend.modFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));
        const auto events = backend.wait(100);
        ASSERT_FALSE(events.empty()) << "恢复关注之后，那份一直就绪的状态再也没有被上报过";
        EXPECT_EQ(events[0].data.ptr, static_cast<void *>(&sentinel));

        EXPECT_TRUE(backend.delFileDescriptor(eventFd.fileDescriptor));
    }

    /**
     * @brief 水平触发的常驻注册在稳态每轮 wait() 都不该碰堆
     * @details 完成端口靠「重新投一次探针」模拟水平触发，因此只要有人注册着，待重投表每轮都非空；
     *          把整张表换到局部变量再销毁，等于每轮把缓冲还给堆、下一轮再按容量长出来（实测 32 条
     *          注册下每轮 12 次分配，改成就地消费后归零）。io_uring 侧另有同一形状的一笔、按在途轮询
     *          条数付的节点分配，也换成了扁平表；三个后端因此共用「零分配」这一条判据。
     */
    TEST(Epoll, LevelTriggeredWaitDoesNotAllocatePerRound)
    {
        constexpr std::size_t kRegisteredDescriptorCount = 32;

        Epoll backend;
        // std::array：注册用的用户数据是各元素自己的地址，因此这批对象一次成型、永不被搬动
        std::array<TestEventFd, kRegisteredDescriptorCount> eventFds;
        for (auto &eventFd: eventFds)
        {
            ASSERT_GE(eventFd.fileDescriptor, 0);
            // 先塞数据再注册：水平触发下每一份都没被消费，于是每一轮都该重新报一次就绪
            ASSERT_TRUE(eventFd.trigger());
            ASSERT_TRUE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &eventFd));
        }

        // 预热：让结果表与索引表先长到位，测量窗口里才只剩「稳态」而不是首次扩容
        for (std::size_t round = 0; round < 50; ++round)
        {
            static_cast<void>(backend.wait(50));
        }
        // 直方图是进程级的，测量窗口前先清零，读数才只属于这一段
        TestSupport::resetAllocationHistogram();

        const auto profile = TestSupport::measurePerOperation(
                [&backend]() -> std::size_t
                {
                    // 窗口里只留「取一批就绪」这一步：注册、触发与预热都在外面
                    return backend.wait(50).size();
                });

        // 读数非零才证明这 1000 轮真的在派发事件，而不是空转到断言上
        EXPECT_GE(profile.resultSum, TestSupport::kMeasurementIterations) << "多数轮次没有取回任何就绪，等于没测";
        // 失败时把「申请大小的分布」一起报出来：一眼分辨多出的是指针表（8/16 字节）还是记录节点
        std::string histogramText;
        const auto  histogram = TestSupport::snapshotAllocationHistogram();
        for (std::size_t bucket = 0; bucket < histogram.size(); ++bucket)
        {
            if (histogram[bucket] != 0)
            {
                histogramText += " " + std::to_string(bucket * TestSupport::kAllocationHistogramBucketBytes) + "B×"
                                 + std::to_string(histogram[bucket]);
            }
        }

        EXPECT_EQ(profile.totalAllocations, 0U)
                << "稳态每轮 wait() 付了 " << (profile.totalAllocations / TestSupport::kMeasurementIterations)
                << " 次分配（一千轮共 " << profile.totalAllocations << " 次，大小分布:" << histogramText << "）";

        for (auto &eventFd: eventFds)
        {
            EXPECT_TRUE(backend.delFileDescriptor(eventFd.fileDescriptor));
        }
    }

    /**
     * @brief 一直挂着不动的那份轮询，要被后面成千张新票据挤过位置也还得接回来
     * @details 空闲连接的轮询会带着**一张很旧的票据**一直挂在途，而活跃连接每轮都在换新票据——
     *          票据号相差整数个槽位时两者会落到同一格上（探测链由此接成一段），删掉其中一条就得把
     *          同段后面的条目往前挪。挪错或漏挪的表现是这条空闲注册从此不再报就绪，而这是服务端
     *          最常见的一种连接（keep-alive 空转），不能等到线上才发现。
     */
    TEST(Epoll, IdleRegistrationSurvivesTicketWraparound)
    {
        // 8 条「挂着不动」的注册：整个用例期间都不触发，所以票据一直留在途、且越来越旧
        constexpr std::size_t kIdleDescriptorCount   = 8;
        // 256 条每轮冲刷的注册：每轮两张新票据，几千张之后必然越过一整圈槽位与旧票据撞格
        constexpr std::size_t kActiveDescriptorCount = 256;
        constexpr std::size_t kRoundCount            = 24;

        Epoll backend;
        std::array<TestEventFd, kIdleDescriptorCount> idleFds;
        std::array<TestEventFd, kActiveDescriptorCount> activeFds;
        for (auto &idleFd: idleFds)
        {
            ASSERT_GE(idleFd.fileDescriptor, 0);
            // 刻意不 trigger：这份轮询要一直挂在途，才有「旧票据」可言
            ASSERT_TRUE(backend.addFileDescriptor(idleFd.fileDescriptor, EPOLLIN, &idleFd));
        }
        for (auto &activeFd: activeFds)
        {
            ASSERT_GE(activeFd.fileDescriptor, 0);
            ASSERT_TRUE(activeFd.trigger());
            ASSERT_TRUE(backend.addFileDescriptor(activeFd.fileDescriptor, EPOLLIN, &activeFd));
        }

        for (std::size_t round = 0; round < kRoundCount; ++round)
        {
            for (auto &activeFd: activeFds)
            {
                ASSERT_TRUE(backend.modFileDescriptor(activeFd.fileDescriptor, 0, &activeFd));
                ASSERT_TRUE(backend.modFileDescriptor(activeFd.fileDescriptor, EPOLLIN, &activeFd));
            }
            static_cast<void>(backend.wait(500));
        }

        // 现在才让空闲注册可读：它在途那份轮询该照常完成并把就绪交回来
        for (auto &idleFd: idleFds)
        {
            ASSERT_TRUE(idleFd.trigger());
        }
        std::size_t idleReportedCount{0};
        for (std::size_t drain = 0; drain < 20 && idleReportedCount < kIdleDescriptorCount; ++drain)
        {
            for (const auto &event: backend.wait(500))
            {
                const auto *const slot = static_cast<const TestEventFd *>(event.data.ptr);
                // 只在空闲那批里比对地址（活跃批的指针也在这里出现，跨数组相减是未定义的）
                const auto idleIterator = std::find_if(idleFds.begin(), idleFds.end(),
                                                       [slot](const TestEventFd &candidate)
                                                       {
                                                           return &candidate == slot;
                                                       });
                if (idleIterator != idleFds.end())
                {
                    ++idleReportedCount;
                }
            }
        }
        EXPECT_GE(idleReportedCount, kIdleDescriptorCount)
                << "空闲注册被冲刷的票据挤掉了：那份在途轮询的归属已经丢了";

        for (auto &idleFd: idleFds)
        {
            EXPECT_TRUE(backend.delFileDescriptor(idleFd.fileDescriptor));
        }
        for (auto &activeFd: activeFds)
        {
            EXPECT_TRUE(backend.delFileDescriptor(activeFd.fileDescriptor));
        }
    }

    /**
     * @brief 大批注册在「反复改关注位」的冲刷下，一条就绪都不许丢
     * @details 每轮把关注位清零再改回来，给后端造出「撤掉在途轮询 + 立刻重投」的成对操作：在途表要
     *          连续摘除与登记，还会晚到陈旧的取消通知。水平触发下每条注册每轮都该报一次就绪，因此
     *          按注册对象分别计数、每条都至少要有轮数次；少一条就是某条重投的轮询被摘丢或错配掉了。
     * @note 单轮交回哪几条不固定（批量取只要等到一条就返回），所以断言落在**每条各自的次数**上
     */
    TEST(Epoll, LargeRegistrationSetSurvivesMaskChurn)
    {
        constexpr std::size_t kRegisteredDescriptorCount = 200;
        constexpr std::size_t kRoundCount                = 40;

        Epoll backend;
        std::array<TestEventFd, kRegisteredDescriptorCount> eventFds;
        for (auto &eventFd: eventFds)
        {
            ASSERT_GE(eventFd.fileDescriptor, 0);
            ASSERT_TRUE(eventFd.trigger());
            ASSERT_TRUE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &eventFd));
        }

        std::array<std::size_t, kRegisteredDescriptorCount> reportedCounts{};
        const auto collectReports = [&backend, &eventFds, &reportedCounts]()
        {
            for (const auto &event: backend.wait(500))
            {
                const auto *const slot  = static_cast<const TestEventFd *>(event.data.ptr);
                const std::size_t index = static_cast<std::size_t>(slot - eventFds.data());
                if (index >= reportedCounts.size())
                {
                    ADD_FAILURE_AT(__FILE__, __LINE__) << "事件带回了不属于本批注册的用户数据";
                    continue;
                }
                ++reportedCounts[index];
            }
        };

        for (std::size_t round = 0; round < kRoundCount; ++round)
        {
            for (auto &eventFd: eventFds)
            {
                ASSERT_TRUE(backend.modFileDescriptor(eventFd.fileDescriptor, 0, &eventFd));
                ASSERT_TRUE(backend.modFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &eventFd));
            }
            collectReports();
        }
        // 数据始终没人消费，靠后的轮次可能把上一轮的就绪推到这一轮：补几轮收完，别把「还没轮到」当成丢了
        for (std::size_t drain = 0; drain < 10; ++drain)
        {
            collectReports();
        }

        for (std::size_t index = 0; index < reportedCounts.size(); ++index)
        {
            EXPECT_GE(reportedCounts[index], kRoundCount)
                    << "第 " << index << " 号描述符只报了 " << reportedCounts[index] << " 次，冲刷中被摘丢了";
        }

        for (auto &eventFd: eventFds)
        {
            EXPECT_TRUE(backend.delFileDescriptor(eventFd.fileDescriptor));
        }
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 完成端口不允许把同一个仍打开的描述符注销之后再注册回来
     * @details Windows 没有把句柄从完成端口解除关联的 API，因此这是平台硬限制而不是本层缺陷。
     *          钉住它有两个理由：与下面那条 POSIX 用例对照，说明「注销后重新注册」不是跨后端可行
     *          的写法；以及三种时序实测一致，免得后来人以为多等一轮就能注册回来。
     */
    TEST(Epoll, CannotReRegisterAnOpenDescriptorAfterDelete)
    {
        Epoll backend;
        TestEventFd drained;
        TestEventFd pending;
        ASSERT_GE(drained.fileDescriptor, 0);
        ASSERT_GE(pending.fileDescriptor, 0);

        // 时序一：探针已经跑完一轮（可读→上报→注销），注销时没有在途取消要等
        ASSERT_TRUE(drained.trigger());
        ASSERT_TRUE(backend.addFileDescriptor(drained.fileDescriptor, EPOLLIN, &drained));
        ASSERT_FALSE(backend.wait(200).empty()) << "前提不成立：可读的描述符没在首轮报出就绪";
        ASSERT_TRUE(backend.delFileDescriptor(drained.fileDescriptor));
        EXPECT_FALSE(backend.addFileDescriptor(drained.fileDescriptor, EPOLLIN, &drained))
                << "探针跑完一轮之后仍不该能把同一个活描述符再注册回来";

        // 时序二：注销时取消完成还压在队列里
        ASSERT_TRUE(backend.addFileDescriptor(pending.fileDescriptor, EPOLLIN, &pending));
        ASSERT_TRUE(backend.delFileDescriptor(pending.fileDescriptor));
        EXPECT_FALSE(backend.addFileDescriptor(pending.fileDescriptor, EPOLLIN, &pending))
                << "取消完成尚未取回时再注册应当同样被拒";

        // 时序三：把那一轮等待做完（取消完成已被收掉）之后再试
        static_cast<void>(backend.wait(200));
        EXPECT_FALSE(backend.addFileDescriptor(pending.fileDescriptor, EPOLLIN, &pending))
                << "收掉取消完成之后就注册得回来了——那这条限制就不是 Windows 的硬边界，文档要改";

        // 关掉之后新句柄复用同一个号是另一回事（下面那条 POSIX 用例钉的就是它），这里先把句柄还掉
        Platform::FileDescriptor::close(drained.fileDescriptor);
        Platform::FileDescriptor::close(pending.fileDescriptor);
        drained.fileDescriptor = Platform::FileDescriptor::kInvalid;
        drained.writeDescriptor = Platform::FileDescriptor::kInvalid;
        pending.fileDescriptor = Platform::FileDescriptor::kInvalid;
        pending.writeDescriptor = Platform::FileDescriptor::kInvalid;
    }
#endif

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 注销一个已武装的描述符之后，同一个描述符号（已被新描述符占用）要能立刻重新注册
     * @details 连接关闭与新建连接拿到同一个描述符号是常态，而 io_uring 后端的在途轮询要等取消
     *          完成通知到齐才销毁记录——但**描述符键必须当场释放**，否则新连接的注册会被直接拒掉。
     *          用例用 dup2 把「号复用」做成确定性的（不赌内核挑号）；Windows 完成端口没有这个窗口。
     */
    TEST(Epoll, AllowsReregisteringDescriptorNumberRightAfterDelete)
    {
        Epoll epoll;

        TestEventFd staleEventFd;
        ASSERT_GE(staleEventFd.fileDescriptor, 0);
        const int reusedDescriptorNumber = staleEventFd.fileDescriptor;

        int firstSentinel = 1;
        ASSERT_TRUE(epoll.addFileDescriptor(reusedDescriptorNumber, EPOLLIN, &firstSentinel));
        ASSERT_TRUE(epoll.delFileDescriptor(reusedDescriptorNumber));

        // 号的复用：新 eventfd 经 dup2 精确落到旧号上（旧号上原来那份随 dup2 一并关闭）
        TestEventFd freshEventFd;
        ASSERT_GE(freshEventFd.fileDescriptor, 0);
        ASSERT_EQ(::dup2(freshEventFd.writeDescriptor, reusedDescriptorNumber), reusedDescriptorNumber);

        int secondSentinel = 2;
        EXPECT_TRUE(epoll.addFileDescriptor(reusedDescriptorNumber, EPOLLIN, &secondSentinel))
                << "注销后同一个描述符号不能再注册：描述符键没有当场释放";

        // 新注册必须真的生效：让这个号变成可读，事件要带新挂的用户数据
        ASSERT_TRUE(freshEventFd.trigger());

        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty()) << "重新注册的描述符没有投递事件";
        EXPECT_EQ(events[0].data.ptr, static_cast<void *>(&secondSentinel)) << "投递的是旧注册的用户数据";

        static_cast<void>(epoll.delFileDescriptor(reusedDescriptorNumber));
    }
#endif

    /**
     * @brief 无注册描述符时 wait() 在超时后返回空列表：既不死等也不报错
     */
    TEST(Epoll, WaitTimeoutReturnsNoEvents)
    {
        Epoll epoll;

        // 无任何注册描述符时，等待应在超时后返回空事件列表
        const auto events = epoll.wait(10);
        EXPECT_TRUE(events.empty());
    }

    /**
     * @brief modFileDescriptor() 真的替换了事件掩码：改为 EPOLLOUT 后按可写就绪，不再按可读
     */
    TEST(Epoll, ModFileDescriptorChangesEventMask)
    {
        Epoll epoll;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        int sentinel = 42;
        ASSERT_TRUE(epoll.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));
        ASSERT_TRUE(epoll.modFileDescriptor(eventFd.fileDescriptor, EPOLLOUT, &sentinel));

        // 描述符始终可写，事件掩码修改为 EPOLLOUT 后应立即就绪
        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty());
        EXPECT_TRUE(events[0].events & EPOLLOUT);
    }

    /**
     * @brief 多路注册时只上报真正就绪的那一个（用用户数据指针定位），未触发的描述符不产生误报
     */
    TEST(Epoll, MultipleFileDescriptorsReportTriggeredOne)
    {
        Epoll epoll;
        TestEventFd firstEventFd;
        TestEventFd secondEventFd;
        ASSERT_GE(firstEventFd.fileDescriptor, 0);
        ASSERT_GE(secondEventFd.fileDescriptor, 0);

        int firstSentinel  = 1;
        int secondSentinel = 2;
        ASSERT_TRUE(epoll.addFileDescriptor(firstEventFd.fileDescriptor, EPOLLIN, &firstSentinel));
        ASSERT_TRUE(epoll.addFileDescriptor(secondEventFd.fileDescriptor, EPOLLIN, &secondSentinel));

        // 仅触发第二个描述符
        secondEventFd.trigger();

        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty());

        // 就绪列表中应能找到第二个描述符挂载的用户数据
        bool foundSecondSentinel = false;
        for (const auto &event: events)
        {
            if (event.data.ptr == &secondSentinel)
            {
                foundSecondSentinel = true;
            }
        }
        EXPECT_TRUE(foundSecondSentinel);
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 连续两次 wait() 不得换掉落地缓冲：上一个视图必须还可读
     * @details wait() 原先在「就绪数达到容量一半」时把缓冲翻倍。翻倍发生在 epoll_wait 返回之后、
     *          交出视图之前，所以**当次**的视图总是有效的——真正被坑的是上一次的视图：
     *          事件循环跨着 handleEvents()（它会同步恢复等待中的协程）持有它，那一趟里
     *          再来一次 wait() 就会把旧缓冲释放掉，之后读 data.ptr 是悬垂读、照着它派发
     *          就是拿垃圾地址当 IoWatcher 用。现在缓冲一次定容、永不改容量。
     *          判据不需要 sanitizer：两次 wait() 的 data() 必须同址。用 dup 把一个可读
     *          描述符复制成一片，是因为 epoll 按 fd 去重，同一个 fd 注册不了两次。
     */
    TEST(Epoll, RepeatedWaitKeepsTheSameLandingBuffer)
    {
        /// 至少要 1024 个就绪 fd 才能连着触发两次「达到容量一半」；留出 dup 之外的余量
        constexpr int kRequiredDescriptors = 2200;

        rlimit descriptorLimit{};
        if (getrlimit(RLIMIT_NOFILE, &descriptorLimit) == 0 && descriptorLimit.rlim_cur < kRequiredDescriptors)
        {
            GTEST_SKIP() << "本进程只允许 " << descriptorLimit.rlim_cur
                         << " 个描述符，凑不出「连续两次推满半容量」的高负载现场";
        }

        TestEventFd trigger;
        ASSERT_TRUE(Platform::FileDescriptor::isValid(trigger.fileDescriptor));
        // 先写一次计数：Linux 上 dup 出来的描述符共享同一个 open file description，
        // 因此这一写让全部副本同时可读，而只要不去读它就一直是可读的（水平触发会反复上报）
        ASSERT_TRUE(trigger.trigger());

        Epoll         backend;
        std::vector<int> descriptors;
        descriptors.reserve(1024);
        for (int index = 0; index < 1024; ++index)
        {
            const int duplicated = ::dup(trigger.fileDescriptor);
            ASSERT_GE(duplicated, 0) << "dup 到第 " << index << " 次就失败了，环境句柄数不够";
            descriptors.push_back(duplicated);
            ASSERT_TRUE(backend.addFileDescriptor(duplicated, EPOLLIN,
                                                  reinterpret_cast<void *>(static_cast<std::uintptr_t>(index + 1U))));
        }

        const auto firstBatch = backend.wait(0);
        ASSERT_EQ(firstBatch.size(), 1024U) << "第一次就该取满单轮上限，才有连续两次触发扩容的现场";
        const auto secondBatch = backend.wait(0);
        ASSERT_EQ(secondBatch.size(), 1024U) << "水平触发下这批描述符仍就绪，第二次也该取满";

        // 同址 = 上一个视图没被换掉；旧实现在这里会因为第二次翻倍而拿到不同的基址
        EXPECT_EQ(firstBatch.data(), secondBatch.data())
                << "wait() 换了落地缓冲：上一次交出去的视图已经悬垂，事件循环跨 handleEvents() 持有它就是野指针读";

        // 再回读一次上一个视图的内容（ASan 下这是最直接的悬垂读探针）
        EXPECT_NE(firstBatch[0].data.ptr, nullptr) << "回读上一个视图的内容应当仍是本次注册的哨兵";

        for (const int descriptor: descriptors)
        {
            static_cast<void>(backend.delFileDescriptor(descriptor));
            Platform::FileDescriptor::close(descriptor);
        }
    }

    /**
     * @brief 一次等待最多交出一批事件，剩下的留到下一次：三个后端必须同口径
     * @details 上限管着两件事：突发就绪时不必把整批派发给完才回头取 IO（尾延迟直接由批大小决定），
     *          以及落地缓冲可以一次定容（改容量会把上一次交出去的视图变成悬垂读）。
     *          io_uring 后端在这里与另外两个不一致过——它把 CQ 里的完成一次全交完。
     *          次轮只断言「剩下的至少补上」：水平触发的描述符仍然就绪，会被再报一次，
     *          所以两批之和大于注册数才是预期，不能拿它当「不丢」的判据。
     */
    TEST(Epoll, WaitDeliversAtMostOneBatchAndKeepsTheRestForNextWait)
    {
        /// 比单轮上限多出一截，才同时有「取满」与「剩下」两半可断言
        constexpr int kTriggeredDescriptorCount = 1200;
        /// 与 Epoll/Iocp 各自那个私有常量同值：三处不一致就是缺陷，不是本用例该放宽的地方
        constexpr std::size_t kMaximumEventsPerWait = 1024;

        rlimit descriptorLimit{};
        if (getrlimit(RLIMIT_NOFILE, &descriptorLimit) != 0
            || descriptorLimit.rlim_cur < kTriggeredDescriptorCount + 256)
        {
            GTEST_SKIP() << "本进程只允许 " << descriptorLimit.rlim_cur
                         << " 个描述符，凑不出「超过单轮上限的一批就绪事件」";
        }

        TestEventFd trigger;
        ASSERT_TRUE(Platform::FileDescriptor::isValid(trigger.fileDescriptor));
        // 写一次计数：dup 出来的副本共享同一个 open file description，因此全部副本同时可读
        ASSERT_TRUE(trigger.trigger());

        Epoll          backend;
        std::vector<int> descriptors;
        descriptors.reserve(kTriggeredDescriptorCount);
        for (int index = 0; index < kTriggeredDescriptorCount; ++index)
        {
            const int duplicated = ::dup(trigger.fileDescriptor);
            ASSERT_GE(duplicated, 0) << "dup 到第 " << index << " 次失败，环境句柄数不够";
            descriptors.push_back(duplicated);
            ASSERT_TRUE(backend.addFileDescriptor(duplicated, EPOLLIN,
                                                  reinterpret_cast<void *>(static_cast<std::uintptr_t>(index + 1U))));
        }

        const auto firstBatch = backend.wait(0);
        EXPECT_EQ(firstBatch.size(), kMaximumEventsPerWait)
                << "单轮交出 " << firstBatch.size() << " 条：要么没按 " << kMaximumEventsPerWait
                << " 的上限截断，要么没把就绪的描述符取满";

        const auto secondBatch = backend.wait(0);
        EXPECT_GE(secondBatch.size(), static_cast<std::size_t>(kTriggeredDescriptorCount) - kMaximumEventsPerWait)
                << "首轮取满之后，剩下的 " << kTriggeredDescriptorCount - kMaximumEventsPerWait
                << " 条至少要留到下一次交付，一条都不能丢";

        for (const int descriptor: descriptors)
        {
            static_cast<void>(backend.delFileDescriptor(descriptor));
            Platform::FileDescriptor::close(descriptor);
        }
    }

    /**
     * @brief 把已武装的轮询改成「可写」之后，无限等待必须当场交付新的就绪事件
     * @details 改掩码在后端里是「取消在途轮询 → 完成通知到达时按新掩码重投」，而重投发生在收单
     *          的过程中。那条重投若没在入睡前交给内核，内核就永远不会为它产出完成通知，而下一次
     *          入睡前也没人再投它——EventLoop 空闲时跑的正是这种无限等待。可写位本就立即可满足，
     *          因此这个形状在线上是「连接发不出请求、也收不到响应」而不是「慢一点」。
     *          那只周期性 timerfd 是**兜底唤醒**而不是被测对象：退化实现下这条等待没有任何人叫醒，
     *          没有它就会把用例变成挂起（挂起不是失败，门禁报出来只看超时看不出是谁的问题）。
     *          判据取「这一批里有没有可写位」而不是「最后有没有」：退化实现会在下一次等待开头把
     *          这条提交补发出去，绕几轮照样交付，只有第一批能把它和正确实现区分开。
     */
    TEST(Epoll, InfiniteWaitDeliversInterestChangedToWritable)
    {
        /// 兜底唤醒的间隔：远大于「提交投进去就立刻完成」所需的时间，因此只有退化实现会等到它
        constexpr int kFallbackWakeupIntervalMs = 300;

        Epoll backend;

        const int timerDescriptor = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        ASSERT_GE(timerDescriptor, 0);
        itimerspec timerSetting{};
        timerSetting.it_value.tv_nsec    = static_cast<long>(kFallbackWakeupIntervalMs) * 1000000L;
        timerSetting.it_interval.tv_nsec = static_cast<long>(kFallbackWakeupIntervalMs) * 1000000L;
        ASSERT_EQ(::timerfd_settime(timerDescriptor, 0, &timerSetting, nullptr), 0);
        // 不带 EPOLLONESHOT：水平触发的注册每轮都会被重新武装，因此才担得起兜底唤醒这个角色
        int timerSentinel = 0;
        ASSERT_TRUE(backend.addFileDescriptor(timerDescriptor, EPOLLIN, &timerSentinel));

        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);
        int sentinel = 0;
        ASSERT_TRUE(backend.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));

        // 先让两条注册真正进内核（此刻两者都还没就绪，这一等是空的）
        static_cast<void>(backend.wait(0));

        // 读→写：这条注册此刻在途，于是走「取消 + 等取消完成通知到达时重投」那条路
        ASSERT_TRUE(backend.modFileDescriptor(eventFd.fileDescriptor, EPOLLOUT, &sentinel));

        bool isWritableReported = false;
        for (const auto &event: backend.wait(-1))
        {
            if (event.data.ptr == static_cast<void *>(&sentinel) && (event.events & EPOLLOUT) != 0)
            {
                isWritableReported = true;
            }
        }
        EXPECT_TRUE(isWritableReported)
                << "无限等待只被兜底的 timerfd 叫醒，没有交付刚改成的可写位："
                   "那条重投还悬在提交队列里，没在入睡前交给内核";

        static_cast<void>(backend.delFileDescriptor(eventFd.fileDescriptor));
        static_cast<void>(backend.delFileDescriptor(timerDescriptor));
        Platform::FileDescriptor::close(timerDescriptor);
    }
    /**
     * @brief 一轮空等待的固定开销不得随在册描述符数线性增长
     * @details 事件循环每收一批事件都要等一次，所以「每次等待先按在册条数走一遍」会被放大成
     *          吞吐上限：连接越多、每条连接上每个事件越贵。io_uring 后端有过这个形状——它的
     *          轮询是一次性的，入睡前要给「刚上报过」的描述符补投，而补投原先靠遍历整张注册表
     *          找目标（实测 128 条 12.3 微秒、4096 条 405 微秒，epoll 后端同形状是平的）。
     *          判据用比值而不是绝对值：绝对值随机器与构建档位漂，比值只反映「有没有那趟遍历」。
     *          三倍小表与三十二倍大表之间放 8 倍余量，退化实现（线性）落不进这个窗口。
     */
    TEST(Epoll, WaitCostDoesNotScaleWithRegisteredDescriptorCount)
    {
        constexpr std::size_t kSmallRegistrationCount = 128;
        constexpr std::size_t kLargeRegistrationCount = 4096;
        constexpr int         kMeasurementIterations  = 2000;
        /// 允许大表比小表贵这么多倍；线性的实现会贵约 32 倍
        constexpr std::int64_t kMaximumCostRatio = 8;

        // 两张表都要挂得上描述符（读写两端各一个号），不够就跳过而不是把用例做成假绿
        rlimit descriptorLimit{};
        if (getrlimit(RLIMIT_NOFILE, &descriptorLimit) != 0
            || descriptorLimit.rlim_cur < kLargeRegistrationCount * 2 + 64)
        {
            GTEST_SKIP() << "本进程只允许 " << descriptorLimit.rlim_cur
                         << " 个描述符，凑不出「大表显著大于小表」的对照现场";
        }

        const auto measureEmptyWaitCost = [](const std::size_t registrationCount) -> std::int64_t
        {
            std::vector<int> descriptors;
            descriptors.reserve(registrationCount);
            for (std::size_t index = 0; index < registrationCount; ++index)
            {
                const int descriptor = static_cast<int>(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
                if (descriptor < 0)
                {
                    return -1;
                }
                descriptors.push_back(descriptor);
            }

            Epoll backend;
            for (const int descriptor: descriptors)
            {
                if (!backend.addFileDescriptor(descriptor, EPOLLIN, nullptr))
                {
                    return -1;
                }
            }

            // 取三轮里的最小值：测量线程被抢占一次就会把均值抬高一个台阶，最小值才代表这条路径本身
            std::int64_t bestNanoseconds = std::numeric_limits<std::int64_t>::max();
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const auto beginTime = std::chrono::steady_clock::now();
                for (int iteration = 0; iteration < kMeasurementIterations; ++iteration)
                {
                    static_cast<void>(backend.wait(0));
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - beginTime)
                                             .count();
                bestNanoseconds = std::min(bestNanoseconds, elapsed / kMeasurementIterations);
            }

            for (const int descriptor: descriptors)
            {
                static_cast<void>(backend.delFileDescriptor(descriptor));
                Platform::FileDescriptor::close(descriptor);
            }
            return bestNanoseconds;
        };

        const std::int64_t smallCost = measureEmptyWaitCost(kSmallRegistrationCount);
        const std::int64_t largeCost = measureEmptyWaitCost(kLargeRegistrationCount);
        ASSERT_GE(smallCost, 0) << "挂不上 " << kSmallRegistrationCount << " 个描述符，环境不允许";
        ASSERT_GE(largeCost, 0) << "挂不上 " << kLargeRegistrationCount << " 个描述符，环境不允许";

        std::printf("PROBE wait0_ns small=%lld large=%lld\n",
                    static_cast<long long>(smallCost), static_cast<long long>(largeCost));

        EXPECT_LT(largeCost, smallCost * kMaximumCostRatio)
                << "空等待的固定开销随在册条数增长：" << kSmallRegistrationCount << " 条 " << smallCost
                << " 纳秒，" << kLargeRegistrationCount << " 条 " << largeCost
                << " 纳秒。每一次等待都不许按在册条数走一遍登记表";
    }
#endif

#if ASYN_PLATFORM_WIN32
    namespace
    {
        /// 空转的重试轮数：待重投表在每次 wait() 之前走一遍，取一个能让「每轮一条」与「只记一次」明显分开的数
        constexpr int kArmRetryRoundCount = 20;

        /**
         * @brief 告警账本：收下根日志器写出的 Warn 事件原文
         * @details Logger 只有「追加 Sink」与「清空全部 Sink」两个口子，没有摘除单条的接口，
         *          而清空会连带毁掉控制台 Sink（其他用例的输出就没了）。因此 Sink 在进程内只挂一份
         *          （见 ScopedWarningCapture::processLedger），用例只负责开关记录与清零——
         *          每进一个作用域就挂一份，会在 --gtest_repeat 下把 Sink 与已捕获文本一路累积下去。
         */
        class WarningLedger
        {
        public:
            /** @brief 开始记录：清掉上一轮留下的文本，避免跨用例串读 */
            void begin()
            {
                {
                    const std::lock_guard lock(m_mutex);
                    m_messages.clear();
                }
                m_isRecording.store(true, std::memory_order_release);
            }

            /** @brief 停止记录：留在 Logger 里的那份 Sink 从此变成一次原子读 */
            void stop() noexcept
            {
                m_isRecording.store(false, std::memory_order_release);
            }

            /**
             * @brief 收下一条日志事件，等级为 Warn 时存下原文
             * @param event 日志事件
             */
            void record(const Base::LogEvent &event)
            {
                if (!m_isRecording.load(std::memory_order_acquire) || event.level != Base::LogLevel::Warn)
                {
                    return;
                }
                const std::lock_guard lock(m_mutex);
                m_messages.push_back(event.message);
            }

            /**
             * @brief 已记下的告警条数
             * @return std::size_t 条数
             */
            [[nodiscard]] std::size_t count() const
            {
                const std::lock_guard lock(m_mutex);
                return m_messages.size();
            }

        private:
            std::atomic<bool>          m_isRecording{false}; ///< 是否仍在记录（默认关，只有作用域内开着）
            mutable std::mutex         m_mutex{};           ///< 保护原文列表
            std::vector<std::string>   m_messages{};        ///< 已记下的告警原文
        };

        /**
         * @brief 把事件转记账本的告警记录 Sink
         */
        class WarningRecordingSink final : public Base::LogSink
        {
        public:
            /**
             * @brief 用给定账本构造 Sink
             * @param ledger 事件账本（进程内唯一的那份，由 ScopedWarningCapture 开关）
             */
            explicit WarningRecordingSink(std::shared_ptr<WarningLedger> ledger) :
                m_ledger(std::move(ledger))
            {
            }

            /**
             * @brief 把事件交给账本记录
             * @param event 日志事件
             */
            void write(const Base::LogEvent &event) override
            {
                m_ledger->record(event);
            }

            /**
             * @brief 空实现：账本没有缓冲，无需刷新
             */
            void flush() override
            {
            }

        private:
            std::shared_ptr<WarningLedger> m_ledger; ///< 事件账本
        };

        /**
         * @brief 作用域内的根日志器告警抓取器
         * @details 只追加 Sink、不动既有的控制台 Sink，因此同一二进制里的其他用例照常输出；
         *          Sink 在进程内只挂一份，作用域只负责清零、开记与关记。
         */
        class ScopedWarningCapture
        {
        public:
            ScopedWarningCapture() :
                m_ledger(processLedger())
            {
                m_ledger->begin();
            }

            ~ScopedWarningCapture()
            {
                m_ledger->stop();
            }

            ScopedWarningCapture(const ScopedWarningCapture &)            = delete;
            ScopedWarningCapture &operator=(const ScopedWarningCapture &) = delete;

            /**
             * @brief 账本本体，供用例断言
             * @return const WarningLedger& 账本引用
             */
            [[nodiscard]] const WarningLedger &ledger() const noexcept
            {
                return *m_ledger;
            }

        private:
            /**
             * @brief 取进程内唯一的那份账本：首次调用时把 Sink 挂到根日志器，之后只复用
             * @details 挂上去就摘不下来（Logger 没有摘除单条的接口），所以必须只挂一次——
             *          每个作用域各挂一份会让 Sink 与捕获文本随 --gtest_repeat 线性累积。
             *          函数内 static 的初始化由标准保证只做一次且线程安全。
             * @return std::shared_ptr<WarningLedger> 共享账本
             */
            static std::shared_ptr<WarningLedger> processLedger()
            {
                static const std::shared_ptr<WarningLedger> kLedger = []
                {
                    auto ledger = std::make_shared<WarningLedger>();
                    Base::LoggerRegistry::instance().getRootLogger().addSink(
                            std::make_unique<WarningRecordingSink>(ledger));
                    return ledger;
                }();
                return kLedger;
            }

            std::shared_ptr<WarningLedger> m_ledger; ///< 进程内唯一账本的引用
        };

        /**
         * @brief 建一个「已创建但没连上」的流套接字
         * @details 正是完成端口投不了读探针的那个常态状态：AsyncSocket 构造即注册 EPOLLIN，
         *          而 connect() 还在后面。
         * @return int 套接字描述符，失败返回 FileDescriptor::kInvalid
         */
        [[nodiscard]] int makeUnconnectedStreamSocket()
        {
            const int fileDescriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (fileDescriptor == INVALID_SOCKET)
            {
                return Platform::FileDescriptor::kInvalid;
            }
            Platform::FileDescriptor::setNonBlocking(fileDescriptor);
            return fileDescriptor;
        }
    } // namespace

    /**
     * @brief 「此刻武装不上」的常态不得写成告警：每条客户端连接都会经过它一次
     * @details 完成端口在 connect() 之前投不进读探针（getpeername 不通过），后端把它记进待重投表
     *          并静默重试。若这里也告警，正常发起的每条连接都会留下一行 Warn，真正需要人看的
     *          资源耗尽类告警就被埋进噪声里。
     * @note 只在 Windows 侧有意义：epoll 与 io_uring 后端注册即由内核盯着，没有这条重试路径。
     */
    TEST(Epoll, ArmRetryForExpectedTransientLogsNothing)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        ScopedWarningCapture capture;
        std::size_t          warningCount = 0;
        {
            Epoll backend;
            // 后端先析构：它要等自己的完成通知排空，描述符不能先被关掉
            const int fileDescriptor = makeUnconnectedStreamSocket();
            ASSERT_NE(fileDescriptor, Platform::FileDescriptor::kInvalid) << "建不出套接字，环境不允许";

            int sentinel = 1;
            ASSERT_TRUE(backend.addFileDescriptor(fileDescriptor, EPOLLIN, &sentinel));

            // 每一轮 wait() 之前都会重投一次失败方向：二十轮全都没武装上，也不该留下一行告警
            for (int round = 0; round < kArmRetryRoundCount; ++round)
            {
                static_cast<void>(backend.wait(1));
            }

            ASSERT_TRUE(backend.delFileDescriptor(fileDescriptor));
            warningCount = capture.ledger().count();
            Platform::FileDescriptor::close(fileDescriptor);
        }

        EXPECT_EQ(warningCount, 0U) << "预期中的暂时状态被写成了告警，日志会随连接数线性膨胀";
        Platform::Socket::finalize();
    }

    /**
     * @brief 两条线程同时用同一份后端要当场抛出，而不是把堆写坏
     * @details 判据是「同时在场」而不是「换了线程」：注册推迟到第一次等待才做，同一个后端被两条线程
     *          **先后**使用是既有的良性形态（用例里就有几百次），按属主线程判会误伤一大片。
     *          两条线程各按自己的节奏反复进出后端，占位在场的窗口彼此重叠，被拒的次数按「两边都记」
     *          来判——抢占是双向的，只盯其中一边会漏（实测那种写法在 30 次里红 15 次）。
     */
    TEST(Epoll, RejectsConcurrentUseFromAnotherThread)
    {
        Epoll backend;
        std::atomic<bool> isStopping{false};
        std::atomic<int>  rejectionCount{0};

        // 占位的一路：wait(1) 每次都在内核里停约 1 毫秒，占空比接近九成
        std::thread holder{
                [&backend, &isStopping, &rejectionCount]
                {
                    // 线程入口必须接住：被拒的一方如果让它抛出，整个测试进程当场就没（terminate）
                    try
                    {
                        while (!isStopping.load(std::memory_order_acquire))
                        {
                            static_cast<void>(backend.wait(1));
                        }
                    } catch (const Base::LogicException &)
                    {
                        ++rejectionCount;
                    }
                }};

        // 探路的一路：拿「未注册的描述符只回 false」这个无副作用入口反复敲门，被拒就记账
        std::thread poker{
                [&backend, &isStopping, &rejectionCount]
                {
                    while (!isStopping.load(std::memory_order_acquire))
                    {
                        try
                        {
                            static_cast<void>(backend.delFileDescriptor(0x7FFF));
                        } catch (const Base::LogicException &)
                        {
                            ++rejectionCount;
                        }
                    }
                }};

        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        isStopping.store(true, std::memory_order_release);
        holder.join();
        poker.join();

        EXPECT_GT(rejectionCount.load(), 0)
                << "并发使用没被拒：注册表与待办表都是无锁容器，两个线程同时进只会把堆写坏，"
                   "而报错位置离肇因隔着几层（实测报成另一处 vector 的 negative-size-param）";

        EXPECT_NO_THROW(static_cast<void>(backend.delFileDescriptor(0x7FFF)))
                << "顺序交接也被拒了：本检查只该管「同时在场」";
    }
#endif
} // namespace AsynGyanis::Core
