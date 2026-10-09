#include "Net/Quic/QuicPathMtuDiscovery.h"

#include <algorithm>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 阶梯里不超过天花板的第一格
         * @details 天花板连第一格都装不下时返回 BASE：那意味着对端只肯收 1200 字节，没有可探的更大尺寸
         * @param ceiling 本端愿意探的最大尺寸
         * @return std::size_t 待探尺寸
         */
        std::size_t firstProbeByteLengthAtMost(const std::size_t ceiling) noexcept
        {
            for (const std::size_t candidate: QuicPathMtuDiscovery::kProbeSizeTable)
            {
                if (candidate <= ceiling)
                {
                    return candidate;
                }
            }
            return QuicPathMtuDiscovery::kBaseDatagramPayloadByteLength;
        }
    } // namespace

    QuicPathMtuDiscovery::QuicPathMtuDiscovery(const QuicTime probeTimeout, const QuicTime raiseTimeout) noexcept : m_probeTimeout(probeTimeout), m_raiseTimeout(raiseTimeout)
    {
    }

    void QuicPathMtuDiscovery::onHandshakeConfirmed(const QuicTime now) noexcept
    {
        if (m_phase != QuicPathMtuPhase::Disabled)
        {
            // 只认第一次：客户端有「解到第一条 1-RTT」与「收到 HANDSHAKE_DONE」两条路径，服务端看的是
            // 对端确认过 Handshake 空间的包——重复调用把计时器推回去会让第一条探针永远排不到点
            return;
        }
        m_datagramPayloadByteLength = kBaseDatagramPayloadByteLength;
        m_failedProbeCount          = 0;
        m_isProbeOutstanding        = false;
        m_probedByteLength          = firstProbeByteLengthAtMost(ceilingByteLength());
        if (m_probedByteLength <= m_datagramPayloadByteLength)
        {
            // 对端的上限不比 BASE 大：没有可探的尺寸，直接进收口态等抬升计时器
            m_phase    = QuicPathMtuPhase::SearchComplete;
            m_deadline = now + m_raiseTimeout;
            return;
        }
        m_phase    = QuicPathMtuPhase::Base;
        m_deadline = now;
    }

    void QuicPathMtuDiscovery::onPeerMaximumPayload(const std::uint64_t peerMaximumPayloadByteLength) noexcept
    {
        // 天花板夹在 [MIN_PLPMTU, 阶梯顶格] 这一段里：低于 MIN 的宣告按 §18.2 进不了连接，但本类对
        // maximumDatagramPayloadByteLength() 的承诺是「不小于 BASE」，那道校验的结果不该被再信一次
        m_peerMaximumPayloadByteLength =
                std::clamp<std::size_t>(static_cast<std::size_t>(std::min<std::uint64_t>(peerMaximumPayloadByteLength, kLargestProbedDatagramPayloadByteLength)),
                                        kMinimumDatagramPayloadByteLength, kLargestProbedDatagramPayloadByteLength);
        // 天花板降下来就立刻折回不超过它的那一格：继续按原来的大尺寸发，就是把字节往黑洞里投
        m_datagramPayloadByteLength = std::min(m_datagramPayloadByteLength, ceilingByteLength());
        if (m_probedByteLength > ceilingByteLength())
        {
            m_probedByteLength = m_datagramPayloadByteLength;
        }
    }

    std::size_t QuicPathMtuDiscovery::maximumDatagramPayloadByteLength() const noexcept
    {
        return m_datagramPayloadByteLength;
    }

    std::optional<std::size_t> QuicPathMtuDiscovery::probeByteLengthIfDue(const QuicTime now, const std::size_t availableByteBudget) noexcept
    {
        if (m_isProbeOutstanding || !m_deadline.has_value() || now < *m_deadline || m_phase == QuicPathMtuPhase::Disabled || m_phase == QuicPathMtuPhase::SearchComplete)
        {
            return std::nullopt;
        }
        if (m_probedByteLength <= m_datagramPayloadByteLength)
        {
            // 没有比当前更大的尺寸可探（对端上限就到这里，或阶梯已经走完）
            return std::nullopt;
        }
        if (m_probedByteLength > availableByteBudget)
        {
            // 窗口腾不出待探尺寸就不算到点：探针要吃拥塞窗口（RFC 9000 §14.4），把它当成免费车道就会
            // 在窗口很小的时候连着「失败」三次，白把 PLPMTU 收口。这里也不记失败，只把下一次尝试推到
            // PROBE_TIMER 之后——不推就会被每个节拍叫起来重问一次，而窗口可能好几秒都回不到那个尺寸
            m_deadline = now + m_probeTimeout;
            return std::nullopt;
        }
        // 一次只探一个尺寸（RFC 8899 §4.1）：这条在途期间 PROBE_TIMER 就是它的收尾时限
        m_isProbeOutstanding = true;
        m_phase              = QuicPathMtuPhase::Searching;
        m_deadline           = now + m_probeTimeout;
        return m_probedByteLength;
    }

    void QuicPathMtuDiscovery::onProbeAcknowledged(const QuicTime now) noexcept
    {
        if (!m_isProbeOutstanding)
        {
            // 不是探针的确认不该走到这里，但外壳是按「被确认的包里有探针」调它的：
            // 探针被确认之后 PROBE_TIMER 已经撤了，这条重复调用只该什么都不做
            return;
        }
        m_isProbeOutstanding = false;
        m_failedProbeCount   = 0;
        // 被确认的待探尺寸升为本端尺寸（RFC 8899 §5.3.1：这一步之后 PLPMTU 才交给上层用）
        m_datagramPayloadByteLength = std::min(m_probedByteLength, ceilingByteLength());
        if (advanceProbedByteLength())
        {
            m_phase    = QuicPathMtuPhase::Searching;
            m_deadline = now + m_probeTimeout;
            return;
        }
        // 阶梯到头（或对端上限就到这里）：进 SEARCH_COMPLETE，只留抬升计时器
        m_phase    = QuicPathMtuPhase::SearchComplete;
        m_deadline = now + m_raiseTimeout;
    }

    void QuicPathMtuDiscovery::onDeadlineReached(const QuicTime now) noexcept
    {
        if (!m_deadline.has_value() || now < *m_deadline)
        {
            return;
        }
        if (m_phase == QuicPathMtuPhase::SearchComplete)
        {
            // 抬升计时器到点：重新从当前尺寸往上找（路可能变宽了，RFC 8899 §5.2 的 PMTU_RAISE_TIMER）。
            // 这一格与「探针失败到点」只能按状态分：收口态没有探针在途，搜索态里没在途的那一种是
            // 被窗口推迟过、根本没发出去的探针——后者不记失败，也不该去动阶梯
            m_failedProbeCount = 0;
            m_probedByteLength = m_datagramPayloadByteLength;
            if (advanceProbedByteLength())
            {
                m_phase    = QuicPathMtuPhase::Searching;
                m_deadline = now;
                return;
            }
            m_deadline = now + m_raiseTimeout;
            return;
        }
        if (!m_isProbeOutstanding)
        {
            // 探针从没发出去过（`probeByteLengthIfDue` 因为窗口装不下把它推迟到了这一格）。重排一次让
            // 下一次出包再问：窗口够就发出去，还是不够就再被推到 PROBE_TIMER 之后，不会每拍空转
            m_deadline = now;
            return;
        }

        m_isProbeOutstanding = false;
        ++m_failedProbeCount;
        if (m_failedProbeCount < kMaximumFailedProbeCount)
        {
            // 单个探针丢了不代表尺寸不行（RFC 8899 §5.1.3 明写「loss of a single probe is not an
            // indication of a PMTU problem」）：同一尺寸立刻再探一次，MAX_PROBES 条都不成才收口
            m_deadline = now;
            return;
        }
        m_failedProbeCount = 0;
        m_probedByteLength = m_datagramPayloadByteLength;
        m_phase            = QuicPathMtuPhase::SearchComplete;
        m_deadline         = now + m_raiseTimeout;
    }

    void QuicPathMtuDiscovery::onConnectivityLost(const QuicTime now) noexcept
    {
        if (m_phase == QuicPathMtuPhase::Disabled)
        {
            return;
        }
        // 持久拥塞说明按现在的尺寸发出去的东西一概没人答：掉回 BASE 从头再探，
        // 这是 RFC 8899 图 5 里「PL indicates loss of connectivity」那一条在本协议的落点
        m_datagramPayloadByteLength = kBaseDatagramPayloadByteLength;
        m_probedByteLength          = firstProbeByteLengthAtMost(ceilingByteLength());
        m_failedProbeCount          = 0;
        m_isProbeOutstanding        = false;
        if (m_probedByteLength <= m_datagramPayloadByteLength)
        {
            m_phase    = QuicPathMtuPhase::SearchComplete;
            m_deadline = now + m_raiseTimeout;
            return;
        }
        m_phase    = QuicPathMtuPhase::Base;
        m_deadline = now;
    }

    QuicPathMtuPhase QuicPathMtuDiscovery::phase() const noexcept
    {
        return m_phase;
    }

    std::size_t QuicPathMtuDiscovery::probedDatagramPayloadByteLength() const noexcept
    {
        return m_probedByteLength;
    }

    std::size_t QuicPathMtuDiscovery::failedProbeCount() const noexcept
    {
        return m_failedProbeCount;
    }

    std::optional<QuicTime> QuicPathMtuDiscovery::nextDeadline() const noexcept
    {
        return m_deadline;
    }

    bool QuicPathMtuDiscovery::advanceProbedByteLength() noexcept
    {
        for (const std::size_t candidate: kProbeSizeTable)
        {
            if (candidate > m_datagramPayloadByteLength && candidate <= ceilingByteLength())
            {
                m_probedByteLength = candidate;
                return true;
            }
        }
        m_probedByteLength = m_datagramPayloadByteLength;
        return false;
    }

    std::size_t QuicPathMtuDiscovery::ceilingByteLength() const noexcept
    {
        return m_peerMaximumPayloadByteLength;
    }
} // namespace AsynGyanis::Net
