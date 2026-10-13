/**
 * @file OtlpMetricsJson.h
 * @brief 把一批进程读数渲染成 OTLP/HTTP 的 JSON 正文
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Tracing/SpanExporter.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// metrics 出口正文里 instrumentationScope 的名字：与链路出口同值、各自登记——OTLP 的 scope 按 signal 分开上报，共用一个符号会让两边想各自改名时互相牵制
    inline constexpr std::string_view kOtlpMetricsScopeName = "AsynGyanis";

    /// OTLP/HTTP 的正文媒体类型：本出口发的是 protobuf 的 JSON mapping，不是 protobuf（与链路出口的同值常量各自登记，理由同 scope 名）
    inline constexpr std::string_view kOtlpMetricsJsonContentType = "application/json";

    /**
     * @brief 把一批进程读数渲染成 OTLP/HTTP 的请求正文（`POST /v1/metrics`，Content-Type: application/json）
     *
     * @details 走 protobuf 的 JSON mapping（与链路出口同一口径，不引入 protobuf 依赖）：
     *          `ProcessMetricKind::Counter` 折成 `sum`（`isMonotonic` 为真、`aggregationTemporality` 取 2
     *          ＝ CUMULATIVE——注册表交出的本来就是自进程启动起的累计值）；`Gauge` 折成 `gauge`。
     *          64 位取值与时间一律写成**字符串**，与各语言 OTel SDK 的输出一致。
     * @param resource 这批读数的出处（服务名与版本）；形状与链路出口共用同一份 TraceResource
     * @param samples 一份读数快照，按注册表的登记顺序；空快照也渲染出结构完整的正文
     * @param collectionMoment 本批的采集时刻，充任每个数据点的 `timeUnixNano`；由调用方给出，渲染因此可复现可断言
     * @return std::string 可直接作为请求正文的 JSON；空串表示有一个取值进不了 JSON（正文含非法 UTF-8）
     * @note 返回空串时整批都发不出去，不只是那一条——与链路出口同一条纪律：一份 OTLP 文档只有一个正文
     * @see OtlpHttpMetricsExporter, formatOtlpTracesJson()
     */
    [[nodiscard]] ASYN_NET_API std::string formatOtlpMetricsJson(const TraceResource &resource, const std::vector<Core::ProcessMetricSample> &samples,
                                                                 std::chrono::system_clock::time_point collectionMoment);
} // namespace AsynGyanis::Net
