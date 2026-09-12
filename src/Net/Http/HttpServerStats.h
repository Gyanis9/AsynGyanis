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
     * @see HttpMetricsCollector, HttpServer::stats()
     */
    struct HttpServerStats
    {
        std::uint64_t totalRequestCount{0};     ///< 累计成功解析（ParseStatus::Done）的请求条数，不含解析失败
        std::uint64_t activeConnectionCount{0}; ///< 取快照那一刻挂在连接管理器上的活跃连接数
        std::uint64_t badRequestCount{0};       ///< 解析失败或协议错误收口的条数（HttpParseErrorKind 各档合并为一类）
        std::uint64_t timeoutClosedCount{0};    ///< 被空闲清扫协程按空闲/读写超时关闭的 HTTP 连接数
        std::uint64_t status1xxCount{0};        ///< 状态码为 1xx 的响应条数
        std::uint64_t status2xxCount{0};        ///< 状态码为 2xx 的响应条数
        std::uint64_t status3xxCount{0};        ///< 状态码为 3xx 的响应条数
        std::uint64_t status4xxCount{0};        ///< 状态码为 4xx 的响应条数
        std::uint64_t status5xxCount{0};        ///< 状态码为 5xx 的响应条数

        /// 延迟直方图的累计条数，下标与 kHttpLatencyUpperBoundMilliseconds 对应；末档为溢出档
        std::array<std::uint64_t, kHttpLatencyBucketCount> latencyBucketCounts{};

        /**
         * @brief 取延迟直方图的样本总数
         * @return std::uint64_t 各档累计值之和；与 totalRequestCount 的差即「已收齐但响应未发出」的条数
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
     * @brief 统计采集端：三组累计计数、状态码分类与延迟直方图
     *
     * @details 与快照放在同一文件：字段集合、档位常量与采集口径必须同步演进，分开写会让
     *          「加了字段却忘了采集」这类漂移难以察觉。
     * @note 计数一律用 std::atomic：同一台服务器的会话可能跑在不同循环线程上，而取快照的
     *       调用方通常在别的线程（运维接口、测试线程）。各字段独立原子，快照因此不是
     *       同一瞬间的一致切面；需要严格一致的切面时应在不再有新请求的时机采样。
     * @note activeConnectionCount 不由本类维护：连接数的唯一真值来源是 Core::ConnectionManager，
     *       由 HttpServer::stats() 在取快照时补上，避免两份计数彼此漂移。
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
         * @brief 记一条已发出的响应：状态码类计数 + 本次耗时落进直方图
         * @param statusCode 响应状态码；1xx~5xx 之外的取值不计入任何一类（本框架不会发出这种状态码）
         * @param elapsed 从「收到完整请求」到「响应发完」的耗时
         */
        void recordResponse(const int statusCode, const std::chrono::steady_clock::duration elapsed) noexcept
        {
            if (std::atomic<std::uint64_t> *classCounter = statusClassCounter(statusCode); classCounter != nullptr)
            {
                classCounter->fetch_add(1, std::memory_order_relaxed);
            }

            // 延迟照记：即便状态码不在 1xx~5xx 内，这次请求的耗时也是真实发生过的
            m_latencyBucketCounts[latencyBucketIndex(elapsed)].fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 取当前计数的快照
         * @return HttpServerStats 各字段分别原子读取的结果；activeConnectionCount 留给调用方填充
         */
        [[nodiscard]] HttpServerStats snapshot() const noexcept
        {
            HttpServerStats stats;
            stats.totalRequestCount  = m_totalRequestCount.load(std::memory_order_relaxed);
            stats.badRequestCount    = m_badRequestCount.load(std::memory_order_relaxed);
            stats.timeoutClosedCount = m_timeoutClosedCount.load(std::memory_order_relaxed);
            stats.status1xxCount     = m_status1xxCount.load(std::memory_order_relaxed);
            stats.status2xxCount     = m_status2xxCount.load(std::memory_order_relaxed);
            stats.status3xxCount     = m_status3xxCount.load(std::memory_order_relaxed);
            stats.status4xxCount     = m_status4xxCount.load(std::memory_order_relaxed);
            stats.status5xxCount     = m_status5xxCount.load(std::memory_order_relaxed);

            for (std::size_t index = 0; index < kHttpLatencyBucketCount; ++index)
            {
                stats.latencyBucketCounts[index] = m_latencyBucketCounts[index].load(std::memory_order_relaxed);
            }
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
    };

} // namespace AsynGyanis::Net
