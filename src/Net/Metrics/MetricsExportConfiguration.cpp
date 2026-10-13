#include "Net/Metrics/MetricsExportConfiguration.h"

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
        /// metrics 段直接支持的键
        constexpr std::array<std::string_view, 5> kMetricsKeys{"enabled", "service_name", "service_version", "export_interval_ms", "otlp"};

        /// otlp 子段支持的键
        constexpr std::array<std::string_view, 3> kOtlpKeys{"endpoint", "timeout_ms", "headers"};

        /// @brief 把一组可接受的键拼成一句可读的提示
        [[nodiscard]] std::string joinKeys(const std::string_view *keys, const std::size_t keyCount)
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

        /**
         * @brief 拒绝该层不认得的键
         * @details 未知键一律报错：把 export_interval_ms 写成 export_interval 时静默忽略等于「配置没生效却看不出来」
         */
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

        /// @brief 取一个可选对象子段；缺失交出 nullptr，不是对象（含写成空的 null）则抛
        [[nodiscard]] const Base::ConfigValue *findOptionalObject(const Base::ConfigValue &node, const std::string_view key, const std::string &pathPrefix)
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

        /// @brief 取一个正整数（毫秒数与条数都用它：0 在这两处都是「永远不来」而不是「不限」）
        [[nodiscard]] std::uint64_t requirePositiveInteger(const Base::ConfigValue &value, const std::string &key)
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

        /**
         * @brief 把配置里的毫秒数折成时长，越界当场拒
         * @details 超过「能与当前时刻相加」可用上限的取值会折回成过去，等于「配得越大反倒立刻到期」，
         *          与 HttpServerConfig.cpp 的 toMilliseconds 是同一个界、同一个理由
         * @param milliseconds 毫秒数（已由 requirePositiveInteger 确保大于 0）
         * @param key 键路径，用于错误信息
         * @return std::chrono::milliseconds 时长
         * @throws Base::ConfigValidationException 超出可用上限
         */
        [[nodiscard]] std::chrono::milliseconds toMilliseconds(const std::uint64_t milliseconds, const std::string &key)
        {
            constexpr std::int64_t kMaximumUsableTimeoutMilliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count() / 2;
            if (milliseconds > static_cast<std::uint64_t>(kMaximumUsableTimeoutMilliseconds))
            {
                throw Base::ConfigValidationException(
                        key, std::format("超出可用的毫秒上限 {}：时限要与当前时刻相加，越过上界会折回成过去，等于「配得越大反倒立刻到期」", kMaximumUsableTimeoutMilliseconds));
            }
            return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(milliseconds));
        }

        /// @brief 取一段非空文本（服务名与地址都吃这条判据）
        [[nodiscard]] std::string requireNonEmptyString(const Base::ConfigValue &value, const std::string &key)
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

        /// @brief 读 otlp 子段：地址、时限与附加头部
        void readOtlpSection(const Base::ConfigValue &node, MetricsExportConfiguration &configuration, const std::string &sectionPath)
        {
            const std::string pathPrefix = sectionPath + ".otlp";
            rejectUnknownKeys(node, kOtlpKeys.data(), kOtlpKeys.size(), pathPrefix);

            if (node.contains("endpoint"))
            {
                configuration.otlpEndpoint = requireNonEmptyString(node.at("endpoint"), pathPrefix + ".endpoint");
            }
            if (node.contains("timeout_ms"))
            {
                configuration.otlpRequestTimeout = toMilliseconds(requirePositiveInteger(node.at("timeout_ms"), pathPrefix + ".timeout_ms"), pathPrefix + ".timeout_ms");
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

        if (section.contains("enabled"))
        {
            const Base::ConfigValue &value = section.at("enabled");
            if (!value.is_boolean())
            {
                throw Base::ConfigValidationException(sectionPath + ".enabled", "必须是 true 或 false");
            }
            configuration.enabled = value.get<bool>();
        }
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
