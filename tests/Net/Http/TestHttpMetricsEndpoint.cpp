// 指标导出与健康检查端点：Prometheus 文本渲染的逐字节契约，以及回环上的真实抓取
#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerStats.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 抓取指标与健康检查时用的等待上限
        constexpr std::chrono::milliseconds kEndpointTimeout{2000};

        /**
         * @brief 从渲染结果里取出某个单值指标的数字
         * @param text 渲染结果
         * @param metricNameWithTrailingSpace 指标名（含其后的一个空格）
         * @return std::optional<std::uint64_t> 解析到的值；指标不存在或数字非法时为空
         * @note 端到端用例只断言「至少打过的那条已计入」，不去钉具体数字：抓 /metrics 的这一次
         *       自身也会被计入（请求在进处理函数之前就已记数），钉死数字等于把实现细节写进断言
         */
        [[nodiscard]] std::optional<std::uint64_t> findMetricValue(const std::string &text,
                                                                  const std::string_view metricNameWithTrailingSpace)
        {
            // 只在**行首**出现的指标名才算样本行：# HELP/# TYPE 两行里也有同样的名字，
            // 从它们后面取数会取到中文说明，解析必然失败
            std::size_t searchBegin = 0;
            while (true)
            {
                const std::size_t namePosition = text.find(metricNameWithTrailingSpace, searchBegin);
                if (namePosition == std::string::npos)
                {
                    return std::nullopt;
                }
                searchBegin = namePosition + 1;

                if (namePosition != 0 && text[namePosition - 1] != '\n')
                {
                    continue;
                }

                const std::size_t valueBegin = namePosition + metricNameWithTrailingSpace.size();
                std::size_t valueEnd         = text.find('\n', valueBegin);
                if (valueEnd == std::string::npos)
                {
                    return std::nullopt;
                }

                // 这里读的是**线上报文的正文**，行尾是 CRLF：把归行的 \r 从数字里剔掉，
                // 否则 from_chars 会停在不该停的位置
                if (valueEnd > valueBegin && text[valueEnd - 1] == '\r')
                {
                    --valueEnd;
                }
                if (valueEnd == valueBegin)
                {
                    return std::nullopt;
                }

                std::uint64_t value = 0;
                const auto [parsedEnd, errorCode] = std::from_chars(text.data() + valueBegin, text.data() + valueEnd, value);
                if (errorCode == std::errc{} && parsedEnd == text.data() + valueEnd)
                {
                    return value;
                }
            }
        }

        /**
         * @brief 造一份各字段都非零的快照：只用真值去渲染，才能发现「漏了一个字段」这类漂移
         * @return HttpServerStats 手填的快照
         */
        [[nodiscard]] HttpServerStats makeFullyPopulatedStats()
        {
            HttpServerStats stats;
            stats.totalRequestCount  = 11;
            stats.activeConnectionCount = 2;
            stats.badRequestCount    = 3;
            stats.timeoutClosedCount = 4;
            stats.status1xxCount     = 1;
            stats.status2xxCount     = 5;
            stats.status3xxCount     = 0;
            stats.status4xxCount     = 2;
            stats.status5xxCount     = 1;
            // 直方图与 sum 对得上：两档各 1 条，合计 2 条样本
            stats.latencyBucketCounts[0]     = 1;
            stats.latencyBucketCounts[1]     = 1;
            stats.totalLatencyMicroseconds   = 500;
            stats.webSocketUpgradeCount            = 6;
            stats.webSocketMessageCount            = 7;
            stats.webSocketProtocolErrorCloseCount = 8;
            stats.webSocketPeerCloseCount          = 9;
            stats.webSocketServerCloseCount        = 10;
            stats.streamCancelledCount             = 12;
            stats.zeroCopySendCount                = 13;
            return stats;
        }
    } // namespace

    /**
     * @brief 钉住：渲染出的每一行都符合展示格式（HELP/TYPE 齐全、指标名带前缀、数值就位）
     */
    TEST(HttpMetricsEndpoint, RendersEveryFieldInPrometheusTextFormat)
    {
        const std::string text = formatPrometheusMetrics(makeFullyPopulatedStats(), "asyn_http");

        // 计数器：每族都必须是 HELP + TYPE + 一条样本
        EXPECT_NE(text.find("# TYPE asyn_http_requests_total counter\nasyn_http_requests_total 11\n"), std::string::npos) << text;
        EXPECT_NE(text.find("asyn_http_bad_requests_total 3\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_timeout_closed_connections_total 4\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_websocket_upgrades_total 6\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_websocket_messages_total 7\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_websocket_protocol_error_closes_total 8\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_websocket_peer_closes_total 9\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_websocket_server_closes_total 10\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_http2_stream_cancelled_total 12\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_zerocopy_sends_total 13\n"), std::string::npos);

        // 活跃连接数是瞬时量，必须是 gauge——报成 counter 采集侧会去算增长率
        EXPECT_NE(text.find("# TYPE asyn_http_active_connections gauge\nasyn_http_active_connections 2\n"), std::string::npos);

        // 状态码分类：一族带标签的计数器，五类都在
        EXPECT_NE(text.find("asyn_http_responses_total{status_class=\"1xx\"} 1\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_responses_total{status_class=\"2xx\"} 5\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_responses_total{status_class=\"4xx\"} 2\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_responses_total{status_class=\"5xx\"} 1\n"), std::string::npos);
    }

    /**
     * @brief 钉住：直方图三件套齐备，档位上界换算成秒、**分档按累计口径输出**、末档为 +Inf
     */
    TEST(HttpMetricsEndpoint, RendersLatencyHistogramWithSumAndCount)
    {
        const std::string text = formatPrometheusMetrics(makeFullyPopulatedStats(), "asyn_http");

        EXPECT_NE(text.find("# TYPE asyn_http_request_duration_seconds histogram\n"), std::string::npos) << text;
        // 档位上界是毫秒常量，导出成秒；四个上界 + 一个溢出档。
        // 快照里前两档各 1 条，而展示格式的 le 是累计口径，故从第二档起就都是 2——
        // 照着「每档区间条数」原样输出会让采集侧算出的分位系统性偏小
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_bucket{le=\"0.001\"} 1\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_bucket{le=\"0.005\"} 2\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_bucket{le=\"0.02\"} 2\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_bucket{le=\"0.1\"} 2\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_bucket{le=\"+Inf\"} 2\n"), std::string::npos);
        // 500 微秒 = 0.000500 秒（定点六位小数，见渲染处的说明）；样本数取各档之和，与 +Inf 档一致
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_sum 0.000500\n"), std::string::npos) << text;
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_count 2\n"), std::string::npos) << text;
    }

    /**
     * @brief 钉住：空前缀不带下划线（否则会产出以 "_" 开头的非法指标名）
     */
    TEST(HttpMetricsEndpoint, EmptyPrefixProducesPlainMetricNames)
    {
        const std::string text = formatPrometheusMetrics(makeFullyPopulatedStats(), "");
        EXPECT_NE(text.find("# TYPE requests_total counter\n"), std::string::npos) << text;
        // 「不留空下划线」的判据是「没有任何指标名以 _ 开头」：不能直接搜 "_requests_total"，
        // 那个子串在 bad_requests_total 里合法存在
        EXPECT_EQ(text.find("# HELP _"), std::string::npos) << "空前缀不该留下划线";
        EXPECT_EQ(text.find("# TYPE _"), std::string::npos) << "空前缀不该留下划线";
    }

    /**
     * @brief 钉住：全零快照也要渲染出完整的 0 值序列（缺行会让采集侧以为指标消失了）
     */
    TEST(HttpMetricsEndpoint, RendersZeroValuedSeriesForEmptyStats)
    {
        const std::string text = formatPrometheusMetrics(HttpServerStats{}, "asyn_http");
        EXPECT_NE(text.find("asyn_http_requests_total 0\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_request_duration_seconds_count 0\n"), std::string::npos);
        EXPECT_NE(text.find("asyn_http_active_connections 0\n"), std::string::npos);
        // 空文本以换行结尾：抓取工具按行解析，末行没有换行会被吞掉
        ASSERT_FALSE(text.empty());
        EXPECT_EQ(text.back(), '\n');
    }

    /**
     * @brief 钉住：路径不以 / 开头属于用法错误，必须当场抛出而不是静默注册一个永不匹配的路由
     */
    TEST(HttpMetricsEndpoint, RejectsPathWithoutLeadingSlash)
    {
        Core::EventLoop loop;
        HttpServer server(loop, Core::InetAddress::localhost(0));

        EXPECT_THROW(server.enableMetricsEndpoint("metrics"), Base::InvalidArgumentException);
        EXPECT_THROW(server.enableMetricsEndpoint(""), Base::InvalidArgumentException);
        EXPECT_THROW(server.enableHealthEndpoint("healthz"), Base::InvalidArgumentException);
    }

    /**
     * @brief 端到端：回环上真实抓一次 /metrics 与 /healthz，验状态码、内容类型与关键行
     */
    TEST(HttpMetricsEndpoint, ServesMetricsAndHealthOverLoopback)
    {
        const ServerConfigurator configureServer = [](TestHttpServer &server)
        {
            server.enableMetricsEndpoint();
            server.enableHealthEndpoint();
        };
        // 只调本用例关心的配置，其余取默认：路由注册与端口绑定都在 start() 之前落定
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, {},
                                         HttpParserLimits{}, configureServer);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        // 先打一条业务请求，让计数与直方图非零
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid()) << "回环连接失败";
            std::string receivedText;
            ASSERT_TRUE(client.sendText("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", kEndpointTimeout));
            ASSERT_TRUE(client.waitForText(receivedText, "served-hello", kEndpointTimeout)) << "业务请求未得到响应";
        }

        // /healthz：200 + 固定 JSON。能由事件循环里的会话协程答出来，就说明循环还在服务连接
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid());
            std::string receivedText;
            ASSERT_TRUE(client.sendText("GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", kEndpointTimeout));
            ASSERT_TRUE(client.waitForText(receivedText, kHealthCheckResponseBody, kEndpointTimeout)) << "健康检查未得到应答";
            EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << receivedText;
            EXPECT_NE(receivedText.find("application/json"), std::string::npos) << receivedText;
        }

        // /metrics：内容类型是 Prometheus 文本，正文里能看到刚打过的那条请求与直方图
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid());
            std::string receivedText;
            ASSERT_TRUE(client.sendText("GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", kEndpointTimeout));
            ASSERT_TRUE(client.waitForText(receivedText, "asyn_http_request_duration_seconds_count", kEndpointTimeout))
                    << "指标端点未返回直方图计数";
            EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << receivedText;
            EXPECT_NE(receivedText.find("text/plain; version=0.0.4"), std::string::npos) << receivedText;
            // 刚打过的 /hello 必须已计入：抓 /metrics 这一次自身也会被计入，故只断言「至少 1 条」
            const std::optional<std::uint64_t> servedRequestCount =
                    findMetricValue(receivedText, "asyn_http_requests_total ");
            ASSERT_TRUE(servedRequestCount.has_value()) << "缺少请求总数指标：「" << receivedText << "」";
            EXPECT_GE(*servedRequestCount, 1u) << "刚打过的请求没有计入请求总数";
            const std::optional<std::uint64_t> latencySampleCount =
                    findMetricValue(receivedText, "asyn_http_request_duration_seconds_count ");
            ASSERT_TRUE(latencySampleCount.has_value()) << "缺少直方图样本数";
            EXPECT_GE(*latencySampleCount, 1u) << "已应答请求没有落进直方图";
        }

        // 未注册的端点仍走兜底 404：开启两个端点不该顺手把别的路径也放行
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid());
            std::string receivedText;
            ASSERT_TRUE(client.sendText("GET /not-registered HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", kEndpointTimeout));
            ASSERT_TRUE(client.waitForText(receivedText, "404", kEndpointTimeout)) << "未知路径未被兜底路由处理";
        }
    }

} // namespace AsynGyanis::Net
