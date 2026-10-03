#include "Net/Http/HttpServerStats.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Platform/System/ProcessInfo.h"

#include <cstddef>

namespace AsynGyanis::Net
{
    void applyRuntimeBacklogStats(HttpServerStats &stats) noexcept
    {
        // 执行器是进程级共享的一份，不住在任何采集端里，因此只能在取快照时现读——与准入闸门同一办法。
        // 放在 snapshot() 这一处而不是各通道的 stats() 里：三条通道都从这里取快照，收在一处就不存在
        // 「哪条通道忘了接」那种分叉
        // 读的是**进程级共享的那一台**执行器：`Database::Queryable::useAsyncExecutor()` 允许注入自建实例，
        // 那几台的队列与拒绝数不在这里（读数因此是「共享执行器」的口径，不是全进程）。help 里同句注明
        stats.blockingTaskQueueDepth    = static_cast<std::uint64_t>(Core::AsyncExecutor::shared().pendingTaskCount());
        stats.blockingTaskRejectedCount = static_cast<std::uint64_t>(Core::AsyncExecutor::shared().saturatedRejectionCount());
        // 常驻内存是同一类「进程级、不住在采集端里」的读数：三条通道共用一份，才有「抓哪台都一样」
        stats.residentMemoryBytes = Platform::ProcessInfo::residentMemoryBytes();
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

        applyRuntimeBacklogStats(stats);
        return stats;
    }
} // namespace AsynGyanis::Net
