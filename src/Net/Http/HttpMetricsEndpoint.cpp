#include "Net/Http/HttpMetricsEndpoint.h"

#include "Base/Log/Sinks/AsyncSink.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

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
        void appendCounter(std::string &out, const std::string &metricName, const std::string_view help, const std::uint64_t value)
        {
            out += std::format("# HELP {} {}\n# TYPE {} counter\n{} {}\n", metricName, help, metricName, metricName, value);
        }

        /**
         * @brief 把线程标识折成定宽十六进制指纹
         * @details 这一列只用来把快照里的行与 OS 线程对上（两条循环的线程不同，行因此可区分），
         *          不要求能反推回 native id——平台间的线程号本来就不是一套。
         * @param id 线程标识
         * @return std::string 16 位十六进制
         */
        [[nodiscard]] std::string threadFingerprint(const std::thread::id id)
        {
            static const std::hash<std::thread::id> hasher{};
            return std::format("{:016x}", hasher(id));
        }

        /// 相位名：与 Core::LoopPhase 一一对应，写成 JSON 里可直接比对的常量文本
        [[nodiscard]] std::string_view phaseName(const Core::LoopPhase phase) noexcept
        {
            switch (phase)
            {
                case Core::LoopPhase::NotStarted:
                    return "not_started";
                case Core::LoopPhase::Working:
                    return "working";
                case Core::LoopPhase::WaitingForEvents:
                    return "waiting_for_events";
            }
            return "unknown";
        }
    } // namespace

    std::string formatPrometheusMetrics(const HttpServerStats &stats, const std::string_view metricNamePrefix)
    {
        std::string out;

        // 预留一些容量：一整套指标大约 40 行、每行几十字节，省掉反复扩容
        out.reserve(2048);

        appendCounter(out, makeMetricName(metricNamePrefix, "requests_total"), "交给业务处理的请求条数（被挡下的进 bad_requests_total）", stats.totalRequestCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "bad_requests_total"), "解析失败或协议错误收口的请求条数", stats.badRequestCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "timeout_closed_connections_total"), "被空闲清扫按超时关闭的连接数", stats.timeoutClosedCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "write_aborted_connections_total"),
                      "本侧没能把响应完整交给传输层就收口的连接数（写出失败，或仍有字节留在待发缓冲与流控队列；仅 TCP 侧）", stats.writeAbortedConnectionCount);

        // 活跃连接数是瞬时量，用 gauge；取值来自连接管理器，见 HttpServer::stats()
        // 口径要说清：多台监听器共用同一份采集端时这一条是**相加的那一份**（刻意如此，否则抓取随机
        // 命中一台会看到 1/N 与计数器回落），而下面那条 maximum_connections 是**本监听器**的上限——
        // 两者不同源，别直接相除当「负载率」；要负载率就按监听器条数折算，或每台各抓各的口
        const std::string activeConnectionsName = makeMetricName(metricNamePrefix, "active_connections");
        out += std::format("# HELP {} 取快照那一刻的活跃连接数（多条监听器共用一份采集端时是各台相加）\n# TYPE {} gauge\n{} {}\n", activeConnectionsName, activeConnectionsName,
                           activeConnectionsName, stats.activeConnectionCount);

        // 两个分母：只有分子画不出「离上限还有多远」这种提前预警，只能等人记得配置文件写过什么。
        // 0 的含义各有一条：并发上限 0 = 本监听器不设这道限；单来源上限 0 = 那道闸门没装
        const std::string maximumConnectionsName = makeMetricName(metricNamePrefix, "maximum_connections");
        out += std::format("# HELP {} 本监听器的并发连接上限（下发到本监听器的实际值；0 = 不设这道限）。"
                           "多进程部署里配置的是整机数，这里报摊到本进程那一份\n# TYPE {} gauge\n{} {}\n",
                           maximumConnectionsName, maximumConnectionsName, maximumConnectionsName, stats.maximumConnections);

        const std::string maximumPerIpName = makeMetricName(metricNamePrefix, "maximum_connections_per_ip");
        out += std::format("# HELP {} 单个来源 IP 的并发上限（按 IP 的那道准入闸门按什么挡；0 = 这道闸门没装）。"
                           "限额器可以被多条通道共用一份，因此抓哪台都是同一个值\n# TYPE {} gauge\n{} {}\n",
                           maximumPerIpName, maximumPerIpName, maximumPerIpName, stats.maximumConnectionsPerIp);

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
            const bool        isOverflowBucket = index + 1 == kHttpLatencyBucketCount;
            const std::string upperBound = isOverflowBucket ? std::string("+Inf") : std::format("{}", static_cast<double>(kHttpLatencyUpperBoundMilliseconds[index]) / 1000.0);
            out += std::format("{}_bucket{{le=\"{}\"}} {}\n", durationName, upperBound, cumulativeBucketCount);
        }
        // _sum 用累计微秒换算成秒。定点六位小数而不是默认的 {}：默认格式对小于 1e-3 的值会切到
        // 科学计数（0.0005 会写成 5e-04），虽然 Prometheus 认这种写法，但固定小数位既更可读，
        // 也正好对上累加器的微秒精度，还免了「同一个值在不同时刻长相不同」的比对麻烦
        out += std::format("{}_sum {:.6f}\n", durationName, static_cast<double>(stats.totalLatencyMicroseconds) / 1'000'000.0);
        out += std::format("{}_count {}\n", durationName, stats.latencySampleCount());

        // WebSocket 与 HTTP/2 各一族：名字里带协议前缀，避免与 HTTP 侧的计数混在一张图里
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_upgrades_total"), "升级成功的 WebSocket 连接数", stats.webSocketUpgradeCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_messages_total"), "收到的 WebSocket 数据消息条数", stats.webSocketMessageCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_protocol_error_closes_total"), "因对端违反 RFC 6455 而收口的连接数", stats.webSocketProtocolErrorCloseCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_peer_closes_total"), "由对端发起关闭握手的连接数", stats.webSocketPeerCloseCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "websocket_server_closes_total"), "由本侧发起关闭握手的连接数", stats.webSocketServerCloseCount);
        // 名字里的 http2 是历史遗留，口径按 HttpServerStats 的类注释含 h3（RESET_STREAM/STOP_SENDING）：
        // 改名会让既有面板与 scripts/h2_adversarial_probe.sh 里那条注释一起失配，故只把帮助文本写准
        appendCounter(out, makeMetricName(metricNamePrefix, "http2_stream_cancelled_total"),
                      "被对端取消单流（HTTP/2 的 RST_STREAM、HTTP/3 的 RESET_STREAM/STOP_SENDING）而本端未发响应的请求条数；名字里的 http2 是历史遗留，口径含 HTTP/3",
                      stats.streamCancelledCount);

        // 发送路径：零拷贝发送只在 Linux 的明文 HTTP/1.1 上发生，其余平台恒为 0，
        // 因此它同时是「静态文件快路径是否在生效」的探针
        appendCounter(out, makeMetricName(metricNamePrefix, "zerocopy_sends_total"), "正文经内核零拷贝（sendfile）直接发出的响应条数（仅 Linux 会增长）", stats.zeroCopySendCount);

        // 准入闸门：被按来源 IP 的并发限额挡掉的连接不会成为「连接」，因此在其它任何计数里都不留痕，
        // 只有这一条能说明闸门有没有在做事
        appendCounter(out, makeMetricName(metricNamePrefix, "admission_rejected_connections_total"), "被按来源 IP 的并发限额挡掉的连接条数（限额器可在多条通道间共用，报的是总量）",
                      stats.admissionRejectedConnectionCount);

        // 整机满载与「某个来源在刷」处置相反（前者加容量、后者收紧限额），因此分成两条读数
        appendCounter(out, makeMetricName(metricNamePrefix, "over_limit_rejected_connections_total"), "因本监听器并发连接上限到顶而被拒的连接条数（未设上限时恒为 0）",
                      stats.overLimitRejectedConnectionCount);

        // 运行期积压：读的是进程级共享的那台执行器，多条通道报的是同一份读数（不是各自的份额）。
        // 自建执行器（`Queryable::useAsyncExecutor()`）不在这份读数的口径里，见 HttpServerStats 的取数处
        const std::string queueDepthName = makeMetricName(metricNamePrefix, "blocking_task_queue_depth");
        out += std::format("# HELP {} 取快照那一刻排在共享阻塞任务执行器队列里的任务条数（只含进程级共享那台，"
                           "自建执行器不计入；多条通道报同一份）\n"
                           "# TYPE {} gauge\n{} {}\n",
                           queueDepthName, queueDepthName, queueDepthName, stats.blockingTaskQueueDepth);
        appendCounter(out, makeMetricName(metricNamePrefix, "blocking_task_rejected_total"),
                      "因排队已满被拒的阻塞任务条数（只含进程级共享那台执行器，自建的不计——全部执行器的合计在 "
                      "asyn_executor_rejected_total；提交方当场收到异常，涨了就说明该降并发或加工作线程）",
                      stats.blockingTaskRejectedCount);

        // 日志被丢的规模是进程级的，而且**有一条对端可驱动的路**：JSON 版式对非法 UTF-8 整条失败，
        // 请求目标里送原始 Latin-1 字节的客户端因此能把自己在访问日志里的那行消音。没有这条读数，
        // 「审计记录被消音」与「这段时间没人写日志」在面板上是同一个形状
        appendCounter(out, makeMetricName(metricNamePrefix, "log_dropped_events_total"),
                      "异步日志出口累计没能落地的事件数（队列满按策略丢、Block 等位超时、已请求停止时还收下"
                      "的、落地时抛出异常的、以及被下游等级挡下的都算；进程级，多条通道报同一份）",
                      Base::droppedAsyncLogEventCount());

        // 常驻内存：进程级读数，抓哪台都是同一份。名字跟着 Prometheus 的既成约定走（process_*）
        const std::string residentMemoryName = makeMetricName(metricNamePrefix, "process_resident_memory_bytes");
        out += std::format("# HELP {} 本进程此刻占住的常驻字节数（进程级；0 表示平台读不出，不代表没有内存）\n"
                           "# TYPE {} gauge\n{} {}\n",
                           residentMemoryName, residentMemoryName, residentMemoryName, stats.residentMemoryBytes);

        // 进程级登记的读数：名字是登记时给的**完整名字**，不套本监听器的业务前缀——
        // 这些数不属于某台 HTTP 服务器，抓哪台监听器都该是同一份（口径与上面那几条进程级读数一致）
        for (const Core::ProcessMetricSample &sample: Core::ProcessMetricsRegistry::samples())
        {
            const char *const typeName = sample.kind == Core::ProcessMetricKind::Counter ? "counter" : "gauge";
            out += std::format("# HELP {} {}\n# TYPE {} {}\n{} {}\n", sample.name, sample.help, sample.name, typeName, sample.name, sample.value);
        }

        return out;
    }

    std::string formatLoopDiagnosticsJson(const std::vector<Core::ObservedEventLoop> &observedLoops, const std::size_t unregisteredLoopCount,
                                          const std::chrono::steady_clock::time_point nowMoment)
    {
        const auto  thresholdMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(Core::kSlowWorkingSegmentAlertThreshold).count();
        std::string out = std::format("{{\"stallThresholdMicroseconds\":{},\"unregisteredLoopCount\":{},\"loops\":[", thresholdMicroseconds, unregisteredLoopCount);

        for (std::size_t rowIndex = 0; const auto &entry: observedLoops)
        {
            const auto &[serialNumber, snapshot] = entry;
            // 当前这一相已经持续多久：读的是循环自己记的原子时刻，与抓取时刻之差
            const auto phaseMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(nowMoment - snapshot.phaseStartedAt).count();
            // 三条都要成立才算停顿：还在跑（停下的循环相位时刻不再更新，读出来会是一个很大的假数）、
            // 正在干活（等事件的那一相再久也只是空闲）、且超过了 Core 落 ERROR 用的同一个阈值
            const bool isStalled = snapshot.isRunning && snapshot.phase == Core::LoopPhase::Working && phaseMicroseconds > thresholdMicroseconds;

            out += std::format("{}{{\"serial\":{},\"thread\":\"{}\",\"running\":{},\"phase\":\"{}\",\"phaseMicroseconds\":{},"
                               "\"completedWorkingSegments\":{},\"slowestWorkingSegmentMicroseconds\":{},\"remotePendingCount\":{},\"stalled\":{}}}",
                               rowIndex++ == 0 ? "" : ",", serialNumber, threadFingerprint(snapshot.ownerThread), snapshot.isRunning, phaseName(snapshot.phase), phaseMicroseconds,
                               snapshot.completedWorkingSegments, snapshot.slowestWorkingSegment.count(), snapshot.remotePendingCount, isStalled);
        }

        out += "]}";
        return out;
    }

    void registerReadinessEndpoint(Router &router, const std::string_view path, std::function<bool()> isAccepting)
    {
        router.get(std::string(path),
                   [isAccepting = std::move(isAccepting)](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setHeader("content-type", "application/json");
                       if (isAccepting())
                       {
                           response.setStatus(200);
                           response.setBody(kReadinessReadyResponseBody);
                       } else
                       {
                           // retry-after 与另两处 503 同一口径（在途预算超量、h3 还没接上路由器）：排空期
                           // 里这台不会自己变回就绪，但探针按这个间隔再问一次的代价是零，而缺这一格的
                           // 采集端会把它当成「服务出故障」而不是「正在收工」
                           response.setStatus(503);
                           response.setHeader("retry-after", "1");
                           response.setBody(kReadinessDrainingResponseBody);
                       }
                       co_return;
                   });
    }

} // namespace AsynGyanis::Net
