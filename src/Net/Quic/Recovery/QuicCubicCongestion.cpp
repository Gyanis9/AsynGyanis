#include "Net/Quic/Recovery/QuicCubicCongestion.h"

#include <algorithm>
#include <cmath>

namespace AsynGyanis::Net
{
    namespace
    {
        /// §4.6：乘性减因子 β 的分子/分母（0.7，SHOULD）；整数表达是为了降窗算式里不落浮点尾差
        constexpr std::size_t kDecreaseNumerator   = 7;
        constexpr std::size_t kDecreaseDenominator = 10;

        /// §4.7 快收敛那一档：降窗前的窗口 × (1+β)/2 = ×0.85
        constexpr std::size_t kFastConvergenceNumerator   = 17;
        constexpr std::size_t kFastConvergenceDenominator = 20;

        /// §4.2 图 1 的 C：单位是「段 / 秒³」，配合段制窗口直接进立方（§4.1.1 允许字节实现）
        constexpr double kCubicScalingC = 0.4;

        /// §4.3 的 α_cubic = 3 × (1−β)/(1+β)；β=0.7 时约 0.5294
        constexpr double kRenoFriendlyAlpha = 3.0 * 0.3 / 1.7;

        /// §4.2 图 1 的上界系数：目标窗最多取当前窗口的 1.5 倍，不许一次确认把增长拉过慢启动的斜率
        constexpr double kMaximumTargetWindowFactor = 1.5;

        /// 微秒到秒的换算因子（QuicTime 是微秒）
        constexpr double kMicrosecondsPerSecond = 1'000'000.0;

        /// §4.2 图 1：W_cubic(t) = C × (t − K)³ + W_max（两窗都按段）
        [[nodiscard]] double cubicWindowSegments(const double secondsSinceEpoch, const double kSeconds, const double wMaxSegments) noexcept
        {
            const double offset = secondsSinceEpoch - kSeconds;
            return kCubicScalingC * offset * offset * offset + wMaxSegments;
        }
    } // namespace

    QuicCubicCongestion::QuicCubicCongestion(const std::size_t maximumDatagramByteLength) noexcept : QuicCongestionAlgorithm(maximumDatagramByteLength)
    {
    }

    std::optional<std::size_t> QuicCubicCongestion::maximumWindowBeforeDecreaseByteLength() const noexcept
    {
        return m_maximumWindowBeforeDecrease;
    }

    bool QuicCubicCongestion::hasActiveCongestionAvoidanceEpoch() const noexcept
    {
        return m_epoch.has_value();
    }

    void QuicCubicCongestion::applyCongestionWindowDecrease() noexcept
    {
        const std::size_t windowBeforeDecrease = m_congestionWindowByteLength;
        m_windowAtDecrease                     = windowBeforeDecrease;

        // §4.7 快收敛：当前窗口低于上一次的 W_max，说明饱和点在往下走，把 W_max 再压低一档
        // （× (1+β)/2），窗口回爬得更早到顶，把带宽让给新流；其余情况 W_max 记降窗前的窗口
        if (m_maximumWindowBeforeDecrease.has_value() && windowBeforeDecrease < *m_maximumWindowBeforeDecrease)
        {
            m_maximumWindowBeforeDecrease = windowBeforeDecrease * kFastConvergenceNumerator / kFastConvergenceDenominator;
        } else
        {
            m_maximumWindowBeforeDecrease = windowBeforeDecrease;
        }

        // §4.6 图 5：ssthresh = max(cwnd × β, 最小窗)，cwnd 落到 ssthresh
        m_slowStartThresholdByteLength = std::max(windowBeforeDecrease * kDecreaseNumerator / kDecreaseDenominator, minimumWindowByteLength());
        m_congestionWindowByteLength   = m_slowStartThresholdByteLength;

        // 新一轮拥塞避免的时钟与 W_est 一并作废：下一次增长时按新 W_max 重开 K
        m_epoch.reset();
        m_renoFriendlyWindow.reset();
    }

    void QuicCubicCongestion::applyCongestionAvoidanceGrowth(const std::size_t acknowledgedByteCount, const QuicTime eventTime)
    {
        const double datagramSize   = static_cast<double>(m_maximumDatagramByteLength);
        const double windowSegments = static_cast<double>(m_congestionWindowByteLength) / datagramSize;

        // §4.2 的「空闲不计时」近似：在途不到半窗说明发送没受窗口限制（应用受限），
        // 时钟与 W_est 从当下重定——空闲时长不该被算进曲线；首帧确认（尚无 epoch）也在此开钟
        if (m_bytesInFlight < m_congestionWindowByteLength / 2 || !m_epoch.has_value())
        {
            beginCongestionAvoidanceEpoch(eventTime);
        }

        const double elapsedSeconds = std::max(0.0, static_cast<double>((eventTime - m_epoch->startTime).count()) / kMicrosecondsPerSecond);
        // RTT 是恢复层注入的环境读数；纯单测忘了注入时给 1 微秒地板，只防除零、不替 RTT 估计
        const double smoothedRttSeconds = static_cast<double>(std::max<std::int64_t>(m_smoothedRoundTripTime.count(), 1)) / kMicrosecondsPerSecond;

        // §4.2 图 1：目标窗取 W_cubic(t + RTT)，再夹进 [cwnd, 1.5×cwnd]
        const double targetSegments = std::clamp(cubicWindowSegments(elapsedSeconds + smoothedRttSeconds, m_epoch->kSeconds, m_epoch->wMaxSegments), windowSegments,
                                                 windowSegments * kMaximumTargetWindowFactor);

        // §4.3 图 4：W_est 从阶段起点的窗口线性加；追平降窗前的窗口（cwnd_prior）后 α 切到 1
        const double cwndPriorSegments = static_cast<double>(m_windowAtDecrease.value_or(m_congestionWindowByteLength)) / datagramSize;
        const double alpha             = (m_renoFriendlyWindow.value_or(windowSegments) >= cwndPriorSegments) ? 1.0 : kRenoFriendlyAlpha;
        const double friendlySegments  = m_renoFriendlyWindow.value_or(windowSegments) + alpha * (static_cast<double>(acknowledgedByteCount) / datagramSize) / windowSegments;
        m_renoFriendlyWindow           = friendlySegments;

        const double cubicAtNowSegments = cubicWindowSegments(elapsedSeconds, m_epoch->kSeconds, m_epoch->wMaxSegments);
        if (cubicAtNowSegments < friendlySegments)
        {
            // 友好区：窗口取 W_est（只进不退）
            const auto friendlyBytes     = static_cast<std::size_t>(friendlySegments * datagramSize);
            m_congestionWindowByteLength = std::max(m_congestionWindowByteLength, friendlyBytes);
            return;
        }

        // 凹/凸区（§4.4/§4.5）：按「目标与当前的差」增长，增量按本次确认的字节数缩放
        // （图 4 的 segments_acked 口径在两条区域上的字节化；一次确认聚合多段时形状与另一家算法一致）
        const double targetBytes = targetSegments * datagramSize;
        const double increment =
                (targetBytes - static_cast<double>(m_congestionWindowByteLength)) * static_cast<double>(acknowledgedByteCount) / static_cast<double>(m_congestionWindowByteLength);
        if (increment > 0.0)
        {
            m_congestionWindowByteLength += static_cast<std::size_t>(increment);
        }
    }

    void QuicCubicCongestion::onWindowReset() noexcept
    {
        // §4.8 的精神：重启后的一轮拥塞避免以当前窗口为新基准（K=0、W_max=当前窗口），
        // 旧曲线的一切记录（W_max、cwnd_prior、epoch、W_est）都不再适用
        m_maximumWindowBeforeDecrease.reset();
        m_windowAtDecrease.reset();
        m_epoch.reset();
        m_renoFriendlyWindow.reset();
    }

    void QuicCubicCongestion::beginCongestionAvoidanceEpoch(const QuicTime eventTime) noexcept
    {
        const double datagramSize   = static_cast<double>(m_maximumDatagramByteLength);
        const double windowEpochSeg = static_cast<double>(m_congestionWindowByteLength) / datagramSize;
        const double wMaxSegments   = static_cast<double>(m_maximumWindowBeforeDecrease.value_or(m_congestionWindowByteLength)) / datagramSize;

        CongestionAvoidanceEpoch epoch;
        epoch.startTime    = eventTime;
        epoch.wMaxSegments = wMaxSegments;
        // §4.2 图 2：K = cbrt((W_max − cwnd_epoch) / C)；W_max 不高于当前窗口时 K 记 0，
        // 曲线从当前窗口立即上爬（§4.8 重启后的第一段就是这个形状）
        epoch.kSeconds = wMaxSegments > windowEpochSeg ? std::cbrt((wMaxSegments - windowEpochSeg) / kCubicScalingC) : 0.0;

        m_epoch              = epoch;
        m_renoFriendlyWindow = windowEpochSeg;
    }
} // namespace AsynGyanis::Net
