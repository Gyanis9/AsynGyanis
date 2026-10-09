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
        constexpr std::int64_t kTimeThresholdNumerator   = 9;
        constexpr std::int64_t kTimeThresholdDenominator = 8;

        /// kPersistentCongestionThreshold：持久拥塞时长是 PTO 基准的几倍，规范给的推荐值是 3（§7.6.1）
        constexpr std::int64_t kPersistentCongestionThresholdMultiplier = 3;

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
        m_estimate.smoothed  = kInitialRoundTripTime;
        m_estimate.variation = kInitialRoundTripTime / 2;
    }

    void QuicRecovery::onPacketSent(const QuicRecoverySpace space, QuicSentPacketInfo packet)
    {
        SpaceState         &state        = m_spaces[spaceIndex(space)];
        const std::uint64_t packetNumber = packet.packetNumber;
        const std::size_t   byteCount    = packet.byteCount;

        const auto existing = state.unacknowledged.find(packetNumber);
        if (existing != state.unacknowledged.end())
        {
            // 同包号重复登记是把上一条覆盖掉：先冲掉旧账，在途字节才不会只增不减
            m_inFlightByteCount -= existing->second.byteCount;
            existing->second = std::move(packet);
        } else
        {
            state.unacknowledged.emplace(packetNumber, std::move(packet));
        }
        m_inFlightByteCount += byteCount;
    }

    QuicAcknowledgementUpdate QuicRecovery::onAcknowledgementReceived(const QuicRecoverySpace space, const QuicAcknowledgementFrame &acknowledgement,
                                                                      const QuicTime acknowledgementTime, const QuicTime acknowledgementDelay)
    {
        SpaceState         &state               = m_spaces[spaceIndex(space)];
        const std::uint64_t largestAcknowledged = acknowledgement.largestAcknowledgedPacketNumber;
        state.largestAcknowledged               = std::max(state.largestAcknowledged.value_or(largestAcknowledged), largestAcknowledged);

        QuicAcknowledgementUpdate         update;
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
            // 重复的 ACK 不更新 RTT（§5.1），但判丢照做：§A.7 的 OnAckReceived 无条件走第 5 步，
            // 否则「时间阈值已经到了、却又没有新包要确认」的局面只能等定时器，重发会晚一个粒度
            update.lost = detectLostPackets(space, acknowledgementTime);
            // 持久拥塞恰恰常在「一帧新确认都没有」的这段里成形：判据要的是「两次判丢之间没有确认」，
            // 所以这条出口同样要评估一次——规范要的「收到确认之后」这一格它是满足的
            update.isPersistentCongestionDetected = notePersistentCongestionSpan(update.acknowledged, update.lost);
            return update;
        }

        if (largestAcknowledgedPacket->packetNumber == largestAcknowledged &&
            std::ranges::any_of(update.acknowledged, [](const QuicSentPacketInfo &info) { return info.isAckEliciting; }))
        {
            update.isRoundTripSampled = true;
            if (!m_firstRoundTripSampleTime.has_value())
            {
                // §7.6.2 要「那两个包发出时已有先前的样本」，所以记下第一样本被收到的那一刻
                m_firstRoundTripSampleTime = acknowledgementTime;
            }
            updateRoundTripTime(acknowledgementTime - largestAcknowledgedPacket->timeSent, acknowledgementDelay);
        }

        update.lost = detectLostPackets(space, acknowledgementTime);

        // §7.6.2：这一帧之后，那段「没有确认打断的判丢」是否已经拉到持久拥塞时长之外
        update.isPersistentCongestionDetected = notePersistentCongestionSpan(update.acknowledged, update.lost);

        // §6.2.1：只有本帧真的确认到了新包，才把探测退避清零；退避的复位依据是「有进展」而不是「有帧到」
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
            m_estimate.minimum   = latestRoundTripTime;
            m_estimate.smoothed  = latestRoundTripTime;
            m_estimate.variation = latestRoundTripTime / 2;
            return;
        }

        // min_rtt 不看对端报告的延迟：它必须是本端能自证的下界（§5.2）
        m_estimate.minimum    = std::min(m_estimate.minimum, latestRoundTripTime);
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
        const QuicTime variationSample = m_estimate.smoothed > adjustedRoundTripTime ? m_estimate.smoothed - adjustedRoundTripTime : adjustedRoundTripTime - m_estimate.smoothed;
        m_estimate.variation           = (3 * m_estimate.variation + variationSample) / 4;
        m_estimate.smoothed            = m_estimate.smoothed + (adjustedRoundTripTime - m_estimate.smoothed) / 8;
    }

    void QuicRecovery::onHandshakeConfirmed(const QuicTime peerMaximumAcknowledgmentDelay) noexcept
    {
        m_isHandshakeConfirmed           = true;
        m_peerMaximumAcknowledgmentDelay = peerMaximumAcknowledgmentDelay;
    }

    std::vector<QuicSentPacketInfo> QuicRecovery::detectLostPackets(const QuicRecoverySpace space, const QuicTime now)
    {
        SpaceState                     &state = m_spaces[spaceIndex(space)];
        std::vector<QuicSentPacketInfo> lost;
        state.lossTime = std::nullopt;
        if (!state.largestAcknowledged.has_value())
        {
            return lost;
        }
        const std::uint64_t largestAcknowledged = *state.largestAcknowledged;

        // loss_delay = max(kTimeThreshold * max(latest_rtt, smoothed_rtt), kGranularity)（§6.1.2）
        const QuicTime largestObserved = std::max(m_estimate.latest, m_estimate.smoothed);
        const QuicTime lossDelay       = std::max(largestObserved * kTimeThresholdNumerator / kTimeThresholdDenominator, kTimerGranularity);
        const QuicTime lostSendTime    = now - lossDelay;

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
        QuicRecoverySpace       space = QuicRecoverySpace::Initial;
        for (const QuicRecoverySpace candidate: {QuicRecoverySpace::Initial, QuicRecoverySpace::Handshake, QuicRecoverySpace::Application})
        {
            const std::optional<QuicTime> &lossTime = m_spaces[spaceIndex(candidate)].lossTime;
            if (lossTime.has_value() && (!earliest.has_value() || *lossTime < *earliest))
            {
                earliest = lossTime;
                space    = candidate;
            }
        }
        return {earliest, space};
    }

    std::pair<std::optional<QuicTime>, QuicRecoverySpace> QuicRecovery::probeTimeoutDeadline() const noexcept
    {
        QuicRecoverySpace       space = QuicRecoverySpace::Initial;
        std::optional<QuicTime> earliest;
        // PTO 基准：smoothed + max(4*rttvar, kGranularity)，再乘退避倍数（§6.2.1）
        const QuicTime baseDuration = m_estimate.smoothed + std::max(4 * m_estimate.variation, kTimerGranularity);
        // 退避倍数是 2 的幂：乘一个整数而不是移位，`duration` 没有位移运算符
        const std::int64_t backoffFactor = static_cast<std::int64_t>(1ULL << std::min<std::size_t>(m_probeBackoffExponent, 16));

        for (const QuicRecoverySpace candidate: {QuicRecoverySpace::Initial, QuicRecoverySpace::Handshake, QuicRecoverySpace::Application})
        {
            const SpaceState             &state    = m_spaces[spaceIndex(candidate)];
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
                space    = candidate;
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
            action.lost      = detectLostPackets(lossSpace, now);
            action.lostSpace = lossSpace;
            // 时间阈值判丢也要并进那一段的锚点，但不在这里下判定：§7.6.2 把「建立」放在收到确认之后，
            // 而 §7.6.1 末段还专门说明不拿连续的探测/定时器事件来建立持久拥塞
            extendLossGap(action.lost);
            return action;
        }

        const auto [probeDeadline, probeSpace] = probeTimeoutDeadline();
        if (!probeDeadline.has_value() || *probeDeadline > now)
        {
            return action;
        }
        // 探测到期本身不代表丢包，所以这里不判丢（§6.2 明确要求别把 PTO 当丢包信号）
        action.isProbeTimeout = true;
        action.probeSpace     = probeSpace;
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
        for (const auto &[packetNumber, packet]: state.unacknowledged)
        {
            m_inFlightByteCount -= packet.byteCount;
        }
        state.unacknowledged.clear();
        state.lossTime = std::nullopt;
    }

    QuicTime QuicRecovery::persistentCongestionDuration() const noexcept
    {
        // §7.6.1 的时长与 §6.2.1 的 PTO 用同一把尺，但那份报告延迟**不论丢包在哪个空间都算进去**
        // （规范明写「unlike the PTO computation」），最后乘 kPersistentCongestionThreshold（推荐 3）
        const QuicTime baseDuration = m_estimate.smoothed + std::max(4 * m_estimate.variation, kTimerGranularity) + m_peerMaximumAcknowledgmentDelay;
        return baseDuration * kPersistentCongestionThresholdMultiplier;
    }

    void QuicRecovery::extendLossGap(const std::vector<QuicSentPacketInfo> &lost) noexcept
    {
        for (const QuicSentPacketInfo &packet: lost)
        {
            if (!packet.isAckEliciting)
            {
                // §7.6.2 明写那两个包必须都是触发确认的：对端只承诺在 max_ack_delay 内确认这种包，
                // 只带 ACK 的包本来就可能没人答，把它算进「这条路不通」的一段会误判（§B.2 也不把它计入在途）
                continue;
            }
            if (!m_firstRoundTripSampleTime.has_value() || packet.timeSent < *m_firstRoundTripSampleTime)
            {
                // §7.6.2 那条 RTT 样本：不是「判的时候手上有没有样本」，而是「这两个包发出时已有一个先前的
                // 样本」——第一样本之前用的是 kInitialRtt，它可能远大于真实 RTT，按它算的时长会把还没探明的
                // 路判死（规范原文：The persistent congestion period SHOULD NOT start until there is at
                // least one RTT sample）。所以在这一段就不让早于第一样本的判丢开起来
                continue;
            }
            if (!m_lossGapEarliestSentTime.has_value())
            {
                // 这一段从这里起算，此前的确认与本段无关：水位重新开始
                m_lossGapEarliestSentTime = packet.timeSent;
                m_lossGapAckedAfterStart  = std::nullopt;
            } else if (packet.timeSent < *m_lossGapEarliestSentTime)
            {
                // 起点往前挪：已经记下的水位都比旧起点晚，自然也比新起点晚，不必重筛
                m_lossGapEarliestSentTime = packet.timeSent;
            }
            if (!m_lossGapLatestSentTime.has_value() || packet.timeSent > *m_lossGapLatestSentTime)
            {
                m_lossGapLatestSentTime = packet.timeSent;
            }
        }
    }

    bool QuicRecovery::notePersistentCongestionSpan(const std::vector<QuicSentPacketInfo> &acknowledged, const std::vector<QuicSentPacketInfo> &lost) noexcept
    {
        // 顺序照 §7.6.2 的三条并列条件排：先并段，再记本帧的确认，最后才比时长——
        // 段里的终点会随后续判丢继续往后长，此刻落在段外的确认（本帧那条最新确认的包必然晚于本趟所有
        // 判丢）之后可能落进段内，所以水位要在段定下来之后再记，判定要在两条都齐了之后才做
        extendLossGap(lost);

        if (m_lossGapEarliestSentTime.has_value())
        {
            // 「这两个判丢的包之间发出的包一个都没被确认」要跨三个空间一起看：锚点是全局的，
            // 这里把本帧确认到的包（不论哪个空间）都记进「晚于锚点起点的最早一次确认」那一个水位
            for (const QuicSentPacketInfo &packet: acknowledged)
            {
                if (packet.timeSent > *m_lossGapEarliestSentTime && (!m_lossGapAckedAfterStart.has_value() || packet.timeSent < *m_lossGapAckedAfterStart))
                {
                    m_lossGapAckedAfterStart = packet.timeSent;
                }
            }
        }

        if (!m_lossGapEarliestSentTime.has_value() || !m_lossGapLatestSentTime.has_value())
        {
            return false;
        }
        if (m_lossGapAckedAfterStart.has_value() && *m_lossGapAckedAfterStart <= *m_lossGapLatestSentTime)
        {
            // 段里出现过确认 ⇒ 这条路在段内通过过一次，不再算「整段都不通」。这一段作废，
            // 下一次判丢从头攒新的一段
            m_lossGapEarliestSentTime = std::nullopt;
            m_lossGapLatestSentTime   = std::nullopt;
            m_lossGapAckedAfterStart  = std::nullopt;
            return false;
        }
        if (*m_lossGapLatestSentTime - *m_lossGapEarliestSentTime <= persistentCongestionDuration())
        {
            return false;
        }

        // 判成之后把这一段的账清掉：下一次要有新的判丢才再攒
        m_lossGapEarliestSentTime = std::nullopt;
        m_lossGapLatestSentTime   = std::nullopt;
        m_lossGapAckedAfterStart  = std::nullopt;
        return true;
    }

    std::size_t QuicRecovery::inFlightByteCount() const noexcept
    {
        return m_inFlightByteCount;
    }

    std::vector<QuicSentPacketInfo> QuicRecovery::unacknowledgedPackets(const QuicRecoverySpace space) const
    {
        const SpaceState               &state = m_spaces[spaceIndex(space)];
        std::vector<QuicSentPacketInfo> packets;
        packets.reserve(state.unacknowledged.size());
        for (const auto &[packetNumber, packet]: state.unacknowledged)
        {
            packets.push_back(packet);
        }
        return packets;
    }

    QuicRoundTripTimeEstimate QuicRecovery::roundTripTimeEstimate() const noexcept
    {
        QuicRoundTripTimeEstimate estimate = m_estimate;
        // 交出去的 PTO 带当前退避倍数，日志与用例看到的和定时器用的是同一个数
        const QuicTime baseDuration = estimate.smoothed + std::max(4 * estimate.variation, kTimerGranularity);
        estimate.probeTimeout       = baseDuration * static_cast<std::int64_t>(1ULL << std::min<std::size_t>(m_probeBackoffExponent, 16));
        return estimate;
    }
} // namespace AsynGyanis::Net
