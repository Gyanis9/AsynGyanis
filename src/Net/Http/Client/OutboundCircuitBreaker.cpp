#include "Net/Http/Client/OutboundCircuitBreaker.h"

#include <algorithm>

namespace AsynGyanis::Net
{
    OutboundCircuitBreaker::OutboundCircuitBreaker(const Configuration configuration) noexcept : m_configuration(configuration)
    {
    }

    std::string_view OutboundCircuitBreaker::stateName(const State state) noexcept
    {
        switch (state)
        {
            case State::Closed:
                return "closed";
            case State::Open:
                return "open";
            case State::HalfOpen:
                return "half-open";
        }
        return "closed";
    }

    OutboundCircuitBreaker::EndpointHealth &OutboundCircuitBreaker::healthFor(const HttpOutboundEndpointKey &endpointKey, const Clock::time_point now)
    {
        const auto existing = m_endpoints.find(endpointKey);
        if (existing != m_endpoints.end())
        {
            return existing->second;
        }

        // 有界：一个会不断换主机的调用方不该把内存换成一张无限大的健康表。淘汰的是最久没更新的那条——
        // 它既没有开闸中的意义，也不会比刚见过的端点更值得占位
        if (m_configuration.maximumTrackedEndpoints > 0 && m_endpoints.size() >= m_configuration.maximumTrackedEndpoints)
        {
            const auto oldest = std::ranges::min_element(m_endpoints, [](const auto &left, const auto &right) { return left.second.lastTouched < right.second.lastTouched; });
            if (oldest != m_endpoints.end())
            {
                static_cast<void>(m_endpoints.erase(oldest));
            }
        }

        EndpointHealth &created = m_endpoints[endpointKey];
        created.lastTouched     = now;
        return created;
    }

    void OutboundCircuitBreaker::advanceLocked(EndpointHealth &health, const Clock::time_point now) noexcept
    {
        if (health.state == State::Open && now - health.openedAt >= m_configuration.openDuration)
        {
            health.state             = State::HalfOpen;
            health.inFlightProbes    = 0;
            health.halfOpenSuccesses = 0;
        }
    }

    bool OutboundCircuitBreaker::allowRequest(const HttpOutboundEndpointKey &endpointKey, const Clock::time_point now) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);

        EndpointHealth &health = healthFor(endpointKey, now);
        advanceLocked(health, now);

        switch (health.state)
        {
            case State::Closed:
                return true;
            case State::Open:
                return false;
            case State::HalfOpen:
                if (health.inFlightProbes >= std::max<std::size_t>(m_configuration.halfOpenProbeLimit, 1))
                {
                    return false;
                }
                // 占掉一个探测名额：不占的话一次抖动会把整串请求都放过去撞同一个塌掉的上游
                ++health.inFlightProbes;
                return true;
        }
        return true;
    }

    void OutboundCircuitBreaker::reportSuccess(const HttpOutboundEndpointKey &endpointKey, const Clock::time_point now) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);

        EndpointHealth &health = healthFor(endpointKey, now);
        advanceLocked(health, now);
        health.lastTouched         = now;
        health.consecutiveFailures = 0;

        if (health.state != State::HalfOpen)
        {
            return;
        }
        if (health.inFlightProbes > 0)
        {
            --health.inFlightProbes;
        }
        ++health.halfOpenSuccesses;
        if (health.halfOpenSuccesses >= std::max<std::size_t>(m_configuration.halfOpenSuccessThreshold, 1))
        {
            health.state             = State::Closed;
            health.halfOpenSuccesses = 0;
            health.inFlightProbes    = 0;
        }
    }

    void OutboundCircuitBreaker::reportFailure(const HttpOutboundEndpointKey &endpointKey, const Clock::time_point now) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);

        EndpointHealth &health = healthFor(endpointKey, now);
        advanceLocked(health, now);
        health.lastTouched = now;

        if (health.state == State::HalfOpen)
        {
            if (health.inFlightProbes > 0)
            {
                --health.inFlightProbes;
            }
            // 半开的探测失败即重新开闸并重新计时：这时上游还没证明自己活了
            health.state             = State::Open;
            health.openedAt          = now;
            health.halfOpenSuccesses = 0;
            return;
        }

        ++health.consecutiveFailures;
        if (health.state == State::Closed && health.consecutiveFailures >= std::max<std::size_t>(m_configuration.consecutiveFailureThreshold, 1))
        {
            health.state    = State::Open;
            health.openedAt = now;
        }
    }

    OutboundCircuitBreaker::State OutboundCircuitBreaker::stateOf(const HttpOutboundEndpointKey &endpointKey, const Clock::time_point now) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);

        EndpointHealth &health = healthFor(endpointKey, now);
        advanceLocked(health, now);
        return health.state;
    }

    std::size_t OutboundCircuitBreaker::trackedEndpointCount() const noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_endpoints.size();
    }

    std::size_t OutboundCircuitBreaker::openEndpointCount() const noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t                       count = 0;
        for (const auto &[key, health]: m_endpoints)
        {
            // 这里不调 advanceLocked：计数不该改变状态机，读的人要看的是「此刻闸是不是还开着」
            if (health.state == State::Open)
            {
                ++count;
            }
        }
        return count;
    }

    void OutboundCircuitBreaker::reset() noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_endpoints.clear();
    }
} // namespace AsynGyanis::Net
