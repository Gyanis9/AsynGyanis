#include "Net/Quic/Recovery/QuicRecovery.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// kInitialRtt：一个样本都没有时的初值（RFC 9002 §7.2、§6.2.2）
        constexpr QuicTime kInitialRoundTripTime{std::chrono::milliseconds{333}};

        /// kGranularity：定时器粒度，推荐 1 毫秒（RFC 9002 §7.2）
        constexpr QuicTime kTimerGranularity{std::chrono::milliseconds{1}};

        /// kPacketThreshold：包号阈值，推荐 3（RFC 9002 §6.1.1）
        constexpr std::uint64_t kPacketReorderingThreshold = 3;

        /// kTimeThreshold 取 9/8（RFC 9002 §6.1.2），拆开写避免整数除法先于乘法发生
        constexpr std::int64_t kTimeThresholdNumerator = 9;
        constexpr std::int64_t kTimeThresholdDenominator = 8;

        /**
         * @brief 包号是否落在 ACK 的任一区间里
         * @details 区间来自 `QuicAcknowledgementFrame`，按包号递减且不重叠；这里线性扫，
         *          区间条数被状态机压到很小，而发送侧的包数本来就有限
         * @param ranges ACK 里的区间列表
         * @param packetNumber 待判包号
         * @return true 表示这个包号被确认了
         */
        bool isWithinAcknowledgementRanges(const std::vector<QuicAcknowledgementRange> &ranges, const std::uint64_t packetNumber)
        {
            return std::ranges::any_of(ranges, [packetNumber](const QuicAcknowledgementRange &range)
            { return packetNumber >= range.smallestAcknowledged && packetNumber <= range.largestAcknowledged; });
        }
    } // namespace

    std::size_t QuicRecovery::spaceIndex(const QuicRecoverySpace space) noexcept
    {
        return static_cast<std::size_t>(space);
    }

    QuicRecovery::QuicRecovery() noexcept
    {
        // 没采过样本前也要有个数能用：PTO 与时间阈值都从这组初值算（§5.3）
        m_estimate.smoothed = kInitialRoundTripTime;
        m_estimate.variation = kInitialRoundTripTime / 2;
    }

    void QuicRecovery::onPacketSent(const QuicRecoverySpace space, QuicSentPacketInfo packet)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        const std::uint64_t packetNumber = packet.packetNumber;
        const std::size_t byteCount = packet.byteCount;

        const auto existing = state.unacknowledged.find(packetNumber);
        if (existing != state.unacknowledged.end())
        {
            // 同包号重复登记是把上一条覆盖掉：先冲掉旧账，在途字节才不会只增不减
            m_inFlightByteCount -= existing->second.byteCount;
            existing->second = std::move(packet);
        }
        else
        {
            state.unacknowledged.emplace(packetNumber, std::move(packet));
        }
        m_inFlightByteCount += byteCount;
    }

    QuicAcknowledgementUpdate QuicRecovery::onAcknowledgementReceived(const QuicRecoverySpace space,
                                                                      const QuicAcknowledgementFrame &acknowledgement,
                                                                      const QuicTime acknowledgementTime,
                                                                      const QuicTime acknowledgementDelay)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        const std::uint64_t largestAcknowledged = acknowledgement.largestAcknowledgedPacketNumber;
        state.largestAcknowledged = std::max(state.largestAcknowledged.value_or(largestAcknowledged), largestAcknowledged);

        QuicAcknowledgementUpdate update;
        std::optional<QuicSentPacketInfo> largestAcknowledgedPacket;
        for (auto packetIterator = state.unacknowledged.begin(); packetIterator != state.unacknowledged.end();)
        {
            if (!isWithinAcknowledgementRanges(acknowledgement.ranges, packetIterator->first))
            {
                ++packetIterator;
                continue;
            }
            const QuicSentPacketInfo acknowledged = packetIterator->second;
            m_inFlightByteCount -= acknowledged.byteCount;
            update.acknowledged.push_back(acknowledged);
            // 区间列表按包号递增地扫过这张表，最后进来的那个就是本次确认里的最大包号
            if (!largestAcknowledgedPacket.has_value() || acknowledged.packetNumber > largestAcknowledgedPacket->packetNumber)
            {
                largestAcknowledgedPacket = acknowledged;
            }
            packetIterator = state.unacknowledged.erase(packetIterator);
        }
        if (update.acknowledged.empty())
        {
            // 没有新确认就没有任何可更新的东西（§5.1：重复的 ACK 不许再采一次样本）
            return update;
        }

        if (largestAcknowledgedPacket->packetNumber == largestAcknowledged &&
            std::ranges::any_of(update.acknowledged, [](const QuicSentPacketInfo &info) { return info.isAckEliciting; }))
        {
            update.isRoundTripSampled = true;
            updateRoundTripTime(acknowledgementTime - largestAcknowledgedPacket->timeSent, acknowledgementDelay);
        }

        update.lost = detectLostPackets(space, acknowledgementTime);

        // 服务端认为对端的地址验证已完成（§A.8 的 PeerCompletedAddressValidation），任何新确认都归零退避
        m_probeBackoffExponent = 0;
        return update;
    }

    void QuicRecovery::updateRoundTripTime(const QuicTime latestRoundTripTime, const QuicTime acknowledgementDelay)
    {
        m_estimate.latest = latestRoundTripTime;
        if (!m_hasRoundTripSample)
        {
            // 第一个样本直接重置估算，不留初值的权重（§5.3）
            m_hasRoundTripSample = true;
            m_estimate.minimum = latestRoundTripTime;
            m_estimate.smoothed = latestRoundTripTime;
            m_estimate.variation = latestRoundTripTime / 2;
            return;
        }

        // min_rtt 不看对端报告的延迟：它必须是本端能自证的下界（§5.2）
        m_estimate.minimum = std::min(m_estimate.minimum, latestRoundTripTime);
        QuicTime clampedDelay = acknowledgementDelay;
        if (m_isHandshakeConfirmed)
        {
            // 握手确认后超出 max_ack_delay 的那部分当作路径延迟收进估算（§5.3）
            clampedDelay = std::min(clampedDelay, m_peerMaximumAcknowledgmentDelay);
        }
        QuicTime adjustedRoundTripTime = latestRoundTripTime;
        if (latestRoundTripTime >= m_estimate.minimum + clampedDelay)
        {
            adjustedRoundTripTime = latestRoundTripTime - clampedDelay;
        }

        // 顺序照 RFC 9002 附录 A.7：rttvar 用的是**更新前**的 smoothed_rtt
        const QuicTime variationSample = m_estimate.smoothed > adjustedRoundTripTime
                                             ? m_estimate.smoothed - adjustedRoundTripTime
                                             : adjustedRoundTripTime - m_estimate.smoothed;
        m_estimate.variation = (3 * m_estimate.variation + variationSample) / 4;
        m_estimate.smoothed = m_estimate.smoothed + (adjustedRoundTripTime - m_estimate.smoothed) / 8;
    }

    void QuicRecovery::onHandshakeConfirmed(const QuicTime peerMaximumAcknowledgmentDelay) noexcept
    {
        m_isHandshakeConfirmed = true;
        m_peerMaximumAcknowledgmentDelay = peerMaximumAcknowledgmentDelay;
    }

    std::vector<QuicSentPacketInfo> QuicRecovery::detectLostPackets(const QuicRecoverySpace space, const QuicTime now)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        std::vector<QuicSentPacketInfo> lost;
        state.lossTime = std::nullopt;
        if (!state.largestAcknowledged.has_value())
        {
            return lost;
        }
        const std::uint64_t largestAcknowledged = *state.largestAcknowledged;

        // loss_delay = max(kTimeThreshold * max(latest_rtt, smoothed_rtt), kGranularity)（§6.1.2）
        const QuicTime largestObserved = std::max(m_estimate.latest, m_estimate.smoothed);
        const QuicTime lossDelay = std::max(largestObserved * kTimeThresholdNumerator / kTimeThresholdDenominator,
                                            kTimerGranularity);
        const QuicTime lostSendTime = now - lossDelay;

        for (auto packetIterator = state.unacknowledged.begin(); packetIterator != state.unacknowledged.end();)
        {
            const QuicSentPacketInfo &packet = packetIterator->second;
            if (packet.packetNumber > largestAcknowledged)
            {
                // 只判「确认过的更晚的包之前」的那些：之后的包还没资格被判（§6.1）
                break;
            }
            const bool isPastPacketThreshold = largestAcknowledged >= packet.packetNumber + kPacketReorderingThreshold;
            if (packet.timeSent <= lostSendTime || isPastPacketThreshold)
            {
                lost.push_back(packet);
                m_inFlightByteCount -= packet.byteCount;
                packetIterator = state.unacknowledged.erase(packetIterator);
                continue;
            }
            // 还没到判丢条件的那一个，正是下一次定时器该醒的点
            const QuicTime candidate = packet.timeSent + lossDelay;
            if (!state.lossTime.has_value() || candidate < *state.lossTime)
            {
                state.lossTime = candidate;
            }
            ++packetIterator;
        }
        return lost;
    }

    std::pair<std::optional<QuicTime>, QuicRecoverySpace> QuicRecovery::earliestLossTime() const noexcept
    {
        std::optional<QuicTime> earliest;
        QuicRecoverySpace space = QuicRecoverySpace::Initial;
        for (const QuicRecoverySpace candidate : {QuicRecoverySpace::Initial, QuicRecoverySpace::Handshake,
                                                  QuicRecoverySpace::Application})
        {
            const std::optional<QuicTime> &lossTime = m_spaces[spaceIndex(candidate)].lossTime;
            if (lossTime.has_value() && (!earliest.has_value() || *lossTime < *earliest))
            {
                earliest = lossTime;
                space = candidate;
            }
        }
        return {earliest, space};
    }

    std::pair<std::optional<QuicTime>, QuicRecoverySpace> QuicRecovery::probeTimeoutDeadline() const noexcept
    {
        QuicRecoverySpace space = QuicRecoverySpace::Initial;
        std::optional<QuicTime> earliest;
        // PTO 基准：smoothed + max(4*rttvar, kGranularity)，再乘退避倍数（§6.2.1）
        const QuicTime baseDuration = m_estimate.smoothed + std::max(4 * m_estimate.variation, kTimerGranularity);
        // 退避倍数是 2 的幂：乘一个整数而不是移位，`duration` 没有位移运算符
        const std::int64_t backoffFactor = static_cast<std::int64_t>(1ULL << std::min<std::size_t>(m_probeBackoffExponent, 16));

        for (const QuicRecoverySpace candidate : {QuicRecoverySpace::Initial, QuicRecoverySpace::Handshake,
                                                  QuicRecoverySpace::Application})
        {
            const SpaceState &state = m_spaces[spaceIndex(candidate)];
            const std::optional<QuicTime> lastSent = lastAckElicitingSentTime(state);
            if (!lastSent.has_value())
            {
                continue;
            }
            QuicTime duration = baseDuration * backoffFactor;
            if (candidate == QuicRecoverySpace::Application)
            {
                if (!m_isHandshakeConfirmed)
                {
                    // 握手确认前不给进入用空间武装 PTO：那时重发的包对端还解不开（§6.2.1）
                    continue;
                }
                // 只有进入用空间要把对端的 max_ack_delay 算进等待时间，且同样随退避放大
                duration += m_peerMaximumAcknowledgmentDelay * backoffFactor;
            }
            const QuicTime deadline = *lastSent + duration;
            if (!earliest.has_value() || deadline < *earliest)
            {
                earliest = deadline;
                space = candidate;
            }
        }
        return {earliest, space};
    }

    std::optional<QuicTime> QuicRecovery::nextDeadline() const noexcept
    {
        // 时间阈值丢包优先：它总比 PTO 早到，也更不会误判（§6.2.1 末段）
        const std::optional<QuicTime> lossTime = earliestLossTime().first;
        if (lossTime.has_value())
        {
            return lossTime;
        }
        return probeTimeoutDeadline().first;
    }

    QuicRecoveryTimeoutAction QuicRecovery::onDeadlineReached(const QuicTime now)
    {
        QuicRecoveryTimeoutAction action;
        const auto [lossTime, lossSpace] = earliestLossTime();
        if (lossTime.has_value())
        {
            if (*lossTime > now)
            {
                return action;
            }
            action.lost = detectLostPackets(lossSpace, now);
            return action;
        }

        const auto [probeDeadline, probeSpace] = probeTimeoutDeadline();
        if (!probeDeadline.has_value() || *probeDeadline > now)
        {
            return action;
        }
        // 探测到期本身不代表丢包，所以这里不判丢（§6.2 明确要求别把 PTO 当丢包信号）
        action.isProbeTimeout = true;
        action.probeSpace = probeSpace;
        ++m_probeBackoffExponent;
        return action;
    }

    std::optional<QuicTime> QuicRecovery::lastAckElicitingSentTime(const SpaceState &state) const noexcept
    {
        // 从最大的包号往回找：PTO 要按最后一个还在途的触发确认的包起算，全部确认完就不该再有定时器
        for (auto packetIterator = state.unacknowledged.rbegin(); packetIterator != state.unacknowledged.rend(); ++packetIterator)
        {
            if (packetIterator->second.isAckEliciting)
            {
                return packetIterator->second.timeSent;
            }
        }
        return std::nullopt;
    }

    void QuicRecovery::discardSpace(const QuicRecoverySpace space)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        for (const auto &[packetNumber, packet] : state.unacknowledged)
        {
            m_inFlightByteCount -= packet.byteCount;
        }
        state.unacknowledged.clear();
        state.lossTime = std::nullopt;
    }

    std::size_t QuicRecovery::inFlightByteCount() const noexcept
    {
        return m_inFlightByteCount;
    }

    QuicRoundTripTimeEstimate QuicRecovery::roundTripTimeEstimate() const noexcept
    {
        QuicRoundTripTimeEstimate estimate = m_estimate;
        // 交出去的 PTO 带当前退避倍数，日志与用例看到的和定时器用的是同一个数
        const QuicTime baseDuration = estimate.smoothed + std::max(4 * estimate.variation, kTimerGranularity);
        estimate.probeTimeout = baseDuration * static_cast<std::int64_t>(1ULL << std::min<std::size_t>(m_probeBackoffExponent, 16));
        return estimate;
    }
} // namespace AsynGyanis::Net
