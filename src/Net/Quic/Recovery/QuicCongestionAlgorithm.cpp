#include "Net/Quic/Recovery/QuicCongestionAlgorithm.h"

#include "Net/Quic/Recovery/QuicCongestionControl.h"
#include "Net/Quic/Recovery/QuicCubicCongestion.h"

#include <algorithm>
#include <limits>

namespace AsynGyanis::Net
{
    namespace
    {
        /// §7.2 里那个「较大者」的字节数：UDP 只算 8 字节开销，比 TCP 的 20 字节少一截
        constexpr std::size_t kInitialWindowFloorByteLength = 14720;

        /// 初始窗口的倍数上限（§7.2：十倍最大数据报）
        constexpr std::size_t kInitialWindowDatagramMultiple = 10;

        /// 最小窗口的倍数（§7.2：两倍最大数据报）
        constexpr std::size_t kMinimumWindowDatagramMultiple = 2;

        /**
         * @brief 减去一个不应超过被减数的量
         * @param total 被减数
         * @param part 减数
         * @return std::size_t 差；算出负数说明两边记账不配对，收成 0 而不是回绕成天文数字
         */
        std::size_t saturatingSubtract(const std::size_t total, const std::size_t part)
        {
            return part > total ? 0 : total - part;
        }
    } // namespace

    QuicCongestionAlgorithm::QuicCongestionAlgorithm(const std::size_t maximumDatagramByteLength) noexcept :
        m_maximumDatagramByteLength(maximumDatagramByteLength), m_congestionWindowByteLength(initialWindowByteLength()),
        // 阈值先取「无穷大」，慢启动这才从第一个确认开始涨（§7.3.1）
        m_slowStartThresholdByteLength(std::numeric_limits<std::size_t>::max())
    {
    }

    std::size_t QuicCongestionAlgorithm::initialWindowByteLength() const noexcept
    {
        return std::min(kInitialWindowDatagramMultiple * m_maximumDatagramByteLength,
                        std::max(kMinimumWindowDatagramMultiple * m_maximumDatagramByteLength, kInitialWindowFloorByteLength));
    }

    void QuicCongestionAlgorithm::setMaximumDatagramByteLength(const std::size_t maximumDatagramByteLength) noexcept
    {
        if (maximumDatagramByteLength == 0)
        {
            // 0 会把最小窗折成 0，也就把「判成持久拥塞」变成「这条连接再也发不出东西」：留着旧尺寸
            return;
        }
        // 只换单位，不折算当前窗口：窗口本身是字节数，换尺寸之后涨窗与最小窗都按新的那格算
        m_maximumDatagramByteLength = maximumDatagramByteLength;
    }

    void QuicCongestionAlgorithm::onPacketSent(const QuicSentPacketInfo &packet) noexcept
    {
        // 只带 ACK 的包不计在途（§B.2）：它是纯反馈，把它算进负荷会让窗口越用越小
        if (packet.isAckEliciting)
        {
            m_bytesInFlight += packet.byteCount;
        }
    }

    void QuicCongestionAlgorithm::setSmoothedRoundTripTime(const QuicTime smoothed) noexcept
    {
        m_smoothedRoundTripTime = smoothed;
    }

    void QuicCongestionAlgorithm::onCongestionUpdate(const std::vector<QuicSentPacketInfo> &acknowledged, const std::vector<QuicSentPacketInfo> &lost, const QuicTime eventTime)
    {
        for (const QuicSentPacketInfo &packet: acknowledged)
        {
            if (!packet.isAckEliciting)
            {
                continue;
            }
            m_bytesInFlight = saturatingSubtract(m_bytesInFlight, packet.byteCount);
        }
        for (const QuicSentPacketInfo &packet: lost)
        {
            if (packet.isAckEliciting)
            {
                m_bytesInFlight = saturatingSubtract(m_bytesInFlight, packet.byteCount);
            }
        }

        // §7.3.2：恢复期是按**每个包发出时**判的——恢复期内发出的包被确认时不涨窗，恢复期之后发出的
        // 包一被确认就把这一轮恢复期结掉（§B.2 的 OnPacketAcked 逐包判）。按帧聚合会把两种包混在一起：
        // 拥塞刚降下来的窗被同一帧里那批旧包又涨回去，等于拥塞信号没接到动作上
        std::size_t growthByteCount = 0;
        bool        endsRecovery    = false;
        for (const QuicSentPacketInfo &packet: acknowledged)
        {
            if (!packet.isAckEliciting)
            {
                continue;
            }
            if (m_recoveryStartTime.has_value() && packet.timeSent <= *m_recoveryStartTime)
            {
                continue;
            }
            growthByteCount += packet.byteCount;
            endsRecovery = true;
        }
        if (endsRecovery)
        {
            m_recoveryStartTime = std::nullopt;
        }

        if (std::ranges::any_of(lost, [](const QuicSentPacketInfo &packet) { return !packet.isPathMtuProbe; }))
        {
            // 已经在恢复期里就不再二次降窗：一轮拥塞只降一次（§7.3.2）——CE 那条路也走这同一个判据。
            // PMTU 探针被判丢不算拥塞证据（RFC 9000 §14.4）：那条包本来就是拿去撞一个更大的尺寸的，
            // 把它算进降窗会让一次正常的探测试验凭空把窗口砍掉一大截
            startCongestionEvent(eventTime);
            return;
        }

        if (isInRecovery() || growthByteCount == 0)
        {
            // 恢复期内窗口纹丝不动（§7.3.2），慢启动与拥塞避免都只在圈外算（§7.3.1、§7.3.3）；
            // 这一帧确认的全是恢复期内的包时同样不涨
            return;
        }

        if (m_congestionWindowByteLength < m_slowStartThresholdByteLength)
        {
            // 慢启动就是「每个确认到的字节都算进窗口」（§7.3.1）：一个往返把整个窗口确认掉，窗口翻一倍。
            // 「一帧最多涨一个数据报」那条上限在 §7.3.3，管的是拥塞避免的加法增长——放到这里会把指数
            // 增长压成线性，而对端把十几个包并进一帧确认是常态（实测：4 MiB 响应因此要发 25 秒）。
            // 两家算法同为 Reno 慢启动（RFC 9438 §4.10 允许），差异全部在拥塞避免期
            m_congestionWindowByteLength += growthByteCount;
            return;
        }
        // 拥塞避免：增长律由算法自己给（NewReno 线性、CUBIC 三次曲线 + 友好区）
        applyCongestionAvoidanceGrowth(growthByteCount, eventTime);
    }

    void QuicCongestionAlgorithm::restartAfterPersistentCongestion() noexcept
    {
        // §7.6.2：窗口与慢启动阈值都落到最小窗，并退出恢复期重新走慢启动。
        // 必须清掉恢复期标记，否则重启后这一轮又被「一轮只降一次」那道闸挡住，
        // 新窗口就只是名义上的——降窗与涨窗的判据都以那枚标记为准（§7.3.2）
        const std::size_t floorByteLength = minimumWindowByteLength();
        m_congestionWindowByteLength      = floorByteLength;
        m_slowStartThresholdByteLength    = floorByteLength;
        m_recoveryStartTime               = std::nullopt;
        onWindowReset();
    }

    void QuicCongestionAlgorithm::resetToInitialState() noexcept
    {
        // RFC 9000 §9.4 的 MUST：确认了对端的新地址之后，拥塞控制器要立刻回到初值。
        // 落点是 §7.2 的初始窗口而不是最小窗：换的是一条活路，未知的是它的容量，不是它不通。
        // 在途字节不动——那些包还挂在网上，该销的账仍由确认与判丢去销；恢复期标记要清，
        // 否则新窗口会被上一轮恢复期的闸挡住，涨窗判据（§7.3.2）就废了
        m_congestionWindowByteLength   = initialWindowByteLength();
        m_slowStartThresholdByteLength = std::numeric_limits<std::size_t>::max();
        m_recoveryStartTime            = std::nullopt;
        onWindowReset();
    }

    void QuicCongestionAlgorithm::startCongestionEvent(const QuicTime eventTime) noexcept
    {
        if (isInRecovery())
        {
            return;
        }
        applyCongestionWindowDecrease();
        m_recoveryStartTime = eventTime;
    }

    void QuicCongestionAlgorithm::noteEcnCongestionEvent(const QuicTime eventTime) noexcept
    {
        // §7.1 的第一句把 CE 当拥塞信号的前提是「这条路的 ECN 已验证」，那一格在恢复层判，
        // 到这里的都已经是校验过的计数；降窗动作与丢包那条完全共用
        startCongestionEvent(eventTime);
    }

    void QuicCongestionAlgorithm::onPacketsDiscarded(const std::vector<QuicSentPacketInfo> &discardedPackets) noexcept
    {
        // 只销账不涨窗：退休一个空间不是「网络变好了」的信号，把它当确认来涨窗口会让拥塞控制白送一段
        for (const QuicSentPacketInfo &packet: discardedPackets)
        {
            if (packet.isAckEliciting)
            {
                m_bytesInFlight = saturatingSubtract(m_bytesInFlight, packet.byteCount);
            }
        }
    }

    std::size_t QuicCongestionAlgorithm::remainingByteBudget() const noexcept
    {
        return saturatingSubtract(m_congestionWindowByteLength, m_bytesInFlight);
    }

    bool QuicCongestionAlgorithm::maySend(const std::size_t byteCount) const noexcept
    {
        return byteCount <= remainingByteBudget();
    }

    QuicCongestionPhase QuicCongestionAlgorithm::phase() const noexcept
    {
        if (isInRecovery())
        {
            return QuicCongestionPhase::Recovery;
        }
        return m_congestionWindowByteLength < m_slowStartThresholdByteLength ? QuicCongestionPhase::SlowStart : QuicCongestionPhase::CongestionAvoidance;
    }

    std::size_t QuicCongestionAlgorithm::congestionWindowByteLength() const noexcept
    {
        return m_congestionWindowByteLength;
    }

    std::size_t QuicCongestionAlgorithm::slowStartThresholdByteLength() const noexcept
    {
        return m_slowStartThresholdByteLength;
    }

    std::size_t QuicCongestionAlgorithm::bytesInFlight() const noexcept
    {
        return m_bytesInFlight;
    }

    bool QuicCongestionAlgorithm::isInRecovery() const noexcept
    {
        return m_recoveryStartTime.has_value();
    }

    std::size_t QuicCongestionAlgorithm::minimumWindowByteLength() const noexcept
    {
        return kMinimumWindowDatagramMultiple * m_maximumDatagramByteLength;
    }

    std::unique_ptr<QuicCongestionAlgorithm> makeQuicCongestionAlgorithm(const QuicCongestionAlgorithmKind kind, const std::size_t maximumDatagramByteLength)
    {
        switch (kind)
        {
            case QuicCongestionAlgorithmKind::Cubic:
                return std::make_unique<QuicCubicCongestion>(maximumDatagramByteLength);
            case QuicCongestionAlgorithmKind::NewReno:
            default:
                return std::make_unique<QuicCongestionControl>(maximumDatagramByteLength);
        }
    }
} // namespace AsynGyanis::Net
