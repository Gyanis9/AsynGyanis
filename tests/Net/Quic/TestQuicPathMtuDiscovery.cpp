// TestQuicPathMtuDiscovery.cpp —— 路径 MTU 探测状态机（RFC 9000 §14.3/§14.4 与 RFC 8899 §5）用例
//
// 时刻一律注入，所以每条断言都是可精确复算的整数：尺寸阶梯、两个计时器的到点、MAX_PROBES 的收口、
// 抬升计时器重新开探，以及天花板（对端宣告与阶梯顶格的较小者）怎么把待探尺寸折回来。
// 期望值都写成注释并标出依据；改运算次序的那些数字会先变红，而不是留下一个看着合理的尺寸。
//
// 覆盖：
//   1) Disabled 那一格：握手没完成就不探、不武装定时器，尺寸停在 BASE（§14.3.1）；
//   2) 握手完成后第一条探针的尺寸与 PROBE_TIMER 的武装（RFC 8899 §5.1.1：不得小于 1 秒且 SHOULD 大于 15 秒）；
//   3) 「一次只探一个尺寸」（§4.1）：在途期间不再给出探针，也不重复武装；
//   4) 被确认的探针把 PLPMTU 抬上去并把阶梯往上推一格，探到顶进 SearchComplete（§5.3.1）；
//   5) 同一尺寸连丢满 MAX_PROBES(3) 条才收口，单条丢失不算尺寸不行（§5.1.3）；
//   6) 抬升计时器到点重新往上找（§5.2 的 PMTU_RAISE_TIMER）；
//   7) 天花板：对端宣告与阶梯顶格取小，宣告变小立刻折回，宣告低于 MIN 时本端尺寸不许掉到 BASE 之下；
//   8) 窗口腾不出待探尺寸时把下一次尝试推到 PROBE_TIMER 之后，且不记失败（§14.4：探针要吃拥塞窗口）；
//   9) 持久拥塞（「发出去的东西一概没人答」）掉回 BASE 从头再探。

#include "Net/Quic/QuicPathMtuDiscovery.h"

#include <gtest/gtest.h>

#include <chrono>

namespace AsynGyanis::Net
{
    namespace
    {
        /// @return QuicTime 毫秒换成本层的微秒时间
        constexpr QuicTime milliseconds(const std::int64_t value)
        {
            return std::chrono::duration_cast<QuicTime>(std::chrono::milliseconds{value});
        }

        /// 测试用的两个计时器周期：与缺省值同形但短到能让整台机器在用例里跑完若干轮
        constexpr QuicTime kProbeTimer{milliseconds(100)};
        constexpr QuicTime kRaiseTimer{milliseconds(1000)};

        /**
         * @brief 造一台已经握手完成、对端上限不设限的状态机
         * @param now 握手完成的时刻
         * @return QuicPathMtuDiscovery 进 Base 那一格的状态机
         */
        QuicPathMtuDiscovery makeConfirmedAt(const QuicTime now)
        {
            QuicPathMtuDiscovery discovery{kProbeTimer, kRaiseTimer};
            discovery.onHandshakeConfirmed(now);
            return discovery;
        }

        /**
         * @brief 发一条探针并让它被确认，顺带钉住「该发的是哪个尺寸」
         * @details 每次确认都会把 PROBE_TIMER 武装到 `now + 周期`，所以调用方要按周期递进时刻——
         *          拿同一个时刻连问两次只会读到「还没到点」，那是计时器的判据而不是这里的
         */
        void probeAndAck(QuicPathMtuDiscovery &discovery, const QuicTime now, const std::size_t expectedSize)
        {
            ASSERT_EQ(discovery.probeByteLengthIfDue(now, 100000U), expectedSize);
            discovery.onProbeAcknowledged(now);
        }
    } // namespace

    TEST(QuicPathMtuDiscovery, StaysDisabledUntilTheHandshakeIsConfirmed)
    {
        QuicPathMtuDiscovery discovery{kProbeTimer, kRaiseTimer};
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Disabled);
        // §14.1 与 §14.3：没探之前就是 BASE，也就是 QUIC 允许的最小数据报尺寸
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1200U);
        EXPECT_EQ(discovery.nextDeadline(), std::nullopt);
        // 预算再宽也不该在 Disabled 里发探针
        EXPECT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 100000U), std::nullopt);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Disabled);
    }

    TEST(QuicPathMtuDiscovery, DefaultTimersSatisfyTheSpecFloors)
    {
        QuicPathMtuDiscovery discovery; // 缺省：PROBE_TIMER 16 秒、PMTU_RAISE_TIMER 600 秒
        discovery.onHandshakeConfirmed(QuicTime{0});
        EXPECT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 100000U), 1232U);
        // RFC 8899 §5.1.1：PROBE_TIMER 不得小于 1 秒且 SHOULD 大于 15 秒
        EXPECT_GE(*discovery.nextDeadline(), milliseconds(15000));
        discovery.onProbeAcknowledged(QuicTime{0});
        QuicTime lastAcknowledged{0};
        for (const std::size_t expectedSize: {1452U, 8952U})
        {
            const QuicTime now = *discovery.nextDeadline();
            ASSERT_EQ(discovery.probeByteLengthIfDue(now, 100000U), expectedSize);
            discovery.onProbeAcknowledged(now);
            lastAcknowledged = now;
        }
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::SearchComplete);
        // §5.1.1 直接给了 PMTU_RAISE_TIMER 这个数：600 秒
        EXPECT_EQ(*discovery.nextDeadline(), lastAcknowledged + milliseconds(600000));
    }

    TEST(QuicPathMtuDiscovery, HandshakeConfirmationOpensTheFirstProbeSize)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(milliseconds(5));
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Base);
        // 阶梯第一格 1232 = IPv6 最小 PMTU 1280 减 IP 头 40 与 UDP 头 8（RFC 8899 §5.3.2 的尺寸表）
        EXPECT_EQ(discovery.probedDatagramPayloadByteLength(), 1232U);
        // Base 这一格不单独发探针（§14.1 的补足已经把 1200 真实确认过了），所以第一条随时可发
        EXPECT_EQ(discovery.nextDeadline(), milliseconds(5));
    }

    TEST(QuicPathMtuDiscovery, OnlyOneProbeIsOutstandingAtATime)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        EXPECT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 4000U), 1232U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Searching);
        // §4.1「一次只探一个尺寸」：在途期间再问一律交空，计时器也不被推后
        const QuicTime armedDeadline = *discovery.nextDeadline();
        EXPECT_EQ(armedDeadline, kProbeTimer);
        EXPECT_EQ(discovery.probeByteLengthIfDue(milliseconds(50), 4000U), std::nullopt);
        EXPECT_EQ(*discovery.nextDeadline(), armedDeadline);
    }

    TEST(QuicPathMtuDiscovery, AcknowledgedProbeRaisesThePayloadAndAdvancesTheLadder)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        ASSERT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 4000U), 1232U);
        discovery.onProbeAcknowledged(milliseconds(10));
        // §5.3.1：被确认的待探尺寸在这一步才升为 PLPMTU
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1232U);
        EXPECT_EQ(discovery.probedDatagramPayloadByteLength(), 1452U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Searching);
        EXPECT_EQ(discovery.failedProbeCount(), 0U);
    }

    TEST(QuicPathMtuDiscovery, LadderTopCompletesTheSearchAndArmsTheRaiseTimer)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        QuicTime             lastAcknowledged{0};
        for (const std::size_t expectedSize: {1232U, 1452U, 8952U})
        {
            // Base 那一格的待定点就是握手完成的时刻，之后每格都要等 PROBE_TIMER 到点
            const QuicTime now = *discovery.nextDeadline();
            probeAndAck(discovery, now, expectedSize);
            lastAcknowledged = now;
        }
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 8952U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::SearchComplete);
        EXPECT_EQ(*discovery.nextDeadline(), lastAcknowledged + kRaiseTimer);
        // 收口之后不再探：抬升计时器到点才重新开一轮
        EXPECT_EQ(discovery.probeByteLengthIfDue(lastAcknowledged, 100000U), std::nullopt);
    }

    TEST(QuicPathMtuDiscovery, SingleProbeLossKeepsTryingTheSameSize)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        ASSERT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 4000U), 1232U);
        discovery.onDeadlineReached(kProbeTimer);
        // §5.1.3：一条探针丢了不代表尺寸不行，同一尺寸立刻再探一次
        EXPECT_EQ(discovery.failedProbeCount(), 1U);
        EXPECT_EQ(discovery.probedDatagramPayloadByteLength(), 1232U);
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1200U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Searching);
        EXPECT_EQ(*discovery.nextDeadline(), kProbeTimer); // 立刻可再发，不必等下一个周期
    }

    TEST(QuicPathMtuDiscovery, MaximumFailedProbesCompleteTheSearch)
    {
        // RFC 8899 §5.1.2 把 MAX_PROBES 直接给了 3：这一格钉的是那个数本身，不是「跟常量同步」——
        // 拿 kMaximumFailedProbeCount 当预期值，改常量就永远测不出来
        EXPECT_EQ(QuicPathMtuDiscovery::kMaximumFailedProbeCount, 3U);
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        // 三条失败要对应三条真发出去的探针：每条发出去之后等它自己的 PROBE_TIMER 到点才算一次失败
        for (std::size_t attempt = 1; attempt <= 3U; ++attempt)
        {
            const QuicTime sentAt = *discovery.nextDeadline();
            ASSERT_EQ(discovery.probeByteLengthIfDue(sentAt, 4000U), 1232U);
            discovery.onDeadlineReached(sentAt + kProbeTimer);
            if (attempt < 3U)
            {
                EXPECT_EQ(discovery.failedProbeCount(), attempt);
                EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Searching) << "第 " << attempt << " 条失败还不该收口";
            }
            EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1200U); // 没收口之前本端尺寸不动
        }
        // MAX_PROBES(3) 条都不成才收口（§5.3.1）
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::SearchComplete);
        EXPECT_EQ(discovery.failedProbeCount(), 0U);
        EXPECT_EQ(*discovery.nextDeadline(), kProbeTimer * 3 + kRaiseTimer);
    }

    TEST(QuicPathMtuDiscovery, RaiseTimerRestartsTheSearchAboveTheCurrentSize)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        probeAndAck(discovery, *discovery.nextDeadline(), 1232U);
        ASSERT_EQ(discovery.maximumDatagramPayloadByteLength(), 1232U);
        // 让 1452 那一格连丢三条收口
        const QuicTime secondProbeStart = *discovery.nextDeadline();
        ASSERT_EQ(discovery.probeByteLengthIfDue(secondProbeStart, 100000U), 1452U);
        for (std::size_t attempt = 1; attempt <= QuicPathMtuDiscovery::kMaximumFailedProbeCount; ++attempt)
        {
            const QuicTime when = *discovery.nextDeadline();
            discovery.onDeadlineReached(when);
            if (attempt < QuicPathMtuDiscovery::kMaximumFailedProbeCount)
            {
                ASSERT_EQ(discovery.probeByteLengthIfDue(when, 100000U), 1452U);
            }
        }
        ASSERT_EQ(discovery.phase(), QuicPathMtuPhase::SearchComplete);
        ASSERT_EQ(discovery.failedProbeCount(), 0U);
        const QuicTime raiseDeadline = *discovery.nextDeadline();
        discovery.onDeadlineReached(raiseDeadline);
        // §5.2：抬升计时器到点就是「路可能变宽了，再看一眼」——从当前尺寸往上找，找到的还是刚才那格
        // （收口只说明「那一刻过不去」，不把这个尺寸永久拉黑）
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Searching);
        EXPECT_EQ(discovery.probedDatagramPayloadByteLength(), 1452U);
        EXPECT_EQ(discovery.failedProbeCount(), 0U);
    }

    TEST(QuicPathMtuDiscovery, PeerCeilingCapsTheProbeSizes)
    {
        QuicPathMtuDiscovery discovery{kProbeTimer, kRaiseTimer};
        discovery.onPeerMaximumPayload(1300); // 对端只肯收 1300：阶梯里只有 1232 那一格够得着
        discovery.onHandshakeConfirmed(QuicTime{0});
        EXPECT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 100000U), 1232U);
        discovery.onProbeAcknowledged(QuicTime{0});
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1232U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::SearchComplete);
    }

    TEST(QuicPathMtuDiscovery, PeerCeilingBelowTheMinimumStillKeepsTheBasePayload)
    {
        QuicPathMtuDiscovery discovery{kProbeTimer, kRaiseTimer};
        // §18.2 让低于 1200 的宣告进不了连接，但本类的 @return 承诺「不小于 BASE」，不能靠那道校验兜
        discovery.onPeerMaximumPayload(0);
        discovery.onHandshakeConfirmed(QuicTime{0});
        EXPECT_GE(discovery.maximumDatagramPayloadByteLength(), 1200U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::SearchComplete); // 没有可探的尺寸
        EXPECT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 100000U), std::nullopt);
    }

    TEST(QuicPathMtuDiscovery, CeilingShrinkFoldsTheConfirmedPayloadBack)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        ASSERT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 100000U), 1232U);
        discovery.onProbeAcknowledged(QuicTime{0});
        ASSERT_EQ(discovery.maximumDatagramPayloadByteLength(), 1232U);
        discovery.onPeerMaximumPayload(1200);
        // 对端把上限改小就立刻折回：继续按原来的大尺寸发就是把字节往黑洞里投
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1200U);
        EXPECT_LE(discovery.probedDatagramPayloadByteLength(), 1200U);
    }

    TEST(QuicPathMtuDiscovery, SmallWindowDefersTheProbeWithoutCountingAFailure)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        // §14.4：探针要吃拥塞窗口。窗口只够 1000 字节时装不下 1232，这一次不算到点
        EXPECT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 1000U), std::nullopt);
        EXPECT_EQ(discovery.failedProbeCount(), 0U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Base);
        // 推到 PROBE_TIMER 之后，免得每个节拍被叫起来重问一次
        EXPECT_EQ(*discovery.nextDeadline(), kProbeTimer);
    }

    TEST(QuicPathMtuDiscovery, ConnectivityLossFallsBackToBase)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        ASSERT_EQ(discovery.probeByteLengthIfDue(QuicTime{0}, 100000U), 1232U);
        discovery.onProbeAcknowledged(QuicTime{0});
        ASSERT_EQ(discovery.maximumDatagramPayloadByteLength(), 1232U);
        discovery.onConnectivityLost(milliseconds(7));
        // RFC 8899 图 5 的「PL indicates loss of connectivity」在 QUIC 里对应持久拥塞：掉回 BASE 从头再探
        EXPECT_EQ(discovery.maximumDatagramPayloadByteLength(), 1200U);
        EXPECT_EQ(discovery.probedDatagramPayloadByteLength(), 1232U);
        EXPECT_EQ(discovery.failedProbeCount(), 0U);
        EXPECT_EQ(discovery.phase(), QuicPathMtuPhase::Base);
        EXPECT_EQ(*discovery.nextDeadline(), milliseconds(7));
    }

    TEST(QuicPathMtuDiscovery, RepeatedHandshakeConfirmationDoesNotPushTheDeadline)
    {
        QuicPathMtuDiscovery discovery = makeConfirmedAt(QuicTime{0});
        ASSERT_EQ(discovery.nextDeadline(), QuicTime{0});
        // 客户端有「解开第一条 1-RTT」与「收到 HANDSHAKE_DONE」两条路径，重复调用把计时器推回去
        // 会让第一条探针永远排不到点
        discovery.onHandshakeConfirmed(milliseconds(9000));
        EXPECT_EQ(*discovery.nextDeadline(), QuicTime{0});
    }
} // namespace AsynGyanis::Net
