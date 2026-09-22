// 日志热路径的分配画像：一条日志在「事件构造」这一段碰几次堆，以及被去掉的那份时间戳文本
// 与那次事件拷贝
//
// 口径与 tests/Net/Http/TestHotPathAllocations.cpp 一致（共用 AllocationProbe）：
//   · 走一条日志（消息 40 字节）：一千次共 1000 次分配，就是消息体那一块；
//   · 时刻渲染进调用方栈缓冲：一千次共 0 次；
//   · 消融对照——把时刻落成 owning 文本：一千次共 1000 次，即本轮从事件里去掉的那一次；
//   · 走异步队列的一行：一千次共 1007 次，多出的七次是槽位数组倍增到配置容量那一串；
//   · 消融对照——把同样的事件投进 std::queue<LogEvent>：一千次共 2007 次，每投一条多取一块
//     136 字节的堆（std::deque 对超过 16 字节的元素按一块一元素分块）。这条读数就是
//     AsyncSink 自己管槽位而不是直接用 std::queue 的依据。
// 分配判据只在 Release 下钉死：Debug 的 STL 迭代器调试代理会给每个容器多挂一块代理，
// 读数被实现细节放大一个量级，钉住它等于把判据交给编译器实现。

#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/LogSink.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        using AsynGyanis::TestSupport::AllocationHistogram;
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kAllocationHistogramBucketBytes;
        using AsynGyanis::TestSupport::kAllocationHistogramBucketCount;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;
        using AsynGyanis::TestSupport::resetAllocationHistogram;
        using AsynGyanis::TestSupport::snapshotAllocationHistogram;

        /// 固定消息：40 字节，长过小串内联，因此「消息体那一块堆」一定会被记到
        constexpr std::string_view kMessageText = "hot path log line payload, above small-string";

        /// 时间戳文本的常规长度，用于自检「消息确实落成了文本」
        constexpr std::uint64_t kTimestampTextCharacters = 23U;

        /**
         * @brief 打印两个直方图快照的逐桶差值（只印非零的那些）
         * @param label 这段读数的标签，走 ASCII 以免控制台代码页吃掉结论
         * @param reference 参照形状的直方图
         * @param current 被测形状的直方图
         */
        void printHistogramDelta(const char *const label, const AllocationHistogram &reference, const AllocationHistogram &current)
        {
            std::printf("%s buckets (width=%llu B):\n", label,
                        static_cast<unsigned long long>(kAllocationHistogramBucketBytes));
            for (std::size_t bucketIndex = 0; bucketIndex < kAllocationHistogramBucketCount; ++bucketIndex)
            {
                // 有符号相减：无符号的下溢会把「少了几次」印成一个巨大的正数，读数就成了假话
                const auto difference = static_cast<long long>(current[bucketIndex])
                                        - static_cast<long long>(reference[bucketIndex]);
                if (difference != 0)
                {
                    std::printf("  size~%4zu  ref=%6llu  cur=%6llu  delta=%+lld\n",
                                bucketIndex * kAllocationHistogramBucketBytes,
                                static_cast<unsigned long long>(reference[bucketIndex]),
                                static_cast<unsigned long long>(current[bucketIndex]),
                                difference);
                }
            }
        }

#ifdef NDEBUG
        /// 一条日志的稳态分配：只有消息体那一次；时间戳文本已改成在调用方缓冲里渲染
        constexpr std::uint64_t kLogLineTotalAllocationsPerThousand = 1000U;

        /// 渲染进栈缓冲：一次堆都不碰
        constexpr std::uint64_t kStackBufferRenderTotalAllocationsPerThousand = 0U;

        /// 消融对照：落成 owning 文本就是每请求一次分配（23 字节长过小串内联的 15）
        constexpr std::uint64_t kOwningTimestampTextTotalAllocationsPerThousand = 1000U;

        /// 异步一条的上限：一千行只多出槽位数组的几何增长，按每 32 行留一次增长余量
        constexpr std::uint64_t kMaximumAsyncLineTotalAllocationsPerThousand = kMeasurementIterations + kMeasurementIterations / 32U;
#endif

        /**
         * @brief 收下事件但不落地的 Sink：把「事件构造」与「格式化加写出」隔开
         * @details 被测的是 Logger 到 Sink 之前那一段。少隔开这一步，一次真实写会把
         *          格式化器与 IO 的分配一起混进读数，量不到本次改动去掉的那一次。
         */
        class NonWritingSink final : public LogSink
        {
        public:
            /**
             * @brief 记下事件规模，不格式化也不写出任何字节
             * @details 重写 LogSink::write()：刻意不调 formatEvent()，因此不产生格式化侧的分配。
             * @param event 日志事件
             */
            void write(const LogEvent &event) override
            {
                m_writeCount += 1U;
                m_lastMessageSize = event.message.size();
            }

            /**
             * @brief 空实现：本 Sink 没有缓冲需要刷
             * @details 重写 LogSink::flush()：不触任何 IO，读数才只反映事件构造侧。
             */
            void flush() override {}

            /// 收到的事件条数，用于证明测量体确实跑满了
            [[nodiscard]] std::uint64_t writeCount() const noexcept { return m_writeCount; }

            /// 最后一条事件的消息字节数，用于自检「消息长过了小串内联」
            [[nodiscard]] std::size_t lastMessageSize() const noexcept { return m_lastMessageSize; }

        private:
            std::uint64_t m_writeCount     = 0U;     ///< 已收到的事件条数
            std::size_t   m_lastMessageSize = 0U;    ///< 最后一条事件的消息字节数
        };

        /**
         * @brief 第一条事件就把工作线程停住的 Sink
         * @details 测量窗口里队列只增不减，std::deque 的分块次数才是确定的；不这样停住的话，
         *          工作线程边收边放，分块的取还次数随线程交错而变，读数就只剩噪声。
         *          闸门抬起后不再等待，收尾时一千条会很快排空，不会拖住析构。
         */
        class WorkerParkingSink final : public LogSink
        {
        public:
            /**
             * @param released 放行闸门，由用例在测量结束后置位
             */
            explicit WorkerParkingSink(std::shared_ptr<std::atomic<bool>> released) :
                m_released(std::move(released))
            {
            }

            /**
             * @brief 在闸门抬起前原地等住
             * @details 重写 LogSink::write()：不记录也不写出，唯一作用是把工作线程钉在这里，
             *          让事件全部留在队列里。
             * @param event 日志事件（刻意不读）
             */
            void write(const LogEvent &event) override
            {
                (void) event;
                while (!m_released->load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }

            /**
             * @brief 空实现：本 Sink 没有缓冲可刷
             * @details 重写 LogSink::flush()：不触 IO。
             */
            void flush() override {}

        private:
            std::shared_ptr<std::atomic<bool>> m_released; ///< 放行闸门，抬起前 write() 不返回
        };
    } // namespace

    /**
     * @brief 走一条日志要付多少次分配
     * @details 钉的是「事件构造侧只剩消息体那一块」：时刻只带 8 字节的 time_point，
     *          名字与线程号都是共享值，因此本形状多出来的那一次都算回归。
     */
    TEST(LogHotPathAllocations, SingleLogLineAllocations)
    {
        Logger logger("hot_path");
        auto sink = std::make_unique<NonWritingSink>();
        NonWritingSink &observedSink = *sink;
        logger.addSink(std::move(sink));

        const auto logOnce = [&logger]
        {
            logger.log(LogLevel::Info, kMessageText);
            return 1U;
        };
        logOnce();
        ASSERT_GT(observedSink.lastMessageSize(), 15U) << "消息短过了小串内联，这条读数对应的形状就不对";

        const AllocationProfile profile = measurePerOperation(logOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没走到 Sink";
        EXPECT_EQ(observedSink.writeCount(), kMeasurementIterations + 1U);
        std::printf("log-line 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kLogLineTotalAllocationsPerThousand)
                << "一条日志又开始多碰堆了：时间戳文本还是消息体？";
#endif
    }

    /**
     * @brief 把时刻渲染进调用方缓冲，本身付零次分配
     * @details 这是本轮改动的正面断言：历法换算按秒缓存在调用线程，文本落在栈缓冲里，
     *          因此逐条渲染既不取堆也不重复折日历。
     */
    TEST(LogHotPathAllocations, RenderIntoStackBufferIsAllocationFree)
    {
        std::array<char, kTimestampTextBufferSize> buffer{};

        const auto renderOnce = [&buffer]
        {
            return formatTimestampText(buffer, std::chrono::system_clock::now()).size();
        };
        ASSERT_EQ(renderOnce(), kTimestampTextCharacters) << "渲染出的文本长度变了，这条读数对应的形状就不对";

        const AllocationProfile profile = measurePerOperation(renderOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * kTimestampTextCharacters);
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kStackBufferRenderTotalAllocationsPerThousand)
                << "时刻渲染开始碰堆了：缓存没命中，还是缓冲被换成了 owning 串？";
#endif
    }

    /**
     * @brief 消融对照：时刻落成一份 owning 文本就是每行一次分配
     * @details 与上一条同语料、同运算，只多一次「把视图拷成字符串」——被本轮从事件里去掉的
     *          正是这一次。两例一起看才说明省下来的是那一次堆块，而不是少做了事。
     */
    TEST(LogHotPathAllocations, TimestampTextAsOwningStringCostsOneAllocation)
    {
        std::array<char, kTimestampTextBufferSize> buffer{};

        const auto renderOwning = [&buffer]
        {
            return std::string{formatTimestampText(buffer, std::chrono::system_clock::now())}.size();
        };
        ASSERT_EQ(renderOwning(), kTimestampTextCharacters);

        const AllocationProfile profile = measurePerOperation(renderOwning);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * kTimestampTextCharacters);
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kOwningTimestampTextTotalAllocationsPerThousand)
                << "owning 文本那一次分配没了，说明对照形状少做了事，这条判据也就失去意义";
#endif
    }

    /**
     * @brief 走异步队列那条路的分配读数
     * @details 同步一条是 1000 次 / 48000 字节（只有消息体那一块），异步一条的上限钉在
     *          「一千行多不超过 1/32 次」：超出即说明投递又在逐条取堆，队列容器退化成了
     *          每个元素一块的形状（对照 QueueContainerAllocationReading 的读数）。
     */
    TEST(LogHotPathAllocations, AsyncLogLineAllocationReading)
    {
        const auto released = std::make_shared<std::atomic<bool>>(false);

        // 参照形状：同一个 Logger 只接一个不落地的 Sink，即同步一条。两条读数逐桶相减，
        // 差出来的那一桶就是异步这条路多付的分配，不用再靠总账反推
        Logger referenceLogger("async_ref");
        referenceLogger.addSink(std::make_unique<NonWritingSink>());
        const auto logReferenceOnce = [&referenceLogger]
        {
            referenceLogger.log(LogLevel::Info, kMessageText);
            return 1U;
        };
        resetAllocationHistogram();
        const AllocationProfile referenceProfile = measurePerOperation(logReferenceOnce);
        const AllocationHistogram referenceHistogram = snapshotAllocationHistogram();

        Logger logger("async_path");
        logger.addSink(std::make_unique<AsyncSink>(std::make_unique<WorkerParkingSink>(released),
                                                  kMeasurementIterations * 4U,
                                                  AsyncSink::OverflowPolicy::Block));

        const auto logOnce = [&logger]
        {
            logger.log(LogLevel::Info, kMessageText);
            return 1U;
        };
        // 先把工作线程钉在第一条上，测量窗口内队列只进不出
        logOnce();
        resetAllocationHistogram();
        const AllocationProfile profile = measurePerOperation(logOnce);
        const AllocationHistogram asyncHistogram = snapshotAllocationHistogram();

        released->store(true, std::memory_order_release);

        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没走进异步队列";
        EXPECT_EQ(referenceProfile.totalAllocations, referenceHistogram[0U] + referenceHistogram[1U] + referenceHistogram[2U] + referenceHistogram[3U])
                << "直方图的桶没兜住同步参照的分配形状，下面的差值就没有意义";
#ifdef NDEBUG
        EXPECT_LE(profile.totalAllocations, kMaximumAsyncLineTotalAllocationsPerThousand)
                << "投递一条日志又在逐条取堆：队列容器退化成了每元素一块的分块";
#endif

        // 标签走 ASCII：控制台代码页会把中文读数弄成乱码，取不到数就白跑一轮
        std::printf("async-line per-op=%llu total=%llu bytes=%llu\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        printHistogramDelta("async-minus-sync", referenceHistogram, asyncHistogram);
    }

    /**
     * @brief 读数：走控制台 Sink 的一条日志在稳态下取几次堆
     * @details 格式化器交回的是长度恰等于内容的串，直接给它追加换行必然再取一块堆并把整行搬一次，
     *          而且这一切在控制台 Sink 的互斥锁内。改成搬进留容量的成员行缓冲后，稳态下拼行不碰堆：
     *          一千行只该剩格式化器那一次。控制台被重定向到内存缓冲，读数里不含 IO 本身
     */
    TEST(LogHotPathAllocations, ConsoleSinkLineAllocationReading)
    {
        std::ostringstream captured;
        auto *const originalBuffer = std::cout.rdbuf(captured.rdbuf());

        // 参照形状：同一条日志只走到「事件构造」为止，格式化与落地都不计。两条读数相减，
        // 差出来的就是「格式化 + 写控制台」这一段付的分配
        Logger referenceLogger("console_ref");
        referenceLogger.addSink(std::make_unique<NonWritingSink>());
        const auto logReferenceOnce = [&referenceLogger]
        {
            referenceLogger.log(LogLevel::Info, kMessageText);
            return 1U;
        };
        resetAllocationHistogram();
        const AllocationProfile referenceProfile = measurePerOperation(logReferenceOnce);

        Logger logger("console_path");
        logger.addSink(std::make_unique<ConsoleSink>(false));

        const auto logOnce = [&logger]
        {
            logger.log(LogLevel::Info, kMessageText);
            return 1U;
        };
        logOnce();                       // 先让行缓冲把容量长出来，测的是稳态
        resetAllocationHistogram();
        const AllocationProfile profile = measurePerOperation(logOnce);

        std::cout.rdbuf(originalBuffer);

        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没走进控制台 Sink";
#ifdef NDEBUG
        // 上限给 1/64 的增长余量：行缓冲第一次长容量那几次要取堆，具体几次取决于实现的分因子，
        // 不钉死。真正的判据是「整段格式化 + 写出不超过事件构造那一段」——未修时这一千行多付
        // 一千次（std::format 造结果串），远超本上限
        EXPECT_LE(profile.totalAllocations, referenceProfile.totalAllocations + kMeasurementIterations / 64U)
                << "拼那一行还在逐条取堆：版式没有直接落进 Sink 的行缓冲";
#endif
        std::printf("console-line per-op=%llu total=%llu bytes=%llu (ref total=%llu)\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes),
                    static_cast<unsigned long long>(referenceProfile.totalAllocations));
    }

    /**
     * @brief 消融对照：异步队列那个容器本身每次投递碰几次堆
     * @details 把 std::queue<LogEvent> 单独拿出来连推一千次，与「同步一条」的直方图相减。
     *          若那多出来的一千块落在这里，就不是 Logger 或 AsyncSink 复制了事件，而是容器
     *          的分块策略为一块装不下的元素反复取堆。
     */
    TEST(LogHotPathAllocations, QueueContainerAllocationReading)
    {
        // 先问容器能不能「搬」事件：不能 nothrow 移动的话，任何按几何增长的头寸都会在扩容时
        // 逐个深拷贝（std::vector 用 move_if_noexcept），这条断言把前提钉住
        static_assert(std::is_nothrow_move_constructible_v<LogEvent>, "LogEvent 的移动构造必须 nothrow，否则容器扩容会深拷贝");
        static_assert(std::is_nothrow_move_assignable_v<LogEvent>, "LogEvent 的移动赋值必须 nothrow，否则槽位复用会深拷贝");

        std::printf("sizeof(LogEvent)=%zu\n", sizeof(LogEvent));

        Logger referenceLogger("queue_ref");
        referenceLogger.addSink(std::make_unique<NonWritingSink>());
        const auto logReferenceOnce = [&referenceLogger]
        {
            referenceLogger.log(LogLevel::Info, kMessageText);
            return 1U;
        };
        resetAllocationHistogram();
        const AllocationProfile referenceProfile = measurePerOperation(logReferenceOnce);
        const AllocationHistogram referenceHistogram = snapshotAllocationHistogram();
        EXPECT_EQ(referenceProfile.resultSum, kMeasurementIterations);

        // 与 AsyncSink 里的成员同型：std::queue<LogEvent>（底层 std::deque）。
        // 构造事件的那几个共享量都留在循环外，每轮只多付「消息拷一份」，与同步参照同形，
        // 差值才只反映容器自己的行为
        const std::shared_ptr<const std::string> loggerNameSnapshot = std::make_shared<const std::string>("queue_ref");
        const std::shared_ptr<const std::string> threadIdSnapshot   = threadIdString();
        const std::string                        messageText{kMessageText};

        std::queue<LogEvent> queue;
        queue.push(LogEvent{LogLevel::Info, TimestampMoment{}, threadIdSnapshot, SourceLocation{}, loggerNameSnapshot, messageText});

        const auto pushOnce = [&queue, &loggerNameSnapshot, &threadIdSnapshot, &messageText]
        {
            queue.push(LogEvent{LogLevel::Info, TimestampMoment{}, threadIdSnapshot, SourceLocation{}, loggerNameSnapshot, messageText});
            return 1U;
        };
        resetAllocationHistogram();
        const AllocationProfile profile = measurePerOperation(pushOnce);
        const AllocationHistogram queueHistogram = snapshotAllocationHistogram();

        EXPECT_EQ(profile.resultSum, kMeasurementIterations);
        std::printf("queue-push per-op=%llu total=%llu bytes=%llu\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        printHistogramDelta("queue-minus-sync", referenceHistogram, queueHistogram);
    }
} // namespace AsynGyanis::Base
