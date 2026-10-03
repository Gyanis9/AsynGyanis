// tracing.* 配置键的用例：每一层都认得自己的键、每个取值都到得了被它配的那一层
//
// 「配了却不生效」是这类装配代码唯一的真缺陷，所以断言落在两处：拒绝面（未知键、类型不符、越界、
// 空串）必须点名到具体键路径；受理面必须让配置真的开出可见的东西来（出口条数、以及文件出口真的落了盘）。

#include "Net/Tracing/TracingConfiguration.h"

#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Tracing/HttpTracingMiddleware.h"
#include "Net/Tracing/Span.h"
#include "Net/Tracing/Tracer.h"
#include "CommonTestSupport.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using AsynGyanis::Base::parseConfigValue;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::buildTracer;
    using AsynGyanis::Net::readTracingConfiguration;
    using AsynGyanis::Net::Span;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::Tracer;
    using AsynGyanis::Net::TracingConfiguration;

    /// 把一段 tracing 配置嵌成消费方拿到的文档形状
    ConfigValue documentWith(const std::string &tracingJson)
    {
        const auto parsed = parseConfigValue(tracingJson);
        EXPECT_TRUE(parsed.has_value()) << tracingJson;
        ConfigValue document = ConfigValue::object();
        document["tracing"]  = parsed.value_or(ConfigValue{});
        return document;
    }

    /**
     * @brief 用对象树拼一份 tracing 段（路径里带反斜杠时不能走 JSON 文本拼接）
     * @param build 往段对象上填键的动作
     * @return ConfigValue 已嵌好段的文档根
     */
    ConfigValue documentFromObject(const std::function<void(ConfigValue &)> &build)
    {
        ConfigValue section = ConfigValue::object();
        build(section);
        ConfigValue document = ConfigValue::object();
        document["tracing"]  = std::move(section);
        return document;
    }

    /// 读一份配置，失败时把原因交回给断言
    std::string rejectionText(const std::string &tracingJson)
    {
        try
        {
            static_cast<void>(readTracingConfiguration(documentWith(tracingJson)));
        } catch (const AsynGyanis::Base::ConfigValidationException &exception)
        {
            return std::string{exception.what()};
        }
        return {};
    }
} // namespace

/**
 * @brief 没有 tracing 段就是不开链路：默认值全在，也不建编排器
 */
TEST(TracingConfiguration, AbsentSectionMeansDisabled)
{
    const ConfigValue          document      = ConfigValue::object();
    const TracingConfiguration configuration = readTracingConfiguration(document);
    EXPECT_FALSE(configuration.enabled);
    EXPECT_DOUBLE_EQ(configuration.sampleRatio, 1.0);
    EXPECT_EQ(buildTracer(configuration), nullptr);
}

/**
 * @brief 关着的配置里其余取值一概不校验：一段用不上的配置不该挡住启动
 */
TEST(TracingConfiguration, DisabledSectionIgnoresTheRest)
{
    const auto text = rejectionText(R"({"enabled": false, "otlp": {"endpoint": ""}, "sample_ratio": 9})");
    EXPECT_TRUE(text.empty()) << text;
}

/**
 * @brief 开了就必须给服务名：出口侧靠它认这批节是谁写的
 */
TEST(TracingConfiguration, RequiresServiceNameWhenEnabled)
{
    const auto text = rejectionText(R"({"enabled": true})");
    EXPECT_NE(text.find("tracing.service_name"), std::string::npos) << text;
    EXPECT_NE(text.find("服务名"), std::string::npos) << text;
}

/**
 * @brief 每一层的未知键都在自己那一层被拒，且报出完整键路径
 * @details 把 sample_ratio 写成 sample_rate 若被静默忽略，运维看到的是「比例没生效」，
 *          而唯一的线索就是这条报错里的键路径
 */
TEST(TracingConfiguration, RejectsUnknownKeysAtTheirOwnLayer)
{
    struct Case
    {
        const char *json;
        const char *expectedKeyPath;
    };

    const Case cases[] = {
            {R"({"enabled": true, "service_name": "s", "sample_rate": 0.5})", "tracing.sample_rate"},
            {R"({"enabled": true, "service_name": "s", "otlp": {"url": "http://c:4318"}})", "tracing.otlp.url"},
            {R"({"enabled": true, "service_name": "s", "file": {"dir": "/tmp"}})", "tracing.file.dir"},
    };
    for (const Case &testCase: cases)
    {
        const auto text = rejectionText(testCase.json);
        EXPECT_NE(text.find(testCase.expectedKeyPath), std::string::npos) << testCase.expectedKeyPath << " → " << text;
    }
}

/**
 * @brief 类型与范围的判据都在读入这一层：不取整、不回落、不把字符串当数
 */
TEST(TracingConfiguration, RejectsMalformedValues)
{
    struct Case
    {
        const char *json;
        const char *expectedFragment;
    };

    const Case cases[] = {
            {R"({"enabled": "yes"})", "tracing.enabled"},
            {R"({"enabled": true, "service_name": "s", "sample_ratio": "0.5"})", "tracing.sample_ratio"},
            {R"({"enabled": true, "service_name": "s", "sample_ratio": 1.5})", "0.0 到 1.0"},
            {R"({"enabled": true, "service_name": "s", "batch_span_count": 0})", "tracing.batch_span_count"},
            {R"({"enabled": true, "service_name": "s", "export_interval_ms": -1})", "tracing.export_interval_ms"},
            {R"({"enabled": true, "service_name": "s", "otlp": "http://collector:4318"})", "tracing.otlp"},
            // 写成空的那一格（`otlp:` 后面什么都没有）读出来是 null：按缺席处理就成了「看着像配了其实
            // 没配」，而 HttpServerConfig 对同一形状一直是拒的——两条读口的口径必须一致
            {R"({"enabled": true, "service_name": "s", "otlp": null})", "tracing.otlp"},
            {R"({"enabled": true, "service_name": "s", "file": null})", "tracing.file"},
            {R"({"enabled": true, "service_name": "s", "otlp": {"endpoint": ""}})", "tracing.otlp.endpoint"},
            {R"({"enabled": true, "service_name": "s", "otlp": {"headers": {"X-Token": 7}}})", "tracing.otlp.headers.X-Token"},
            {R"({"enabled": true, "service_name": ""})", "tracing.service_name"},
    };
    for (const Case &testCase: cases)
    {
        const auto text = rejectionText(testCase.json);
        EXPECT_NE(text.find(testCase.expectedFragment), std::string::npos) << testCase.expectedFragment << " → " << text;
    }
}

/**
 * @brief 配了两个出口就挂两个，只配一个就挂一个：这条判据把「键到对象」的连线钉住
 */
TEST(TracingConfiguration, AttachesExactlyTheConfiguredExporters)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"TracingConfig"};
    const std::string                                 spanFile = (temporaryDirectory.path() / "spans.jsonl").string();

    const TracingConfiguration nothingConfigured = readTracingConfiguration(
            documentWith(R"({"enabled": true, "service_name": "none", "sample_ratio": 0.5, "pending_span_count": 64, "batch_span_count": 8, "export_interval_ms": 500})"));
    EXPECT_TRUE(nothingConfigured.enabled);
    EXPECT_EQ(nothingConfigured.sampleRatio, 0.5);
    EXPECT_EQ(nothingConfigured.pendingSpanCount, 64U);
    EXPECT_EQ(nothingConfigured.batchSpanCount, 8U);
    EXPECT_EQ(nothingConfigured.exportInterval.count(), 500);
    const auto noExporter = buildTracer(nothingConfigured);
    ASSERT_TRUE(noExporter != nullptr);
    EXPECT_EQ(noExporter->exporterCount(), 0U);

    const TracingConfiguration fileOnly   = readTracingConfiguration(documentFromObject(
            [&spanFile](ConfigValue &section)
            {
                section["enabled"]      = true;
                section["service_name"] = "file";
                section["file"]["path"] = spanFile;
            }));
    const auto                 fileTracer = buildTracer(fileOnly);
    ASSERT_TRUE(fileTracer != nullptr);
    EXPECT_EQ(fileTracer->exporterCount(), 1U);

    const TracingConfiguration both = readTracingConfiguration(documentFromObject(
            [&spanFile](ConfigValue &section)
            {
                section["enabled"]                          = true;
                section["service_name"]                     = "both";
                section["file"]["path"]                     = spanFile;
                section["otlp"]["endpoint"]                 = "http://127.0.0.1:4318/v1/traces";
                section["otlp"]["timeout_ms"]               = 800;
                section["otlp"]["headers"]["Authorization"] = "Bearer t";
            }));
    EXPECT_EQ(both.otlpRequestTimeout.count(), 800);
    ASSERT_EQ(both.otlpHeaders.size(), 1U);
    EXPECT_EQ(both.otlpHeaders[0].first, "Authorization");
    const auto bothTracer = buildTracer(both);
    ASSERT_TRUE(bothTracer != nullptr);
    EXPECT_EQ(bothTracer->exporterCount(), 2U);

    // 配上去的链路要真能用：开一节、收口、flush，文件里就该有一行
    Span span = bothTracer->startSpan("config-wired", SpanKind::Server);
    span.finish();
    bothTracer->flush();
    std::ifstream     probe{spanFile};
    const std::string written{std::istreambuf_iterator<char>{probe}, std::istreambuf_iterator<char>{}};
    EXPECT_FALSE(written.empty()) << "文件出口没落到盘上";
}

/**
 * @brief 地址写错的 otlp.endpoint 在装配这一层就抛，而不是留下一条永远发不出去的出口
 */
TEST(TracingConfiguration, RejectsAnUnusableOtlpEndpointWhenBuilding)
{
    const TracingConfiguration configuration =
            readTracingConfiguration(documentWith(R"({"enabled": true, "service_name": "bad-endpoint", "otlp": {"endpoint": "collector:4318"}})"));
    EXPECT_THROW(static_cast<void>(buildTracer(configuration)), AsynGyanis::Base::InvalidArgumentException);
}

/**
 * @brief 装配出来的编排器能直接喂给中间件：配置到运行期的最后一根线
 */
TEST(TracingConfiguration, BuiltTracerDrivesTheMiddleware)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"TracingConfigWire"};
    const std::string                                 spanFile = (temporaryDirectory.path() / "spans.jsonl").string();

    const auto tracer = buildTracer(readTracingConfiguration(documentFromObject(
            [&spanFile](ConfigValue &section)
            {
                section["enabled"]      = true;
                section["service_name"] = "wired";
                section["file"]["path"] = spanFile;
            })));
    ASSERT_TRUE(tracer != nullptr);
    const auto middleware = AsynGyanis::Net::tracingSpanMiddleware(tracer);

    AsynGyanis::Net::HttpRequest request;
    request.setMethod(AsynGyanis::Net::HttpMethod::GET);
    request.setUri("/reports/monthly");
    AsynGyanis::Net::HttpResponse response;
    const auto                    terminal = [&response]() -> Task<void>
    {
        response.setStatus(200);
        co_return;
    };
    Task<void> chain = middleware(request, response, terminal);
    static_cast<void>(chain.handle().resume());
    ASSERT_TRUE(chain.isReady());
    tracer->flush();

    std::ifstream     probe{spanFile};
    const std::string contents{std::istreambuf_iterator<char>{probe}, std::istreambuf_iterator<char>{}};
    EXPECT_NE(contents.find("\"url.path\":\"/reports/monthly\""), std::string::npos) << contents;
}

/**
 * @brief 配置文件 → ConfigManager → 读出的那根线要真的通：这是部署里唯一的入口
 * @details 单测里直接喂 JSON 文本会漏掉一格：ConfigManager 把键扁平成 tracing.file.path，
 *          getSection() 再还原成嵌套对象。少还原一步，配置就是「写了但读不到」，
 *          而现象只是「链路一直没数据」——最难查的那种。
 */
TEST(TracingConfiguration, ReadsTheSectionFromAConfigFile)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"TracingConfigFile"};
    const std::filesystem::path                       configFile = temporaryDirectory.path() / "server.json";
    // 路径写成斜杠形式：JSON 文本里的反斜杠要双重转义，而这正是这条用例不想测试的东西
    const std::string spanFile = (temporaryDirectory.path() / "spans.jsonl").generic_string();

    {
        std::ofstream file{configFile};
        file << R"({"tracing":{"enabled":true,"service_name":"from-file","sample_ratio":1.0,"pending_span_count":321,"batch_span_count":12,"export_interval_ms":456,"otlp":{"timeout_ms":700},"file":{"path":")"
             << spanFile << R"("}}})";
    }

    auto &manager = AsynGyanis::Base::ConfigManager::instance();
    manager.clear();
    const auto loaded = manager.loadFiles({configFile.string()});
    ASSERT_TRUE(loaded.success) << "配置文件没能加载";

    // 装配方（reference_server）递过来的形状是「先攒一张 ConfigObject，再整个转成 ConfigValue」：
    // 那条转换必须走圆括号——花括号会去配 initializer_list，整份文档变成一个数组，两个读取器都找不到自己的段
    AsynGyanis::Base::ConfigObject assembled;
    assembled.emplace(std::string(AsynGyanis::Net::kTracingConfigSection), manager.getSection(AsynGyanis::Net::kTracingConfigSection));
    const ConfigValue assembledDocument(std::move(assembled));
    ASSERT_TRUE(assembledDocument.contains("tracing")) << "ConfigObject → ConfigValue 的转换没有交出对象";
    EXPECT_EQ(readTracingConfiguration(assembledDocument).serviceName, "from-file");

    ConfigValue document = ConfigValue::object();
    document["tracing"]  = manager.getSection("tracing");

    const TracingConfiguration configuration = readTracingConfiguration(document);
    EXPECT_TRUE(configuration.enabled);
    EXPECT_EQ(configuration.serviceName, "from-file");
    EXPECT_DOUBLE_EQ(configuration.sampleRatio, 1.0) << "比例小于 1 时这一条用例的导出断言就成了概率事件";
    EXPECT_EQ(configuration.pendingSpanCount, 321U);
    EXPECT_EQ(configuration.batchSpanCount, 12U);
    EXPECT_EQ(configuration.exportInterval.count(), 456);
    EXPECT_EQ(configuration.otlpRequestTimeout.count(), 700);
    EXPECT_EQ(configuration.spanFilePath, spanFile) << "getSection 没把 tracing.file.path 还原成嵌套对象";

    const auto tracer = buildTracer(configuration);
    ASSERT_TRUE(tracer != nullptr);
    EXPECT_EQ(tracer->exporterCount(), 1U);
    Span span = tracer->startSpan("from-file-operation", SpanKind::Server);
    span.finish();
    tracer->flush();

    std::ifstream     probe{spanFile};
    const std::string contents{std::istreambuf_iterator<char>{probe}, std::istreambuf_iterator<char>{}};
    EXPECT_NE(contents.find("from-file-operation"), std::string::npos) << contents;
    manager.clear();
}
