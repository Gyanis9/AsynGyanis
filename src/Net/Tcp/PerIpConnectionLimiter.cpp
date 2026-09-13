#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <utility>

namespace AsynGyanis::Net
{
    PerIpConnectionLimiter::PerIpConnectionLimiter(const std::size_t maximumConnectionsPerIp) :
        m_state(std::make_shared<State>()), m_maximumConnectionsPerIp(maximumConnectionsPerIp)
    {
    }

    PerIpConnectionLimiter::Lease::Lease(std::shared_ptr<State> state, std::string ipKey) :
        m_state(std::move(state)), m_ipKey(std::move(ipKey))
    {
    }

    PerIpConnectionLimiter::Lease::Lease(Lease &&other) noexcept :
        m_state(std::move(other.m_state)), m_ipKey(std::move(other.m_ipKey))
    {
        // 把被移走的一方置空：否则它的析构会把同一个名额再还一次，计数越还越少、上限被悄悄放大
        other.m_state.reset();
    }

    PerIpConnectionLimiter::Lease &PerIpConnectionLimiter::Lease::operator=(Lease &&other) noexcept
    {
        if (this != &other)
        {
            // 先归还自己手里的名额，再接管对方的；直接覆盖会让本对象原有的名额漏还
            releaseIfTracking();
            m_state = std::move(other.m_state);
            m_ipKey = std::move(other.m_ipKey);
            other.m_state.reset();
        }
        return *this;
    }

    PerIpConnectionLimiter::Lease::~Lease()
    {
        releaseIfTracking();
    }

    bool PerIpConnectionLimiter::Lease::isTracking() const noexcept
    {
        return m_state != nullptr;
    }

    void PerIpConnectionLimiter::Lease::releaseIfTracking() noexcept
    {
        if (m_state != nullptr)
        {
            PerIpConnectionLimiter::release(*m_state, m_ipKey);
            m_state.reset();
        }
    }

    std::optional<PerIpConnectionLimiter::Lease> PerIpConnectionLimiter::tryAcquire(std::string ipKey)
    {
        // 限额为 0 表示关闭该项保护：既不记账也不拒绝，交出一个空壳凭据，
        // 调用方因此不必在「有保护」与「没保护」两条路径上分别写要不要还名额
        if (m_maximumConnectionsPerIp == 0)
        {
            return Lease{};
        }

        std::lock_guard<std::mutex> guard(m_state->mutex);

        // 用 find 而不是 operator[]：后者会为每个来过的来源留下一个 0 计数条目，
        // 被拒的连接也会把来源地址写进表里，长期运行下这张表只增不减
        const auto iterator = m_state->activeCounts.find(ipKey);
        if (iterator == m_state->activeCounts.end())
        {
            // 首次见到这个来源：插入而不是自增（ipKey 还要交给凭据保管，故传拷贝）
            m_state->activeCounts.emplace(ipKey, 1);
        }
        else
        {
            if (iterator->second >= m_maximumConnectionsPerIp)
            {
                return std::nullopt;
            }
            ++iterator->second;
        }

        return Lease{m_state, std::move(ipKey)};
    }

    std::size_t PerIpConnectionLimiter::activeCountFor(const std::string &ipKey) const
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        const auto iterator = m_state->activeCounts.find(ipKey);
        return iterator == m_state->activeCounts.end() ? 0 : iterator->second;
    }

    void PerIpConnectionLimiter::release(State &state, const std::string &ipKey)
    {
        std::lock_guard<std::mutex> guard(state.mutex);

        const auto iterator = state.activeCounts.find(ipKey);
        if (iterator == state.activeCounts.end())
        {
            // 表里没有这个来源：只可能是重复归还。照旧扣减会让计数越还越少，把上限悄悄放大，
            // 所以宁可什么都不做——凭据的移动语义已经保证正常路径下不会走到这里
            return;
        }

        if (--iterator->second == 0)
        {
            // 计数归零即删条目：否则这张表会按「历史见过的来源数」只增不减
            state.activeCounts.erase(iterator);
        }
    }

} // namespace AsynGyanis::Net
