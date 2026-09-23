// Epoll 单元测试：实例句柄、移动语义、事件注册/修改/移除与超时等待

#include "Core/EventLoop/Epoll.h"
#include "Platform/Platform.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

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
} // namespace AsynGyanis::Core
