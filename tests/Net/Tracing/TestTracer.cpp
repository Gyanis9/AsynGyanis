// Tracer 用例：采样判定、父子关系、攒批与出口计数的每一条承诺
//
// 这里的时序条件都是用例自己造出来的：需要「有一批正卡在出口里」时用会停住的出口替身，
// 并等到它确实进场之后再断言；需要「出口线程不要自己动」时把出口时限设成一分钟，
// 交付一律由 flush() 驱动。

#include "Net/Tracing/Tracer.h"

#include "Base/Exception/LogicException.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "MetricsTestSupport.h"
#include "Net/Http/TraceContext.h"
#include "Net/Tracing/Span.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using AsynGyanis::Net::Span;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::SpanRecord;
    using AsynGyanis::Net::SpanStatusCode;
    using AsynGyanis::Net::TraceIdentifiers;
    using AsynGyanis::Net::Traceparent;
    using AsynGyanis::Net::Tracer;
    using AsynGyanis::Net::TestSupport::CapturingSpanExporter;

    /// 出口时限在绝大多数用例里设成一分钟：交付只由 flush() 驱动，免得断言与出口线程抢顺序
    constexpr std::chrono::milliseconds kNoTimedFlush{60000};

    /**
     * @brief 造一份最小可用配置
     * @param sampleRatio 新链路的采样比例
     * @return Tracer::Configuration 服务名固定，批量给到 1（一次受理就叫醒出口线程）
     */
    Tracer::Configuration makeConfiguration(const double sampleRatio = 1.0)
    {
        Tracer::Configuration configuration;
        configuration.serviceName          = "unit-test-service";
        configuration.sampleRatio          = sampleRatio;
        configuration.exportBatchSpanCount = 1U;
        configuration.exportInterval       = kNoTimedFlush;
        return configuration;
    }

    /// 收口一条节并交给编排器
    void recordOneSpan(const std::shared_ptr<Tracer> &tracer, const std::string &name)
    {
        Span span = tracer->startSpan(name, SpanKind::Internal);
        span.finish();
    }
} // namespace

/**
 * @brief 被采的根节整条落到出口：标识、名字、角色、维度顺序与结局都不该在路上丢
 */
TEST(Tracer, SampledRootSpanReachesTheExporterWithItsIdentity)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    {
        Span span = tracer->startSpan("root-operation", SpanKind::Server);
        ASSERT_TRUE(span.isRecording());
        span.setAttribute("http.request.method", std::string_view{"GET"});
        span.setAttribute("http.response.status_code", std::int64_t{200});
        span.setStatus(SpanStatusCode::Ok);
        EXPECT_EQ(span.name(), "root-operation");
    }
    tracer->flush();

    const std::vector<SpanRecord> records = exporter->records();
    ASSERT_EQ(records.size(), 1U);
    const SpanRecord &record = records.front();
    EXPECT_EQ(record.name, "root-operation");
    EXPECT_EQ(record.kind, SpanKind::Server);
    EXPECT_EQ(record.identity.parentSpanIdText().size(), 0U) << "没有上游的节就是根节，parentSpanId 要为空";
    EXPECT_EQ(record.identity.traceIdText().size(), AsynGyanis::Net::kTraceIdHexDigitCount);
    EXPECT_EQ(record.identity.spanIdText().size(), AsynGyanis::Net::kSpanIdHexDigitCount);
    // 标识必须是「只有小写十六进制」且「不是全零」：W3C 把全零判为非法值，出口侧会整条拒收
    EXPECT_EQ(record.identity.traceIdText().find_first_not_of("0123456789abcdef"), std::string_view::npos) << record.identity.traceIdText();
    EXPECT_NE(record.identity.traceIdText(), std::string(AsynGyanis::Net::kTraceIdHexDigitCount, '0'));
    EXPECT_EQ(record.status, SpanStatusCode::Ok);
    ASSERT_EQ(record.attributes.size(), 2U);
    // 维度按写入顺序报出：先方法后状态码，两处各写一次顺序就对了
    EXPECT_EQ(record.attributes[0].key, "http.request.method");
    EXPECT_EQ(record.attributes[1].key, "http.response.status_code");
    EXPECT_EQ(std::get<std::int64_t>(record.attributes[1].value), 200);
    EXPECT_EQ(record.droppedAttributeCount, 0U);
    EXPECT_GE(record.duration.count(), 0) << "时长由单调钟量得，不该出现负数";
    EXPECT_EQ(exporter->lastResourceName(), "unit-test-service");
    EXPECT_EQ(tracer->exportedSpanCount(), 1U);
    EXPECT_EQ(tracer->droppedSpanCount(), 0U);
}

/**
 * @brief 子节沿用上游的链路标识、把上游的段标识当上一节，并且照原样带着未定义的标志位与高版本号
 */
TEST(Tracer, ChildSpanInheritsTraceAndParentAndPreservesUnknownFlagBits)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    TraceIdentifiers parent = Traceparent::generate(true);
    parent.flags |= 0x02U; // 未定义的标志位：规范要求原样传递，不许就地清掉
    parent.version = 1U;   // 比本实现更高的版本号

    Span child = tracer->startSpan("child-operation", SpanKind::Client, parent);
    ASSERT_TRUE(child.isRecording());
    // identifiers() 交出的是「给下一跳看的上下文」：段标识位置放的必须是本节的标识
    EXPECT_EQ(child.identifiers().traceIdText(), parent.traceIdText());
    EXPECT_EQ(child.identifiers().parentIdText(), child.identity().spanIdText());
    EXPECT_NE(child.identifiers().parentIdText(), parent.parentIdText());
    EXPECT_EQ(child.identifiers().flags, parent.flags);
    EXPECT_EQ(child.identifiers().version, parent.version);
    EXPECT_TRUE(child.identifiers().isSampled());
    child.finish();

    // 孙节的上一节必须是子节，而不是最初那个上游
    Span grandchild = tracer->startSpan("grandchild-operation", SpanKind::Internal, child.identifiers());
    grandchild.finish();
    tracer->flush();

    const std::vector<SpanRecord> records = exporter->records();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].identity.parentSpanIdText(), parent.parentIdText());
    EXPECT_EQ(records[1].identity.parentSpanIdText(), records[0].identity.spanIdText());
    EXPECT_EQ(records[1].identity.traceIdText(), records[0].identity.traceIdText());
    EXPECT_NE(records[0].identity.spanIdText(), records[1].identity.spanIdText()) << "同一条链路上两节的标识不该相同";
}

/**
 * @brief 上游判定为不采时，本节是非记录的替身：什么也不出口，但仍带着可继续传播的上下文
 */
TEST(Tracer, UnsampledParentYieldsNonRecordingChildAndExportsNothing)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    TraceIdentifiers parent = Traceparent::generate(false);
    Span             child  = tracer->startSpan("child-operation", SpanKind::Server, parent);
    EXPECT_FALSE(child.isRecording());
    // 写入全是空操作，但上下文照旧能往下传：下游要看见「这条链路没人采」而不是「没有上文」
    child.setAttribute("dropped", std::string_view{"ignored"});
    child.setStatus(SpanStatusCode::Error, "ignored too");
    EXPECT_TRUE(child.name().empty());
    EXPECT_EQ(child.identifiers().traceIdText(), parent.traceIdText());
    EXPECT_FALSE(child.identifiers().isSampled());
    child.finish();

    tracer->flush();
    EXPECT_EQ(exporter->recordCount(), 0U);
    EXPECT_EQ(tracer->droppedSpanCount(), 0U) << "没采的节压根没进缓冲，不该算成丢失";
}

/**
 * @brief 收口两次只交一份成品；收口之后的节不再记录
 */
TEST(Tracer, FinishingTwiceDeliversOnlyOneRecord)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    Span span = tracer->startSpan("double-finish", SpanKind::Internal);
    span.finish();
    EXPECT_FALSE(span.isRecording());
    span.setAttribute("late", std::int64_t{1});
    span.finish();
    tracer->flush();

    EXPECT_EQ(exporter->recordCount(), 1U);
    EXPECT_TRUE(exporter->records().front().attributes.empty()) << "收口之后再写的维度不该出现在成品里";
}

/**
 * @brief 没显式收口的节走出作用域也会交出去：处理器的多条 return 不必各写一次 finish()
 */
TEST(Tracer, DestructorOfAnUnfinishedSpanStillDeliversIt)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    {
        Span span = tracer->startSpan("no-explicit-finish", SpanKind::Internal);
        span.setAttribute("only", std::string_view{"one"});
    }
    tracer->flush();

    const std::vector<SpanRecord> records = exporter->records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].name, "no-explicit-finish");
    ASSERT_EQ(records[0].attributes.size(), 1U);
    EXPECT_EQ(records[0].attributes[0].key, "only");
}

/**
 * @brief 移动之后只有新主能收口：源对象变成一个不会重复交货的空壳
 */
TEST(Tracer, MovingASpanLeavesTheSourceUnableToDeliver)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    Span original = tracer->startSpan("moved-operation", SpanKind::Internal);
    Span moved{std::move(original)};
    EXPECT_FALSE(original.isRecording());
    EXPECT_TRUE(moved.isRecording());
    original.finish();
    moved.finish();
    tracer->flush();

    EXPECT_EQ(exporter->recordCount(), 1U);
    EXPECT_EQ(exporter->records().front().name, "moved-operation");
}

/**
 * @brief 比例两端：0.0 一条都不记，1.0 一条不落
 */
TEST(Tracer, RatioOfZeroRecordsNothingAndRatioOfOneRecordsEverything)
{
    for (const double ratio: {0.0, 1.0})
    {
        const auto exporter = std::make_shared<CapturingSpanExporter>();
        const auto tracer   = Tracer::create(makeConfiguration(ratio));
        tracer->addExporter(exporter);

        for (int index = 0; index < 50; ++index)
        {
            recordOneSpan(tracer, "sampled-probe");
        }
        tracer->flush();
        EXPECT_EQ(exporter->recordCount(), ratio == 0.0 ? 0U : 50U) << "比例 " << ratio << " 两端都该是确定的";
    }
}

/**
 * @brief 中间比例真的在两边之间切，且「记不记」与「采样位清没清」逐条对得上
 * @details 断言只要求「既有采的也有不采的」：这是不随调度与随机种子漂移的判定，
 *          而一致性那条（记下的条数 == 标志位说「采了」的条数）才是本次要钉的规矩。
 */
TEST(Tracer, HalfRatioBothSamplesAndDropsAndClearsTheFlagForDroppedOnes)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration(0.5));
    tracer->addExporter(exporter);

    std::size_t reportingSampled = 0U;
    for (int index = 0; index < 200; ++index)
    {
        Span span = tracer->startSpan("half-ratio", SpanKind::Internal);
        // 采样位是本端判定结果的唯一对外表达：非记录的替身必须清掉这一位
        if (span.identifiers().isSampled())
        {
            ++reportingSampled;
        }
        span.finish();
    }
    tracer->flush();

    const std::size_t recorded = exporter->recordCount();
    EXPECT_GT(recorded, 0U);
    EXPECT_LT(recorded, 200U);
    EXPECT_EQ(recorded, reportingSampled) << "记下的节与「采样位为真」的节必须是同一批";
}

/**
 * @brief 缓冲到上界就丢新到的并计数：出口卡在磁盘上时内存有用，而不是无限攒
 */
TEST(Tracer, PendingBufferDropsBeyondItsBound)
{
    auto configuration                    = makeConfiguration();
    configuration.maximumPendingSpanCount = 2U;
    const auto exporter                   = std::make_shared<CapturingSpanExporter>(true /* 第一批卡在出口里 */);
    const auto tracer                     = Tracer::create(configuration);
    tracer->addExporter(exporter);

    recordOneSpan(tracer, "in-flight");
    // 等这一批确实被取走（pending 归零）之后再往下灌，「装得下几条」才是确定的
    ASSERT_TRUE(exporter->waitUntilBatchEntered()) << "出口线程没有把第一批取走";

    // 空出来的 2 格装下 in-buffer-1/2，其后两条撞上界
    recordOneSpan(tracer, "in-buffer-1");
    recordOneSpan(tracer, "in-buffer-2");
    recordOneSpan(tracer, "overflow-1");
    recordOneSpan(tracer, "overflow-2");

    EXPECT_EQ(tracer->pendingSpanCount(), 2U);
    EXPECT_EQ(tracer->droppedSpanCount(), 2U);
    EXPECT_EQ(exporter->recordCount(), 0U) << "出口还卡在第一批里";

    exporter->release();
    tracer->flush();
    EXPECT_EQ(exporter->recordCount(), 3U);
    EXPECT_EQ(tracer->exportedSpanCount(), 3U);
    EXPECT_EQ(tracer->exportedSpanCount() + tracer->droppedSpanCount(), 5U) << "五条的账要么交出去要么计成丢弃，不能有第三种下落";
}

/**
 * @brief flush() 等的是「本次受理的都处理完」，不是「缓冲空了」：在途那一批也要算进来
 */
TEST(Tracer, FlushWaitsForTheBatchInFlight)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>(true);
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    recordOneSpan(tracer, "in-flight");
    ASSERT_TRUE(exporter->waitUntilBatchEntered()) << "出口线程没有把第一批取走";

    // 此刻缓冲是空的，但那一批还没落地：等「缓冲空」的实现会当场返回，用例就在骗自己
    std::atomic<bool> hasFlushReturned{false};
    std::thread       flushThread(
            [&hasFlushReturned, tracer]
            {
                tracer->flush();
                hasFlushReturned.store(true, std::memory_order_release);
            });
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    EXPECT_FALSE(hasFlushReturned.load(std::memory_order_acquire)) << "在途批次还没落地，flush() 不该返回";

    exporter->release();
    flushThread.join();
    EXPECT_TRUE(hasFlushReturned.load(std::memory_order_acquire));
    EXPECT_EQ(exporter->recordCount(), 1U);
}

/**
 * @brief 一个出口都没挂时：节照样被丢掉并计数，不会把缓冲一直占着
 */
TEST(Tracer, TracerWithoutExporterCountsEverythingDropped)
{
    const auto tracer = Tracer::create(makeConfiguration());
    EXPECT_EQ(tracer->exporterCount(), 0U);

    for (int index = 0; index < 5; ++index)
    {
        recordOneSpan(tracer, "orphan");
    }
    tracer->flush();

    EXPECT_EQ(tracer->droppedSpanCount(), 5U);
    EXPECT_EQ(tracer->exportedSpanCount(), 0U);
    EXPECT_EQ(tracer->pendingSpanCount(), 0U) << "没人要的节该被清出去，白占着缓冲";
}

/**
 * @brief 多个出口各看同一批：一路拒收不影响另一路，两本账各记各的
 */
TEST(Tracer, OneFailingExporterDoesNotStopTheOther)
{
    const auto good = std::make_shared<CapturingSpanExporter>();
    const auto bad  = std::make_shared<CapturingSpanExporter>();
    // 批量设得比本次要交的条数大：三条节因此一定合成一次交付，「失败按批计」才是个确定的数
    auto configuration                 = makeConfiguration();
    configuration.exportBatchSpanCount = 100U;
    const auto tracer                  = Tracer::create(configuration);
    tracer->addExporter(good);
    tracer->addExporter(bad);
    bad->configureRejecting();
    EXPECT_EQ(tracer->exporterCount(), 2U);

    for (int index = 0; index < 3; ++index)
    {
        recordOneSpan(tracer, "two-exporters");
    }
    tracer->flush();

    EXPECT_EQ(good->recordCount(), 3U);
    EXPECT_EQ(tracer->exportedSpanCount(), 3U) << "收下这一批的那个出口计一次";
    EXPECT_EQ(tracer->droppedSpanCount(), 3U) << "拒收的那个出口把这批记成丢弃";
    EXPECT_EQ(tracer->exportFailureCount(), 1U) << "失败按批计，不按条计";
}

/**
 * @brief 三本账同时挂在 `/metrics` 上，且与 C++ 读数是同一份增量；实例放手后把手要跟着注销
 * @details 句柄通常攥在业务内部，运维面上原本问不出「丢了多少节、几批被出口拒了」，而丢节正是采样配错、
 *          缓冲配小、出口挂掉这三种现场的共同症状。注册表是进程级求和，同一二进制里别的 Tracer 也在灌，
 *          所以这里比的是**同一趟操作两侧的增量**而不是绝对值。
 * @details 放手后名字要退回三条：把手没注销就留下一条永远读不到东西的死指标，而按名字抓的人分不出
 *          「没发生」与「没人登记」。
 */
TEST(Tracer, SpanAccountingIsAlsoExposedOnMetrics)
{
    using AsynGyanis::Core::ProcessMetricsRegistry;
    using AsynGyanis::TestSupport::hasRegistrySample;
    using AsynGyanis::TestSupport::registryValue;

    const std::size_t   namesBefore    = ProcessMetricsRegistry::nameCount();
    const std::uint64_t exportedBefore = registryValue("asyn_tracing_exported_spans_total");
    const std::uint64_t droppedBefore  = registryValue("asyn_tracing_dropped_spans_total");
    const std::uint64_t failuresBefore = registryValue("asyn_tracing_export_failures_total");

    {
        auto configuration                 = makeConfiguration();
        configuration.exportBatchSpanCount = 100U; ///< 三条节合成一次交付，「按批计」才是个确定的数
        const auto good                    = std::make_shared<CapturingSpanExporter>();
        const auto bad                     = std::make_shared<CapturingSpanExporter>();
        const auto tracer                  = Tracer::create(configuration);
        tracer->addExporter(good);
        tracer->addExporter(bad);
        bad->configureRejecting();

        ASSERT_TRUE(hasRegistrySample("asyn_tracing_exported_spans_total")) << "已交付那本账没接进 /metrics";
        ASSERT_TRUE(hasRegistrySample("asyn_tracing_dropped_spans_total")) << "丢弃那本账没接进 /metrics";
        ASSERT_TRUE(hasRegistrySample("asyn_tracing_export_failures_total")) << "整批没收那本账没接进 /metrics";

        for (int index = 0; index < 3; ++index)
        {
            recordOneSpan(tracer, "exposed");
        }
        tracer->flush();

        ASSERT_EQ(tracer->exportedSpanCount(), 3U);
        ASSERT_EQ(tracer->droppedSpanCount(), 3U);
        ASSERT_EQ(tracer->exportFailureCount(), 1U);
        EXPECT_EQ(registryValue("asyn_tracing_exported_spans_total") - exportedBefore, tracer->exportedSpanCount()) << "运维面读到的已交付数与业务面问到的不是同一份账";
        EXPECT_EQ(registryValue("asyn_tracing_dropped_spans_total") - droppedBefore, tracer->droppedSpanCount()) << "丢节在 /metrics 上不涨：告警永远不会有依据";
        EXPECT_EQ(registryValue("asyn_tracing_export_failures_total") - failuresBefore, tracer->exportFailureCount()) << "按批计的那本账没接出来";
    }

    EXPECT_EQ(ProcessMetricsRegistry::nameCount(), namesBefore) << "Tracer 放手后把手没注销，会留下三条死指标";
}

/**
 * @brief 析构要把残留送出去并叫一声 shutdown：正常退出时最后那批链路最需要看
 */
TEST(Tracer, DestructorExportsWhatIsStillPending)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    auto       tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    // 批量设得比待交的多，且不用 flush()：交付全靠析构那条路
    recordOneSpan(tracer, "pending-1");
    recordOneSpan(tracer, "pending-2");
    tracer.reset();

    EXPECT_EQ(exporter->recordCount(), 2U);
    EXPECT_TRUE(exporter->isShutdown());
}

/**
 * @brief 配置不成立时逐条拒绝，且每条都说得出是哪个字段
 */
TEST(Tracer, RejectedConfigurationNamesTheField)
{
    struct Case
    {
        Tracer::Configuration configuration;
        std::string           expectedFragment;
    };

    const std::vector<Case> cases = {
            []
            {
                auto configuration        = makeConfiguration();
                configuration.serviceName = std::string{};
                return Case{configuration, "服务名为空"};
            }(),
            [] { return Case{makeConfiguration(std::numeric_limits<double>::quiet_NaN()), "采样比例"}; }(),
            [] { return Case{makeConfiguration(1.5), "采样比例"}; }(),
            [] { return Case{makeConfiguration(-0.5), "采样比例"}; }(),
            []
            {
                auto configuration                    = makeConfiguration();
                configuration.maximumPendingSpanCount = 0U;
                return Case{configuration, "条数上界"};
            }(),
            []
            {
                auto configuration                 = makeConfiguration();
                configuration.exportBatchSpanCount = 0U;
                return Case{configuration, "出口批量"};
            }(),
            []
            {
                auto configuration           = makeConfiguration();
                configuration.exportInterval = std::chrono::milliseconds::zero();
                return Case{configuration, "出口时限"};
            }(),
            // 同一格的另一半：太大的一侧要绕进「截止时刻落在过去」，出口线程就从按时睡变成一直空转
            []
            {
                auto configuration           = makeConfiguration();
                configuration.exportInterval = std::chrono::milliseconds{std::numeric_limits<std::chrono::milliseconds::rep>::max()};
                return Case{configuration, "出口时限"};
            }(),
    };

    for (const Case &testCase: cases)
    {
        EXPECT_THROW(static_cast<void>(Tracer::create(testCase.configuration)), AsynGyanis::Base::LogicException);
        try
        {
            static_cast<void>(Tracer::create(testCase.configuration));
        } catch (const AsynGyanis::Base::LogicException &exception)
        {
            EXPECT_NE(std::string{exception.what()}.find(testCase.expectedFragment), std::string::npos)
                    << "拒绝原因里要点名「" << testCase.expectedFragment << "」：" << exception.what();
        }
    }
}

/**
 * @brief 边界上的合法配置要能建起来：拒绝规则不得比声明的更宽
 */
TEST(Tracer, AcceptsBoundaryConfigurations)
{
    for (const double ratio: {0.0, 1.0})
    {
        auto configuration                    = makeConfiguration(ratio);
        configuration.exportBatchSpanCount    = 1U;
        configuration.maximumPendingSpanCount = 1U;
        EXPECT_NO_THROW(static_cast<void>(Tracer::create(configuration)));
    }

    // 时限那一格的上限本身要建得起来（正向对照：拒绝规则不得宽到把「长到不现实但表达到过来」的取值也拒掉）
    auto boundaryConfiguration           = makeConfiguration();
    boundaryConfiguration.exportInterval = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count() / 2LL)};
    EXPECT_NO_THROW(static_cast<void>(Tracer::create(boundaryConfiguration)));
}

/**
 * @brief 超出条数上限的维度只丢不再收，并把丢掉的条数带进成品
 */
TEST(Tracer, AttributeCapDropsExtrasAndCountsThem)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    Span span = tracer->startSpan("many-attributes", SpanKind::Internal);
    for (std::size_t index = 0; index < AsynGyanis::Net::kMaximumSpanAttributeCount + 8U; ++index)
    {
        span.setAttribute("attribute-" + std::to_string(index), static_cast<std::int64_t>(index));
    }
    span.finish();
    tracer->flush();

    const std::vector<SpanRecord> records = exporter->records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].attributes.size(), AsynGyanis::Net::kMaximumSpanAttributeCount);
    EXPECT_EQ(records[0].droppedAttributeCount, 8U);
}

/**
 * @brief 同名维度是换值不是追加，且留在原来的位置上
 */
TEST(Tracer, SameKeyAttributeIsReplacedInPlace)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    Span span = tracer->startSpan("replace-attribute", SpanKind::Internal);
    span.setAttribute("first", std::string_view{"1"});
    span.setAttribute("retried", std::int64_t{1});
    span.setAttribute("last", std::string_view{"3"});
    span.setAttribute("retried", std::string_view{"second time"});
    span.finish();
    tracer->flush();

    const std::vector<SpanRecord> records = exporter->records();
    ASSERT_EQ(records.size(), 1U);
    const std::vector<AsynGyanis::Net::SpanAttribute> &attributes = records[0].attributes;
    ASSERT_EQ(attributes.size(), 3U) << "同名写入不该多出一条";
    EXPECT_EQ(attributes[1].key, "retried") << "换值要留在原位，否则两次快照逐行比对会失真";
    EXPECT_EQ(std::get<std::string>(attributes[1].value), "second time");
    EXPECT_EQ(records[0].droppedAttributeCount, 0U);
}

/**
 * @brief 改名接口是给「开始一节时还不知道自己属于哪个操作」的调用方用的
 */
TEST(Tracer, RenameIsVisibleInTheRecord)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    Span span = tracer->startSpan("http request", SpanKind::Server);
    span.setName("GET /api/orders");
    span.finish();
    tracer->flush();

    ASSERT_EQ(exporter->recordCount(), 1U);
    EXPECT_EQ(exporter->records().front().name, "GET /api/orders");
}

/**
 * @brief 错误结局不被后写的「成功」抹平：收尾路径上的误判不该把失败藏起来
 */
TEST(Tracer, ErrorStatusIsNotOverwrittenByOk)
{
    const auto exporter = std::make_shared<CapturingSpanExporter>();
    const auto tracer   = Tracer::create(makeConfiguration());
    tracer->addExporter(exporter);

    Span span = tracer->startSpan("flaky-finish", SpanKind::Internal);
    span.setStatus(SpanStatusCode::Error, "上游超时");
    span.setStatus(SpanStatusCode::Ok);
    span.finish();
    tracer->flush();

    const std::vector<SpanRecord> records = exporter->records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].status, SpanStatusCode::Error);
    EXPECT_EQ(records[0].statusMessage, "上游超时");
}

/**
 * @brief nullptr 出口按「没挂」处理：一个空条目不该打死整条链路
 */
TEST(Tracer, NullExporterIsIgnored)
{
    const auto tracer = Tracer::create(makeConfiguration());
    tracer->addExporter(nullptr);
    EXPECT_EQ(tracer->exporterCount(), 0U);
    recordOneSpan(tracer, "no-exporter");
    tracer->flush();
    EXPECT_EQ(tracer->droppedSpanCount(), 1U);
}
