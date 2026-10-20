// OTLP 日志 Sink：真回环采集端上的成功交付、5xx 重试、队列上界与丢弃计数

#include "Net/Logging/OtlpLogSink.h"

#include "Base/Log/LogContext.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"
#include "Net/Tracing/SpanExporter.h"

#include "HttpTestSupport.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

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
            mutable std::mutex         bodyMutex;    ///< 保护最后一份请求正文
            std::string                lastBody;     ///< 最后一份请求正文，供属性断言读取

            /**
             * @brief 记下本次请求正文
             * @param body 请求正文
             */
            void recordBody(const std::string_view body)
            {
                const std::lock_guard lock(bodyMutex);
                lastBody.assign(body);
            }

            /**
             * @brief 取最后一份请求正文
             * @return std::string 正文副本
             */
            [[nodiscard]] std::string bodySnapshot() const
            {
                const std::lock_guard lock(bodyMutex);
                return lastBody;
            }
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
                                                                              [state](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                                              {
                                                                                  state->postCount.fetch_add(1, std::memory_order_relaxed);
                                                                                  state->recordBody(request.body());
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

    /**
     * @brief 写线程没有 TLS 上下文时，事件自带的上下文仍要进 OTLP attributes
     * @details 本 Sink 若被 AsyncSink 包裹，write() 跑在工作线程上；此时只能靠事件快照。
     */
    TEST(OtlpLogSink, ExportsEventCarriedContextWhenTheWriterThreadHasNoTls)
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
        configuration.maximumBatchRecordCount   = 1;
        OtlpLogSink sink(TraceResource{.serviceName = "log-export-test"}, configuration);

        Base::LogEvent event = makeEvent("事件自带上下文");
        event.context        = std::make_shared<const Base::LogContext>(std::map<std::string, std::string>{{"trace_id", "event-context-id"}});
        sink.write(std::move(event));

        ASSERT_TRUE(waitForCondition([&sink] { return sink.deliveredBatchCount() > 0 && sink.pendingRecordCount() == 0; }, kWaitTimeout)) << "批次没有送达";
        const std::string body = state->bodySnapshot();
        ASSERT_FALSE(body.empty());

        const nlohmann::json  document   = nlohmann::json::parse(body);
        const nlohmann::json &record     = document.at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords").at(0);
        bool                  sawTraceId = false;
        for (const nlohmann::json &attribute: record.at("attributes"))
        {
            if (attribute.at("key") == "trace_id" && attribute.at("value").at("stringValue") == "event-context-id")
            {
                sawTraceId = true;
            }
        }
        EXPECT_TRUE(sawTraceId) << "事件自带的上下文没有进 OTLP attributes：" << body;
    }
} // namespace AsynGyanis::Net
