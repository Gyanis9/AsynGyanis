#include "Net/Http/HttpMetricsEndpoint.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Core/Socket/Connection.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
         * @brief 追加一族 gauge 的 HELP/TYPE 与单条样本
         * @param out 输出缓冲
         * @param metricName 完整指标名
         * @param help 中文说明（该格式是 UTF-8，无需转义）
         * @param value 瞬时读数
         */
        void appendGauge(std::string &out, const std::string &metricName, const std::string_view help, const std::uint64_t value)
        {
            out += std::format("# HELP {} {}\n# TYPE {} gauge\n{} {}\n", metricName, help, metricName, metricName, value);
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

        /// @brief 把探针文本转义成 JSON 串内安全的形态（引号与反斜杠照转义、控制字符折成空格）
        /// @details 作业名来自路由模式或调用方标注，理论上可能含引号；不转义就会把 /debug/loops
        ///          的正文撕成坏 JSON——渲染层的底线是不产出读不回的正文
        [[nodiscard]] std::string escapedWorkloadText(const std::string_view text)
        {
            std::string escaped;
            escaped.reserve(text.size());
            for (const char character: text)
            {
                if (character == '"' || character == '\\')
                {
                    escaped.push_back('\\');
                    escaped.push_back(character);
                } else if (static_cast<unsigned char>(character) < 0x20U)
                {
                    escaped.push_back(' ');
                } else
                {
                    escaped.push_back(character);
                }
            }
            return escaped;
        }

        /// @brief 把标签值转义成 Prometheus 展示格式允许的形态（反斜杠、引号与换行三种）
        [[nodiscard]] std::string escapedPrometheusLabelValue(const std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char character: value)
            {
                if (character == '\\' || character == '"')
                {
                    escaped.push_back('\\');
                    escaped.push_back(character);
                } else if (character == '\n')
                {
                    escaped += "\\n";
                } else
                {
                    escaped.push_back(character);
                }
            }
            return escaped;
        }

        /// @brief 追加按路由分组的响应计数（route × status_class）
        void appendRouteResponseCounters(std::string &out, const std::string &responsesName, const HttpServerStats &stats)
        {
            constexpr std::array<std::string_view, 5> kStatusClassNames{"1xx", "2xx", "3xx", "4xx", "5xx"};
            for (const HttpRouteCount &routeCount: httpRouteCounts(stats))
            {
                const std::string escapedRoute = escapedPrometheusLabelValue(routeCount.route);
                for (std::size_t classIndex = 0; classIndex < kStatusClassNames.size(); ++classIndex)
                {
                    // 只输出实际出现过的类：一个只回过 200 的路由因此只有一条序列，基数用例才数得清
                    if (routeCount.statusClassCounts[classIndex] == 0)
                    {
                        continue;
                    }
                    out += std::format("{}{{route=\"{}\",{}=\"{}\"}} {}\n", responsesName, escapedRoute, kHttpStatusClassLabelKey, kStatusClassNames[classIndex],
                                       routeCount.statusClassCounts[classIndex]);
                }
            }
        }

        /// @brief 追加按路由分组的耗时直方图（route 标签 + 累计桶 + _sum/_count）
        void appendRouteLatencyHistograms(std::string &out, const std::string &durationName, const HttpServerStats &stats)
        {
            for (const HttpRouteCount &routeCount: httpRouteCounts(stats))
            {
                const Core::MetricHistogramSample routeHistogram   = httpRouteLatencyHistogramSample(routeCount);
                const std::vector<std::uint64_t>  cumulativeCounts = Core::cumulativeBucketCounts(routeHistogram);
                const std::string                 escapedRoute     = escapedPrometheusLabelValue(routeCount.route);
                for (std::size_t index = 0; index < cumulativeCounts.size(); ++index)
                {
                    // 末档没有上界，按约定记作 +Inf；其余档是模型里的秒上界
                    const bool        isOverflowBucket = index + 1 == cumulativeCounts.size();
                    const std::string upperBound       = isOverflowBucket ? std::string("+Inf") : std::format("{}", routeHistogram.upperBoundsSeconds[index]);
                    out += std::format("{}_bucket{{route=\"{}\",le=\"{}\"}} {}\n", durationName, escapedRoute, upperBound, cumulativeCounts[index]);
                }
                // _sum 由模型给出（单位秒）。定点六位小数而不是默认的 {}：默认格式对小于 1e-3 的值会切到
                // 科学计数（0.0005 会写成 5e-04），虽然 Prometheus 认这种写法，但固定小数位既更可读，
                // 也正好对上累加器的微秒精度，还免了「同一个值在不同时刻长相不同」的比对麻烦
                out += std::format("{}_sum{{route=\"{}\"}} {:.6f}\n", durationName, escapedRoute, routeHistogram.sumSeconds);
                out += std::format("{}_count{{route=\"{}\"}} {}\n", durationName, escapedRoute, routeHistogram.count);
            }
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

        /// 运维端点上的等级文本：`logLevelToString()` 只认可记录的消息等级，`Off` 在那里是
        /// "?????"；但 `Off` 是合法阈值，读回来的文本必须能原样再写进去
        [[nodiscard]] std::string logLevelEndpointLabel(const Base::LogLevel level)
        {
            std::string label{level == Base::LogLevel::Off ? std::string_view{"OFF"} : std::string_view{Base::logLevelToString(level)}};
            while (!label.empty() && label.back() == ' ')
            {
                label.pop_back();
            }
            return label;
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
        // 口径要说清：多台监听器共用同一份采集端时这一条是相加的那一份（刻意如此，否则抓取随机
        // 命中一台会看到 1/N 与计数器回落），而下面那条 maximum_connections 是本监听器的上限——
        // 两者不同源，别直接相除当「负载率」；要负载率就按监听器条数折算，或每台各抓各的口
        appendGauge(out, makeMetricName(metricNamePrefix, "active_connections"), "取快照那一刻的活跃连接数（多条监听器共用一份采集端时是各台相加）", stats.activeConnectionCount);

        // 两个分母：只有分子画不出「离上限还有多远」这种提前预警，只能等人记得配置文件写过什么。
        // 0 的含义各有一条：并发上限 0 = 本监听器不设这道限；单来源上限 0 = 那道闸门没装
        appendGauge(out, makeMetricName(metricNamePrefix, "maximum_connections"),
                    "本监听器的并发连接上限（下发到本监听器的实际值；0 = 不设这道限）。多进程部署里配置的是整机数，这里报摊到本进程那一份", stats.maximumConnections);

        appendGauge(out, makeMetricName(metricNamePrefix, "maximum_connections_per_ip"),
                    "单个来源 IP 的并发上限（按 IP 的那道准入闸门按什么挡；0 = 这道闸门没装）。限额器可以被多条通道共用一份，因此抓哪台都是同一个值",
                    stats.maximumConnectionsPerIp);

        // 状态码分类：一族带 route 与 status_class 标签的计数器，采集侧可按路由聚合。
        // route 取路由模式原文（绝不取实际 URI）；未匹配、静态文件与超限归并各有固定哨兵。
        // 具体状态码明细仍按全局一维报：route × status 的笛卡尔积不在基数预算里（见 HttpRouteCount）
        const std::string responsesName = makeMetricName(metricNamePrefix, "responses_total");
        out += std::format("# HELP {} 已发出的响应条数，按路由模式（route）与状态码类（status_class）聚合；具体状态码（status）另按全局一维报\n# TYPE {} counter\n", responsesName,
                           responsesName);
        appendRouteResponseCounters(out, responsesName, stats);
        // 具体状态码：只输出实际出现过的码（不预置 100..599 的空序列）。与 OTLP 出口逐点同解；
        // 明细表满后被归并的条数另有一条计数器
        for (const HttpStatusCodeCount &statusCodeCount: httpStatusCodeCounts(stats))
        {
            out += std::format("{}{{{}=\"{}\"}} {}\n", responsesName, kHttpStatusCodeLabelKey, statusCodeCount.statusCode, statusCodeCount.count);
        }
        appendCounter(out, makeMetricName(metricNamePrefix, "responses_status_merged_total"),
                      "具体状态码明细表已满后被归并进状态码类那一族的响应条数（这些响应仍计入 status_class）", stats.statusCodeMergedCount);
        appendCounter(out, makeMetricName(metricNamePrefix, "route_series_merged_total"),
                      "路由明细表已满或路由模式过长后被归并到 merged 哨兵的响应条数（仍计入全局状态码类与直方图）", stats.routeSeriesMergedCount);

        // 耗时直方图：每条路由一族，带 route 标签；单位换算与分格语义由公共模型一次做对
        // （Core::MetricHistogramSample），本出口只把「每格条数」折成展示格式要的累计数
        const std::string durationName = makeMetricName(metricNamePrefix, "request_duration_seconds");
        out += std::format("# HELP {} 已应答请求的耗时分布，按路由模式（route）分组\n# TYPE {} histogram\n", durationName, durationName);
        appendRouteLatencyHistograms(out, durationName, stats);

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
        appendGauge(out, makeMetricName(metricNamePrefix, "blocking_task_queue_depth"),
                    "取快照那一刻排在共享阻塞任务执行器队列里的任务条数（只含进程级共享那台，自建执行器不计入；多条通道报同一份）", stats.blockingTaskQueueDepth);
        appendCounter(out, makeMetricName(metricNamePrefix, "blocking_task_rejected_total"),
                      "因排队已满被拒的阻塞任务条数（只含进程级共享那台执行器，自建的不计——全部执行器的合计在 "
                      "asyn_executor_rejected_total；提交方当场收到异常，涨了就说明该降并发或加工作线程）",
                      stats.blockingTaskRejectedCount);

        // 日志被丢的规模是进程级的，而且有一条对端可驱动的路：JSON 版式对非法 UTF-8 整条失败，
        // 请求目标里送原始 Latin-1 字节的客户端因此能把自己在访问日志里的那行消音。没有这条读数，
        // 「审计记录被消音」与「这段时间没人写日志」在面板上是同一个形状
        appendCounter(out, makeMetricName(metricNamePrefix, "log_dropped_events_total"),
                      "异步日志出口累计没能落地的事件数（队列满按策略丢、Block 等位超时、已请求停止时还收下"
                      "的、落地时抛出异常的、以及被下游等级挡下的都算；进程级，多条通道报同一份）",
                      Base::droppedAsyncLogEventCount());

        // 常驻内存：进程级读数，抓哪台都是同一份。名字跟着 Prometheus 的既成约定走（process_*）
        appendGauge(out, makeMetricName(metricNamePrefix, "process_resident_memory_bytes"), "本进程此刻占住的常驻字节数（进程级；0 表示平台读不出，不代表没有内存）",
                    stats.residentMemoryBytes);

        // process_* 三件套：CPU 计数器、打开的句柄数、启动时刻。都是进程级，抓哪台监听器都是同一份；
        // 0 一律表示「平台读不出」，与常驻内存同口径。抓取端拿它们对 CPU 饱和、句柄泄漏与进程重启设告警
        appendCounter(out, makeMetricName(metricNamePrefix, "process_cpu_seconds_total"), "本进程累计消耗的 CPU 时间（用户态加内核态，整数秒；进程级；0 表示平台读不出）",
                      stats.processCpuSeconds);
        appendGauge(out, makeMetricName(metricNamePrefix, "process_open_fds"), "本进程此刻打开的句柄/文件描述符数（进程级；0 表示平台读不出）", stats.openHandleCount);
        appendGauge(out, makeMetricName(metricNamePrefix, "process_start_time_seconds"),
                    "本进程启动时刻（Unix epoch 秒；进程级；0 表示平台读不出）——计数器回落时据它判断是不是进程重启", stats.processStartTimeSeconds);

        // 进程级登记的读数：名字是登记时给的完整名字，不套本监听器的业务前缀——
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

            out += std::format(
                    "{}{{\"serial\":{},\"thread\":\"{}\",\"running\":{},\"phase\":\"{}\",\"phaseMicroseconds\":{},"
                    "\"completedWorkingSegments\":{},\"slowestWorkingSegmentMicroseconds\":{},\"remotePendingCount\":{},"
                    "\"saturatedRemoteRejectionCount\":{},\"skippedCompletedHandleCount\":{},\"activeWorkload\":\"{}\",\"lastCompletedWorkload\":\"{}\",\"stalled\":{}}}",
                    rowIndex++ == 0 ? "" : ",", serialNumber, threadFingerprint(snapshot.ownerThread), snapshot.isRunning, phaseName(snapshot.phase), phaseMicroseconds,
                    snapshot.completedWorkingSegments, snapshot.slowestWorkingSegment.count(), snapshot.remotePendingCount, snapshot.saturatedRemoteRejectionCount,
                    snapshot.skippedCompletedHandleCount, escapedWorkloadText(snapshot.activeWorkload), escapedWorkloadText(snapshot.lastCompletedWorkload), isStalled);
        }

        out += "]}";
        return out;
    }

    void registerReadinessEndpoint(Router &router, const std::string_view path, std::function<bool()> isAccepting, ReadinessCheck dependencyCheck)
    {
        router.get(std::string(path),
                   [isAccepting = std::move(isAccepting), dependencyCheck = std::move(dependencyCheck)](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setHeader("content-type", "application/json");
                       if (!isAccepting())
                       {
                           // retry-after 与另两处 503 同一口径（在途预算超量、h3 还没接上路由器）：排空期
                           // 里这台不会自己变回就绪，但探针按这个间隔再问一次的代价是零，而缺这一格的
                           // 采集端会把它当成「服务出故障」而不是「正在收工」
                           response.setStatus(503);
                           response.setHeader("retry-after", "1");
                           response.setBody(kReadinessDrainingResponseBody);
                           co_return;
                       }

                       if (dependencyCheck)
                       {
                           // 回调是快照读取：真实依赖的探测在调用方自己的后台协程/线程里跑，
                           // 这里只读上一轮结论，绝不在探针路径上做网络或磁盘 IO
                           const ReadinessCheckResult result = dependencyCheck();
                           if (!result.ready)
                           {
                               Base::ConfigValue body = Base::ConfigValue::object();
                               body["status"]         = "not_ready";
                               body["reason"]         = result.reason;
                               response.setStatus(503);
                               response.setHeader("retry-after", "1");
                               response.setBody(Base::serializeConfigValue(body).value_or(R"({"status":"not_ready"})"));
                               co_return;
                           }
                       }

                       response.setStatus(200);
                       response.setBody(kReadinessReadyResponseBody);
                       co_return;
                   });
    }

    void registerHealthEndpoint(Router &router, const std::string_view path)
    {
        router.get(std::string(path),
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       // 存活性只回答「进程还在不在转」：排空期间答案仍是「在」，
                       // 因此这里刻意不读任何停止标志
                       response.setStatus(200);
                       response.setHeader("content-type", "application/json");
                       response.setBody(kHealthCheckResponseBody);
                       co_return;
                   });
    }

    void registerLoopDiagnosticsEndpoint(Router &router, const std::string_view path)
    {
        router.get(std::string(path),
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       // 现取整表现场渲染：读的都是各条循环自己的原子量，不需要把动作投进任何一条循环
                       response.setStatus(200);
                       response.setHeader("content-type", kLoopDiagnosticsContentType);
                       response.setBody(formatLoopDiagnosticsJson(Core::eventLoopSnapshots(), Core::unregisteredEventLoopCount(), std::chrono::steady_clock::now()));
                       co_return;
                   });
    }

    std::string formatConnectionsJson(const std::vector<std::shared_ptr<Core::Connection>> &connections, const std::chrono::steady_clock::time_point nowMoment)
    {
        std::map<std::string, std::uint64_t> byProtocol;
        std::map<std::string, std::uint64_t> byState;
        std::int64_t                         oldestAgeSeconds     = 0;
        std::uint64_t                        receivedBytesTotal   = 0;
        std::uint64_t                        sentBytesTotal       = 0;
        std::uint64_t                        maximumReceivedBytes = 0;
        std::uint64_t                        maximumSentBytes     = 0;
        for (const std::shared_ptr<Core::Connection> &connection: connections)
        {
            if (connection == nullptr)
            {
                continue;
            }
            ++byProtocol[std::string{connection->protocolName()}];
            ++byState[connection->isAliveFlag() ? "alive" : "closed"];
            const auto ageSeconds             = std::chrono::duration_cast<std::chrono::seconds>(nowMoment - connection->createdAt()).count();
            oldestAgeSeconds                  = std::max(oldestAgeSeconds, ageSeconds);
            const std::uint64_t receivedBytes = connection->receivedBytes();
            const std::uint64_t sentBytes     = connection->sentBytes();
            receivedBytesTotal += receivedBytes;
            sentBytesTotal += sentBytes;
            maximumReceivedBytes = std::max(maximumReceivedBytes, receivedBytes);
            maximumSentBytes     = std::max(maximumSentBytes, sentBytes);
        }

        Base::ConfigValue protocolCounts = Base::ConfigValue::object();
        for (const auto &[protocol, count]: byProtocol)
        {
            protocolCounts[protocol] = count;
        }
        Base::ConfigValue stateCounts = Base::ConfigValue::object();
        for (const auto &[state, count]: byState)
        {
            stateCounts[state] = count;
        }

        Base::ConfigValue document       = Base::ConfigValue::object();
        document["total"]                = connections.size();
        document["by_protocol"]          = std::move(protocolCounts);
        document["by_state"]             = std::move(stateCounts);
        document["oldest_age_seconds"]   = oldestAgeSeconds;
        document["received_bytes_total"] = receivedBytesTotal;
        document["sent_bytes_total"]     = sentBytesTotal;
        document["max_received_bytes"]   = maximumReceivedBytes;
        document["max_sent_bytes"]       = maximumSentBytes;
        return Base::serializeConfigValue(document).value_or("{}");
    }

    void registerLogLevelEndpoint(Router &router, const std::string_view path)
    {
        // GET 与写操作共用同一份等级渲染：两边各拼一次迟早出现「读回来是 INFO、写进去是 INFO 」
        const auto writeLevelResponse = [](HttpResponse &response, const Base::LogLevel level)
        {
            Base::ConfigValue body = Base::ConfigValue::object();
            body["level"]          = logLevelEndpointLabel(level);
            response.setStatus(200);
            response.setHeader("content-type", "application/json");
            response.setBody(Base::serializeConfigValue(body).value_or("{}"));
        };

        const auto rejectRequest = [](HttpResponse &response, const std::string &reason)
        {
            Base::ConfigValue body = Base::ConfigValue::object();
            body["error"]          = reason;
            response.setStatus(400);
            response.setHeader("content-type", "application/json");
            response.setBody(Base::serializeConfigValue(body).value_or(R"({"error":"bad request"})"));
        };

        // 严格解析：LogLevel.h 的 logLevelFromString 对未知串回落成 Info，那是配置加载的容错口径；
        // 运维端点写错等级必须回 400，不能静默改成一个用户没要的等级
        const auto parseLevel = [](const std::string_view text) -> std::optional<Base::LogLevel>
        {
            if (Base::Detail::logLevelLabelEquals(text, "TRACE"))
                return Base::LogLevel::Trace;
            if (Base::Detail::logLevelLabelEquals(text, "DEBUG"))
                return Base::LogLevel::Debug;
            if (Base::Detail::logLevelLabelEquals(text, "INFO"))
                return Base::LogLevel::Info;
            if (Base::Detail::logLevelLabelEquals(text, "WARN"))
                return Base::LogLevel::Warn;
            if (Base::Detail::logLevelLabelEquals(text, "ERROR"))
                return Base::LogLevel::Error;
            if (Base::Detail::logLevelLabelEquals(text, "FATAL"))
                return Base::LogLevel::Fatal;
            if (Base::Detail::logLevelLabelEquals(text, "OFF"))
                return Base::LogLevel::Off;
            return std::nullopt;
        };

        router.get(std::string(path),
                   [writeLevelResponse](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       writeLevelResponse(response, Base::LoggerRegistry::instance().getRootLogger().getLevel());
                       co_return;
                   });

        const auto handleWrite = [writeLevelResponse, rejectRequest, parseLevel](HttpRequest &request, HttpResponse &response) -> Core::Task<>
        {
            const std::optional<Base::ConfigValue> parsed = Base::parseConfigValue(request.body());
            if (!parsed.has_value() || !parsed->is_object() || !parsed->contains("level") || !parsed->at("level").is_string())
            {
                rejectRequest(response, R"(正文必须是 {"level":"debug"} 这样的 JSON 对象)");
                co_return;
            }
            const std::optional<Base::LogLevel> newLevel = parseLevel(parsed->at("level").get<std::string>());
            if (!newLevel.has_value())
            {
                rejectRequest(response, "未知日志等级；可用取值：TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF（不区分大小写）");
                co_return;
            }

            Base::Logger        &rootLogger = Base::LoggerRegistry::instance().getRootLogger();
            const Base::LogLevel oldLevel   = rootLogger.getLevel();
            // 先记审计再改：新等级高于 WARN 时，改完这条 WARN 就写不出来了
            LOG_WARN_FMT("运维端点：root 日志等级由 {} 改为 {}（客户端 {}）", logLevelEndpointLabel(oldLevel), logLevelEndpointLabel(*newLevel), request.remoteAddress());
            rootLogger.setLevel(*newLevel);
            writeLevelResponse(response, *newLevel);
            co_return;
        };
        router.put(std::string(path), handleWrite);
        router.post(std::string(path), handleWrite);
    }

    void registerMetricsEndpoint(Router &router, const std::string_view path, const std::string_view metricNamePrefix, std::function<HttpServerStats()> snapshotProvider)
    {
        // 前缀按值捕进处理函数：字符串是调用方的，可能比服务器先走；这里只留一份拷贝
        const std::string metricPrefix(metricNamePrefix);
        router.get(std::string(path),
                   [metricPrefix, snapshotProvider = std::move(snapshotProvider)](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       // 每次抓取现取一次快照：计数是原子的，不必把动作投递到事件循环
                       response.setStatus(200);
                       response.setHeader("content-type", kPrometheusTextContentType);
                       response.setBody(formatPrometheusMetrics(snapshotProvider(), metricPrefix));
                       co_return;
                   });
    }

} // namespace AsynGyanis::Net
