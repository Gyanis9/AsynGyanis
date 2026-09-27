/**
 * @file TracingTestSupport.h
 * @brief 链路测试共用的出口替身与记录构造小工具
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 出口替身要能「收下 / 整批拒收 / 第一批里停住」三种形态：前两种判计数口径，
 *          第三种用来把「缓冲上界」「flush 等的是在途批次」这类判据做成与调度无关的确定形状。
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Net/Tracing/SpanExporter.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    /**
     * @brief 记下收到的每一批节的出口替身
     * @note 线程安全：Tracer 的出口线程与调用 flush() 的线程都可能在它上面写，因此全部状态受锁保护
     */
    class CapturingSpanExporter final : public SpanExporter
    {
    public:
        /**
         * @brief 构造替身
         * @param isBlockingFirstBatch true 时第一批交付会停住：先报「已进场」，再等 release()。
         *        停住在出口内部而不是 Tracer 里，于是「在途一批」这个条件是用例自己造出来的
         */
        explicit CapturingSpanExporter(const bool isBlockingFirstBatch = false) : m_isBlockingFirstBatch(isBlockingFirstBatch)
        {
        }

        /**
         * @brief 收下（或拒收）这一批并记下来
         * @param resource 出处，记在 lastResourceName()/lastResourceVersion() 里供断言
         * @param spans 待收的节
         * @return true 收下（除被 configureRejecting() 改成拒收）
         * @return false 整批拒收
         */
        bool exportSpans(const TraceResource &resource, const std::vector<SpanRecord> &spans) override
        {
            if (m_isBlockingFirstBatch && !m_hasBlocked)
            {
                // 只卡第一批：后面的批次照常走，否则用例放行之后的每一次交付都还得手动开门。
                // 先报「已到出口」再等放行：调用方据此确认「这一批确实被取走了」，
                // 而不是赌调度时序猜它有没有被处理
                m_hasBlocked = true;
                m_enteredPromise.set_value();
                std::unique_lock lock(m_gateMutex);
                m_gateCondition.wait(lock, [this] { return m_isReleased; });
            }

            const std::lock_guard lock(m_stateMutex);
            ++m_batchCount;
            m_lastResourceName    = resource.serviceName;
            m_lastResourceVersion = resource.serviceVersion;
            if (!m_isAccepting)
            {
                return false;
            }
            m_records.insert(m_records.end(), spans.begin(), spans.end());
            return true;
        }

        /// @brief 出口名，固定为替身标识
        [[nodiscard]] std::string_view exporterName() const noexcept override
        {
            return "capturing-test-exporter";
        }

        /**
         * @brief 记下 shutdown 被叫过
         * @details Tracer 析构时最后一次交付之后应当叫到这里；用例据此断言收尾真的走通了。
         */
        void shutdown() noexcept override
        {
            const std::lock_guard lock(m_stateMutex);
            m_isShutdown = true;
        }

        /// @brief 等到第一批交付进场（替身已停在出口里）；true 表示确实等到了
        [[nodiscard]] bool waitUntilBatchEntered(const std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        {
            return m_enteredFuture.wait_for(std::chrono::duration_cast<std::chrono::microseconds>(timeout)) == std::future_status::ready;
        }

        /// @brief 放行停在出口里的那一批
        void release()
        {
            {
                const std::lock_guard lock(m_gateMutex);
                m_isReleased = true;
            }
            m_gateCondition.notify_all();
        }

        /// @brief 改成「整批拒收」，用来验证失败计数的口径
        void configureRejecting()
        {
            const std::lock_guard lock(m_stateMutex);
            m_isAccepting = false;
        }

        /// @brief 已记下的节的总数
        [[nodiscard]] std::size_t recordCount() const
        {
            const std::lock_guard lock(m_stateMutex);
            return m_records.size();
        }

        /// @brief 已记下的节，按交付顺序
        [[nodiscard]] std::vector<SpanRecord> records() const
        {
            const std::lock_guard lock(m_stateMutex);
            return m_records;
        }

        /// @brief 被交付过几批
        [[nodiscard]] std::size_t batchCount() const
        {
            const std::lock_guard lock(m_stateMutex);
            return m_batchCount;
        }

        /// @brief shutdown() 是否被叫过
        [[nodiscard]] bool isShutdown() const
        {
            const std::lock_guard lock(m_stateMutex);
            return m_isShutdown;
        }

        /// @brief 最后一次交付带来的出处（服务名）
        [[nodiscard]] std::string lastResourceName() const
        {
            const std::lock_guard lock(m_stateMutex);
            return m_lastResourceName;
        }

        /// @brief 最后一次交付带来的出处（版本）
        [[nodiscard]] std::string lastResourceVersion() const
        {
            const std::lock_guard lock(m_stateMutex);
            return m_lastResourceVersion;
        }

    private:
        bool                     m_isBlockingFirstBatch{false};                  ///< 是否要在第一批交付里停住
        bool                     m_hasBlocked{false};                            ///< 已经卡过第一批：只卡一次，放行之后的批次照常走
        std::promise<void>       m_enteredPromise;                               ///< 「已到出口」的报信通道
        std::shared_future<void> m_enteredFuture{m_enteredPromise.get_future()}; ///< 与之配对的等待端
        std::mutex               m_gateMutex;                                    ///< 护住放行标记
        std::condition_variable  m_gateCondition;                                ///< 放行等待
        bool                     m_isReleased{false};                            ///< 是否已放行

        mutable std::mutex      m_stateMutex;            ///< 护住下面全部记录与开关
        std::vector<SpanRecord> m_records{};             ///< 记下的节
        std::size_t             m_batchCount{0};         ///< 交付批数
        bool                    m_isAccepting{true};     ///< 是否收下（false 即整批拒收）
        bool                    m_isShutdown{false};     ///< shutdown() 是否被叫过
        std::string             m_lastResourceName{};    ///< 最后一次的出处名
        std::string             m_lastResourceVersion{}; ///< 最后一次的出处版本
    };

    /**
     * @brief 取一个 JSON 字段的文本
     * @details 断言一律先转成 std::string 再比：nlohmann 的 operator== 与 std::string_view
     *          之间有一条歧义转换，直接拿 json 和文本常量比会让用例编不过。
     */
    inline std::string jsonTextOf(const AsynGyanis::Base::ConfigValue &value)
    {
        return value.get<std::string>();
    }

    /// @brief 取一个 JSON 字段的整数
    inline std::int64_t jsonNumberOf(const AsynGyanis::Base::ConfigValue &value)
    {
        return value.get<std::int64_t>();
    }

    /**
     * @brief 造一条内容可控的成品节，供渲染类用例直接断言正文
     * @param name 操作名
     * @param traceIdText 链路标识文本（32 位小写十六进制）
     * @param spanIdText 本节标识文本（16 位小写十六进制）
     * @param parentSpanIdText 上一节标识文本；空串表示根节
     * @return SpanRecord 起点固定在 2023-11-14T22:13:20.1234567Z、时长 2.5 秒，维度由用例按需补。
     *         这个取值在两个平台的墙钟刻度（1 ns 与 100 ns）下都能精确表示，于是渲染结果可以逐位断言
     */
    inline SpanRecord makeRecord(std::string_view name, std::string_view traceIdText, std::string_view spanIdText, std::string_view parentSpanIdText = {})
    {
        SpanRecord record;
        // std::array 没有 assign，按 ranges::copy 写进定长缓冲再把紧随其后的那一格放 NUL
        static_cast<void>(std::ranges::copy(traceIdText, record.identity.traceId.begin()));
        record.identity.traceId[traceIdText.size()] = '\0';
        static_cast<void>(std::ranges::copy(spanIdText, record.identity.spanId.begin()));
        record.identity.spanId[spanIdText.size()] = '\0';
        static_cast<void>(std::ranges::copy(parentSpanIdText, record.identity.parentSpanId.begin()));
        record.identity.parentSpanId[parentSpanIdText.size()] = '\0';

        record.name = std::string(name);
        record.startMoment =
                std::chrono::system_clock::time_point{} + std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{1700000000123456700LL});
        record.duration = std::chrono::nanoseconds{2500000000LL};
        return record;
    }
} // namespace AsynGyanis::Net::TestSupport
