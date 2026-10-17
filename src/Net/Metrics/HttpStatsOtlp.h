/**
 * @file HttpStatsOtlp.h
 * @brief 把一台 HTTP 服务器的统计快照折成 OTLP 读数批（推进模式的适配层）
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Http/HttpServerStats.h"
#include "Net/Metrics/OtlpMetricsJson.h"

#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 把 HttpServerStats 折成 OTLP 读数批（标量 + responses_total 标签族 + 延迟直方图）
     *
     * @details 名字与口径与 `/metrics` 的 Prometheus 渲染逐条对齐（同一份前缀、同样的 `*_total`
     *          约定、直方图同样换算成秒），两份出口报的是同一件事的两种编码——面板从一处切到
     *          另一处不用改查询。两处容易走样的换算在这里一次做对：直方图档位上界是毫秒常量、
     *          `_sum` 是微秒累计，折成 OTLP 时都换到秒；`bucketCounts` 保持「每格条数」语义
     *          （OTLP 与 stats 同形，不做 Prometheus 那种前缀和，那属于展示端的活）。
     *          刻意不映射的字段：stats 里的进程级读数（常驻内存、CPU 秒、句柄数、启动时刻、
     *          阻塞执行器积压）与日志丢弃计数——它们不是某台服务器的量，推进模式一行留给进程级
     *          读数注册表（同一份数不从两条路各报一次）。
     * @param stats 一份统计快照（调用方按自己的节奏现取）
     * @param metricNamePrefix 指标名前缀，与 `/metrics` 的 `metric_name_prefix` 同一取值；空串表示不带
     * @return OtlpMetricsBatch 标量批 + 标签族 + 直方图；全零快照同样交出结构完整的批
     */
    [[nodiscard]] ASYN_NET_API OtlpMetricsBatch buildOtlpMetricsFromHttpStats(const HttpServerStats &stats, std::string_view metricNamePrefix);
} // namespace AsynGyanis::Net
