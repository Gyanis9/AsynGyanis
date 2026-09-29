/**
 * @file OutboundCircuitBreaker.h
 * @brief 出站端点的熔断与半开探测：塌掉的上游不再被反复重试
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Http/Client/HttpOutboundConnectionPool.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 出站端点的熔断器
     *
     * @details 三态机：`Closed` 累计连续失败，达到阈值即开闸进 `Open`；`Open` 期间对新请求一律直接拒掉
     *          （不解析地址、不建连接、不握手），到点后转 `HalfOpen` 放**一条**探测请求过去；
     *          探测成功回 `Closed`，失败重回 `Open` 并重新计时。这是 Envoy outlier detection 与
     *          resilience4j 那一族的常见形状，用「连续失败」而不是「失败率」当判据：连续口径不需要
     *          维护采样窗口，也不会让一个刚刚上线、请求量还小的端点被一次失败按比例判成不健康。
     * @details 为什么需要它：连接池会在取连接时判健康并丢弃坏连接，但**下一次请求照样会去建一条新的**。
     *          上游整个塌掉时，这笔钱是每次请求都付一遍（DNS、TCP、TLS 握手），而调用方看到的只是
     *          「超时」——熔断器把这件事从「每请求付一次」变成「一个窗口内付一次」。
     * @note 成功与失败的口径由调用方告诉它：传输层失败与 5xx 算失败，4xx 不算——4xx 是使用方的请求
     *       有问题，把它记到上游头上会让一次错误的调用把所有人挡在门外。
     * @note 跟踪的端点数是**有界**的（`Configuration::maximumTrackedEndpoints`），超出时按最近一次
     *       更新时间淘汰最旧的。一个会不断换主机的调用方（例如按 URL 拉取任意资源）不该把内存
     *       换成一张无限大的健康表
     * @note 线程安全：所有方法都取内部锁，可以与同一个 `HttpClient` 的多条循环线程共用
     */
    class ASYN_NET_API OutboundCircuitBreaker
    {
    public:
        /// 熔断器的三态
        enum class State
        {
            Closed,   ///< 正常放行，累计连续失败
            Open,     ///< 开闸，新请求直接拒
            HalfOpen, ///< 半开，放一条探测请求
        };

        /**
         * @brief 熔断配置
         */
        struct Configuration
        {
            /// 连续失败多少条即开闸
            std::size_t consecutiveFailureThreshold{5};

            /// 开闸后多久转半开
            std::chrono::milliseconds openDuration{std::chrono::seconds{30}};

            /// 半开时同时在途的探测请求上限（1 就是「一条一条试」）
            std::size_t halfOpenProbeLimit{1};

            /// 半开时连续探测成功多少条即回闭
            std::size_t halfOpenSuccessThreshold{1};

            /// 最多跟踪多少个端点，超出即按最近更新时间淘汰
            std::size_t maximumTrackedEndpoints{256};
        };

        /// 稳态时钟读数，注入它是为了让判据能被确定性地理
        using Clock = std::chrono::steady_clock;

        /**
         * @brief 按默认配置建一个空熔断器
         * @details 与下面的带参构造分开写而不是给 `Configuration` 给缺省实参：GCC 不接受
         *          「嵌套类带非静态数据成员初始化器」当外层类成员的默认实参（本仓在两处踩过同一条），
         *          而 `Configuration{}` 正是这种写法
         */
        OutboundCircuitBreaker() noexcept;

        /**
         * @brief 按给定配置建一个空熔断器
         * @param configuration 阈值与时限
         */
        explicit OutboundCircuitBreaker(Configuration configuration) noexcept;

        /**
         * @brief 询问某端点现在能不能发请求
         * @details 状态到点会自动从 `Open` 滑到 `HalfOpen`；半开且还有探测名额时，本次询问会**占掉**
         *          一个名额（因此后续并发请求会被挡住，直到这次探测有了结果）。
         * @param endpointKey 端点键（主机、端口、是否 TLS）
         * @param now 当前时刻
         * @return true 放行
         */
        bool allowRequest(const HttpOutboundEndpointKey &endpointKey, Clock::time_point now = Clock::now()) noexcept;

        /**
         * @brief 报告一次成功
         * @param endpointKey 端点键
         * @param now 当前时刻
         */
        void reportSuccess(const HttpOutboundEndpointKey &endpointKey, Clock::time_point now = Clock::now()) noexcept;

        /**
         * @brief 报告一次失败（传输层失败或 5xx，口径见类说明）
         * @param endpointKey 端点键
         * @param now 当前时刻
         */
        void reportFailure(const HttpOutboundEndpointKey &endpointKey, Clock::time_point now = Clock::now()) noexcept;

        /**
         * @brief 查某端点当前处于哪一态（到点的 Open 会在这里滑成 HalfOpen）
         * @param endpointKey 端点键
         * @param now 当前时刻
         * @return State 状态；没跟踪过的端点按 Closed 报
         */
        [[nodiscard]] State stateOf(const HttpOutboundEndpointKey &endpointKey, Clock::time_point now = Clock::now()) noexcept;

        /// 当前跟踪的端点条数（运维入口与用例读它）
        [[nodiscard]] std::size_t trackedEndpointCount() const noexcept;

        /// 开闸中的端点数（运维入口与用例读它）
        [[nodiscard]] std::size_t openEndpointCount() const noexcept;

        /// 丢弃全部状态（配置换了或运维手动复位时用）
        void reset() noexcept;

        /**
         * @brief 把状态转成可读文本，用于日志与 /metrics
         * @param state 状态
         * @return std::string_view closed / open / half-open
         */
        [[nodiscard]] static std::string_view stateName(State state) noexcept;

    private:
        /// 一个端点的健康账
        struct EndpointHealth
        {
            std::size_t       consecutiveFailures{0}; ///< 连续失败条数（一次成功即清零）
            State             state{State::Closed};   ///< 当前状态
            Clock::time_point openedAt{};             ///< 本次开闸的时刻，用于算半开的到点
            std::size_t       inFlightProbes{0};      ///< 半开时已占掉名额的探测数
            std::size_t       halfOpenSuccesses{0};   ///< 半开以来累计的探测成功数
            Clock::time_point lastTouched{};          ///< 最近一次更新时间，用于有界淘汰
        };

        /// 取（并按需创建、按上限淘汰）某端点的账；调用方须持锁
        [[nodiscard]] EndpointHealth &healthFor(const HttpOutboundEndpointKey &endpointKey, Clock::time_point now);

        /// 把到点的 Open 滑成 HalfOpen；调用方须持锁
        void advanceLocked(EndpointHealth &health, Clock::time_point now) noexcept;

        Configuration                                     m_configuration; ///< 阈值与时限
        mutable std::mutex                                m_mutex;         ///< 保护 m_endpoints
        std::map<HttpOutboundEndpointKey, EndpointHealth> m_endpoints;     ///< 各端点的健康账
    };
} // namespace AsynGyanis::Net
