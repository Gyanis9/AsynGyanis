#include "Net/Metrics/HttpStatsOtlp.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// @brief 拼指标名：前缀 + 下划线 + 后缀（与 formatPrometheusMetrics 的 makeMetricName 同规则；空前缀不留下划线）
        std::string makeName(const std::string_view prefix, const std::string_view suffix)
        {
            if (prefix.empty())
            {
                return std::string(suffix);
            }
            std::string name;
            name.reserve(prefix.size() + 1 + suffix.size());
            name.append(prefix);
            name.push_back('_');
            name.append(suffix);
            return name;
        }
    } // namespace

    OtlpMetricsBatch buildOtlpMetricsFromHttpStats(const HttpServerStats &stats, const std::string_view metricNamePrefix)
    {
        OtlpMetricsBatch batch;

        const auto addCounter = [&batch, metricNamePrefix](const std::string_view suffix, const std::string_view help, const std::uint64_t value)
        {
            batch.scalars.push_back(
                    Core::ProcessMetricSample{.name = makeName(metricNamePrefix, suffix), .help = std::string(help), .kind = Core::ProcessMetricKind::Counter, .value = value});
        };
        const auto addGauge = [&batch, metricNamePrefix](const std::string_view suffix, const std::string_view help, const std::uint64_t value)
        {
            batch.scalars.push_back(
                    Core::ProcessMetricSample{.name = makeName(metricNamePrefix, suffix), .help = std::string(help), .kind = Core::ProcessMetricKind::Gauge, .value = value});
        };

        // 请求与连接：文案与 /metrics 的逐条登记一致（同一件事的两种编码，措辞不另起一套）
        addCounter("requests_total", "交给业务处理的请求条数（被挡下的进 bad_requests_total）", stats.totalRequestCount);
        addCounter("bad_requests_total", "解析失败或协议错误收口的请求条数", stats.badRequestCount);
        addCounter("timeout_closed_connections_total", "被空闲清扫按超时关闭的连接数", stats.timeoutClosedCount);
        addCounter("write_aborted_connections_total", "本侧没能把响应完整交给传输层就收口的连接数（仅 TCP 侧）", stats.writeAbortedConnectionCount);
        addGauge("active_connections", "取快照那一刻的活跃连接数（多条监听器共用一份采集端时是各台相加）", stats.activeConnectionCount);
        addGauge("maximum_connections", "本监听器的并发连接上限（0 = 不设这道限）", stats.maximumConnections);
        addGauge("maximum_connections_per_ip", "单个来源 IP 的并发上限（0 = 这道闸门没装）", stats.maximumConnectionsPerIp);

        // 状态码五类：OTLP 侧的正统形状是一条 metric 多数据点、每点带属性。状态码类这一组带
        // route 标签（路由模式原文或固定哨兵），具体状态码仍按全局一维报——route × status 的
        // 笛卡尔积不在基数预算里（口径见 HttpRouteCount）
        {
            OtlpLabeledSample responses;
            responses.name = makeName(metricNamePrefix, "responses_total");
            responses.help = "已发出的响应条数，按路由模式（route）与状态码类（status_class）聚合；具体状态码（status）另按全局一维报";
            responses.kind = Core::ProcessMetricKind::Counter;
            constexpr std::array<std::string_view, 5> kStatusClassNames{"1xx", "2xx", "3xx", "4xx", "5xx"};
            for (const HttpRouteCount &routeCount: httpRouteCounts(stats))
            {
                for (std::size_t classIndex = 0; classIndex < kStatusClassNames.size(); ++classIndex)
                {
                    // 只输出实际出现过的类：基数用例要能数出「一条路由只有一条序列」
                    if (routeCount.statusClassCounts[classIndex] == 0)
                    {
                        continue;
                    }
                    responses.points.push_back(OtlpLabeledPoint{.attributes = {{std::string(kHttpRouteLabelKey), routeCount.route},
                                                                               {std::string(kHttpStatusClassLabelKey), std::string(kStatusClassNames[classIndex])}},
                                                                .value      = routeCount.statusClassCounts[classIndex]});
                }
            }
            // 具体状态码是同一条 metric 的第二组数据点：OTLP 允许同名 metric 的点带不同属性集，
            // 与 /metrics 的两组标签逐点同解
            for (const HttpStatusCodeCount &statusCodeCount: httpStatusCodeCounts(stats))
            {
                responses.points.push_back(
                        OtlpLabeledPoint{.attributes = {{std::string(kHttpStatusCodeLabelKey), std::to_string(statusCodeCount.statusCode)}}, .value = statusCodeCount.count});
            }
            batch.labeled.push_back(std::move(responses));
        }
        addCounter("responses_status_merged_total", "具体状态码明细表已满后被归并进状态码类那一族的响应条数（这些响应仍计入 status_class）", stats.statusCodeMergedCount);
        addCounter("route_series_merged_total", "路由明细表已满或路由模式过长后被归并到 merged 哨兵的响应条数（仍计入全局状态码类与直方图）", stats.routeSeriesMergedCount);

        // 延迟直方图：每条路由一个数据点，属性键是 route；单位换算与分格语义由公共模型一次做对
        // （Core::MetricHistogramSample），OTLP 与模型同形（每格条数、上界与和都是秒）
        {
            const Core::MetricHistogramSample boundsSample = httpLatencyHistogramSample(stats);

            OtlpHistogramSample histogram;
            histogram.name                  = makeName(metricNamePrefix, "request_duration_seconds");
            histogram.help                  = "已应答请求的耗时分布，按路由模式（route）分组";
            histogram.explicitBoundsSeconds = boundsSample.upperBoundsSeconds;
            for (const HttpRouteCount &routeCount: httpRouteCounts(stats))
            {
                const Core::MetricHistogramSample routeSample = httpRouteLatencyHistogramSample(routeCount);
                histogram.points.push_back(OtlpHistogramPoint{.attributes   = {{std::string(kHttpRouteLabelKey), routeCount.route}},
                                                              .bucketCounts = routeSample.bucketCounts,
                                                              .count        = routeSample.count,
                                                              .sumSeconds   = routeSample.sumSeconds});
            }
            batch.histograms.push_back(std::move(histogram));
        }

        // WebSocket 一族
        addCounter("websocket_upgrades_total", "升级成功的 WebSocket 连接数", stats.webSocketUpgradeCount);
        addCounter("websocket_messages_total", "收到的 WebSocket 数据消息条数", stats.webSocketMessageCount);
        addCounter("websocket_protocol_error_closes_total", "因对端违反 RFC 6455 而收口的连接数", stats.webSocketProtocolErrorCloseCount);
        addCounter("websocket_peer_closes_total", "由对端发起关闭握手的连接数", stats.webSocketPeerCloseCount);
        addCounter("websocket_server_closes_total", "由本侧发起关闭握手的连接数", stats.webSocketServerCloseCount);

        // 单流取消与零拷贝
        addCounter("http2_stream_cancelled_total", "被对端取消单流（HTTP/2 的 RST_STREAM、HTTP/3 的 RESET_STREAM）而本端未发响应的请求条数", stats.streamCancelledCount);
        addCounter("zerocopy_sends_total", "正文经内核零拷贝（sendfile）直接发出的响应条数（仅 Linux 会增长）", stats.zeroCopySendCount);

        // 准入闸门：按来源 IP 挡下的与整机满载挡下的分开报（处置相反）
        addCounter("admission_rejected_connections_total", "被按来源 IP 的并发限额挡掉的连接条数（限额器可共用，报的是总量）", stats.admissionRejectedConnectionCount);
        addCounter("over_limit_rejected_connections_total", "因本监听器并发连接上限到顶而被拒的连接条数", stats.overLimitRejectedConnectionCount);

        return batch;
    }
} // namespace AsynGyanis::Net
