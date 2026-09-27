/**
 * @file TracingConfiguration.h
 * @brief 链路子系统的可配置项：从配置文档读出，并按读出的结果装出编排器与出口
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Tracing/Tracer.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 链路配置在文档里的段名
    inline constexpr std::string_view kTracingConfigSection = "tracing";

    /**
     * @brief 链路子系统的可配置项集合
     * @details 没写的键保持这里的默认值，因此配置里只写要改的那几项。`otlpEndpoint` 与 `spanFilePath`
     *          是「挂哪些出口」的开关：填哪个就挂哪个，两个都填就同时挂（各自的失败各自计数）。
     * @see readTracingConfiguration(), buildTracer()
     */
    struct TracingConfiguration
    {
        bool                               enabled{false};           ///< 总开关；false 时不建编排器，其余取值一概不看
        std::string                        serviceName{};            ///< enabled 时必填：出口侧的 service.name
        std::string                        serviceVersion{};         ///< 可空：非空时随 resource 一起报出
        double                             sampleRatio{1.0};         ///< 新链路的采样比例，0.0~1.0
        std::size_t                        pendingSpanCount{8192};   ///< 待出口缓冲的条数上界
        std::size_t                        batchSpanCount{512};      ///< 攒够这么多就唤一次出口线程
        std::chrono::milliseconds          exportInterval{2000};     ///< 没攒够也在这个点上出口一次
        std::string                        otlpEndpoint{};           ///< OTLP/HTTP 采集端地址；空串表示不挂这一路出口
        std::chrono::milliseconds          otlpRequestTimeout{3000}; ///< 单次 OTLP 请求的整体时限
        std::vector<HttpClientHeaderField> otlpHeaders{};            ///< OTLP 请求的附加头部（鉴权令牌这类）
        std::string                        spanFilePath{};           ///< 链路文件的落盘路径；空串表示不挂文件出口
    };

    /**
     * @brief 从配置文档的 tracing 段读出链路配置
     * @param configurationRoot 已嵌好段的文档根（ConfigManager 的键是扁平的，取段要自己 emplace 一次）
     * @return TracingConfiguration 读出来的配置；缺失的键取结构体默认值
     * @throws Base::ConfigValidationException 段或子段不是对象、出现未知键、类型不符或取值越界
     * @note 未知键算错是刻意的：写成 `sample_rate` 而这里读的是 `sample_ratio` 时静默忽略，
     *       等于配置没生效却看不出来
     * @see readHttpServerConfiguration() —— 同一套「未知键即拒」的口径
     */
    [[nodiscard]] TracingConfiguration readTracingConfiguration(const Base::ConfigValue &configurationRoot);

    /**
     * @brief 按读出的配置装出编排器，并挂上配置点名的出口
     * @param configuration 已读出的链路配置
     * @return std::shared_ptr<Tracer> enabled 为真时交出编排器；为假交出空指针（调用方据此不挂中间件）
     * @throws Base::LogicException 服务名为空或比例/上界/批量/时限不成立（Tracer 自己的判据）
     * @throws Base::InvalidArgumentException otlp.endpoint 为空或畸形、附加头部写法会撕裂请求行
     * @note 出口的所有权交给编排器：它们的存活期就是「这条进程还在记链路」，与编排器同生同死最省事
     * @see Tracer::create(), OtlpHttpSpanExporter, FileSpanExporter
     */
    [[nodiscard]] std::shared_ptr<Tracer> buildTracer(const TracingConfiguration &configuration);
} // namespace AsynGyanis::Net
