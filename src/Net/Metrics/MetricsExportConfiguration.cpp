#include "Net/Metrics/MetricsExportConfiguration.h"

#include "Net/Telemetry/OtlpConfigurationSupport.h"
#include "Net/Http/HttpConfigValidation.h"
#include "Net/Http/HttpConfigDuration.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        using HttpConfig::findOptionalObject;
        using HttpConfig::joinKeys;
        using HttpConfig::readOptionalBoolean;
        using HttpConfig::rejectUnknownKeys;
        using HttpConfig::requireNonEmptyString;
        using HttpConfig::requirePositiveInteger;

        using HttpConfig::toMilliseconds;

        /// metrics 段直接支持的键
        constexpr std::array<std::string_view, 5> kMetricsKeys{"enabled", "service_name", "service_version", "export_interval_ms", "otlp"};

        /// @brief 读 otlp 子段：地址、时限与附加头部
        void readOtlpSection(const Base::ConfigValue &node, MetricsExportConfiguration &configuration, const std::string &sectionPath)
        {
            Telemetry::OtlpSectionValues values = Telemetry::readOtlpSection(node, sectionPath);
            configuration.otlpEndpoint          = std::move(values.endpoint);
            if (values.requestTimeout.has_value())
            {
                configuration.otlpRequestTimeout = *values.requestTimeout;
            }
            configuration.otlpHeaders = std::move(values.headers);
        }
    } // namespace

    MetricsExportConfiguration readMetricsExportConfiguration(const Base::ConfigValue &configurationRoot)
    {
        MetricsExportConfiguration configuration;

        // 整段缺失即「不发读数」：不必为了不开导出而写一个空的 metrics 段
        if (!configurationRoot.contains(kMetricsExportConfigSection))
        {
            return configuration;
        }
        const Base::ConfigValue &section = configurationRoot.at(kMetricsExportConfigSection);
        if (section.is_null())
        {
            return configuration;
        }
        if (!section.is_object())
        {
            throw Base::ConfigValidationException(std::string(kMetricsExportConfigSection), "必须是一个对象");
        }

        const std::string sectionPath(kMetricsExportConfigSection);
        rejectUnknownKeys(section, kMetricsKeys.data(), kMetricsKeys.size(), sectionPath);

        configuration.enabled = readOptionalBoolean(section, "enabled", sectionPath).value_or(configuration.enabled);
        // 关着的时候其余取值一概不看：开了才要求服务名与地址，关掉却还在校验地址，等于让一段用不上的配置挡住启动
        if (!configuration.enabled)
        {
            return configuration;
        }

        if (!section.contains("service_name"))
        {
            throw Base::ConfigValidationException(sectionPath + ".service_name", "开了读数导出就必须给出服务名：推送侧靠它认这份读数是谁报的");
        }
        configuration.serviceName = requireNonEmptyString(section.at("service_name"), sectionPath + ".service_name");
        if (section.contains("service_version"))
        {
            configuration.serviceVersion = requireNonEmptyString(section.at("service_version"), sectionPath + ".service_version");
        }
        if (section.contains("export_interval_ms"))
        {
            configuration.exportInterval =
                    toMilliseconds(requirePositiveInteger(section.at("export_interval_ms"), sectionPath + ".export_interval_ms"), sectionPath + ".export_interval_ms");
        }
        if (const Base::ConfigValue *otlpNode = findOptionalObject(section, "otlp", sectionPath))
        {
            readOtlpSection(*otlpNode, configuration, sectionPath);
        }
        if (configuration.otlpEndpoint.empty())
        {
            throw Base::ConfigValidationException(sectionPath + ".otlp.endpoint", "开了读数导出就必须给出采集端地址（形如 http://collector:4318/v1/metrics）");
        }

        return configuration;
    }

    std::unique_ptr<OtlpHttpMetricsExporter> buildMetricsExporter(const MetricsExportConfiguration &configuration, std::function<OtlpMetricsBatch()> extraReadings)
    {
        if (!configuration.enabled)
        {
            return nullptr;
        }
        if (configuration.serviceName.empty())
        {
            // 读取器已拦下这一种写法；这里是装配层对自己的守卫，防的是手工拼一份配置绕过来
            throw Base::LogicException("读数导出的服务名为空：启用导出时必须给出服务名（读取器本应已拦下）");
        }

        OtlpHttpMetricsExporter::Configuration exporterConfiguration;
        exporterConfiguration.endpoint       = configuration.otlpEndpoint;
        exporterConfiguration.requestTimeout = configuration.otlpRequestTimeout;
        exporterConfiguration.exportInterval = configuration.exportInterval;
        exporterConfiguration.extraHeaders   = configuration.otlpHeaders;

        // 快照来源固定为本进程的读数注册表（登记的是「现取」的回调，每个导出周期自然拿到最新值）：
        // 它出标量那一路；额外来源（如某台 HTTP 服务器的统计与延迟直方图）由调用方折好后在三路合并
        auto snapshotProvider = [extraReadings = std::move(extraReadings)]
        {
            OtlpMetricsBatch batch{.scalars = Core::ProcessMetricsRegistry::samples()};
            if (extraReadings)
            {
                OtlpMetricsBatch extra = extraReadings();
                batch.scalars.insert(batch.scalars.end(), extra.scalars.begin(), extra.scalars.end());
                batch.labeled    = std::move(extra.labeled);
                batch.histograms = std::move(extra.histograms);
            }
            return batch;
        };
        return std::make_unique<OtlpHttpMetricsExporter>(TraceResource{.serviceName = configuration.serviceName, .serviceVersion = configuration.serviceVersion},
                                                         std::move(exporterConfiguration), std::move(snapshotProvider));
    }
} // namespace AsynGyanis::Net
