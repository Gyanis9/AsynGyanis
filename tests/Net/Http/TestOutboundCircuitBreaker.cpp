// 出站熔断器单元测试：三态机的每一次转移、端点之间互不牵连、探测名额的占用与释放、有界跟踪。
// 时刻一律由用例注入，不用 sleep 等状态自己变——真实时钟下的「刚好赶上」不是一条判据。
#include "Net/Http/Client/OutboundCircuitBreaker.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        using Clock = OutboundCircuitBreaker::Clock;

        /// 所有用例共用的起点，之后的时刻都按它加出来
        const Clock::time_point kBaseTime{std::chrono::seconds{1'000'000}};

        /// 相对起点的时刻
        [[nodiscard]] Clock::time_point at(const std::chrono::milliseconds offset) noexcept
        {
            return kBaseTime + offset;
        }

        /// 一个可辨识的端点键
        [[nodiscard]] HttpOutboundEndpointKey endpointOf(std::string_view host, const std::uint16_t port = 443, const bool isTls = true)
        {
            return HttpOutboundEndpointKey{std::string(host), port, isTls};
        }

        /// 用例常用的配置：连续 3 次失败开闸，开闸 1 秒后半开
        [[nodiscard]] OutboundCircuitBreaker::Configuration sampleConfiguration()
        {
            OutboundCircuitBreaker::Configuration configuration;
            configuration.consecutiveFailureThreshold = 3;
            configuration.openDuration                = std::chrono::milliseconds{1000};
            return configuration;
        }

        /**
         * @brief 把某个端点推到开闸
         * @param breaker 目标熔断器
         * @param key 端点
         * @param failures 失败条数
         */
        void failNTimes(OutboundCircuitBreaker &breaker, const HttpOutboundEndpointKey &key, const std::size_t failures, const Clock::time_point now = kBaseTime)
        {
            for (std::size_t index = 0; index < failures; ++index)
            {
                breaker.reportFailure(key, now);
            }
        }
    } // namespace

    /**
     * @brief 连续失败到阈值即开闸，差一次都不开
     */
    TEST(OutboundCircuitBreakerTest, OpensExactlyAtTheConsecutiveFailureThreshold)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");

        failNTimes(breaker, key, 2);
        EXPECT_EQ(breaker.stateOf(key, kBaseTime), OutboundCircuitBreaker::State::Closed);
        EXPECT_TRUE(breaker.allowRequest(key, kBaseTime));

        breaker.reportFailure(key, kBaseTime);
        EXPECT_EQ(breaker.stateOf(key, kBaseTime), OutboundCircuitBreaker::State::Open);
        EXPECT_FALSE(breaker.allowRequest(key, kBaseTime)) << "开闸后还放行，等于这笔握手的钱照付";
        EXPECT_EQ(breaker.openEndpointCount(), 1U);
    }

    /**
     * @brief 一次成功就把连击清零：间歇性抖动的端点不该被累积判死
     */
    TEST(OutboundCircuitBreakerTest, SuccessResetsTheFailureStreak)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");

        failNTimes(breaker, key, 2);
        breaker.reportSuccess(key, kBaseTime);
        failNTimes(breaker, key, 2);

        EXPECT_EQ(breaker.stateOf(key, kBaseTime), OutboundCircuitBreaker::State::Closed) << "清零没生效：三次失败被记成了四次";
    }

    /**
     * @brief 到点转半开，且只放一条探测出去
     * @details 一次抖动恢复时如果放整串请求过去，等于把还没证明活着的上游再打倒一次
     */
    TEST(OutboundCircuitBreakerTest, HalfOpenAdmitsExactlyOneProbeAtATime)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");
        failNTimes(breaker, key, 3);

        EXPECT_FALSE(breaker.allowRequest(key, at(std::chrono::milliseconds{999}))) << "还没到点就放行";
        EXPECT_EQ(breaker.stateOf(key, at(std::chrono::milliseconds{1000})), OutboundCircuitBreaker::State::HalfOpen);

        EXPECT_TRUE(breaker.allowRequest(key, at(std::chrono::milliseconds{1000}))) << "到点后第一条探测必须能出去";
        EXPECT_FALSE(breaker.allowRequest(key, at(std::chrono::milliseconds{1000}))) << "探测名额没占住，第二条也跟着撞上去";
    }

    /**
     * @brief 探测成功即回闭，之后的请求全部照常放行
     */
    TEST(OutboundCircuitBreakerTest, ProbeSuccessClosesTheBreaker)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");
        failNTimes(breaker, key, 3);

        const Clock::time_point halfOpenAt = at(std::chrono::milliseconds{1000});
        ASSERT_TRUE(breaker.allowRequest(key, halfOpenAt));
        breaker.reportSuccess(key, halfOpenAt);

        EXPECT_EQ(breaker.stateOf(key, halfOpenAt), OutboundCircuitBreaker::State::Closed);
        EXPECT_TRUE(breaker.allowRequest(key, halfOpenAt));
        EXPECT_TRUE(breaker.allowRequest(key, halfOpenAt));
        EXPECT_EQ(breaker.openEndpointCount(), 0U);
    }

    /**
     * @brief 探测失败重新开闸并从这一刻重新计时
     */
    TEST(OutboundCircuitBreakerTest, ProbeFailureReopensAndRestartsTheWindow)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");
        failNTimes(breaker, key, 3);

        const Clock::time_point halfOpenAt = at(std::chrono::milliseconds{1000});
        ASSERT_TRUE(breaker.allowRequest(key, halfOpenAt));
        breaker.reportFailure(key, halfOpenAt);

        EXPECT_EQ(breaker.stateOf(key, halfOpenAt), OutboundCircuitBreaker::State::Open);
        EXPECT_FALSE(breaker.allowRequest(key, at(std::chrono::milliseconds{1999}))) << "重新计时没生效：旧窗口已经到点";
        EXPECT_TRUE(breaker.allowRequest(key, at(std::chrono::milliseconds{2000})));
    }

    /**
     * @brief 一个端点开闸不牵连别的端点，也不牵连同一主机的另一条通路
     */
    TEST(OutboundCircuitBreakerTest, BreakerStateIsPerEndpoint)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey broken        = endpointOf("broken.example.com");
        const HttpOutboundEndpointKey healthy       = endpointOf("healthy.example.com");
        const HttpOutboundEndpointKey plainSameHost = endpointOf("broken.example.com", 80, false);

        failNTimes(breaker, broken, 5);

        EXPECT_EQ(breaker.stateOf(broken, kBaseTime), OutboundCircuitBreaker::State::Open);
        EXPECT_TRUE(breaker.allowRequest(healthy, kBaseTime));
        EXPECT_TRUE(breaker.allowRequest(plainSameHost, kBaseTime)) << "端口与 TLS 是端点身份的一部分，明文通路不该被 TLS 通路的故障挡住";
        EXPECT_EQ(breaker.trackedEndpointCount(), 3U);
    }

    /**
     * @brief 跟踪的端点数有上限，超出即淘汰最久没更新的
     * @details 不设上限时，一个会不断换主机的调用方会把内存换成一张无限大的健康表
     */
    TEST(OutboundCircuitBreakerTest, TrackedEndpointsAreBounded)
    {
        OutboundCircuitBreaker::Configuration configuration = sampleConfiguration();
        configuration.maximumTrackedEndpoints               = 3;
        OutboundCircuitBreaker breaker(configuration);

        for (std::size_t index = 0; index < 5; ++index)
        {
            // 每条都用新的时刻，因此「最久没更新」就是最先插入的那条
            breaker.reportFailure(endpointOf("host" + std::to_string(index)), at(std::chrono::milliseconds{static_cast<long>(index) * 10}));
        }

        EXPECT_EQ(breaker.trackedEndpointCount(), 3U);
        EXPECT_EQ(breaker.stateOf(endpointOf("host0"), at(std::chrono::milliseconds{500})), OutboundCircuitBreaker::State::Closed) << "被淘汰的端点应当当作没见过";
        EXPECT_EQ(breaker.stateOf(endpointOf("host4"), at(std::chrono::milliseconds{500})), OutboundCircuitBreaker::State::Closed);
    }

    /**
     * @brief 阈值配成 0 不会让熔断器永不生效或立刻开闸
     * @details 「0」这种配错值要落在安全的一侧：按至少一次失败处理，而不是除零或恒开闸
     */
    TEST(OutboundCircuitBreakerTest, DegenerateConfigurationFailsSafe)
    {
        OutboundCircuitBreaker::Configuration configuration;
        configuration.consecutiveFailureThreshold = 0;
        configuration.halfOpenProbeLimit          = 0;
        configuration.halfOpenSuccessThreshold    = 0;
        configuration.maximumTrackedEndpoints     = 0;
        OutboundCircuitBreaker        breaker(configuration);
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");

        breaker.reportFailure(key, kBaseTime);
        EXPECT_EQ(breaker.stateOf(key, kBaseTime), OutboundCircuitBreaker::State::Open);
        EXPECT_TRUE(breaker.allowRequest(key, at(configuration.openDuration)));
        breaker.reportSuccess(key, at(configuration.openDuration));
        EXPECT_EQ(breaker.stateOf(key, at(configuration.openDuration)), OutboundCircuitBreaker::State::Closed);
    }

    /**
     * @brief reset() 抹掉全部账；状态名的文本形状供日志与 /metrics 用
     */
    TEST(OutboundCircuitBreakerTest, ResetClearsEveryEndpoint)
    {
        OutboundCircuitBreaker        breaker(sampleConfiguration());
        const HttpOutboundEndpointKey key = endpointOf("api.example.com");
        failNTimes(breaker, key, 3);
        ASSERT_EQ(breaker.trackedEndpointCount(), 1U);

        breaker.reset();
        EXPECT_EQ(breaker.trackedEndpointCount(), 0U);
        EXPECT_TRUE(breaker.allowRequest(key, kBaseTime));

        EXPECT_EQ(OutboundCircuitBreaker::stateName(OutboundCircuitBreaker::State::Closed), "closed");
        EXPECT_EQ(OutboundCircuitBreaker::stateName(OutboundCircuitBreaker::State::Open), "open");
        EXPECT_EQ(OutboundCircuitBreaker::stateName(OutboundCircuitBreaker::State::HalfOpen), "half-open");
    }
} // namespace AsynGyanis::Net
