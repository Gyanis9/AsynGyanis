#include "Net/Quic/Recovery/QuicCongestionControl.h"

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

        /// 降窗比例（§7.3.2：阈值取当前窗口的一半）
        constexpr std::size_t kLossReductionDivisor = 2;

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

    QuicCongestionControl::QuicCongestionControl(const std::size_t maximumDatagramByteLength) noexcept
        : m_maximumDatagramByteLength(maximumDatagramByteLength),
          m_congestionWindowByteLength(std::min(kInitialWindowDatagramMultiple * maximumDatagramByteLength,
                                               std::max(kMinimumWindowDatagramMultiple * maximumDatagramByteLength,
                                                        kInitialWindowFloorByteLength))),
          // 阈值先取「无穷大」，慢启动这才从第一个确认开始涨（§7.3.1）
          m_slowStartThresholdByteLength(std::numeric_limits<std::size_t>::max())
    {
    }

    void QuicCongestionControl::onPacketSent(const QuicSentPacketInfo &packet) noexcept
    {
        // 只带 ACK 的包不计在途（§B.2）：它是纯反馈，把它算进负荷会让窗口越用越小
        if (packet.isAckEliciting)
        {
            m_bytesInFlight += packet.byteCount;
        }
    }

    void QuicCongestionControl::onCongestionUpdate(const std::vector<QuicSentPacketInfo> &acknowledged,
                                                  const std::vector<QuicSentPacketInfo> &lost,
                                                  const QuicTime eventTime)
    {
        std::size_t acknowledgedByteCount = 0;
        for (const QuicSentPacketInfo &packet : acknowledged)
        {
            if (!packet.isAckEliciting)
            {
                continue;
            }
            m_bytesInFlight = saturatingSubtract(m_bytesInFlight, packet.byteCount);
            acknowledgedByteCount += packet.byteCount;
        }
        for (const QuicSentPacketInfo &packet : lost)
        {
            if (packet.isAckEliciting)
            {
                m_bytesInFlight = saturatingSubtract(m_bytesInFlight, packet.byteCount);
            }
        }

        // §7.3.2：恢复期在「恢复期之后发出的包被确认」时结束。先看这一帧有没有把恢复期结掉，
        // 再判新的丢包，否则同一轮里既出圈又进圈会被漏掉
        if (m_recoveryStartTime.has_value() &&
            std::ranges::any_of(acknowledged, [this](const QuicSentPacketInfo &packet)
            { return packet.timeSent > *m_recoveryStartTime; }))
        {
            m_recoveryStartTime = std::nullopt;
        }

        if (!lost.empty() && !isInRecovery())
        {
            // 已经在恢复期里就不再二次降窗：一轮拥塞只降一次（§7.3.2）
            m_slowStartThresholdByteLength = std::max(m_congestionWindowByteLength / kLossReductionDivisor,
                                                      kMinimumWindowDatagramMultiple * m_maximumDatagramByteLength);
            m_congestionWindowByteLength = m_slowStartThresholdByteLength;
            m_recoveryStartTime = eventTime;
            return;
        }

        if (isInRecovery() || acknowledgedByteCount == 0)
        {
            // 恢复期内窗口纹丝不动（§7.3.2），慢启动与拥塞避免都只在圈外算（§7.3.1、§7.3.3）
            return;
        }

        if (m_congestionWindowByteLength < m_slowStartThresholdByteLength)
        {
            // 慢启动：按本次确认到的字节数长，但一次最多长一个数据报——多个包合并在同一帧里
            // 被确认时，指数增长才不会被算成好几倍（§7.3.1 与 §B.2 的口径）
            m_congestionWindowByteLength += std::min(acknowledgedByteCount, m_maximumDatagramByteLength);
            return;
        }
        // 拥塞避免：每个「确认掉整个窗口」的时间才涨一个数据报，整数除法向下取整，最少 1 字节
        m_congestionWindowByteLength += std::max<std::size_t>(1,
                                                             m_maximumDatagramByteLength * acknowledgedByteCount /
                                                             m_congestionWindowByteLength);
    }

    std::size_t QuicCongestionControl::remainingByteBudget() const noexcept
    {
        return saturatingSubtract(m_congestionWindowByteLength, m_bytesInFlight);
    }

    bool QuicCongestionControl::maySend(const std::size_t byteCount) const noexcept
    {
        return byteCount <= remainingByteBudget();
    }

    QuicCongestionPhase QuicCongestionControl::phase() const noexcept
    {
        if (isInRecovery())
        {
            return QuicCongestionPhase::Recovery;
        }
        return m_congestionWindowByteLength < m_slowStartThresholdByteLength ? QuicCongestionPhase::SlowStart
                                                                            : QuicCongestionPhase::CongestionAvoidance;
    }

    std::size_t QuicCongestionControl::congestionWindowByteLength() const noexcept
    {
        return m_congestionWindowByteLength;
    }

    std::size_t QuicCongestionControl::slowStartThresholdByteLength() const noexcept
    {
        return m_slowStartThresholdByteLength;
    }

    std::size_t QuicCongestionControl::bytesInFlight() const noexcept
    {
        return m_bytesInFlight;
    }

    bool QuicCongestionControl::isInRecovery() const noexcept
    {
        return m_recoveryStartTime.has_value();
    }
} // namespace AsynGyanis::Net
