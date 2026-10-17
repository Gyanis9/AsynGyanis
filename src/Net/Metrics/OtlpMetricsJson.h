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
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /// metrics 出口正文里 instrumentationScope 的名字：与链路出口同值、各自登记——OTLP 的 scope 按 signal 分开上报，共用一个符号会让两边想各自改名时互相牵制
    inline constexpr std::string_view kOtlpMetricsScopeName = "AsynGyanis";

    /// OTLP/HTTP 的正文媒体类型：本出口发的是 protobuf 的 JSON mapping，不是 protobuf（与链路出口的同值常量各自登记，理由同 scope 名）
    inline constexpr std::string_view kOtlpMetricsJsonContentType = "application/json";

    /**
     * @brief 一条带维度读数的单个数据点
     * @details 属性按给出的顺序渲染（OTLP 认数组序）；值为 64 位无符号数，渲染成字符串形态的 `asInt`
     */
    struct ASYN_NET_API OtlpLabeledPoint
    {
        std::vector<std::pair<std::string, std::string>> attributes{}; ///< 这一点的维度键值（如 status_class=2xx）
        std::uint64_t                                    value{0};     ///< 这一点的取值
    };

    /**
     * @brief 一条带维度读数：同名多数据点各带一组属性（如 responses_total 按 status_class 分点）
     * @details 折成 OTLP 的「一条 metric、多数据点、每点带 attributes」——状态码类这类家族在
     *          OTLP 侧的正统形状就是属性而不是拼进名字，采集端转回 Prometheus 时能还出同名同标签
     */
    struct ASYN_NET_API OtlpLabeledSample
    {
        std::string                   name{};                               ///< 指标名（完整，不带前缀——前缀由适配方拼好）
        std::string                   help{};                               ///< 一句中文说明
        Core::ProcessMetricKind       kind{Core::ProcessMetricKind::Gauge}; ///< Counter 折 sum、Gauge 折 gauge
        std::vector<OtlpLabeledPoint> points{};                             ///< 各数据点；空点集合不渲染
    };

    /**
     * @brief 一条分布读数里的一个带维度数据点
     * @details 与标量读数那边的 `OtlpLabeledPoint` 同形，只是承载的是分桶、count 与 sum。同一条
     *          histogram metric 下的多个点按属性区分（如 `route`），OTLP 侧的正统形状正是如此。
     */
    struct ASYN_NET_API OtlpHistogramPoint
    {
        std::vector<std::pair<std::string, std::string>> attributes{};    ///< 这一点的维度键值（如 route=/user/:id）
        std::vector<std::uint64_t>                       bucketCounts{};  ///< 每格条数（非累计），末格为溢出
        std::uint64_t                                    count{0};        ///< 样本总数
        double                                           sumSeconds{0.0}; ///< 观测值之和，单位秒
    };

    /**
     * @brief 一条分布读数（OTLP histogram 数据点）
     * @details 桶语义与 Prometheus 版的区别要盯住：OTLP 的 `bucketCounts` 是每格区间的条数
     *          （不累计），末格固定是 +Inf 溢出档，因此长度恒为 `explicitBoundsSeconds` 长度加一；
     *          这与本框架 HttpServerStats 的延迟档位同形，换算只发生在单位（毫秒/微秒 → 秒）。
     * @note 需要按维度分点时填 `points`（此时忽略下面四个单点字段）；只报一条无标签分布时沿用
     *       单点字段即可。两条路都渲染成同一条 metric 的 dataPoints，不各起一个同名 metric。
     */
    struct ASYN_NET_API OtlpHistogramSample
    {
        std::string                     name{};                  ///< 指标名（完整）
        std::string                     help{};                  ///< 一句中文说明
        std::vector<double>             explicitBoundsSeconds{}; ///< 各档上界（升序，单位秒）；长度 = bucketCounts - 1
        std::vector<std::uint64_t>      bucketCounts{};          ///< 每格条数（非累计），末格为溢出（单点模式）
        std::uint64_t                   count{0};                ///< 样本总数（与 bucketCounts 之和同解，单点模式）
        double                          sumSeconds{0.0};         ///< 观测值之和，单位秒（单点模式）
        std::vector<OtlpHistogramPoint> points{};                ///< 带维度的数据点；非空时优先于单点字段
    };

    /**
     * @brief 一次导出的读数批：三路条目合成同一份 OTLP 正文
     * @details 标量（进程级读数）与带维度/分布读数分开承载：前者沿用注册表的样本形状，
     *          后者各自带够渲染所需的一切。名字与 help 都由产出方拼好（含任何前缀），
     *          渲染层不再做命名加工。
     */
    struct ASYN_NET_API OtlpMetricsBatch
    {
        std::vector<Core::ProcessMetricSample> scalars{};    ///< 标量读数（注册表样式的单值条目）
        std::vector<OtlpLabeledSample>         labeled{};    ///< 带维度读数（一条 metric 多数据点）
        std::vector<OtlpHistogramSample>       histograms{}; ///< 分布读数（OTLP histogram）

        /// @brief 三路全空（导出的「没有读数可报」判据）
        [[nodiscard]] bool isEmpty() const noexcept
        {
            return scalars.empty() && labeled.empty() && histograms.empty();
        }
    };

    /**
     * @brief 把一批读数渲染成 OTLP/HTTP 的请求正文（`POST /v1/metrics`，Content-Type: application/json）
     *
     * @details 走 protobuf 的 JSON mapping（与链路出口同一口径，不引入 protobuf 依赖）：
     *          `ProcessMetricKind::Counter` 折成 `sum`（`isMonotonic` 为真、`aggregationTemporality` 取 2
     *          ＝ CUMULATIVE——注册表交出的本来就是自进程启动起的累计值）；`Gauge` 折成 `gauge`；
     *          带维度读数折成多数据点（每点带 attributes）；直方图折成 `histogram` 数据点
     *          （`bucketCounts` 非累计、末格溢出、`explicitBounds` 与单位秒的 `sum`/`count` 同数组）。
     *          64 位取值与时间一律写成字符串，与各语言 OTel SDK 的输出一致。
     * @param resource 这批读数的出处（服务名与版本）；形状与链路出口共用同一份 TraceResource
     * @param batch 三路条目；全空也渲染出结构完整的正文（三条空数组）
     * @param collectionMoment 本批的采集时刻，充任每个数据点的 `timeUnixNano`；由调用方给出，渲染因此可复现可断言
     * @return std::string 可直接作为请求正文的 JSON；空串表示有一个取值进不了 JSON（正文含非法 UTF-8）
     * @note 返回空串时整批都发不出去，不只是那一条——与链路出口同一条纪律：一份 OTLP 文档只有一个正文
     * @see OtlpHttpMetricsExporter, formatOtlpTracesJson()
     */
    [[nodiscard]] ASYN_NET_API std::string formatOtlpMetricsJson(const TraceResource &resource, const OtlpMetricsBatch &batch,
                                                                 std::chrono::system_clock::time_point collectionMoment);

    /**
     * @brief 只含标量的一批读数（早期入口，等价于把样本包成 batch 再渲染）
     * @param resource 出处
     * @param samples 标量样本
     * @param collectionMoment 采集时刻
     * @return std::string 同上面那支
     */
    [[nodiscard]] ASYN_NET_API std::string formatOtlpMetricsJson(const TraceResource &resource, const std::vector<Core::ProcessMetricSample> &samples,
                                                                 std::chrono::system_clock::time_point collectionMoment);
} // namespace AsynGyanis::Net
