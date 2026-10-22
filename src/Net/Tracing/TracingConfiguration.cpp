#include "Net/Tracing/TracingConfiguration.h"

#include "Net/Telemetry/OtlpConfigurationSupport.h"
#include "Net/Http/HttpConfigValidation.h"
#include "Net/Http/HttpConfigDuration.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Net/Tracing/FileSpanExporter.h"
#include "Net/Tracing/OtlpHttpSpanExporter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
        using HttpConfig::rejectUnknownKeys;
        using HttpConfig::requireNonEmptyString;
        using HttpConfig::requirePositiveInteger;

        using HttpConfig::toMilliseconds;

        /// tracing 段直接支持的键
        constexpr std::array<std::string_view, 9> kTracingKeys{
                "enabled", "service_name", "service_version", "sample_ratio", "pending_span_count", "batch_span_count", "export_interval_ms", "otlp", "file",
        };


        /// file 子段支持的键
        constexpr std::array<std::string_view, 1> kFileKeys{"path"};
        /**
         * @brief 读 otlp 子段：地址、时限与附加头部
         * @details headers 的键值都按文本收：鉴权令牌这类头部本来就是一串字符，
         *          而「值写成数字」这种写法在下一层会被出口再拒一次，不如在这里就说清是谁的要求
         */
        void readOtlpSection(const Base::ConfigValue &node, TracingConfiguration &configuration, const std::string &sectionPath)
        {
            Telemetry::OtlpSectionValues values = Telemetry::readOtlpSection(node, sectionPath);
            configuration.otlpEndpoint          = std::move(values.endpoint);
            if (values.requestTimeout.has_value())
            {
                configuration.otlpRequestTimeout = *values.requestTimeout;
            }
            configuration.otlpHeaders = std::move(values.headers);
        }

        /// @brief 读 file 子段（只有落盘路径一项）
        void readFileSection(const Base::ConfigValue &node, TracingConfiguration &configuration, const std::string &sectionPath)
        {
            const std::string pathPrefix = sectionPath + ".file";
            rejectUnknownKeys(node, kFileKeys.data(), kFileKeys.size(), pathPrefix);

            if (node.contains("path"))
            {
                configuration.spanFilePath = requireNonEmptyString(node.at("path"), pathPrefix + ".path");
            }
        }
    } // namespace

    TracingConfiguration readTracingConfiguration(const Base::ConfigValue &configurationRoot)
    {
        TracingConfiguration configuration;

        // 整段缺失即「不记链路」：不必为了不开链路而写一个空的 tracing 段
        if (!configurationRoot.contains(kTracingConfigSection))
        {
            return configuration;
        }
        const Base::ConfigValue &section = configurationRoot.at(kTracingConfigSection);
        if (section.is_null())
        {
            return configuration;
        }
        if (!section.is_object())
        {
            throw Base::ConfigValidationException(std::string(kTracingConfigSection), "必须是一个对象");
        }

        const std::string sectionPath(kTracingConfigSection);
        rejectUnknownKeys(section, kTracingKeys.data(), kTracingKeys.size(), sectionPath);

        if (section.contains("enabled"))
        {
            const Base::ConfigValue &value = section.at("enabled");
            if (!value.is_boolean())
            {
                throw Base::ConfigValidationException(sectionPath + ".enabled", "必须是 true 或 false");
            }
            configuration.enabled = value.get<bool>();
        }
        // 关着的时候其余取值一概不看：开了才要求服务名，关掉却还在校验地址，等于让一段用不上的配置挡住启动
        if (!configuration.enabled)
        {
            return configuration;
        }

        if (!section.contains("service_name"))
        {
            throw Base::ConfigValidationException(sectionPath + ".service_name", "开了链路就必须给出服务名：出口侧靠它认这批节是谁写的");
        }
        configuration.serviceName = requireNonEmptyString(section.at("service_name"), sectionPath + ".service_name");
        if (section.contains("service_version"))
        {
            configuration.serviceVersion = requireNonEmptyString(section.at("service_version"), sectionPath + ".service_version");
        }
        if (section.contains("sample_ratio"))
        {
            const Base::ConfigValue &value = section.at("sample_ratio");
            // 严格判数值：字符串 "0.5" 与取整后的 0 都不接受，宁可报错也不替调用方猜一个比例
            if (!value.is_number() || std::isnan(value.get<double>()))
            {
                throw Base::ConfigValidationException(sectionPath + ".sample_ratio", "必须是 0.0 到 1.0 之间的数");
            }
            configuration.sampleRatio = value.get<double>();
            if (configuration.sampleRatio < 0.0 || configuration.sampleRatio > 1.0)
            {
                throw Base::ConfigValidationException(sectionPath + ".sample_ratio", "必须在 0.0 到 1.0 之间（当前 " + std::to_string(configuration.sampleRatio) + "）");
            }
        }
        if (section.contains("pending_span_count"))
        {
            configuration.pendingSpanCount = static_cast<std::size_t>(requirePositiveInteger(section.at("pending_span_count"), sectionPath + ".pending_span_count"));
        }
        if (section.contains("batch_span_count"))
        {
            configuration.batchSpanCount = static_cast<std::size_t>(requirePositiveInteger(section.at("batch_span_count"), sectionPath + ".batch_span_count"));
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
        if (const Base::ConfigValue *fileNode = findOptionalObject(section, "file", sectionPath))
        {
            readFileSection(*fileNode, configuration, sectionPath);
        }

        return configuration;
    }

    std::shared_ptr<Tracer> buildTracer(const TracingConfiguration &configuration)
    {
        if (!configuration.enabled)
        {
            return nullptr;
        }

        Tracer::Configuration tracerConfiguration;
        tracerConfiguration.serviceName             = configuration.serviceName;
        tracerConfiguration.serviceVersion          = configuration.serviceVersion;
        tracerConfiguration.sampleRatio             = configuration.sampleRatio;
        tracerConfiguration.maximumPendingSpanCount = configuration.pendingSpanCount;
        tracerConfiguration.exportBatchSpanCount    = configuration.batchSpanCount;
        tracerConfiguration.exportInterval          = configuration.exportInterval;
        auto tracer                                 = Tracer::create(std::move(tracerConfiguration));

        if (!configuration.spanFilePath.empty())
        {
            tracer->addExporter(std::make_shared<FileSpanExporter>(configuration.spanFilePath));
        }
        if (!configuration.otlpEndpoint.empty())
        {
            OtlpHttpSpanExporter::Configuration otlpConfiguration;
            otlpConfiguration.endpoint       = configuration.otlpEndpoint;
            otlpConfiguration.requestTimeout = configuration.otlpRequestTimeout;
            otlpConfiguration.extraHeaders   = configuration.otlpHeaders;
            tracer->addExporter(std::make_shared<OtlpHttpSpanExporter>(std::move(otlpConfiguration)));
        }
        return tracer;
    }
} // namespace AsynGyanis::Net
