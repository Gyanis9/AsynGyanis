// TestQuicCubicCongestion.cpp —— CUBIC 拥塞控制（RFC 9438）用例
//
// 时间、包号、字节数全部注入，期望值全部按规范公式在用例里独立推导（不从被测代码取）：
//   1) 初始窗口与慢启动沿用 RFC 9002 §7.2/§7.3.1 的共用骨架；
//   2) 降窗按 β=0.7 且不低于最小窗，降窗前的窗口记进 W_max（§4.6）；
//   3) 快收敛：窗口低于上一次的 W_max 时把 W_max 再压低到 ×(1+β)/2（§4.7）；
//   4) 三次曲线：t=K 时目标窗回到 W_max，1.5× 上限截断（§4.2 图 1）；
//   5) Reno 友好区：低在途下时钟重定、增长走 W_est（§4.3 与 §4.2 的「空闲不计时」）；
//   6) W_est 追平降窗前的窗口后 α 切到 1（§4.3）；
//   7) 持久拥塞重启清掉曲线状态（§4.8 的精神）；
//   8) ECN 与丢包共用「一轮只降一次」的闸（RFC 9002 §7.3.2）。
// 纯计算，不起网络也不依赖外部服务。

#include "Net/Quic/Recovery/QuicCubicCongestion.h"

#include "Net/Quic/Recovery/QuicCongestionControl.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端目前唯一使用的数据报上限（没做 PMTU 探测，见 RFC 9000 §14.1）
        constexpr std::size_t kDatagramSize = 1200;

        /// §7.2 算出来的初始窗口：min(10×1200, max(2×1200, 14720)) = 12000
        constexpr std::size_t kInitialWindow = 12000;

        /// 一秒的微秒数（QuicTime 是微秒）
        constexpr std::int64_t kSecondMicroseconds = 1'000'000;

        /// 用例里注入的 srtt：100 毫秒，够大而不夸张，公式复算好读
        constexpr std::int64_t kSmoothedRttMicroseconds = 100'000;

        /// §4.2 的 C=0.4、§4.3 的 α 与 §4.2 的 1.5× 上限（β 与快收敛因子在用例里按有理式 7/10、17/20 对表）
        constexpr double kCubicC            = 0.4;
        constexpr double kRenoFriendlyAlpha = 3.0 * 0.3 / 1.7;
        constexpr double kTargetCapFactor   = 1.5;

        /**
         * @brief 造一个已发包记录（与 NewReno 用例同形）
         */
        QuicSentPacketInfo makePacket(const std::uint64_t packetNumber, const std::int64_t timeSent, const std::size_t byteCount, const bool isAckEliciting = true)
        {
            QuicSentPacketInfo packet;
            packet.packetNumber   = packetNumber;
            packet.timeSent       = QuicTime{timeSent};
            packet.byteCount      = byteCount;
            packet.isAckEliciting = isAckEliciting;
            return packet;
        }

        /// 发一个数据报并在稍后确认掉它，包号自增
        void driveOneAck(QuicCubicCongestion &congestion, std::uint64_t &packetNumber, const std::int64_t sendMicroseconds, const std::int64_t ackMicroseconds)
        {
            const QuicSentPacketInfo sent = makePacket(packetNumber, sendMicroseconds, kDatagramSize);
            congestion.onPacketSent(sent);
            congestion.onCongestionUpdate({sent}, {}, QuicTime{ackMicroseconds});
            ++packetNumber;
        }

        /// 发一个数据报并判丢它，包号自增
        void driveOneLoss(QuicCubicCongestion &congestion, std::uint64_t &packetNumber, const std::int64_t sendMicroseconds, const std::int64_t lossMicroseconds)
        {
            const QuicSentPacketInfo sent = makePacket(packetNumber, sendMicroseconds, kDatagramSize);
            congestion.onPacketSent(sent);
            congestion.onCongestionUpdate({}, {sent}, QuicTime{lossMicroseconds});
            ++packetNumber;
        }

        /// §4.2 图 1 与图 2：由 W_max/（段）、cwnd_epoch（段）与时刻 t（秒）算 W_cubic(t)（段）
        double cubicSegments(const double secondsSinceEpoch, const double kSeconds, const double wMaxSegments)
        {
            const double offset = secondsSinceEpoch - kSeconds;
            return kCubicC * offset * offset * offset + wMaxSegments;
        }

        /// §4.2 图 2：K = cbrt((W_max − cwnd_epoch) / C)，单位秒（两窗都按段）
        double cubicKSeconds(const double wMaxSegments, const double cwndEpochSegments)
        {
            return std::cbrt(std::max(0.0, (wMaxSegments - cwndEpochSegments) / kCubicC));
        }
    } // namespace

    /**
     * @brief 建好就是慢启动、窗口取 §7.2 的初始值：骨架与另一家算法共用
     */
    TEST(QuicCubicCongestion, StartsInSlowStartWithTheInitialWindow)
    {
        const QuicCubicCongestion congestion{kDatagramSize};

        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow);
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), std::numeric_limits<std::size_t>::max());
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::SlowStart);
        EXPECT_FALSE(congestion.maximumWindowBeforeDecreaseByteLength().has_value()) << "还没过拥塞事件，W_max 应当是空的";
        EXPECT_FALSE(congestion.hasActiveCongestionAvoidanceEpoch());
    }

    /**
     * @brief 丢包降窗乘 β=0.7（不是 NewReno 的一半），并把降窗前的窗口记进 W_max
     * @details 首次降窗时 W_max 就是降窗前的窗口（§4.7 的 otherwise 分支）；同一轮里
     *          再报一次拥塞也不二次降窗（那道闸在基类，这里顺带钉住 CUBIC 也吃它）
     */
    TEST(QuicCubicCongestion, ReducesByThreeTenthsOnLossAndRecordsThePreDecreaseWindow)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        std::uint64_t       packetNumber = 0;

        // 慢启动涨一格：12000 + 1200 = 13200
        driveOneAck(congestion, packetNumber, 0, 1000);
        ASSERT_EQ(congestion.congestionWindowByteLength(), kInitialWindow + kDatagramSize);

        const std::size_t windowBeforeDecrease = congestion.congestionWindowByteLength();
        driveOneLoss(congestion, packetNumber, 2000, 3000);

        // 0.7 按有理式 7/10 对表：浮点 0.7 乘完再截断会差 1 个字节
        const std::size_t expectedThreshold = windowBeforeDecrease * 7 / 10;
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), expectedThreshold) << "ssthresh 应当乘 0.7";
        EXPECT_EQ(congestion.congestionWindowByteLength(), expectedThreshold) << "窗口落到阈值";
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::Recovery);
        ASSERT_TRUE(congestion.maximumWindowBeforeDecreaseByteLength().has_value());
        EXPECT_EQ(*congestion.maximumWindowBeforeDecreaseByteLength(), windowBeforeDecrease) << "首次降窗：W_max 记降窗前的窗口";

        // 同一轮里再降一次：被「一轮只降一次」挡住，窗口纹丝不动
        driveOneLoss(congestion, packetNumber, 3500, 4000);
        EXPECT_EQ(congestion.congestionWindowByteLength(), expectedThreshold) << "恢复期内不该二次降窗";
    }

    /**
     * @brief 快收敛：窗口低于上一次的 W_max 时，W_max 再压低到 ×(1+β)/2
     * @details 判据是「降窗前的窗口 < 上一次的 W_max」，关系式从被测读数现算，不问中间值细节
     */
    TEST(QuicCubicCongestion, FastConvergenceLowersWMaxAfterADownwardShift)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        std::uint64_t       packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000);
        driveOneLoss(congestion, packetNumber, 2000, 3000); // 第一次降窗：W_max = 13200

        // 恢复期结束（确认一个恢复期之后发出的包），此刻窗口 ≈ 9240，仍低于 W_max
        driveOneAck(congestion, packetNumber, 4000, 5000);
        ASSERT_TRUE(congestion.maximumWindowBeforeDecreaseByteLength().has_value()) << "本用例的前提：第一次降窗后已记 W_max";
        const std::size_t windowBeforeSecondDecrease = congestion.congestionWindowByteLength();
        ASSERT_LT(windowBeforeSecondDecrease, *congestion.maximumWindowBeforeDecreaseByteLength()) << "本用例的前提：窗口低于 W_max";

        driveOneLoss(congestion, packetNumber, 6000, 7000);
        // 快收敛那一档按有理式 17/20 对表（(1+β)/2 = 0.85），避开浮点截断的 1 字节噪声
        const std::size_t expectedWMax = windowBeforeSecondDecrease * 17 / 20;
        ASSERT_TRUE(congestion.maximumWindowBeforeDecreaseByteLength().has_value());
        EXPECT_EQ(*congestion.maximumWindowBeforeDecreaseByteLength(), expectedWMax) << "快收敛应当把 W_max 压到 ×(1+β)/2";
    }

    /**
     * @brief 三次曲线：t=K 时目标窗回到 W_max
     * @details 期望值按 §4.2 的公式在用例里独立推导；在途全程压满半窗以上（保曲线时钟累积，
     *          也被「低在途重定时钟」那条近似隔离在外）
     */
    TEST(QuicCubicCongestion, ReturnsTowardWMaxAtTheCurveTime)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        congestion.setSmoothedRoundTripTime(QuicTime{kSmoothedRttMicroseconds});
        std::uint64_t packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000); // 慢启动到 13200
        // 压满 7 个数据报再丢一个：在途 7200，够半窗以上
        std::vector<QuicSentPacketInfo> inFlight;
        for (int index = 0; index < 7; ++index)
        {
            inFlight.push_back(makePacket(packetNumber, 2000, kDatagramSize));
            congestion.onPacketSent(inFlight.back());
            ++packetNumber;
        }
        congestion.onCongestionUpdate({}, {inFlight.front()}, QuicTime{3000}); // 第一次降窗：cwnd = 9240

        // 恢复期结束（确认一个恢复期之后发出的那个包），这一步同时开出拥塞避免的 epoch
        driveOneAck(congestion, packetNumber, 4000, 5000);
        const std::size_t windowAtEpochStart = congestion.congestionWindowByteLength();
        ASSERT_TRUE(congestion.hasActiveCongestionAvoidanceEpoch()) << "第一次拥塞避免的确认应当开钟";

        const std::size_t wMaxBytes         = *congestion.maximumWindowBeforeDecreaseByteLength();
        const double      wMaxSegments      = static_cast<double>(wMaxBytes) / static_cast<double>(kDatagramSize);
        const double      windowEpochSeg    = static_cast<double>(windowAtEpochStart) / static_cast<double>(kDatagramSize);
        const double      kSeconds          = cubicKSeconds(wMaxSegments, windowEpochSeg);
        const double      elapsedSeconds    = kSeconds; // 走到 t = K
        const double      smoothedRttSecond = static_cast<double>(kSmoothedRttMicroseconds) / static_cast<double>(kSecondMicroseconds);

        const auto   windowBefore    = congestion.congestionWindowByteLength();
        const double windowBeforeSeg = static_cast<double>(windowBefore) / static_cast<double>(kDatagramSize);
        // §4.2 图 1：目标窗是曲线值再夹进 [cwnd, 1.5×cwnd]——期望式按同一条公式算全（只算曲线值会在
        // 上限收紧的场景里漏掉截断，那是期望式的缺口而不是被测的错）
        const double targetSegments = std::clamp(cubicSegments(elapsedSeconds + smoothedRttSecond, kSeconds, wMaxSegments), windowBeforeSeg, windowBeforeSeg * kTargetCapFactor);

        const auto sent = makePacket(packetNumber, 6000, kDatagramSize);
        congestion.onPacketSent(sent);
        congestion.onCongestionUpdate({sent}, {}, QuicTime{5000 + static_cast<std::int64_t>(elapsedSeconds * static_cast<double>(kSecondMicroseconds))});
        ++packetNumber;

        const double targetBytes = targetSegments * static_cast<double>(kDatagramSize);
        const double expectedWindowAfterCubic =
                static_cast<double>(windowBefore) + (targetBytes - static_cast<double>(windowBefore)) * static_cast<double>(kDatagramSize) / static_cast<double>(windowBefore);
        EXPECT_NEAR(static_cast<double>(congestion.congestionWindowByteLength()), expectedWindowAfterCubic, 2.0)
                << "t=K 的确认该把窗口沿曲线带向 W_max 附近；实际 " << congestion.congestionWindowByteLength() << " 期望约 " << expectedWindowAfterCubic;
    }

    /**
     * @brief 目标窗的 1.5× 上限：曲线冲得再远，一次确认也只按 1.5×cwnd 算增量
     * @details 把 epoch 时间一次推进 100 秒（远过 K），曲线值远超上限，期望值取 1.5×cwnd 的关系式现算
     */
    TEST(QuicCubicCongestion, CapsTheTargetAtOneAndAHalfTimesCwnd)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        congestion.setSmoothedRoundTripTime(QuicTime{kSmoothedRttMicroseconds});
        std::uint64_t packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000);
        std::vector<QuicSentPacketInfo> inFlight;
        for (int index = 0; index < 6; ++index)
        {
            inFlight.push_back(makePacket(packetNumber, 2000, kDatagramSize));
            congestion.onPacketSent(inFlight.back());
            ++packetNumber;
        }
        congestion.onCongestionUpdate({}, {inFlight.front()}, QuicTime{3000});

        driveOneAck(congestion, packetNumber, 4000, 5000); // 出恢复期 + 开 epoch
        ASSERT_TRUE(congestion.hasActiveCongestionAvoidanceEpoch());

        const auto windowBefore = congestion.congestionWindowByteLength();
        const auto sent         = makePacket(packetNumber, 6000, kDatagramSize);
        congestion.onPacketSent(sent);
        congestion.onCongestionUpdate({sent}, {}, QuicTime{5000 + 100 * kSecondMicroseconds}); // t = 100 秒
        ++packetNumber;

        // 曲线在 t=100s 处远超 1.5×cwnd：target 被截到 1.5×，增量按关系式现算
        const double cappedTargetBytes         = static_cast<double>(windowBefore) * kTargetCapFactor;
        const double expectedWindowAfterCapped = static_cast<double>(windowBefore) +
                                                 (cappedTargetBytes - static_cast<double>(windowBefore)) * static_cast<double>(kDatagramSize) / static_cast<double>(windowBefore);
        EXPECT_NEAR(static_cast<double>(congestion.congestionWindowByteLength()), expectedWindowAfterCapped, 2.0)
                << "上限截断下的增量不符合 1.5×cwnd 的关系式；实际 " << congestion.congestionWindowByteLength() << " 期望约 " << expectedWindowAfterCapped;
        EXPECT_LT(congestion.congestionWindowByteLength(), static_cast<std::size_t>(cappedTargetBytes) + kDatagramSize) << "一次确认不该把窗口顶过 1.5×cwnd 加上这一格的量级";
    }

    /**
     * @brief Reno 友好区：低在途的确认不累积曲线时间，增长走 W_est 的线性式
     * @details 每步只压一个数据报（在途低于半窗）：时钟反复重定（§4.2 的「空闲不计时」近似），
     *          曲线停在起点下方，增长全部由 W_est 的 α 式给出——期望值按 §4.3 图 4 逐步现算
     */
    TEST(QuicCubicCongestion, GrowsInTheRenoFriendlyRegionWhenTheCubicCurveIsSlow)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        congestion.setSmoothedRoundTripTime(QuicTime{kSmoothedRttMicroseconds});
        std::uint64_t packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000);
        driveOneLoss(congestion, packetNumber, 2000, 3000);
        driveOneAck(congestion, packetNumber, 4000, 5000); // 出恢复期；epoch 建立，W_est 从当前窗口起步

        // W_est 的字节式：每步增量 = α × 确认字节 × 数据报上限 / 当前窗口（§4.3 图 4 的字节化）
        double            expectedWindow = static_cast<double>(congestion.congestionWindowByteLength());
        const std::size_t windowStart    = congestion.congestionWindowByteLength();
        for (int step = 0; step < 20; ++step)
        {
            const std::int64_t sendMicroseconds = 5000 + static_cast<std::int64_t>(step + 1) * kSecondMicroseconds;
            driveOneAck(congestion, packetNumber, sendMicroseconds, sendMicroseconds + 1000);
            expectedWindow += kRenoFriendlyAlpha * static_cast<double>(kDatagramSize) * static_cast<double>(kDatagramSize) / expectedWindow;
        }

        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::CongestionAvoidance);
        // 容差 25：每步窗口落进整数字节时截断最多丢 1 字节，20 步累计到十几字节——容差要把这个
        // 截断尾巴盖住；而它离「守卫失效时一步跳几百字节」的突变红区还差一个数量级
        EXPECT_NEAR(static_cast<double>(congestion.congestionWindowByteLength()), expectedWindow, 25.0)
                << "友好区的增长应当贴着 W_est 的线性式；实际 " << congestion.congestionWindowByteLength() << " 期望约 " << expectedWindow;
        EXPECT_GT(congestion.congestionWindowByteLength(), windowStart) << "总得有增长";
        EXPECT_LT(congestion.congestionWindowByteLength(), 12000U) << "低在途下不该出现曲线的大幅追赶（时钟重定把空闲挡在计时之外）";
    }

    /**
     * @brief W_est 追平降窗前的窗口后，α 从 0.5294… 切到 1
     * @details 逐步驱动并在跨过 cwnd_prior 之后的下一步换用 α=1 的关系式比对；200 步内必须跨过，
     *          跨不过说明增长式根本没在走
     */
    TEST(QuicCubicCongestion, SwitchesToUnitAlphaOnceWEstReachesThePreDecreaseWindow)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        congestion.setSmoothedRoundTripTime(QuicTime{kSmoothedRttMicroseconds});
        std::uint64_t packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000);
        const std::size_t windowAtDecrease = congestion.congestionWindowByteLength();
        driveOneLoss(congestion, packetNumber, 2000, 3000);
        driveOneAck(congestion, packetNumber, 4000, 5000);

        bool crossedPreDecreaseWindow = false;
        for (int step = 0; step < 200 && !crossedPreDecreaseWindow; ++step)
        {
            const std::int64_t sendMicroseconds = 5000 + static_cast<std::int64_t>(step + 1) * kSecondMicroseconds;
            driveOneAck(congestion, packetNumber, sendMicroseconds, sendMicroseconds + 1000);
            crossedPreDecreaseWindow = congestion.congestionWindowByteLength() >= windowAtDecrease;
        }
        ASSERT_TRUE(crossedPreDecreaseWindow) << "W_est 该在 200 步内追平降窗前的窗口";

        // 已过切换点：下一步的增量按 α=1 的关系式给
        const auto         windowBefore     = congestion.congestionWindowByteLength();
        const std::int64_t sendMicroseconds = 5000 + 500 * kSecondMicroseconds;
        driveOneAck(congestion, packetNumber, sendMicroseconds, sendMicroseconds + 1000);
        const double expectedWindowAfter =
                static_cast<double>(windowBefore) + 1.0 * static_cast<double>(kDatagramSize) * static_cast<double>(kDatagramSize) / static_cast<double>(windowBefore);
        EXPECT_NEAR(static_cast<double>(congestion.congestionWindowByteLength()), expectedWindowAfter, 5.0)
                << "跨过 cwnd_prior 之后 α 应当切到 1；实际 " << congestion.congestionWindowByteLength() << " 期望约 " << expectedWindowAfter;
    }

    /**
     * @brief 持久拥塞重启：窗口与阈值落最小窗，曲线状态（W_max/epoch）一并清掉
     */
    TEST(QuicCubicCongestion, RestartAfterPersistentCongestionClearsTheCurveState)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        std::uint64_t       packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000);
        driveOneLoss(congestion, packetNumber, 2000, 3000);
        driveOneAck(congestion, packetNumber, 4000, 5000);
        ASSERT_TRUE(congestion.hasActiveCongestionAvoidanceEpoch());

        congestion.restartAfterPersistentCongestion();

        EXPECT_EQ(congestion.congestionWindowByteLength(), 2 * kDatagramSize) << "§7.6.2：窗口落到最小窗";
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), 2 * kDatagramSize);
        EXPECT_FALSE(congestion.maximumWindowBeforeDecreaseByteLength().has_value()) << "重启后 W_max 记录该清掉（§4.8 从当前窗口重定 K）";
        EXPECT_FALSE(congestion.hasActiveCongestionAvoidanceEpoch()) << "epoch 该清掉";

        // 重启后从最小窗重新走：一次确认有增长（K=0 的曲线从当前窗口起步）
        const std::size_t windowBefore = congestion.congestionWindowByteLength();
        driveOneAck(congestion, packetNumber, 6000, 7000);
        EXPECT_GT(congestion.congestionWindowByteLength(), windowBefore);
    }

    /**
     * @brief ECN-CE 与丢包共用「一轮只降一次」：CE 按 β 降一次，随后同轮内再来不降
     */
    TEST(QuicCubicCongestion, EcnSharesTheOnePerRoundDecreaseWithLoss)
    {
        QuicCubicCongestion congestion{kDatagramSize};
        std::uint64_t       packetNumber = 0;

        driveOneAck(congestion, packetNumber, 0, 1000);
        const std::size_t windowBeforeDecrease = congestion.congestionWindowByteLength();

        congestion.noteEcnCongestionEvent(QuicTime{2000});
        EXPECT_EQ(congestion.congestionWindowByteLength(), windowBeforeDecrease * 7 / 10) << "CE 也按 0.7 降";
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::Recovery);

        congestion.noteEcnCongestionEvent(QuicTime{2500});
        EXPECT_EQ(congestion.congestionWindowByteLength(), windowBeforeDecrease * 7 / 10) << "同轮第二个 CE 不二次降窗";
    }

    /**
     * @brief 工厂按选择键造出对应算法：cubic 出 CUBIC、newreno 与越界取值都出 NewReno（缺省分支）
     * @details 越界取值是「未来的键被旧二进制读到」的形状：落到缺省分支而不是崩或空指针
     */
    TEST(QuicCubicCongestion, FactoryBuildsTheRequestedAlgorithm)
    {
        const std::unique_ptr<QuicCongestionAlgorithm> cubic = makeQuicCongestionAlgorithm(QuicCongestionAlgorithmKind::Cubic, kDatagramSize);
        ASSERT_NE(cubic, nullptr);
        EXPECT_NE(dynamic_cast<QuicCubicCongestion *>(cubic.get()), nullptr) << "cubic 应造出 CUBIC";
        EXPECT_EQ(cubic->congestionWindowByteLength(), kInitialWindow) << "初始窗两家同值（RFC 9002 §7.2）";

        const std::unique_ptr<QuicCongestionAlgorithm> reno = makeQuicCongestionAlgorithm(QuicCongestionAlgorithmKind::NewReno, kDatagramSize);
        ASSERT_NE(reno, nullptr);
        EXPECT_NE(dynamic_cast<QuicCongestionControl *>(reno.get()), nullptr) << "newreno 应造出 NewReno";

        const std::unique_ptr<QuicCongestionAlgorithm> unknown = makeQuicCongestionAlgorithm(static_cast<QuicCongestionAlgorithmKind>(99), kDatagramSize);
        ASSERT_NE(unknown, nullptr);
        EXPECT_NE(dynamic_cast<QuicCongestionControl *>(unknown.get()), nullptr) << "未知选择键应落到 NewReno 缺省分支";
    }
} // namespace AsynGyanis::Net
