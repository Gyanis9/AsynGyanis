#include "Net/Http/HttpServerStats.h"

#include "Core/Coroutine/AsyncExecutor.h"

#include <cstddef>

namespace AsynGyanis::Net
{
    void applyRuntimeBacklogStats(HttpServerStats &stats) noexcept
    {
        // 执行器是进程级共享的一份，不住在任何采集端里，因此只能在取快照时现读——与准入闸门同一办法。
        // 放在 snapshot() 这一处而不是各通道的 stats() 里：三条通道都从这里取快照，收在一处就不存在
        // 「哪条通道忘了接」那种分叉
        stats.blockingTaskQueueDepth    = static_cast<std::uint64_t>(Core::AsyncExecutor::shared().pendingTaskCount());
        stats.blockingTaskRejectedCount = static_cast<std::uint64_t>(Core::AsyncExecutor::shared().saturatedRejectionCount());
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
