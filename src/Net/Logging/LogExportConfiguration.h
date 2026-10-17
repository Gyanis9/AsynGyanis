/**
 * @file LogExportConfiguration.h
 * @brief `log_export` 配置段：把 root 日志推给 OTLP 采集端
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Net/Http/Client/HttpClient.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 日志导出在配置文档里的段名
    inline constexpr std::string_view kLogExportConfigSection = "log_export";

    /**
     * @brief `log_export` 段的取值
     * @details 与 `metrics` 段同一形状：段缺失或 `enabled` 为 false 即关着；开了就必须有服务名与采集端地址。
     *          段内未知键当场拒——`export_interval` 少写 `_ms` 不会被静默忽略。
     */
    struct ASYN_NET_API LogExportConfiguration
    {
        bool                               enabled{false};                  ///< 是否启用 OTLP 日志推送
        std::string                        serviceName{};                   ///< 服务名（OTLP resource 的 service.name）
        std::string                        serviceVersion{};                ///< 服务版本（可空）
        std::string                        otlpEndpoint{};                  ///< 采集端地址，形如 http(s)://collector:4318/v1/logs
        std::chrono::milliseconds          exportInterval{5000};            ///< 攒批周期
        std::chrono::milliseconds          requestTimeout{3000};            ///< 单次请求时限
        std::chrono::milliseconds          shutdownTimeout{3000};           ///< 收尾期限
        std::size_t                        maximumPendingRecordCount{4096}; ///< 待发记录上界
        std::size_t                        maximumBatchRecordCount{256};    ///< 单批条数上界
        std::vector<HttpClientHeaderField> otlpHeaders{};                   ///< 附加头部（鉴权令牌这类）
    };

    /**
     * @brief 读 `log_export` 段
     * @param configurationRoot 配置文档根
     * @return LogExportConfiguration 段缺失时返回默认值（关着）
     * @throws Base::ConfigValidationException 段内未知键、类型不符、或启用时缺少必填项
     */
    [[nodiscard]] ASYN_NET_API LogExportConfiguration readLogExportConfiguration(const Base::ConfigValue &configurationRoot);

    /**
     * @brief 按配置造一个 OTLP 日志 Sink
     * @param configuration 已读好的配置
     * @return std::unique_ptr<Base::LogSink> 关着时为空；开了时由调用方交给 root logger 接管
     * @throws Base::InvalidArgumentException 出口参数不成立（地址畸形、头部非法等）
     * @throws Base::LogicException 启用但服务名为空（读取器本应已拦下）
     */
    [[nodiscard]] ASYN_NET_API std::unique_ptr<Base::LogSink> buildOtlpLogSink(const LogExportConfiguration &configuration);
} // namespace AsynGyanis::Net
