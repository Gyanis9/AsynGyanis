#include "Net/Http/Server/HttpServerStats.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Platform/System/ProcessInfo.h"

#include <algorithm>
#include <cstddef>

namespace AsynGyanis::Net
{
    void applyRuntimeBacklogStats(HttpServerStats &stats) noexcept
    {
        // 执行器是进程级共享的一份，不住在任何采集端里，因此只能在取快照时现读——与准入闸门同一办法。
        // 放在 snapshot() 这一处而不是各通道的 stats() 里：三条通道都从这里取快照，收在一处就不存在
        // 「哪条通道忘了接」那种分叉
        // 读的是进程级共享的那一台执行器：`Database::Queryable::useAsyncExecutor()` 允许注入自建实例，
        // 那几台的队列与拒绝数不在这里（读数因此是「共享执行器」的口径，不是全进程）。help 里同句注明
        stats.blockingTaskQueueDepth    = static_cast<std::uint64_t>(Core::AsyncExecutor::shared().pendingTaskCount());
        stats.blockingTaskRejectedCount = static_cast<std::uint64_t>(Core::AsyncExecutor::shared().saturatedRejectionCount());
        // 常驻内存是同一类「进程级、不住在采集端里」的读数：三条通道共用一份，才有「抓哪台都一样」
        stats.residentMemoryBytes = Platform::ProcessInfo::residentMemoryBytes();
        // 同族的另外三件套（Prometheus 惯例的 process_* 三件）：CPU 计数器、打开的句柄数、启动时刻。
        // 启动时刻读的是进程事实，取快照多次读数相同；另两条随进程状态走
        stats.processCpuSeconds       = Platform::ProcessInfo::cpuSeconds();
        stats.openHandleCount         = Platform::ProcessInfo::openHandleCount();
        stats.processStartTimeSeconds = Platform::ProcessInfo::startTimeSeconds();
    }

    void applyAdmissionSnapshot(HttpServerStats &stats, const std::size_t maximumConnections, const std::size_t maximumConnectionsPerIp,
                                const std::uint64_t admissionRejectedConnections) noexcept
    {
        // 三个数一起填：只报分子（在册数、拒过的条数）而不报分母，抓取端就画不出「离上限还有多远」
        // 这种提前预警，只能靠人记得配置文件里写过什么——而多进程时配置是整机数、生效的是摊后的份额
        stats.maximumConnections               = static_cast<std::uint64_t>(maximumConnections);
        stats.maximumConnectionsPerIp          = static_cast<std::uint64_t>(maximumConnectionsPerIp);
        stats.admissionRejectedConnectionCount = admissionRejectedConnections;
    }

    HttpServerStats HttpMetricsCollector::snapshot() const noexcept
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

        for (std::size_t index = 0; index < kHttpStatusCodeSeriesLimit; ++index)
        {
            stats.statusCodeCounts[index].statusCode = m_statusCodeSlots[index].code.load(std::memory_order_relaxed);
            stats.statusCodeCounts[index].count      = m_statusCodeSlots[index].count.load(std::memory_order_relaxed);
        }
        stats.statusCodeMergedCount = m_statusCodeMergedCount.load(std::memory_order_relaxed);

        // 路由维度：只复制已占用的槽。模式原文与长度都在哈希发布之前写好，acquire 读到哈希即读到它们
        stats.routeCounts.reserve(kHttpRouteSeriesLimit + 1U);
        for (const AtomicRouteSlot &slot: m_routeSlots)
        {
            if (slot.patternHash.load(std::memory_order_acquire) == 0)
            {
                continue;
            }
            HttpRouteCount routeCount;
            routeCount.route.assign(slot.pattern.data(), slot.patternLength.load(std::memory_order_acquire));
            for (std::size_t classIndex = 0; classIndex < routeCount.statusClassCounts.size(); ++classIndex)
            {
                routeCount.statusClassCounts[classIndex] = slot.statusClassCounts[classIndex].load(std::memory_order_relaxed);
            }
            for (std::size_t bucketIndex = 0; bucketIndex < routeCount.latencyBucketCounts.size(); ++bucketIndex)
            {
                routeCount.latencyBucketCounts[bucketIndex] = slot.latencyBucketCounts[bucketIndex].load(std::memory_order_relaxed);
            }
            routeCount.totalLatencyMicroseconds = slot.totalLatencyMicroseconds.load(std::memory_order_relaxed);
            if (routeCount.responseCount() == 0 && routeCount.latencySampleCount() == 0)
            {
                continue;
            }
            stats.routeCounts.push_back(std::move(routeCount));
        }

        const std::uint64_t mergedRouteResponses = m_routeSeriesMergedCount.load(std::memory_order_relaxed);
        stats.routeSeriesMergedCount             = mergedRouteResponses;
        if (mergedRouteResponses > 0)
        {
            HttpRouteCount mergedCount;
            mergedCount.route = std::string(kHttpRouteMergedLabel);
            for (std::size_t classIndex = 0; classIndex < mergedCount.statusClassCounts.size(); ++classIndex)
            {
                mergedCount.statusClassCounts[classIndex] = m_mergedRouteStatusClassCounts[classIndex].load(std::memory_order_relaxed);
            }
            for (std::size_t bucketIndex = 0; bucketIndex < mergedCount.latencyBucketCounts.size(); ++bucketIndex)
            {
                mergedCount.latencyBucketCounts[bucketIndex] = m_mergedRouteLatencyBucketCounts[bucketIndex].load(std::memory_order_relaxed);
            }
            mergedCount.totalLatencyMicroseconds = m_mergedRouteLatencyMicroseconds.load(std::memory_order_relaxed);
            stats.routeCounts.push_back(std::move(mergedCount));
        }
        std::ranges::sort(stats.routeCounts, {}, &HttpRouteCount::route);

        stats.webSocketUpgradeCount            = m_webSocketUpgradeCount.load(std::memory_order_relaxed);
        stats.webSocketMessageCount            = m_webSocketMessageCount.load(std::memory_order_relaxed);
        stats.webSocketProtocolErrorCloseCount = m_webSocketProtocolErrorCloseCount.load(std::memory_order_relaxed);
        stats.webSocketPeerCloseCount          = m_webSocketPeerCloseCount.load(std::memory_order_relaxed);
        stats.webSocketServerCloseCount        = m_webSocketServerCloseCount.load(std::memory_order_relaxed);
        stats.streamCancelledCount             = m_streamCancelledCount.load(std::memory_order_relaxed);
        stats.executorSaturatedResponseCount   = m_executorSaturatedResponseCount.load(std::memory_order_relaxed);
        stats.zeroCopySendCount                = m_zeroCopySendCount.load(std::memory_order_relaxed);

        for (std::size_t index = 0; index < kHttpLatencyBucketCount; ++index)
        {
            stats.latencyBucketCounts[index] = m_latencyBucketCounts[index].load(std::memory_order_relaxed);
        }
        stats.totalLatencyMicroseconds = m_totalLatencyMicroseconds.load(std::memory_order_relaxed);

        applyRuntimeBacklogStats(stats);
        return stats;
    }

    std::size_t HttpMetricsCollector::insertRouteSlot(const std::uint64_t hash, const std::string_view label) noexcept
    {
        // 只在首次出现某条路由时进这里：拿锁期间的模式只做写入，热路径的读不经过这把锁
        while (m_routeInsertLock.test_and_set(std::memory_order_acquire))
        {
        }

        std::size_t slotIndex = kHttpRouteSeriesLimit;
        for (std::size_t index = 0; index < kHttpRouteSeriesLimit; ++index)
        {
            AtomicRouteSlot    &slot     = m_routeSlots[index];
            const std::uint64_t slotHash = slot.patternHash.load(std::memory_order_acquire);
            if (slotHash == hash && slot.patternLength.load(std::memory_order_acquire) == label.size() && std::memcmp(slot.pattern.data(), label.data(), label.size()) == 0)
            {
                slotIndex = index;
                break;
            }
            if (slotHash == 0)
            {
                std::memcpy(slot.pattern.data(), label.data(), label.size());
                slot.patternLength.store(static_cast<std::uint32_t>(label.size()), std::memory_order_release);
                // 哈希是发布点：它用 release 写出，读取方的 acquire 因此能看见上面两笔写入
                slot.patternHash.store(hash, std::memory_order_release);
                slotIndex = index;
                break;
            }
        }

        m_routeInsertLock.clear(std::memory_order_release);
        return slotIndex;
    }

    std::array<HttpStatusClassCount, 5> httpStatusClassCounts(const HttpServerStats &stats) noexcept
    {
        // 五档的顺序与类名是契约的一部分：两条出口按同一顺序编码，采集侧才可能把两种格式对上
        return {HttpStatusClassCount{.className = "1xx", .count = stats.status1xxCount}, HttpStatusClassCount{.className = "2xx", .count = stats.status2xxCount},
                HttpStatusClassCount{.className = "3xx", .count = stats.status3xxCount}, HttpStatusClassCount{.className = "4xx", .count = stats.status4xxCount},
                HttpStatusClassCount{.className = "5xx", .count = stats.status5xxCount}};
    }

    std::vector<HttpStatusCodeCount> httpStatusCodeCounts(const HttpServerStats &stats)
    {
        std::vector<HttpStatusCodeCount> counts;
        counts.reserve(kHttpStatusCodeSeriesLimit);
        for (const HttpStatusCodeCount &slot: stats.statusCodeCounts)
        {
            // 空槽（statusCode 0）与「刚占槽还没落到计数上」的瞬时态都不输出
            if (slot.statusCode >= 100 && slot.count > 0)
            {
                counts.push_back(slot);
            }
        }
        std::ranges::sort(counts, {}, &HttpStatusCodeCount::statusCode);
        return counts;
    }

    Core::MetricHistogramSample httpLatencyHistogramSample(const HttpServerStats &stats)
    {
        Core::MetricHistogramSample sample;
        sample.upperBoundsSeconds.reserve(kHttpLatencyUpperBoundMilliseconds.size());
        for (const std::int64_t upperBoundMilliseconds: kHttpLatencyUpperBoundMilliseconds)
        {
            sample.upperBoundsSeconds.push_back(static_cast<double>(upperBoundMilliseconds) / 1000.0);
        }
        sample.bucketCounts.assign(stats.latencyBucketCounts.begin(), stats.latencyBucketCounts.end());
        sample.count      = stats.latencySampleCount();
        sample.sumSeconds = static_cast<double>(stats.totalLatencyMicroseconds) / 1'000'000.0;
        return sample;
    }

    std::vector<HttpRouteCount> httpRouteCounts(const HttpServerStats &stats)
    {
        if (!stats.routeCounts.empty())
        {
            return stats.routeCounts;
        }

        // 手工构造的快照（或只填了全局计数的旧路径）没有路由明细：给一条 unmatched 的合成条目，
        // 让「所有直方图都带 route 标签」这条形状在空快照上也成立
        HttpRouteCount fallback;
        fallback.route                    = std::string(kHttpRouteUnmatchedLabel);
        fallback.statusClassCounts        = {stats.status1xxCount, stats.status2xxCount, stats.status3xxCount, stats.status4xxCount, stats.status5xxCount};
        fallback.latencyBucketCounts      = stats.latencyBucketCounts;
        fallback.totalLatencyMicroseconds = stats.totalLatencyMicroseconds;
        return {std::move(fallback)};
    }

    Core::MetricHistogramSample httpRouteLatencyHistogramSample(const HttpRouteCount &routeCount)
    {
        Core::MetricHistogramSample sample;
        sample.upperBoundsSeconds.reserve(kHttpLatencyUpperBoundMilliseconds.size());
        for (const std::int64_t upperBoundMilliseconds: kHttpLatencyUpperBoundMilliseconds)
        {
            sample.upperBoundsSeconds.push_back(static_cast<double>(upperBoundMilliseconds) / 1000.0);
        }
        sample.bucketCounts.assign(routeCount.latencyBucketCounts.begin(), routeCount.latencyBucketCounts.end());
        sample.count      = routeCount.latencySampleCount();
        sample.sumSeconds = static_cast<double>(routeCount.totalLatencyMicroseconds) / 1'000'000.0;
        return sample;
    }
} // namespace AsynGyanis::Net
