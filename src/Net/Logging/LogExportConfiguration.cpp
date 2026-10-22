#include "Net/Logging/LogExportConfiguration.h"

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
        using HttpConfig::toMilliseconds;

        /// log_export 段直接支持的键
        constexpr std::array<std::string_view, 8> kLogExportKeys{
                "enabled", "service_name", "service_version", "export_interval_ms", "shutdown_timeout_ms", "maximum_pending_records", "maximum_batch_records", "otlp",
        };

        /// otlp 子段支持的键
        constexpr std::array<std::string_view, 3> kOtlpKeys{"endpoint", "timeout_ms", "headers"};

        /// @brief 把一组可接受的键拼成一句可读的提示
        std::string joinKeys(const std::string_view *keys, const std::size_t keyCount)
        {
            std::string text;
            for (std::size_t index = 0; index < keyCount; ++index)
            {
                if (index != 0)
                {
                    text += "、";
                }
                text += keys[index];
            }
            return text;
        }

        /// @brief 拒绝该层不认得的键
        void rejectUnknownKeys(const Base::ConfigValue &node, const std::string_view *keys, const std::size_t keyCount, const std::string &pathPrefix)
        {
            for (const auto &[memberName, memberValue]: node.items())
            {
                const bool isAccepted = std::any_of(keys, keys + keyCount, [&memberName](const std::string_view accepted) { return accepted == memberName; });
                if (!isAccepted)
                {
                    throw Base::ConfigValidationException(pathPrefix + "." + memberName, "未知的配置键；本层支持：" + joinKeys(keys, keyCount));
                }
            }
        }

        /// @brief 取可选对象子段
        const Base::ConfigValue *findOptionalObject(const Base::ConfigValue &node, const std::string_view key, const std::string &pathPrefix)
        {
            if (!node.contains(key))
            {
                return nullptr;
            }
            const Base::ConfigValue &child = node.at(key);
            if (!child.is_object())
            {
                throw Base::ConfigValidationException(pathPrefix + "." + std::string(key), "必须是一个对象");
            }
            return &child;
        }

        /// @brief 取正整数
        std::uint64_t requirePositiveInteger(const Base::ConfigValue &value, const std::string &key)
        {
            if (!value.is_number_integer() && !value.is_number_unsigned())
            {
                throw Base::ConfigValidationException(key, "必须是整数（不能写成字符串或小数）");
            }
            if (value.is_number_integer())
            {
                const std::int64_t signedValue = value.get<std::int64_t>();
                if (signedValue <= 0)
                {
                    throw Base::ConfigValidationException(key, "必须大于 0");
                }
                return static_cast<std::uint64_t>(signedValue);
            }
            const std::uint64_t magnitude = value.get<std::uint64_t>();
            if (magnitude == 0U)
            {
                throw Base::ConfigValidationException(key, "必须大于 0");
            }
            return magnitude;
        }
        /// @brief 取非空文本
        std::string requireNonEmptyString(const Base::ConfigValue &value, const std::string &key)
        {
            if (!value.is_string())
            {
                throw Base::ConfigValidationException(key, "必须是字符串");
            }
            std::string text = value.get<std::string>();
            if (text.empty())
            {
                throw Base::ConfigValidationException(key, "不能是空串");
            }
            return text;
        }

        /// @brief 读 otlp 子段
        void readOtlpSection(const Base::ConfigValue &node, LogExportConfiguration &configuration, const std::string &sectionPath)
        {
            const std::string pathPrefix = sectionPath + ".otlp";
            rejectUnknownKeys(node, kOtlpKeys.data(), kOtlpKeys.size(), pathPrefix);

            if (node.contains("endpoint"))
            {
                configuration.otlpEndpoint = requireNonEmptyString(node.at("endpoint"), pathPrefix + ".endpoint");
            }
            if (node.contains("timeout_ms"))
            {
                configuration.requestTimeout = toMilliseconds(requirePositiveInteger(node.at("timeout_ms"), pathPrefix + ".timeout_ms"), pathPrefix + ".timeout_ms");
            }
            if (node.contains("headers"))
            {
                const Base::ConfigValue &headers = node.at("headers");
                if (!headers.is_object())
                {
                    throw Base::ConfigValidationException(pathPrefix + ".headers", "必须是一个「头部名: 值」的对象");
                }
                for (const auto &[name, value]: headers.items())
                {
                    if (!value.is_string())
                    {
                        throw Base::ConfigValidationException(pathPrefix + ".headers." + name, "头部取值必须是字符串");
                    }
                    configuration.otlpHeaders.emplace_back(name, value.get<std::string>());
                }
            }
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

        if (section.contains("enabled"))
        {
            const Base::ConfigValue &value = section.at("enabled");
            if (!value.is_boolean())
            {
                throw Base::ConfigValidationException(sectionPath + ".enabled", "必须是 true 或 false");
            }
            configuration.enabled = value.get<bool>();
        }
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
