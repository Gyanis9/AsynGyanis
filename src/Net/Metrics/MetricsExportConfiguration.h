/**
 * @file MetricsExportConfiguration.h
 * @brief 进程读数导出的可配置项：从配置文档读出，并按读出的结果装出出口
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Config/ConfigValue.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Metrics/OtlpHttpMetricsExporter.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 读数导出配置在文档里的段名
    inline constexpr std::string_view kMetricsExportConfigSection = "metrics";

    /**
     * @brief 读数导出的可配置项集合
     * @details 没写的键保持这里的默认值，因此配置里只写要改的那几项。服务名独立于 tracing 段：
     *          两个开关各读各的，不会出现「metrics 段配全了却因为 tracing 段没写而不生效」这种跨段耦合。
     * @see readMetricsExportConfiguration(), buildMetricsExporter()
     */
    struct ASYN_NET_API MetricsExportConfiguration
    {
        bool                               enabled{false};           ///< 总开关；false 时不建出口，其余取值一概不看
        std::string                        serviceName{};            ///< enabled 时必填：推送侧 resource 的 service.name
        std::string                        serviceVersion{};         ///< 可空：非空时随 resource 一起报出
        std::string                        otlpEndpoint{};           ///< OTLP/HTTP 采集端地址；enabled 时必填
        std::chrono::milliseconds          exportInterval{15000};    ///< 导出周期：每隔这么久推一份快照
        std::chrono::milliseconds          otlpRequestTimeout{3000}; ///< 单次 OTLP 请求的整体时限
        std::vector<HttpClientHeaderField> otlpHeaders{};            ///< OTLP 请求的附加头部（鉴权令牌这类）
    };

    /**
     * @brief 从配置文档的 metrics 段读出读数导出配置
     * @param configurationRoot 已嵌好段的文档根（ConfigManager 的键是扁平的，取段要自己 emplace 一次）
     * @return MetricsExportConfiguration 读出来的配置；缺失的键取结构体默认值
     * @throws Base::ConfigValidationException 段或子段不是对象、出现未知键、类型不符或取值越界
     * @note 未知键算错是刻意的（与 tracing 段同一条口径）：写成 `export_interval` 而这里读的是
     *       `export_interval_ms` 时静默忽略，等于配置没生效却看不出来
     * @see readTracingConfiguration() —— 同一套「未知键即拒」的口径
     */
    [[nodiscard]] ASYN_NET_API MetricsExportConfiguration readMetricsExportConfiguration(const Base::ConfigValue &configurationRoot);

    /**
     * @brief 按读出的配置装出出口；快照来源固定为本进程的读数注册表
     * @param configuration 已读出的读数导出配置
     * @param extraReadings 每期追加的读数批（可选）：与注册表快照在三路上合并后一起送出——
     *        给的是「注册表表达不了的读数」（如某台 HTTP 服务器的统计与延迟直方图，经
     *        buildOtlpMetricsFromHttpStats 折成批）；空函数对象表示只用注册表
     * @return std::unique_ptr<OtlpHttpMetricsExporter> enabled 为真时交出出口；为假交出空指针（调用方据此不挂）
     * @throws Base::LogicException 服务名为空（enabled 时的守卫；读取器已先拦一道）
     * @throws Base::InvalidArgumentException otlp.endpoint 为空或畸形、附加头部写法会撕裂请求行
     * @note 快照来源是 Core::ProcessMetricsRegistry::samples()——每个导出周期现取一次；
     *       出口的所有权交给调用方（进程级对象，与进程同生共死最省事）
     * @see readMetricsExportConfiguration(), OtlpHttpMetricsExporter
     */
    [[nodiscard]] ASYN_NET_API std::unique_ptr<OtlpHttpMetricsExporter> buildMetricsExporter(const MetricsExportConfiguration &configuration,
                                                                                             std::function<OtlpMetricsBatch()> extraReadings = {});
} // namespace AsynGyanis::Net
