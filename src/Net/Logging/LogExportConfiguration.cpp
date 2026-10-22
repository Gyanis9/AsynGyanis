#include "Net/Logging/LogExportConfiguration.h"

#include "Net/Telemetry/OtlpConfigurationSupport.h"
#include "Net/Http/HttpConfigValidation.h"
#include "Net/Http/HttpConfigDuration.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/LogicException.h"
#include "Net/Logging/OtlpLogSink.h"
#include "Net/Tracing/SpanExporter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
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

        /// log_export 段直接支持的键
        constexpr std::array<std::string_view, 8> kLogExportKeys{
                "enabled", "service_name", "service_version", "export_interval_ms", "shutdown_timeout_ms", "maximum_pending_records", "maximum_batch_records", "otlp",
        };

        /// @brief 读 otlp 子段
        void readOtlpSection(const Base::ConfigValue &node, LogExportConfiguration &configuration, const std::string &sectionPath)
        {
            Telemetry::OtlpSectionValues values = Telemetry::readOtlpSection(node, sectionPath);
            configuration.otlpEndpoint          = std::move(values.endpoint);
            if (values.requestTimeout.has_value())
            {
                configuration.requestTimeout = *values.requestTimeout;
            }
            configuration.otlpHeaders = std::move(values.headers);
        }
    } // namespace

    LogExportConfiguration readLogExportConfiguration(const Base::ConfigValue &configurationRoot)
    {
        LogExportConfiguration configuration;

        if (!configurationRoot.contains(kLogExportConfigSection))
        {
            return configuration;
        }
        const Base::ConfigValue &section = configurationRoot.at(kLogExportConfigSection);
        if (section.is_null())
        {
            return configuration;
        }
        if (!section.is_object())
        {
            throw Base::ConfigValidationException(std::string(kLogExportConfigSection), "必须是一个对象");
        }

        const std::string sectionPath(kLogExportConfigSection);
        rejectUnknownKeys(section, kLogExportKeys.data(), kLogExportKeys.size(), sectionPath);

        configuration.enabled = readOptionalBoolean(section, "enabled", sectionPath).value_or(configuration.enabled);
        // 关着时其余取值一概不看：开了才要求服务名与地址
        if (!configuration.enabled)
        {
            return configuration;
        }

        if (!section.contains("service_name"))
        {
            throw Base::ConfigValidationException(sectionPath + ".service_name", "开了日志导出就必须给出服务名：采集端靠它认这批日志是谁写的");
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
        if (section.contains("shutdown_timeout_ms"))
        {
            configuration.shutdownTimeout =
                    toMilliseconds(requirePositiveInteger(section.at("shutdown_timeout_ms"), sectionPath + ".shutdown_timeout_ms"), sectionPath + ".shutdown_timeout_ms");
        }
        if (section.contains("maximum_pending_records"))
        {
            configuration.maximumPendingRecordCount =
                    static_cast<std::size_t>(requirePositiveInteger(section.at("maximum_pending_records"), sectionPath + ".maximum_pending_records"));
        }
        if (section.contains("maximum_batch_records"))
        {
            configuration.maximumBatchRecordCount = static_cast<std::size_t>(requirePositiveInteger(section.at("maximum_batch_records"), sectionPath + ".maximum_batch_records"));
        }
        if (const Base::ConfigValue *otlpNode = findOptionalObject(section, "otlp", sectionPath))
        {
            readOtlpSection(*otlpNode, configuration, sectionPath);
        }
        if (configuration.otlpEndpoint.empty())
        {
            throw Base::ConfigValidationException(sectionPath + ".otlp.endpoint", "开了日志导出就必须给出采集端地址（形如 http://collector:4318/v1/logs）");
        }
        return configuration;
    }

    std::unique_ptr<Base::LogSink> buildOtlpLogSink(const LogExportConfiguration &configuration)
    {
        if (!configuration.enabled)
        {
            return nullptr;
        }
        if (configuration.serviceName.empty())
        {
            // 读取器已拦下这一种写法；这里是装配层对自己的守卫
            throw Base::LogicException("日志导出的服务名为空：启用导出时必须给出服务名（读取器本应已拦下）");
        }

        OtlpLogSink::Configuration exporterConfiguration;
        exporterConfiguration.endpoint                  = configuration.otlpEndpoint;
        exporterConfiguration.requestTimeout            = configuration.requestTimeout;
        exporterConfiguration.exportInterval            = configuration.exportInterval;
        exporterConfiguration.shutdownTimeout           = configuration.shutdownTimeout;
        exporterConfiguration.maximumPendingRecordCount = configuration.maximumPendingRecordCount;
        exporterConfiguration.maximumBatchRecordCount   = configuration.maximumBatchRecordCount;
        exporterConfiguration.extraHeaders              = configuration.otlpHeaders;
        return std::make_unique<OtlpLogSink>(TraceResource{.serviceName = configuration.serviceName, .serviceVersion = configuration.serviceVersion},
                                             std::move(exporterConfiguration));
    }
} // namespace AsynGyanis::Net
