/**
 * @file HttpServerStats.h
 * @brief HTTP 服务器统计快照与它的原子采集端
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace AsynGyanis::Net
{
    /// 延迟直方图的档位数：四个「上界档」加一个溢出档，与 kHttpLatencyUpperBoundMilliseconds 配套使用
    inline constexpr std::size_t kHttpLatencyBucketCount = 5;

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
     * @note 请求计数口径：totalRequestCount 只统计**已收齐**的请求，解析失败的条数单独进
     *       badRequestCount；状态码类计数同样只对应已发出的响应，故 status1xxCount 至
     *       status5xxCount 之和在稳态下等于 totalRequestCount（发送失败的应答不计入）。
     * @note 升级到 WebSocket 的连接是上述口径的例外：升级请求计入 totalRequestCount 与
     *       webSocketUpgradeCount，但 101 由握手模块逐字节生成、不走 HttpResponse 序列化，
     *       故不进状态码类计数；101 之后的帧错误另进 webSocketProtocolErrorCloseCount，
     *       不并入 badRequestCount（后者的口径是 HTTP 报文解析失败，回的是 4xx）。
     * @note streamCancelledCount 是「单流取消」这一形态的口径（HTTP/2 的 RST_STREAM、HTTP/3 的
     *       RESET_STREAM/STOP_SENDING）：只作废这一条请求（连接与同连接上的其它流照旧工作），
     *       因此既不算已应答也不算坏请求；HTTP/1.1 上没有这一形态（取消即断连），该字段恒为 0。
     * @see HttpMetricsCollector, HttpServer::stats()
     */
    struct HttpServerStats
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
    class HttpMetricsCollector
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
         * @brief 记一条已发出的响应：状态码类计数 + 本次耗时落进直方图
         * @param statusCode 响应状态码；1xx~5xx 之外的取值不计入任何一类（本框架不会发出这种状态码）
         * @param elapsed 从「收到完整请求」到「响应发完」的耗时
         */
        void recordResponse(const int statusCode, const std::chrono::steady_clock::duration elapsed) noexcept
        {
            countResponseStatus(statusCode);

            // 延迟照记：即便状态码不在 1xx~5xx 内，这次请求的耗时也是真实发生过的
            m_latencyBucketCounts[latencyBucketIndex(elapsed)].fetch_add(1, std::memory_order_relaxed);
            m_totalLatencyMicroseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()), std::memory_order_relaxed);
        }

        /**
         * @brief 只记一条已发出的响应的状态码类，不落延迟样本
         * @details 给拿不到可信请求耗时的会话用（HTTP/3 各流由传输层驱动，会话没有「收到完整请求」
         *          那一刻的戳），让它们的流量至少出现在请求数与状态码类里。**不要用 0 秒糊弄**：
         *          那会把延迟直方图与 _sum 写脏，让两个协议的耗时混在一张图里失去意义
         * @param statusCode 响应状态码；1xx~5xx 之外的取值不计入任何一类（本框架不会发出这种状态码）
         */
        void countResponseStatus(const int statusCode) noexcept
        {
            if (std::atomic<std::uint64_t> *classCounter = statusClassCounter(statusCode); classCounter != nullptr)
            {
                classCounter->fetch_add(1, std::memory_order_relaxed);
            }
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
         * @return HttpServerStats 各字段分别原子读取的结果；activeConnectionCount 是这一刻
         *         共用本采集端的连接管理器在册条数之和
         */
        [[nodiscard]] HttpServerStats snapshot() const noexcept
        {
            HttpServerStats stats;
            stats.totalRequestCount           = m_totalRequestCount.load(std::memory_order_relaxed);
            stats.activeConnectionCount       = m_activeConnectionCount.load(std::memory_order_relaxed);
            stats.badRequestCount             = m_badRequestCount.load(std::memory_order_relaxed);
            stats.timeoutClosedCount          = m_timeoutClosedCount.load(std::memory_order_relaxed);
            stats.writeAbortedConnectionCount = m_writeAbortedConnectionCount.load(std::memory_order_relaxed);
            stats.status1xxCount              = m_status1xxCount.load(std::memory_order_relaxed);
            stats.status2xxCount              = m_status2xxCount.load(std::memory_order_relaxed);
            stats.status3xxCount              = m_status3xxCount.load(std::memory_order_relaxed);
            stats.status4xxCount              = m_status4xxCount.load(std::memory_order_relaxed);
            stats.status5xxCount              = m_status5xxCount.load(std::memory_order_relaxed);

            stats.webSocketUpgradeCount            = m_webSocketUpgradeCount.load(std::memory_order_relaxed);
            stats.webSocketMessageCount            = m_webSocketMessageCount.load(std::memory_order_relaxed);
            stats.webSocketProtocolErrorCloseCount = m_webSocketProtocolErrorCloseCount.load(std::memory_order_relaxed);
            stats.webSocketPeerCloseCount          = m_webSocketPeerCloseCount.load(std::memory_order_relaxed);
            stats.webSocketServerCloseCount        = m_webSocketServerCloseCount.load(std::memory_order_relaxed);
            stats.streamCancelledCount             = m_streamCancelledCount.load(std::memory_order_relaxed);
            stats.zeroCopySendCount                = m_zeroCopySendCount.load(std::memory_order_relaxed);

            for (std::size_t index = 0; index < kHttpLatencyBucketCount; ++index)
            {
                stats.latencyBucketCounts[index] = m_latencyBucketCounts[index].load(std::memory_order_relaxed);
            }
            stats.totalLatencyMicroseconds = m_totalLatencyMicroseconds.load(std::memory_order_relaxed);
            return stats;
        }

    private:
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
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace AsynGyanis::Net
