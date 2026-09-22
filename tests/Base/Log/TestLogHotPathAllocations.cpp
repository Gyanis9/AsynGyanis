// 日志热路径的分配画像：一条日志在「事件构造」这一段碰几次堆，以及被去掉的那份时间戳文本
// 与那次事件拷贝
//
// 口径与 tests/Net/Http/TestHotPathAllocations.cpp 一致（共用 AllocationProbe）：
//   · 走一条日志（消息 40 字节）：一千次共 1000 次分配，就是消息体那一块；
//   · 时刻渲染进调用方栈缓冲：一千次共 0 次；
//   · 消融对照——把时刻落成 owning 文本：一千次共 1000 次，即本轮从事件里去掉的那一次；
//   · 走异步队列的一行：只报读数不钉阈值（见该用例的 @details，多出来的一千次尚未定位）。
// 分配判据只在 Release 下钉死：Debug 的 STL 迭代器调试代理会给每个容器多挂一块代理，
// 读数被实现细节放大一个量级，钉住它等于把判据交给编译器实现。

#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/LogSink.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

        /// 固定消息：40 字节，长过小串内联，因此「消息体那一块堆」一定会被记到
        constexpr std::string_view kMessageText = "hot path log line payload, above small-string";

        /// 时间戳文本的常规长度，用于自检「消息确实落成了文本」
        constexpr std::uint64_t kTimestampTextCharacters = 23U;

#ifdef NDEBUG
        /// 一条日志的稳态分配：只有消息体那一次；时间戳文本已改成在调用方缓冲里渲染
        constexpr std::uint64_t kLogLineTotalAllocationsPerThousand = 1000U;

        /// 渲染进栈缓冲：一次堆都不碰
        constexpr std::uint64_t kStackBufferRenderTotalAllocationsPerThousand = 0U;

        /// 消融对照：落成 owning 文本就是每请求一次分配（23 字节长过小串内联的 15）
        constexpr std::uint64_t kOwningTimestampTextTotalAllocationsPerThousand = 1000U;
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
     * @brief 走异步队列那条路的分配读数（先记录，不钉阈值）
     * @details 工作线程停在第一条上，队列只进不出，因此读数是稳的：一千行 2007 次 / 200334 字节，
     *          而同步那条是 1000 次 / 48000 字节。对账下来多出来的是「约一千次 48 字节」与
     *          「约 63 次 2432 字节」——后者是 std::deque 的分块（16 个事件一块），前者是一块
     *          与消息体同尺寸的分配，即异步那条仍然每行多复制了一次消息。
     * @warning 那第二次复制尚未定位（接管在 Logger 与 AsyncSink 两处都按行为验过），定位之前
     *          不钉阈值：把上限放宽到能通过，等于把判据换成一句好话。
     */
    TEST(LogHotPathAllocations, AsyncLogLineAllocationReading)
    {
        const auto released = std::make_shared<std::atomic<bool>>(false);

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
        const AllocationProfile profile = measurePerOperation(logOnce);

        released->store(true, std::memory_order_release);

        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没走进异步队列";
        // 标签走 ASCII：控制台代码页会把中文读数弄成乱码，取不到数就白跑一轮
        std::printf("async-line per-op=%llu total=%llu bytes=%llu\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
    }
} // namespace AsynGyanis::Base
