/**
 * @file OtlpConfigurationSupport.h
 * @brief OTLP 三个信号共用的 otlp 配置段读取
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Net/Http/HttpConfigDuration.h"
#include "Net/Http/HttpConfigValidation.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net::Telemetry
{
    /**
     * @brief otlp 子段里与本层有关的三项取值（读到什么填什么）
     */
    struct OtlpSectionValues
    {
        std::string                                      endpoint;       ///< 采集端地址；未配置为空
        std::optional<std::chrono::milliseconds>         requestTimeout; ///< 单次请求时限；未配置为空
        std::vector<std::pair<std::string, std::string>> headers;        ///< 附加头部（保持配置顺序）
    };

    /**
     * @brief 读并校验配置里的 otlp 子段
     * @param node otlp 子段对象
     * @param sectionPath 外层 section 的路径（用于拼出 otlp 的完整键路径）
     * @return OtlpSectionValues 读到的取值
     * @throws Base::ConfigValidationException 出现未知键、类型不符或越界
     */
    [[nodiscard]] inline OtlpSectionValues readOtlpSection(const Base::ConfigValue &node, const std::string &sectionPath)
    {
        constexpr std::string_view kOtlpKeys[] = {"endpoint", "timeout_ms", "headers"};
        const std::string          pathPrefix  = sectionPath + ".otlp";
        HttpConfig::rejectUnknownKeys(node, kOtlpKeys, std::size(kOtlpKeys), pathPrefix);

        OtlpSectionValues values;
        if (node.contains("endpoint"))
        {
            values.endpoint = HttpConfig::requireNonEmptyString(node.at("endpoint"), pathPrefix + ".endpoint");
        }
        if (node.contains("timeout_ms"))
        {
            const std::string timeoutKey = pathPrefix + ".timeout_ms";
            values.requestTimeout        = HttpConfig::toMilliseconds(HttpConfig::requirePositiveInteger(node.at("timeout_ms"), timeoutKey), timeoutKey);
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
                values.headers.emplace_back(name, value.get<std::string>());
            }
        }
        return values;
    }
} // namespace AsynGyanis::Net::Telemetry
