// metrics.* 配置键的用例：每一层都认得自己的键、每个取值都到得了被它配的那一层
//
// 「配了却不生效」是这类装配代码唯一的真缺陷，所以断言落在两处：拒绝面（未知键、类型不符、越界、
// 空串）必须点名到具体键路径；受理面必须让配置真的开出出口来。

#include "Net/Metrics/MetricsExportConfiguration.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "ExpectedAssertions.h"
#include "AssertionSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using AsynGyanis::Base::parseConfigValue;
    using AsynGyanis::Net::buildMetricsExporter;
    using AsynGyanis::Net::MetricsExportConfiguration;
    using AsynGyanis::Net::readMetricsExportConfiguration;

    /// 把一段 metrics 配置嵌成消费方拿到的文档形状
    ConfigValue documentWith(const std::string &metricsJson)
    {
        const auto parsed = parseConfigValue(metricsJson);
        EXPECT_HAS_VALUE(parsed) << metricsJson;
        ConfigValue document = ConfigValue::object();
        document["metrics"]  = parsed.value_or(ConfigValue{});
        return document;
    }

    /// 读一份配置，失败时把原因交回给断言
    std::string rejectionText(const std::string &metricsJson)
    {
        try
        {
            static_cast<void>(readMetricsExportConfiguration(documentWith(metricsJson)));
        } catch (const AsynGyanis::Base::ConfigValidationException &exception)
        {
            return std::string{exception.what()};
        }
        return {};
    }
} // namespace

/**
 * @brief 没有 metrics 段就是不发读数：默认值全在，也不建出口
 */
TEST(MetricsExportConfiguration, AbsentSectionMeansDisabled)
{
    const ConfigValue                document      = ConfigValue::object();
    const MetricsExportConfiguration configuration = readMetricsExportConfiguration(document);
    EXPECT_FALSE(configuration.enabled);
    EXPECT_EQ(configuration.exportInterval, std::chrono::milliseconds{15000});
    EXPECT_EQ(buildMetricsExporter(configuration), nullptr);
}

/**
 * @brief 全键读回：每个取值都到得了被它配的那一层
 */
TEST(MetricsExportConfiguration, ReadsKeysAndKeepsDefaults)
{
    const MetricsExportConfiguration configuration = readMetricsExportConfiguration(
            documentWith(R"({"enabled": true, "service_name": "svc", "service_version": "1.2.3", "export_interval_ms": 5000, )"
                         R"("otlp": {"endpoint": "http://collector:4318/v1/metrics", "timeout_ms": 2500, "headers": {"Authorization": "Bearer t"}}})"));

    EXPECT_TRUE(configuration.enabled);
    EXPECT_EQ(configuration.serviceName, "svc");
    EXPECT_EQ(configuration.serviceVersion, "1.2.3");
    EXPECT_EQ(configuration.exportInterval, std::chrono::milliseconds{5000});
    EXPECT_EQ(configuration.otlpEndpoint, "http://collector:4318/v1/metrics");
    EXPECT_EQ(configuration.otlpRequestTimeout, std::chrono::milliseconds{2500});
    ASSERT_EQ(configuration.otlpHeaders.size(), 1U);
    EXPECT_EQ(configuration.otlpHeaders.at(0).first, "Authorization");
    EXPECT_EQ(configuration.otlpHeaders.at(0).second, "Bearer t");
}

/**
 * @brief 关着的配置里其余取值一概不校验：一段用不上的配置不该挡住启动
 */
TEST(MetricsExportConfiguration, DisabledSectionIgnoresTheRest)
{
    const auto text = rejectionText(R"({"enabled": false, "export_interval_ms": 0, "otlp": {"endpoint": ""}})");
    EXPECT_TRUE(text.empty()) << text;
}

/**
 * @brief 开了就必须给服务名与采集端地址：推送的 resource 靠前者、请求靠后者
 */
TEST(MetricsExportConfiguration, RequiresServiceNameAndEndpointWhenEnabled)
{
    const auto missingName = rejectionText(R"({"enabled": true, "otlp": {"endpoint": "http://collector:4318/v1/metrics"}})");
    EXPECT_NE(missingName.find("metrics.service_name"), std::string::npos) << missingName;

    const auto missingEndpoint = rejectionText(R"({"enabled": true, "service_name": "svc"})");
    EXPECT_NE(missingEndpoint.find("metrics.otlp.endpoint"), std::string::npos) << missingEndpoint;
}

/**
 * @brief 未知键当场拒并点名：写成 `export_interval` 而这里读的是 `export_interval_ms` 时静默忽略，等于配置没生效却看不出来
 */
TEST(MetricsExportConfiguration, RejectsUnknownKeys)
{
    const auto text = rejectionText(R"({"enabled": true, "service_name": "svc", "export_interval": 5000})");
    EXPECT_NE(text.find("export_interval"), std::string::npos) << text;
}

/**
 * @brief 类型不符与越界一律拒：字符串化的布尔、0 周期都一样
 */
TEST(MetricsExportConfiguration, RejectsWrongTypes)
{
    EXPECT_NE(rejectionText(R"({"enabled": "true"})").find("true 或 false"), std::string::npos);
    EXPECT_NE(rejectionText(R"({"enabled": true, "service_name": "svc", "export_interval_ms": 0, "otlp": {"endpoint": "http://c:4318/v1/metrics"}})").find("大于 0"),
              std::string::npos);
    EXPECT_NE(rejectionText(R"({"enabled": true, "service_name": "svc", "export_interval_ms": 1500.5, "otlp": {"endpoint": "http://c:4318/v1/metrics"}})").find("整数"),
              std::string::npos);
}

/**
 * @brief 开关开着但地址不成形：读取器照收、装配期当场拒——「一直发不出去」比直接拒更糟
 */
TEST(MetricsExportConfiguration, BuildRejectsMalformedEndpoint)
{
    const MetricsExportConfiguration configuration =
            readMetricsExportConfiguration(documentWith(R"({"enabled": true, "service_name": "svc", "otlp": {"endpoint": "collector:4318"}})"));
    AsynGyanis::TestSupport::expectFailure<AsynGyanis::Base::InvalidArgumentException>([&] { static_cast<void>(buildMetricsExporter(configuration)); },
                                                                                       "不是一枚可用的 http(s) URL", "缺协议的采集端");
}

/**
 * @brief 装配出来的出口是活的：注册表读数真的被推出了周期（快照来源固定接本进程的读数注册表）
 * @details 判据要自己造成立条件：被测进程的注册表此刻未必有读数（登记随对象析构注销），空快照会被
 *          跳过、一个请求都不发——所以在用例里临时登记一条读数保活到收尾，再看导出周期真的跑起来。
 *          对端取 127.0.0.1:1（特权端口，测试机上不会有人监听）：连不上是本用例要的形态，
 *          失败计数随周期增长就证明「活物在跑」，而不是建完就躺下的空壳。
 */
TEST(MetricsExportConfiguration, BuildCreatesAWorkingExporter)
{
    const auto keepAliveReading = AsynGyanis::Core::ProcessMetricsRegistry::registerMetric(
            "asyn_metrics_config_test_total", "配置装配用例的读数", AsynGyanis::Core::ProcessMetricKind::Counter, AsynGyanis::Core::ProcessMetricMerge::Sum, [] { return 1U; });

    const MetricsExportConfiguration configuration = readMetricsExportConfiguration(
            documentWith(R"({"enabled": true, "service_name": "svc", "export_interval_ms": 50, "otlp": {"endpoint": "http://127.0.0.1:1/v1/metrics", "timeout_ms": 300}})"));
    const auto exporter = buildMetricsExporter(configuration);
    ASSERT_TRUE(exporter != nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
    while (exporter->failedExportCount() == 0U && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_GE(exporter->failedExportCount(), 1U) << "装配出来的出口没有在跑周期：" << exporter->lastFailureReason();
}
