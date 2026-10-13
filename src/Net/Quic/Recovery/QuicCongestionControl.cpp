#include "Net/Quic/Recovery/QuicCongestionControl.h"

#include <algorithm>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 降窗比例（§7.3.2：阈值取当前窗口的一半）
        constexpr std::size_t kLossReductionDivisor = 2;
    } // namespace

    QuicCongestionControl::QuicCongestionControl(const std::size_t maximumDatagramByteLength) noexcept : QuicCongestionAlgorithm(maximumDatagramByteLength)
    {
    }

    void QuicCongestionControl::applyCongestionWindowDecrease() noexcept
    {
        m_slowStartThresholdByteLength = std::max(m_congestionWindowByteLength / kLossReductionDivisor, minimumWindowByteLength());
        m_congestionWindowByteLength   = m_slowStartThresholdByteLength;
    }

    void QuicCongestionControl::applyCongestionAvoidanceGrowth(const std::size_t acknowledgedByteCount, const QuicTime)
    {
        // 每个「确认掉整个窗口」的时间才涨一个数据报，整数除法向下取整，最少 1 字节
        m_congestionWindowByteLength += std::max<std::size_t>(1, m_maximumDatagramByteLength * acknowledgedByteCount / m_congestionWindowByteLength);
    }
} // namespace AsynGyanis::Net
