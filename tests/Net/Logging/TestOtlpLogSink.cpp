// OTLP 日志 Sink：真回环采集端上的成功交付、5xx 重试、队列上界与丢弃计数

#include "Net/Logging/OtlpLogSink.h"

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"
#include "Net/Tracing/SpanExporter.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 造一条只关心消息的记录（其余字段固定）
        Base::LogEvent makeEvent(std::string message)
        {
            return {Base::LogLevel::Info, std::chrono::system_clock::now(), "tid", Base::SourceLocation::current(), "export-test", std::move(message)};
        }

        /// 采集端替身：按开关回 500 或 200，并记下收到的批数
        struct CollectorState
        {
            std::atomic<bool>          fail{true};   ///< true = 回 500
            std::atomic<std::uint64_t> postCount{0}; ///< 收到的 POST 次数
        };

        /**
         * @brief 造一台只服务 `/v1/logs` 的回环采集端
         * @param state 回 500/200 的开关与批数
         */
        std::unique_ptr<RunningHttpServerFixture> makeCollector(const std::shared_ptr<CollectorState> &state)
        {
            return std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                                              [state](Router &router, Core::EventLoop &)
                                                              {
                                                                  router.post("/v1/logs",
                                                                              [state](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                                              {
                                                                                  state->postCount.fetch_add(1, std::memory_order_relaxed);
                                                                                  response.setStatus(state->fail.load(std::memory_order_acquire) ? 500 : 200);
                                                                                  response.setBody(state->fail.load(std::memory_order_acquire) ? "nope" : "");
                                                                                  co_return;
                                                                              });
                                                              });
        }
    } // namespace

    TEST(OtlpLogSink, DeliversBatchesAfterTheCollectorRecoversFrom5xx)
    {
        auto                                            state   = std::make_shared<CollectorState>();
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCollector(state);
        ASSERT_TRUE(fixture->awaitRunning(kWaitTimeout));

        OtlpLogSink::Configuration configuration;
        configuration.endpoint                  = "http://127.0.0.1:" + std::to_string(fixture->listeningPort()) + "/v1/logs";
        configuration.exportInterval            = std::chrono::milliseconds{50};
        configuration.requestTimeout            = std::chrono::milliseconds{1000};
        configuration.shutdownTimeout           = std::chrono::milliseconds{1000};
        configuration.maximumPendingRecordCount = 8;
        configuration.maximumBatchRecordCount   = 2;
        OtlpLogSink sink(TraceResource{.serviceName = "log-export-test"}, configuration);

        sink.write(makeEvent("第一条"));
        sink.write(makeEvent("第二条"));
        sink.write(makeEvent("第三条"));

        // 采集端还在 500：必须留下失败读数，并且记录不丢
        ASSERT_TRUE(waitForCondition([&sink] { return sink.failedBatchCount() > 0; }, kWaitTimeout)) << "采集端回 500 时没有失败计数";
        EXPECT_EQ(sink.droppedRecordCount(), 0U) << "失败的那批应留在队列里重试，不该直接丢弃";

        // 采集端恢复：重试把它们送达；所有记录最终都要发出去
        state->fail.store(false, std::memory_order_release);
        ASSERT_TRUE(waitForCondition([&sink] { return sink.deliveredBatchCount() > 0 && sink.pendingRecordCount() == 0; }, kWaitTimeout)) << "采集端恢复后队列没有排空";

        EXPECT_GE(state->postCount.load(std::memory_order_relaxed), 2U) << "500 与 200 至少各收到一次 POST";
        EXPECT_EQ(sink.droppedRecordCount(), 0U) << "整段过程中没有记录该被丢弃";
    }

    /**
     * @brief 一批含非法 UTF-8 的记录只计一次丢弃，且不把后面的日志堵在队列里
     * @details 整批成不了 JSON 是确定性失败：放回队列只会每周期再失败一次、重复计丢弃，
     *          而队首那批会一直挡着后面的记录。它必须按「已处置」出队，网络失败才值得重排
     */
    TEST(OtlpLogSink, CountsAMalformedUtf8BatchOnceAndKeepsLaterBatchesMoving)
    {
        auto state = std::make_shared<CollectorState>();
        state->fail.store(false, std::memory_order_release);
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCollector(state);
        ASSERT_TRUE(fixture->awaitRunning(kWaitTimeout));

        OtlpLogSink::Configuration configuration;
        configuration.endpoint                  = "http://127.0.0.1:" + std::to_string(fixture->listeningPort()) + "/v1/logs";
        configuration.exportInterval            = std::chrono::milliseconds{50};
        configuration.requestTimeout            = std::chrono::milliseconds{1000};
        configuration.shutdownTimeout           = std::chrono::milliseconds{1000};
        configuration.maximumPendingRecordCount = 8;
        configuration.maximumBatchRecordCount   = 8;
        OtlpLogSink sink(TraceResource{.serviceName = "log-export-test"}, configuration);

        // "\xC3\x28" 是一个不成立的两字节序列：C3 后面缺延续字节
        sink.write(makeEvent(std::string{"\xC3\x28", 2}));
        ASSERT_TRUE(waitForCondition([&sink] { return sink.droppedRecordCount() >= 1U; }, kWaitTimeout)) << "非法 UTF-8 的批没有按丢弃处置";
        EXPECT_EQ(sink.droppedRecordCount(), 1U) << "一批不可重试的记录只该计一次丢弃";
        EXPECT_GE(sink.failedBatchCount(), 1U);

        sink.write(makeEvent("这一批要照常送出去"));
        ASSERT_TRUE(waitForCondition([&sink] { return sink.deliveredBatchCount() >= 1U && sink.pendingRecordCount() == 0; }, kWaitTimeout))
                << "非法 UTF-8 的那批把后面的日志堵住了：lastFailureReason=" << sink.lastFailureReason();
        EXPECT_EQ(sink.droppedRecordCount(), 1U) << "不可重试的批被反复重排，丢弃数在重复上涨";
        EXPECT_EQ(state->postCount.load(std::memory_order_relaxed), 1U) << "成不了 JSON 的批不该碰到网络";
    }

    TEST(OtlpLogSink, DropsNewRecordsWhenTheBoundedQueueIsFull)
    {
        auto state = std::make_shared<CollectorState>();
        state->fail.store(false, std::memory_order_release);
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCollector(state);
        ASSERT_TRUE(fixture->awaitRunning(kWaitTimeout));

        OtlpLogSink::Configuration configuration;
        configuration.endpoint                  = "http://127.0.0.1:" + std::to_string(fixture->listeningPort()) + "/v1/logs";
        configuration.exportInterval            = std::chrono::milliseconds{10000}; // 长到用例期间不会自己发
        configuration.requestTimeout            = std::chrono::milliseconds{1000};
        configuration.shutdownTimeout           = std::chrono::milliseconds{1000};
        configuration.maximumPendingRecordCount = 1;
        configuration.maximumBatchRecordCount   = 1;
        OtlpLogSink sink(TraceResource{.serviceName = "log-export-test"}, configuration);

        sink.write(makeEvent("一"));
        sink.write(makeEvent("二"));
        sink.write(makeEvent("三"));

        EXPECT_EQ(sink.pendingRecordCount(), 1U) << "队列上界是 1，只能留下第一条";
        EXPECT_EQ(sink.droppedRecordCount(), 2U) << "溢出的新记录应计入丢弃数";
    }
} // namespace AsynGyanis::Net
