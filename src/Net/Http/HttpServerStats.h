/**
 * @file HttpServerStats.h
 * @brief HTTP 服务器统计快照与它的原子采集端
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Metrics/MetricData.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 延迟直方图的档位数：四个「上界档」加一个溢出档，与 kHttpLatencyUpperBoundMilliseconds 配套使用
    inline constexpr std::size_t kHttpLatencyBucketCount = 5;

    /// 状态码分类标签的键名：Prometheus 的 `label="…"` 与 OTLP 的 attribute 共用这一份（见 HttpStatusClassCount）
    inline constexpr std::string_view kHttpStatusClassLabelKey = "status_class";

    /// 具体状态码标签的键名：与 `status_class` 并列的第二维（429、404、500 各自可单独告警）
    inline constexpr std::string_view kHttpStatusCodeLabelKey = "status";

    /// 具体状态码明细序列的上限：状态码天然只有 500 个取值，但仍给固定上限，防止有人把非规范码灌进来
    inline constexpr std::size_t kHttpStatusCodeSeriesLimit = 64;

    /// 路由标签的键名：与状态码那两维并列，两条导出出口共用
    inline constexpr std::string_view kHttpRouteLabelKey = "route";

    /**
     * @brief 路由明细序列的上限
     * @details 路由模式由业务代码注册，数量没有天然上界；不设限时「几千条路由 × 5 个状态码类」会把
     *          Prometheus 的序列数撑爆。与具体状态码表同一条有界策略：占满之后新出现的模式归并进
     *          `merged` 哨兵，序列数不再增长，被归并的条数记在 `routeSeriesMergedCount`。
     */
    inline constexpr std::size_t kHttpRouteSeriesLimit = 64;

    /// 路由模式原文的存放上限：超过它的模式不单独成序列，直接归并（标签里不截断，截断会造出假模式）
    inline constexpr std::size_t kHttpRoutePatternCapacity = 160;

    /// 未匹配到任何路由时的固定标签值（需要区分「路径没进标签」与「真的没路由」）
    inline constexpr std::string_view kHttpRouteUnmatchedLabel = "unmatched";

    /// 静态文件那一路的固定标签值：它注册的是通配路由 `"*"`，真正的路径同样不进标签
    inline constexpr std::string_view kHttpRouteStaticLabel = "static";

    /// 路由明细表占满之后，新模式的响应归并到这个固定标签
    inline constexpr std::string_view kHttpRouteMergedLabel = "merged";

    /**
     * @brief 状态码分类的一档：类名（1xx…5xx）与该类已发出的响应条数
     *
     * @details 两条导出出口（Prometheus 文本、OTLP）此前各自拼了一遍这五档的名字与取值表；名字拼法
     *          分岔会让采集侧在两种编码之间看到不同的标签值。取值表因此只有这一处（由
     *          `httpStatusClassCounts()` 给出），出口只负责把键值对编码成各自的形状。
     */
    struct HttpStatusClassCount
    {
        std::string_view className; ///< 类名文本（`1xx`…`5xx`），两条出口共用
        std::uint64_t    count{0};  ///< 该类已发出的响应条数
    };

    /**
     * @brief 一个具体状态码与它已发出的响应条数
     * @details 与状态码类那一族并列，键名见 kHttpStatusCodeLabelKey。只出现在实际发出过的码上——
     *          不预置 100..599 的全表（那是 500 条空序列）。表满之后新出现的码归并进状态码类那一族，
     *          归并条数记在 HttpServerStats::statusCodeMergedCount。
     */
    struct HttpStatusCodeCount
    {
        int           statusCode{0}; ///< 具体状态码（100..599）
        std::uint64_t count{0};      ///< 该状态码已发出的响应条数
    };

    /**
     * @brief 一条路由模式下的响应与耗时统计
     * @details 路由标签取值是路由模式原文（`/user/:id`），绝不取请求的实际 URI：后者每条请求都会
     *          长出新的序列，正是本轮要消灭的基数爆炸。未匹配、静态文件与超限归并各有一个固定哨兵
     *          （见 kHttpRouteUnmatchedLabel / kHttpRouteStaticLabel / kHttpRouteMergedLabel）。
     * @note 状态码只细到类（1xx…5xx）：route × status 的笛卡尔积不在本轮的基数预算里，具体状态码
     *       明细仍按全局一维报（见 HttpServerStats::statusCodeCounts）。按路由要看的是类级计数。
     */
    struct ASYN_NET_API HttpRouteCount
    {
        std::string                                        route;                       ///< 路由模式原文或固定哨兵
        std::array<std::uint64_t, 5>                       statusClassCounts{};         ///< 1xx…5xx 五类的响应条数
        std::array<std::uint64_t, kHttpLatencyBucketCount> latencyBucketCounts{};       ///< 延迟直方图各档条数
        std::uint64_t                                      totalLatencyMicroseconds{0}; ///< 该路由的耗时之和（微秒）

        /// @brief 该路由的响应总条数（五类之和）
        [[nodiscard]] std::uint64_t responseCount() const noexcept
        {
            std::uint64_t count = 0;
            for (const std::uint64_t classCount: statusClassCounts)
            {
                count += classCount;
            }
            return count;
        }

        /// @brief 该路由的延迟样本数（各档之和）
        [[nodiscard]] std::uint64_t latencySampleCount() const noexcept
        {
            std::uint64_t count = 0;
            for (const std::uint64_t bucketCount: latencyBucketCounts)
            {
                count += bucketCount;
            }
            return count;
        }
    };

    /// 缓存行字节数：把不同写者热度的原子计数按行边界分开，避免伪共享。取 64（x86-64 通用行宽）；
    /// 刻意不用 std::hardware_destructive_interference_size——它的取值随实现变、会触发跨编译器告警。
    inline constexpr std::size_t kCacheLineBytes = 64;

    /**
     * @brief 延迟直方图各档的耗时上界，单位毫秒
     *
     * @details 下标 i 与 HttpServerStats::latencyBucketCounts[i] 一一对应，语义是「耗时不超过第 i 档上界」；
     *          排在最后的那一档没有上界，收纳超过 back() 的全部请求。档位压到毫秒量级
     *          （本机直连 / 正常业务 / 慢消费者三档），亚毫秒分布交给采样器而非常驻计数。
     */
    inline constexpr std::array<std::int64_t, kHttpLatencyBucketCount - 1> kHttpLatencyUpperBoundMilliseconds{1, 5, 20, 100};

    /**
     * @brief HTTP 服务器统计快照：某次采样时刻的计数切面
     *
     * @details 纯数据，不持有任何同步设施，可由调用方按值取走、跨线程传递或落进监控上报。
     *          各字段分别读取，因此不是严格同一瞬间的一致切面（跨字段求和可能与某次采样略有偏差）。
     * @note 请求计数口径：totalRequestCount 只统计交给业务处理的那批请求；解析失败的、以及被协议层
     *       挡在业务之外的（头部/请求目标/正文越限，回的是 4xx）都只进 badRequestCount 那一笔，
     *       三条通道在这一条上同解——否则同一份越界请求，`requests_total` 会随对端选了 h1/h2/h3 而不同。
     *       「在途正文预算不足」不在这一笔里：它回的是 503，而本字段口径是「对端的报文不合规」，
     *       全局预算用尽不是对端的错。那一格的读数是进程级的 `asyn_http_memory_budget_rejections_total`
     *       （三条通道共用同一份预算账，被拒次数一律记在那里）。
     *       状态码类计数同样只对应已发出的响应，故 status1xxCount 至
     *       status5xxCount 之和在稳态下等于 totalRequestCount（发送失败的应答不计入）。
     *       这两个全局量与 routeCounts 里的同名分项同源同一次计入；按路由导出时用后者，
     *       保留前者是为了「整台服务器」这一维仍有一个现成读数。
     * @note 路由维度只细到状态码类：route × status 的笛卡尔积会突破序列预算，具体状态码明细
     *       仍按全局一维报（statusCodeCounts）。route 的取值是路由模式原文或固定哨兵，绝不取实际 URI。
     * @note 升级到 WebSocket 的连接是上述口径的例外：升级请求计入 totalRequestCount 与
     *       webSocketUpgradeCount，但 101 由握手模块逐字节生成、不走 HttpResponse 序列化，
     *       故不进状态码类计数；101 之后的帧错误另进 webSocketProtocolErrorCloseCount，
     *       不并入 badRequestCount（后者的口径是 HTTP 报文解析失败，回的是 4xx）。
     * @note streamCancelledCount 是「单流取消」这一形态的口径（HTTP/2 的 RST_STREAM、HTTP/3 的
     *       RESET_STREAM/STOP_SENDING）：只作废这一条请求（连接与同连接上的其它流照旧工作），
     *       因此既不算已应答也不算坏请求；HTTP/1.1 上没有这一形态（取消即断连），该字段恒为 0。
     * @see HttpMetricsCollector, HttpServer::stats()
     */
    struct ASYN_NET_API HttpServerStats
    {
        std::uint64_t totalRequestCount{0};     ///< 累计成功解析（ParseStatus::Done）的请求条数，不含解析失败
        std::uint64_t activeConnectionCount{0}; ///< 取快照那一刻共用本采集端的全部连接管理器在册的连接数
        std::uint64_t badRequestCount{0};       ///< 解析失败或协议错误收口的条数（HttpParseErrorKind 各档合并为一类）
        std::uint64_t timeoutClosedCount{0};    ///< 被空闲清扫协程按空闲/读写超时关闭的 HTTP 连接数
        /// 「本侧交出去的」响应没能完整交给传输层就收口的连接数：要么写出失败（对端带未读数据关闭，
        /// 内核回 RST），要么收口时仍有响应留在待发缓冲或流控队列里（对端不读也不还窗口，那部分字节
        /// 一次都没碰过套接字，写侧因此永不报错）。与 timeoutClosedCount 分开是因为两者是两种毛病——
        /// 那条是「没人来取」，这条是「取到一半不取了」。
        /// @warning 口径止于本侧：已经被传输层收下、但对端再没读走的字节看不见（256 KiB 的响应
        ///          能整个塞进环回套接字缓冲，对端随后关掉，本侧一次失败都不会遇到）
        std::uint64_t writeAbortedConnectionCount{0};
        std::uint64_t status1xxCount{0}; ///< 状态码为 1xx 的响应条数
        std::uint64_t status2xxCount{0}; ///< 状态码为 2xx 的响应条数
        std::uint64_t status3xxCount{0}; ///< 状态码为 3xx 的响应条数
        std::uint64_t status4xxCount{0}; ///< 状态码为 4xx 的响应条数
        std::uint64_t status5xxCount{0}; ///< 状态码为 5xx 的响应条数

        /// 具体状态码的明细计数（只含出现过的码，未使用的槽位 statusCode 为 0）
        std::array<HttpStatusCodeCount, kHttpStatusCodeSeriesLimit> statusCodeCounts{};
        /// 因明细表已满而被归并进状态码类那一族的响应条数（这些响应仍计入 status4xxCount 等）
        std::uint64_t statusCodeMergedCount{0};

        /// 按路由模式聚合的响应与耗时（只含实际出现过的路由；按 route 升序，至少含一条 unmatched）
        std::vector<HttpRouteCount> routeCounts;
        /// 因路由明细表已满（或模式长于 kHttpRoutePatternCapacity）而被归并进 merged 哨兵的响应条数
        std::uint64_t routeSeriesMergedCount{0};

        /// 延迟直方图的累计条数，下标与 kHttpLatencyUpperBoundMilliseconds 对应；末档为溢出档
        std::array<std::uint64_t, kHttpLatencyBucketCount> latencyBucketCounts{};

        /// 已应答请求的耗时之和，单位微秒。与 latencyBucketCounts 同源、同一次计入：
        /// 只有分档计数时采集侧能算分位却算不出均值，导出成 Prometheus 直方图时也就缺一条 `_sum`
        std::uint64_t totalLatencyMicroseconds{0};

        /// ---- WebSocket（101 升级之后的那条连接）的累计计数，与上面几组并列、口径互不覆盖 ----
        std::uint64_t webSocketUpgradeCount{0};            ///< 升级成功的连接数（101 已发出）
        std::uint64_t webSocketMessageCount{0};            ///< 收到的数据消息条数：分片重组后的一条算一次，控制帧不计
        std::uint64_t webSocketProtocolErrorCloseCount{0}; ///< 因对端违反 RFC 6455 而收口的次数：1002/1007/1009 合并为一类，具体码见日志
        std::uint64_t webSocketPeerCloseCount{0};          ///< 对端发起关闭握手的次数（收到对端 Close 帧）
        std::uint64_t webSocketServerCloseCount{0};        ///< 本侧发起关闭握手的次数：正常收尾与协议错误收口都算，回应对端 Close 的回帧不算；按发起计，不看该帧是否写出成功

        /// ---- 单流取消计数：取消不改连接状态，故与连接级的各组计数并列、口径互不覆盖 ----
        std::uint64_t streamCancelledCount{0}; ///< 请求已收齐（或正在收），但对端用 RST_STREAM（h2）/ RESET_STREAM（h3）取消了该流，本端因此未发响应的条数

        /// ---- 发送路径计数：正文经内核零拷贝（sendfile）直接发出的响应条数，仅 Linux 上恒可能非零 ----
        std::uint64_t zeroCopySendCount{0}; ///< 正文走零拷贝发送的响应条数；运维据此确认静态文件的快路径是否在生效

        /// ---- 准入闸门计数：由取快照的那台服务器从自己持有的限额器读进来，不经采集端 ----
        /**
         * @brief 按来源 IP 的并发限额累计挡掉的连接条数
         * @details 为什么不进 `HttpMetricsCollector`：限额器是可以被多条通道（h1/h2、h3、WebSocket）
         *          共用的一份对象，计数住在它自己身上才是「这道闸门一共挡了多少」；采集端只知道
         *          请求与连接，不知道准入判定。取快照时由各服务器读一次（见 HttpServer::stats()）。
         * @note 全零有两种意思：压根没设限额器，或设了但一条都没挡过。抓取端分不开这两种，
         *       要分辨得看部署里有没有配这项限额——因此它适合作为「闸门有没有在做事」的信号，
         *       不适合作为「闸门有没有装上」的证据
         */
        std::uint64_t admissionRejectedConnectionCount{0};

        /**
         * @brief 本监听器的并发连接上限（分母，瞬时量；0 = 不设这道限）
         * @details 为什么单独记一份：`activeConnectionCount` 与 `overLimitRejectedConnectionCount`
         *          一直在报分子和「已经撞到顶之后拒了多少」，但上限本身从未出现在 `/metrics` 上。
         *          于是「还剩多少余量」「是不是该扩容了」这两条最该提前预警的判据只能靠人记得配置文件
         *          里写过什么——而多进程部署里配置的是整机数、实际生效的是摊到本进程那一份，两边根本
         *          对不上。报的是下发到本监听器的实际值，与 `TcpServer::maximumConnections()` 同一个口径
         */
        std::uint64_t maximumConnections{0};

        /**
         * @brief 单个来源 IP 的并发上限（分母，瞬时量；0 = 这道闸门没装）
         * @details 与 `admissionRejectedConnectionCount` 配成一对：那个数说「挡过多少」，这个数说
         *          「按什么挡的」。抓取端因此能算出某个来源离上限多远，也能分开「闸门没上」
         *          （本值为 0）与「闸门上了但没挡过东西」（那个数为 0 而本值非 0）这两种全零形状
         */
        std::uint64_t maximumConnectionsPerIp{0};

        /**
         * @brief 因本服务器自身并发连接上限（`maximum_connections`）而被拒的连接条数
         * @details 与上面那道按来源 IP 的闸门分开计数：限额器可以多台共用、报的是闸门总量，而这个数
         *          是「这台服务器已经满载」。两者混在一个读数里就分不出「某个来源在刷」与「整体容量到顶」，
         *          而这两种情况的处置完全相反（前者收紧按 IP 的限额，后者加 worker 或抬上限）。
         * @note 为零同样分不开「没设上限」与「设了但没满过」——要确认上限装上了，看得是取值本身而不是这条计数
         */
        std::uint64_t overLimitRejectedConnectionCount{0};

        /**
         * @brief 取快照这一刻排在阻塞任务执行器队列里的任务条数（进程级，0 表示没有积压）
         * @details 这是「事件循环有没有被拖住」最早的信号：队列开始涨，说明下游变慢或工作线程不够，
         *          而请求侧此时往往还只是延迟，看不出问题。
         */
        std::uint64_t blockingTaskQueueDepth{0};

        /// 因排队已满而被拒的阻塞任务条数（进程级累计）；提交方当场收到异常，运维据此判断要不要降并发
        std::uint64_t blockingTaskRejectedCount{0};

        /**
         * @brief 取快照这一刻本进程的常驻内存字节数（进程级；0 表示平台读不出）
         * @details 泄漏与无界缓存最先体现在这一列的趋势上。它不是「活跃分配量」：分配器留下的空闲页
         *          仍算常驻（glibc 就不把页面还给内核），所以掉下来的曲线才算「真的还回来了」。
         */
        std::uint64_t residentMemoryBytes{0};

        /// 取快照这一刻本进程累计消耗的 CPU 时间（整数秒；进程级，0 表示平台读不出）——计数器口径，配合 rate() 看趋势
        std::uint64_t processCpuSeconds{0};

        /// 取快照这一刻本进程打开的句柄/文件描述符数（进程级，0 表示平台读不出）——连接/文件泄漏的通用信号
        std::uint64_t openHandleCount{0};

        /// 本进程的启动时刻（Unix epoch 秒；进程级，0 表示平台读不出）——计数器回落时据它判断「进程重启过」
        std::uint64_t processStartTimeSeconds{0};

        /**
         * @brief 取延迟直方图的样本总数
         * @return std::uint64_t 各档累计值之和；与 totalRequestCount 的差即「已收齐但响应未落账」的
         *         条数，其中既有响应未发出的，也有升级到 WebSocket 的（101 不经 recordResponse 落账）
         */
        [[nodiscard]] std::uint64_t latencySampleCount() const noexcept
        {
            std::uint64_t sampleCount = 0;
            for (const std::uint64_t bucketCount: latencyBucketCounts)
            {
                sampleCount += bucketCount;
            }
            return sampleCount;
        }
    };

    /**
     * @brief 取五档状态码分类（顺序固定 1xx→5xx）的全局合计
     * @details 两条导出出口现在按路由分组输出状态码类（见 HttpRouteCount），本函数保留为「整台服务器
     *          的五类合计」这一维的唯一取值表：需要全局视图的调用方与用例仍从它取，避免各自拼一遍
     *          类名；分路由的导出不再用它，否则同名序列会与按路由的序列重复计数。
     * @param stats 统计快照
     * @return std::array<HttpStatusClassCount, 5> 五档类名与该类条数
     */
    [[nodiscard]] ASYN_NET_API std::array<HttpStatusClassCount, 5> httpStatusClassCounts(const HttpServerStats &stats) noexcept;

    /**
     * @brief 取具体状态码的明细计数（只含实际出现过的码，按状态码升序）
     * @details 键名见 kHttpStatusCodeLabelKey；Prometheus 与 OTLP 两侧共用这一份过滤与排序，
     *          免得一边按哈希顺序、一边按插入顺序输出。表满后被归并的码不在这里，见 statusCodeMergedCount。
     * @param stats 统计快照
     * @return std::vector<HttpStatusCodeCount> 明细；一个码都没出现过时为空
     */
    [[nodiscard]] ASYN_NET_API std::vector<HttpStatusCodeCount> httpStatusCodeCounts(const HttpServerStats &stats);

    /**
     * @brief 取按路由聚合的统计（两条导出出口共用同一份过滤与排序）
     * @details 快照里没有路由明细时（手工构造的统计、或只填了全局计数的旧路径），交回一条
     *          `unmatched` 的合成条目，分档与全局直方图同值——渲染端因此永远有 `route` 标签可写，
     *          不会出现「有的直方图带标签、有的不带」这种两套形状。
     * @param stats 统计快照
     * @return std::vector<HttpRouteCount> 按 route 升序；至少一条
     */
    [[nodiscard]] ASYN_NET_API std::vector<HttpRouteCount> httpRouteCounts(const HttpServerStats &stats);

    /**
     * @brief 把一条路由的延迟分档折成公共数据模型（单位秒，与全局直方图同一套档位）
     * @param routeCount 单条路由的统计
     * @return Core::MetricHistogramSample 上界（秒）+ 每格条数 + count + sum（秒）
     */
    [[nodiscard]] ASYN_NET_API Core::MetricHistogramSample httpRouteLatencyHistogramSample(const HttpRouteCount &routeCount);

    /**
     * @brief 把延迟直方图折成公共数据模型（`Core::MetricHistogramSample`，单位统一到秒）
     *
     * @details 换算在这里只做一次：档位上界是毫秒常量、`totalLatencyMicroseconds` 是微秒累计，
     *          都换到秒。分格保持每格条数（与快照同形，非累计）——Prometheus 展示格式要的累计
     *          数由 `Core::cumulativeBucketCounts()` 现算，那属于渲染端的活，模型里不存两份。
     * @param stats 统计快照
     * @return Core::MetricHistogramSample 上界（秒）+ 每格条数 + count + sum（秒）
     */
    [[nodiscard]] ASYN_NET_API Core::MetricHistogramSample httpLatencyHistogramSample(const HttpServerStats &stats);

    /**
     * @brief 统计采集端：HTTP 与 WebSocket 的累计计数、状态码分类与延迟直方图
     *
     * @details 与快照放在同一文件：字段集合、档位常量与采集口径必须同步演进，分开写会让
     *          「加了字段却忘了采集」这类漂移难以察觉。
     * @note 计数一律用 std::atomic：同一台服务器的会话可能跑在不同循环线程上，而取快照的
     *       调用方通常在别的线程（运维接口、测试线程）。各字段独立原子，快照因此不是
     *       同一瞬间的一致切面；需要严格一致的切面时应在不再有新请求的时机采样。
     * @note activeConnectionCount 由本类持有、由 Core::ConnectionManager 在增删连接的临界区内
     *       同步维护（见 activeConnectionCountMirror()）。它刻意不由某台服务器自己填：同一端口
     *       常由多台服务器（每线程一个）共同监听，各自读自己的连接表只会报出 1/N 的量。
     */
    class ASYN_NET_API HttpMetricsCollector
    {
    public:
        /// 构造：所有计数从零开始
        HttpMetricsCollector() = default;

        /**
         * @brief 记一条已收齐的请求
         * @note 计数用放宽内存序：只做累加，读侧不依赖它与其它字段的先后次序
         */
        void countParsedRequest() noexcept
        {
            m_totalRequestCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条因解析失败或协议错误收口的请求
         */
        void countBadRequest() noexcept
        {
            m_badRequestCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条被空闲清扫协程按超时关闭的连接
         */
        void countTimeoutClosedConnection() noexcept
        {
            m_timeoutClosedCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条「响应没送完就收口」的连接
         * @details 两个落点：写侧失败处（h1 的 recordSendFailure、h2 的 flushOutgoingBytes 失败分支），
         *          以及 h2 会话收口时仍有响应留在待发缓冲/流控队列里的场合。写侧那处挂在「连接判死」
         *          的翻转上，收口那处以判死为否——因此一条连接至多记一次。
         * @note 只覆盖 h1/h2 的 TCP 侧：QUIC 的发送由传输层自行重传，没有这个形态
         */
        void countWriteAbortedConnection() noexcept
        {
            m_writeAbortedConnectionCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条已发出的响应：状态码类计数 + 本次耗时落进直方图（全局与按路由各一份）
         * @param statusCode 响应状态码；1xx~5xx 之外的取值不计入任何一类（本框架不会发出这种状态码）
         * @param elapsed 从「收到完整请求」到「响应发完」的耗时
         * @param matchedRoute 本次派发命中的路由模式（`HttpRequest::matchedRoute()`）；空表示没有匹配到
         *        路由，落进 `unmatched` 哨兵。不要传请求的实际 URI——那会让序列数随请求数增长
         */
        void recordResponse(const int statusCode, const std::chrono::steady_clock::duration elapsed, const std::string_view matchedRoute) noexcept
        {
            countGlobalResponseStatus(statusCode);

            // 延迟照记：即便状态码不在 1xx~5xx 内，这次请求的耗时也是真实发生过的
            const std::size_t   bucketIndex   = latencyBucketIndex(elapsed);
            const std::uint64_t elapsedMicros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
            m_latencyBucketCounts[bucketIndex].fetch_add(1, std::memory_order_relaxed);
            m_totalLatencyMicroseconds.fetch_add(elapsedMicros, std::memory_order_relaxed);

            recordRouteSample(matchedRoute, statusCode, bucketIndex, elapsedMicros);
        }

        /**
         * @brief 只记一条已发出的响应的状态码类，不落延迟样本（全局与按路由各一份）
         * @details 给拿不到可信请求耗时的会话用（HTTP/3 各流由传输层驱动，会话没有「收到完整请求」
         *          那一刻的戳），让它们的流量至少出现在请求数与状态码类里。不要用 0 秒糊弄：
         *          那会把延迟直方图与 _sum 写脏，让两个协议的耗时混在一张图里失去意义
         * @param statusCode 响应状态码；1xx~5xx 之外的取值不计入任何一类（本框架不会发出这种状态码）
         * @param matchedRoute 路由模式；拿不到就传 `kHttpRouteUnmatchedLabel`，不要在这里改成从别处取
         */
        void countResponseStatus(const int statusCode, const std::string_view matchedRoute) noexcept
        {
            countGlobalResponseStatus(statusCode);
            countRouteResponseStatus(matchedRoute, statusCode);
        }

        /// WebSocket 各项计数与上面几组同档：都用放宽内存序，只做累加，读侧不依赖字段间的先后次序

        /**
         * @brief 记一条升级成功的连接（101 已发出）
         */
        void countWebSocketUpgrade() noexcept
        {
            m_webSocketUpgradeCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条收到的数据消息（分片重组后的一条算一次）
         */
        void countWebSocketMessage() noexcept
        {
            m_webSocketMessageCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一次因对端违反 RFC 6455 而收口的连接（关闭码 1002/1007/1009 合并为一类）
         */
        void countWebSocketProtocolErrorClose() noexcept
        {
            m_webSocketProtocolErrorCloseCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一次由对端发起关闭握手的连接
         */
        void countWebSocketPeerClose() noexcept
        {
            m_webSocketPeerCloseCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一次由本侧发起关闭握手的连接
         */
        void countWebSocketServerClose() noexcept
        {
            m_webSocketServerCloseCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条被对端取消了的单流请求（HTTP/2 的 RST_STREAM、HTTP/3 的 RESET_STREAM）
         * @note 只作废这一条请求：连接与同连接上的其它流照旧工作，故不计入 badRequestCount
         */
        void countStreamCancelled() noexcept
        {
            m_streamCancelledCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 记一条正文走零拷贝发送的响应
         * @note 只有 Linux 的明文 HTTP/1.1 会话会产生该计数：TLS 与 Windows 都没有可用的
         *       零拷贝原语，正文仍走用户态缓冲
         */
        void countZeroCopySend() noexcept
        {
            m_zeroCopySendCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 取「活跃连接数」的镜像目标，交给连接管理器在增删连接时同步维护
         * @details 由 HttpServer/HttpsServer 在构造时把自己的管理器接到这里；多台服务器共用一份
         *          采集端时，本采集端上的活跃连接数就是这几台的合计。
         * @return std::atomic<std::uint64_t>& 引用有效期同本采集端
         * @warning 除连接管理器外不要写它：那会让它与各管理器在册的连接表脱钩
         */
        [[nodiscard]] std::atomic<std::uint64_t> &activeConnectionCountMirror() noexcept
        {
            return m_activeConnectionCount;
        }

        /**
         * @brief 取当前计数的快照
         * @details 除采集端自己的计数外，这里还并入进程级的运行期积压读数（阻塞任务队列的深度与
         *          被拒条数，见 `applyRuntimeBacklogStats`）。并入放在这一处而不是各通道的 `stats()`
         *          里：三条通道都从这里取快照，收在一处就不存在「哪条通道忘了接」。
         * @return HttpServerStats 各字段分别原子读取的结果；activeConnectionCount 是这一刻
         *         共用本采集端的连接管理器在册条数之和
         */
        [[nodiscard]] HttpServerStats snapshot() const noexcept;

    private:
        /**
         * @brief 具体状态码明细表的槽位
         * @details `code == -1` 表示空槽。键用 CAS 发布，计数用 fetch_add 累加：并发插入同一个码时
         *          只有一个能把 -1 换成它，输的一方在重读后走「已有同码」的累加分支，不会丢掉这一次。
         */
        struct AtomicStatusCodeSlot
        {
            std::atomic<int>           code{-1}; ///< 状态码；-1 为空槽
            std::atomic<std::uint64_t> count{0}; ///< 该码的累计条数
        };

        /**
         * @brief 把一条响应记进具体状态码明细（有界表，满则归并）
         * @details 开放寻址、无锁：每个响应只做一次哈希 + 至多 table-size 次探测。表满或探测链走完时
         *          直接记进归并计数——这些响应仍然在状态码类那一族里，明细只是不再细分。
         * @param statusCode 响应状态码；不在 100~599 内时什么都不记
         */
        void countStatusCodeSeries(const int statusCode) noexcept
        {
            if (statusCode < 100 || statusCode > 599)
            {
                return;
            }

            std::size_t index = (static_cast<std::size_t>(statusCode) * std::size_t{2654435761U}) % kHttpStatusCodeSeriesLimit;
            for (std::size_t probe = 0; probe < kHttpStatusCodeSeriesLimit; ++probe, index = (index + 1U) % kHttpStatusCodeSeriesLimit)
            {
                AtomicStatusCodeSlot &slot     = m_statusCodeSlots[index];
                const int             slotCode = slot.code.load(std::memory_order_acquire);
                if (slotCode == statusCode)
                {
                    slot.count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (slotCode == -1)
                {
                    int expected = -1;
                    if (slot.code.compare_exchange_strong(expected, statusCode, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        slot.count.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    if (expected == statusCode)
                    {
                        // 输给了同一个码：算进这个槽，而不是再占一个
                        slot.count.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            }

            // 表满（或探测链被异码占满）：明细不再细分，但仍计入状态码类
            m_statusCodeMergedCount.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 取某个状态码所属类别的计数器
         * @param statusCode 响应状态码
         * @return 指向该类计数器的指针；状态码不在 100~599 内时返回 nullptr
         */
        [[nodiscard]] std::atomic<std::uint64_t> *statusClassCounter(const int statusCode) noexcept
        {
            // 按百位归类：只有三位数状态码才谈得上类别，其余取值一律不计
            switch (statusCode / 100)
            {
                case 1:
                    return &m_status1xxCount;
                case 2:
                    return &m_status2xxCount;
                case 3:
                    return &m_status3xxCount;
                case 4:
                    return &m_status4xxCount;
                case 5:
                    return &m_status5xxCount;
                default:
                    return nullptr;
            }
        }

        /// @brief 全局那一维的状态码类与具体状态码明细（既有口径，不经路由表）
        void countGlobalResponseStatus(const int statusCode) noexcept
        {
            if (std::atomic<std::uint64_t> *classCounter = statusClassCounter(statusCode); classCounter != nullptr)
            {
                classCounter->fetch_add(1, std::memory_order_relaxed);
            }
            countStatusCodeSeries(statusCode);
        }

        /// @brief 状态码所属的类下标（0..4）；不在 100~599 内时为空
        [[nodiscard]] static std::optional<std::size_t> statusClassIndex(const int statusCode) noexcept
        {
            if (statusCode < 100 || statusCode > 599)
            {
                return std::nullopt;
            }
            return static_cast<std::size_t>(statusCode / 100 - 1);
        }

        /// @brief 路由模式归一成标签：未匹配与静态文件各有固定哨兵，其余原样
        [[nodiscard]] static std::string_view routeLabelOf(const std::string_view matchedRoute) noexcept
        {
            if (matchedRoute.empty())
            {
                return kHttpRouteUnmatchedLabel;
            }
            if (matchedRoute == "*")
            {
                return kHttpRouteStaticLabel;
            }
            return matchedRoute;
        }

        /// @brief FNV-1a 哈希：0 留给「空槽」，故结果为零时抬成 1
        [[nodiscard]] static std::uint64_t routeLabelHash(const std::string_view label) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const char character: label)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ULL;
            }
            return hash == 0 ? 1 : hash;
        }

        /**
         * @brief 一个路由槽位：模式原文 + 五类状态码 + 延迟分档
         * @details 模式原文写在定长缓冲里、长度先于哈希发布（release/acquire 一对），读取方因此不必
         *          持锁，也不会看到半截模式。写缓冲只在插入（表未满且该模式首次出现）时发生。
         */
        struct AtomicRouteSlot
        {
            std::atomic<std::uint64_t>                                      patternHash{0};              ///< 0 = 空槽
            std::atomic<std::uint32_t>                                      patternLength{0};            ///< 模式字节数
            std::array<char, kHttpRoutePatternCapacity>                     pattern{};                   ///< 模式原文（无结尾 NUL）
            std::array<std::atomic<std::uint64_t>, 5>                       statusClassCounts{};         ///< 1xx…5xx
            std::array<std::atomic<std::uint64_t>, kHttpLatencyBucketCount> latencyBucketCounts{};       ///< 各档条数
            std::atomic<std::uint64_t>                                      totalLatencyMicroseconds{0}; ///< 耗时之和
        };

        /**
         * @brief 找到（或插入）一条路由的槽位
         * @return 槽位下标；表满或模式过长时返回 kHttpRouteSeriesLimit
         * @note 常见路径只做原子读 + 一次定长比较；只有首次出现的模式才去拿插入锁
         */
        [[nodiscard]] std::size_t findRouteSlot(const std::uint64_t hash, const std::string_view label) noexcept
        {
            for (std::size_t index = 0; index < kHttpRouteSeriesLimit; ++index)
            {
                AtomicRouteSlot    &slot     = m_routeSlots[index];
                const std::uint64_t slotHash = slot.patternHash.load(std::memory_order_acquire);
                if (slotHash == hash && slot.patternLength.load(std::memory_order_acquire) == label.size() && std::memcmp(slot.pattern.data(), label.data(), label.size()) == 0)
                {
                    return index;
                }
                if (slotHash == 0)
                {
                    return insertRouteSlot(hash, label);
                }
            }
            return kHttpRouteSeriesLimit;
        }

        /// @brief 在插入锁内复探并写入新槽位；返回下标或 kHttpRouteSeriesLimit（表满）
        [[nodiscard]] std::size_t insertRouteSlot(std::uint64_t hash, std::string_view label) noexcept;

        /// @brief 把一条响应记进路由维度（状态码类 + 延迟分档）；表满或模式过长时归并
        void recordRouteSample(const std::string_view matchedRoute, const int statusCode, const std::size_t bucketIndex, const std::uint64_t elapsedMicros) noexcept
        {
            const std::string_view label = routeLabelOf(matchedRoute);
            if (label.size() > kHttpRoutePatternCapacity)
            {
                recordMergedRouteSample(statusCode, bucketIndex, elapsedMicros);
                return;
            }
            const std::size_t slotIndex = findRouteSlot(routeLabelHash(label), label);
            if (slotIndex == kHttpRouteSeriesLimit)
            {
                recordMergedRouteSample(statusCode, bucketIndex, elapsedMicros);
                return;
            }

            AtomicRouteSlot &slot = m_routeSlots[slotIndex];
            if (const std::optional<std::size_t> classIndex = statusClassIndex(statusCode); classIndex.has_value())
            {
                slot.statusClassCounts[*classIndex].fetch_add(1, std::memory_order_relaxed);
            }
            slot.latencyBucketCounts[bucketIndex].fetch_add(1, std::memory_order_relaxed);
            slot.totalLatencyMicroseconds.fetch_add(elapsedMicros, std::memory_order_relaxed);
        }

        /// @brief 只把状态码类记进路由维度（中间响应没有可信耗时）
        void countRouteResponseStatus(const std::string_view matchedRoute, const int statusCode) noexcept
        {
            const std::string_view label = routeLabelOf(matchedRoute);
            if (label.size() > kHttpRoutePatternCapacity)
            {
                countMergedRouteResponse(statusCode);
                return;
            }
            const std::size_t slotIndex = findRouteSlot(routeLabelHash(label), label);
            if (slotIndex == kHttpRouteSeriesLimit)
            {
                countMergedRouteResponse(statusCode);
                return;
            }
            if (const std::optional<std::size_t> classIndex = statusClassIndex(statusCode); classIndex.has_value())
            {
                m_routeSlots[slotIndex].statusClassCounts[*classIndex].fetch_add(1, std::memory_order_relaxed);
            }
        }

        /// @brief 归并一条带耗时的响应：计数只落在 merged 哨兵的账上
        void recordMergedRouteSample(const int statusCode, const std::size_t bucketIndex, const std::uint64_t elapsedMicros) noexcept
        {
            m_routeSeriesMergedCount.fetch_add(1, std::memory_order_relaxed);
            if (const std::optional<std::size_t> classIndex = statusClassIndex(statusCode); classIndex.has_value())
            {
                m_mergedRouteStatusClassCounts[*classIndex].fetch_add(1, std::memory_order_relaxed);
            }
            m_mergedRouteLatencyBucketCounts[bucketIndex].fetch_add(1, std::memory_order_relaxed);
            m_mergedRouteLatencyMicroseconds.fetch_add(elapsedMicros, std::memory_order_relaxed);
        }

        /// @brief 归并一条不带耗时的响应
        void countMergedRouteResponse(const int statusCode) noexcept
        {
            m_routeSeriesMergedCount.fetch_add(1, std::memory_order_relaxed);
            if (const std::optional<std::size_t> classIndex = statusClassIndex(statusCode); classIndex.has_value())
            {
                m_mergedRouteStatusClassCounts[*classIndex].fetch_add(1, std::memory_order_relaxed);
            }
        }

        /**
         * @brief 把一次耗时折算成直方图下标
         * @param elapsed 本次请求耗时
         * @return std::size_t 首个满足「耗时不超过其上界」的档位下标；超过全部上界时为溢出档下标
         */
        [[nodiscard]] static std::size_t latencyBucketIndex(const std::chrono::steady_clock::duration elapsed) noexcept
        {
            for (std::size_t index = 0; index < kHttpLatencyUpperBoundMilliseconds.size(); ++index)
            {
                // 直接与毫秒档位比较：duration 的比较由标准库完成单位换算，不手写 1000 倍因子
                if (elapsed <= std::chrono::milliseconds(kHttpLatencyUpperBoundMilliseconds[index]))
                {
                    return index;
                }
            }
            return kHttpLatencyUpperBoundMilliseconds.size();
        }

        std::atomic<std::uint64_t> m_totalRequestCount{0};  ///< 累计已收齐的请求条数
        std::atomic<std::uint64_t> m_badRequestCount{0};    ///< 累计解析失败或协议错误收口的条数
        std::atomic<std::uint64_t> m_timeoutClosedCount{0}; ///< 累计被空闲清扫按超时关闭的连接数

        std::atomic<std::uint64_t> m_status1xxCount{0}; ///< 累计 1xx 响应条数
        std::atomic<std::uint64_t> m_status2xxCount{0}; ///< 累计 2xx 响应条数
        std::atomic<std::uint64_t> m_status3xxCount{0}; ///< 累计 3xx 响应条数
        std::atomic<std::uint64_t> m_status4xxCount{0}; ///< 累计 4xx 响应条数
        std::atomic<std::uint64_t> m_status5xxCount{0}; ///< 累计 5xx 响应条数

        /// 具体状态码明细（有界、无锁开放寻址）；只在实际出现过的码上占槽
        std::array<AtomicStatusCodeSlot, kHttpStatusCodeSeriesLimit> m_statusCodeSlots{};
        std::atomic<std::uint64_t>                                   m_statusCodeMergedCount{0}; ///< 明细表满后被归并的响应条数

        /// 延迟直方图的累计条数；元素默认值初始化（C++20 起 std::atomic 的默认构造即置零）
        std::array<std::atomic<std::uint64_t>, kHttpLatencyBucketCount> m_latencyBucketCounts{};

        std::atomic<std::uint64_t> m_totalLatencyMicroseconds{0}; ///< 累计耗时之和，单位微秒（给直方图补 _sum）

#if defined(_MSC_VER)
// 刻意的 alignas 缓存行填充会触发 MSVC 的「因对齐说明符而填充结构」告警 C4324——那正是本意，局部关掉。
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
        // WebSocket / h2 / 零拷贝这组计数由不同子系统的线程写，与上面「每响应必写」的延迟/直方图/totalLatency
        // 若落在同一 cache line，两类写者会互相把对方的行踢出缓存（伪共享）。用行边界把它们分开：
        // 20 线程 HTTP 写 × WS 写争用实测聚合吞吐从约 74.9M 提到约 114M ops/s（约 1.52 倍）。
        alignas(kCacheLineBytes) std::atomic<std::uint64_t> m_webSocketUpgradeCount{0}; ///< 累计升级成功的连接数
        std::atomic<std::uint64_t> m_webSocketMessageCount{0};                          ///< 累计收到的数据消息条数
        std::atomic<std::uint64_t> m_webSocketProtocolErrorCloseCount{0};               ///< 累计因协议错误收口的连接数
        std::atomic<std::uint64_t> m_webSocketPeerCloseCount{0};                        ///< 累计由对端发起关闭握手的连接数
        std::atomic<std::uint64_t> m_webSocketServerCloseCount{0};                      ///< 累计由本侧发起关闭握手的连接数

        std::atomic<std::uint64_t> m_streamCancelledCount{0};        ///< 累计被对端 RST_STREAM 取消了单流的 HTTP/2 请求条数
        std::atomic<std::uint64_t> m_zeroCopySendCount{0};           ///< 累计正文走零拷贝发送的响应条数（仅 Linux 会增长）
        std::atomic<std::uint64_t> m_writeAbortedConnectionCount{0}; ///< 累计「响应没送完就收口」的连接条数（写出失败或收口时仍有未送出字节）

        // 活跃连接数按「每条连接一次加、一次减」被写，与上面「每请求都写」的那几组不同热度：
        // 同处一行会让长连接的建连/断连把请求计数所在的行反复踢出缓存
        alignas(kCacheLineBytes) std::atomic<std::uint64_t> m_activeConnectionCount{0}; ///< 共用本采集端的连接管理器在册连接数

        // 路由维度：表本身有界（kHttpRouteSeriesLimit），模式原文写进槽位后只读；插入只在首次出现时发生
        alignas(kCacheLineBytes) std::array<AtomicRouteSlot, kHttpRouteSeriesLimit> m_routeSlots{};
        std::atomic_flag m_routeInsertLock = ATOMIC_FLAG_INIT; ///< 只保护「写入新槽位」，热路径不碰它

        /// 归并哨兵的账：表满/模式过长之后，新模式的响应落在这里（仍计入全局状态码类与全局直方图）
        std::array<std::atomic<std::uint64_t>, 5>                       m_mergedRouteStatusClassCounts{};
        std::array<std::atomic<std::uint64_t>, kHttpLatencyBucketCount> m_mergedRouteLatencyBucketCounts{};
        std::atomic<std::uint64_t>                                      m_mergedRouteLatencyMicroseconds{0};
        std::atomic<std::uint64_t>                                      m_routeSeriesMergedCount{0};
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    /**
     * @brief 把进程级的运行期积压读数并进一份统计快照。
     * @details 阻塞任务执行器是进程级共享的一份，不住在任何采集端里，因此与准入闸门同一办法：
     *          取快照时现读。由 `HttpMetricsCollector::snapshot()` 统一调用，三条通道（明文 HTTP、
     *          HTTPS/HTTP2、QUIC）因此报的是同一份读数，也不会出现某条通道忘了接的情况。
     *          读数本身与「哪个实例在报」无关，HELP 文案里已按进程级说明。
     * @param stats 待补的快照，就地改写 blockingTaskQueueDepth 与 blockingTaskRejectedCount
     */
    ASYN_NET_API void applyRuntimeBacklogStats(HttpServerStats &stats) noexcept;

    /**
     * @brief 把「这台的准入分母」与闸门累计拒绝并进一份统计快照
     * @details 三个数一向是各通道的 `stats()` 自己填的（只填了拒绝数那一个），因此 `/metrics` 上
     *          永远只有分子没有分母。这里把三样并成一次调用，让明文 HTTP、HTTPS/HTTP2、QUIC 三条
     *          通道对同一个问句给出同形答案：本监听器的并发上限、单个来源的上限、闸门挡过的条数。
     *          调用方负责交回实际生效的那两个数（多进程部署里配置是整机数、生效的是摊后的份额）。
     * @param stats 待补的快照
     * @param maximumConnections 本监听器的并发连接上限，0 表示不设
     * @param maximumConnectionsPerIp 单个来源 IP 的并发上限，0 表示这道闸门没装
     * @param admissionRejectedConnections 闸门累计挡掉的连接条数（限额器共用一份时报的是闸门总量）
     */
    ASYN_NET_API void applyAdmissionSnapshot(HttpServerStats &stats, std::size_t maximumConnections, std::size_t maximumConnectionsPerIp,
                                             std::uint64_t admissionRejectedConnections) noexcept;

} // namespace AsynGyanis::Net
