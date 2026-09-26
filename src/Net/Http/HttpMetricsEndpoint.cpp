#include "Net/Http/HttpMetricsEndpoint.h"

#include <array>
#include <cstddef>
#include <format>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 把前缀与指标名拼起来
         * @param metricNamePrefix 前缀；空串表示不带前缀
         * @param metricName 指标名（ASCII）
         * @return std::string 完整指标名
         */
        [[nodiscard]] std::string makeMetricName(const std::string_view metricNamePrefix, const std::string_view metricName)
        {
            // 空前缀不留下划线：否则会产出以 "_" 开头的非法指标名
            if (metricNamePrefix.empty())
            {
                return std::string(metricName);
            }
            return std::format("{}_{}", metricNamePrefix, metricName);
        }

        /**
         * @brief 追加一族计数器的 HELP/TYPE 与单条样本
         * @param out 输出缓冲
         * @param metricName 完整指标名
         * @param help 中文说明（该格式是 UTF-8，无需转义）
         * @param value 计数值
         */
        void appendCounter(std::string &out, const std::string &metricName, const std::string_view help,
                           const std::uint64_t value)
        {
            out += std::format("# HELP {} {}\n# TYPE {} counter\n{} {}\n", metricName, help, metricName, metricName, value);
        }
    } // namespace

    std::string formatPrometheusMetrics(const HttpServerStats &stats, const std::string_view metricNamePrefix)
    {
        std::string out;

        // 预留一些容量：一整套指标大约 40 行、每行几十字节，省掉反复扩容
        out.reserve(2048);

        appendCounter(out, makeMetricName(metricNamePrefix, "requests_total"), "已收齐的请求条数", stats.totalRequestCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "bad_requests_total"), "解析失败或协议错误收口的请求条数",
                      stats.badRequestCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "timeout_closed_connections_total"),
                      "被空闲清扫按超时关闭的连接数", stats.timeoutClosedCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "write_aborted_connections_total"),
                      "本侧没能把响应完整交给传输层就收口的连接数（写出失败，或仍有字节留在待发缓冲与流控队列；仅 TCP 侧）",
                      stats.writeAbortedConnectionCount);

        // 活跃连接数是瞬时量，用 gauge；取值来自连接管理器，见 HttpServer::stats()
        const std::string activeConnectionsName = makeMetricName(metricNamePrefix, "active_connections");
        out += std::format("# HELP {} 取快照那一刻的活跃连接数\n# TYPE {} gauge\n{} {}\n", activeConnectionsName,
                           activeConnectionsName, activeConnectionsName, stats.activeConnectionCount);

        // 状态码分类：一族带 status_class 标签的计数器，采集侧可直接按类聚合
        const std::string responsesName = makeMetricName(metricNamePrefix, "responses_total");
        out += std::format("# HELP {} 已发出的响应条数，按状态码类聚合\n# TYPE {} counter\n", responsesName, responsesName);
        const std::array<std::pair<const char *, std::uint64_t>, 5> statusClassCounts{{
                {"1xx", stats.status1xxCount},
                {"2xx", stats.status2xxCount},
                {"3xx", stats.status3xxCount},
                {"4xx", stats.status4xxCount},
                {"5xx", stats.status5xxCount},
        }};
        for (const auto &[statusClass, count]: statusClassCounts)
        {
            out += std::format("{}{{status_class=\"{}\"}} {}\n", responsesName, statusClass, count);
        }

        // 耗时直方图：档位上界是毫秒常量，导出时换算成秒（Prometheus 的时间单位约定）
        const std::string durationName = makeMetricName(metricNamePrefix, "request_duration_seconds");
        out += std::format("# HELP {} 已应答请求的耗时分布\n# TYPE {} histogram\n", durationName, durationName);

        // 展示格式的分档语义是**累计**：第 i 条是「耗时不超过该上界」的观测数；而本框架的直方图
        // 记的是每一档区间内的条数（不累计）。这里必须自己累加，否则 histogram_quantile() 算出的
        // 分位会系统性偏小——两套语义只差一个前缀和，错了不会报错、只会悄悄给错数
        std::uint64_t cumulativeBucketCount = 0;
        for (std::size_t index = 0; index < kHttpLatencyBucketCount; ++index)
        {
            cumulativeBucketCount += stats.latencyBucketCounts[index];

            // 末档没有上界，按约定记作 +Inf；其余档把毫秒上界换算成秒
            const bool isOverflowBucket = index + 1 == kHttpLatencyBucketCount;
            const std::string upperBound = isOverflowBucket
                                                   ? std::string("+Inf")
                                                   : std::format("{}", static_cast<double>(kHttpLatencyUpperBoundMilliseconds[index]) / 1000.0);
            out += std::format("{}_bucket{{le=\"{}\"}} {}\n", durationName, upperBound, cumulativeBucketCount);
        }
        // _sum 用累计微秒换算成秒。定点六位小数而不是默认的 {}：默认格式对小于 1e-3 的值会切到
        // 科学计数（0.0005 会写成 5e-04），虽然 Prometheus 认这种写法，但固定小数位既更可读，
        // 也正好对上累加器的微秒精度，还免了「同一个值在不同时刻长相不同」的比对麻烦
        out += std::format("{}_sum {:.6f}\n", durationName, static_cast<double>(stats.totalLatencyMicroseconds) / 1'000'000.0);
        out += std::format("{}_count {}\n", durationName, stats.latencySampleCount());

        // WebSocket 与 HTTP/2 各一族：名字里带协议前缀，避免与 HTTP 侧的计数混在一张图里
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_upgrades_total"), "升级成功的 WebSocket 连接数",
                      stats.webSocketUpgradeCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_messages_total"), "收到的 WebSocket 数据消息条数",
                      stats.webSocketMessageCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_protocol_error_closes_total"),
                      "因对端违反 RFC 6455 而收口的连接数", stats.webSocketProtocolErrorCloseCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_peer_closes_total"), "由对端发起关闭握手的连接数",
                      stats.webSocketPeerCloseCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_server_closes_total"), "由本侧发起关闭握手的连接数",
                      stats.webSocketServerCloseCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "http2_stream_cancelled_total"),
                      "被对端 RST_STREAM 取消了单流的 HTTP/2 请求条数", stats.streamCancelledCount);

        // 发送路径：零拷贝发送只在 Linux 的明文 HTTP/1.1 上发生，其余平台恒为 0，
        // 因此它同时是「静态文件快路径是否在生效」的探针
        appendCounter(out, makeMetricName(metricNamePrefix, "zerocopy_sends_total"),
                      "正文经内核零拷贝（sendfile）直接发出的响应条数（仅 Linux 会增长）", stats.zeroCopySendCount);

        // 准入闸门：被按来源 IP 的并发限额挡掉的连接不会成为「连接」，因此在其它任何计数里都不留痕，
        // 只有这一条能说明闸门有没有在做事
        appendCounter(out, makeMetricName(metricNamePrefix, "admission_rejected_connections_total"),
                      "被按来源 IP 的并发限额挡掉的连接条数（限额器可在多条通道间共用，报的是总量）",
                      stats.admissionRejectedConnectionCount);

        return out;
    }

} // namespace AsynGyanis::Net
