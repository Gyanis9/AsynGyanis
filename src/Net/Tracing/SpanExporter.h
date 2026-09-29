/**
 * @file SpanExporter.h
 * @brief 链路出口的接口：收下一批已收口的节，以及这批节的出处标识
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Tracing/Span.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 一批节的出处：由 Tracer 的配置给出，出口侧拿来填 resource
     * @details 刻意由 Tracer 交出来而不是让每个出口自己配一份：出口与编排器的服务名对不上时，
     *          报上去的链路会指向一个不存在的进程，而这在任何检索侧都看不出来。
     */
    struct ASYN_NET_API TraceResource
    {
        std::string serviceName{};    ///< 服务名（OTLP 的 service.name），非空
        std::string serviceVersion{}; ///< 版本，空串表示未配置
    };

    /**
     * @brief 链路出口
     *
     * @details 出口只做一件事：把 Tracer 交出的一批节送出去（落文件、发 HTTP、进队列…）。
     *          调用线程是 Tracer 自己的出口线程，且一次只交一批：flush() 等的是这批落地，它本身不交付。
     *          实现因此不必再自带串行化，但也不能假设调用者是事件循环线程——一次文件写入或一次
     *          网络请求正是发生在这里。
     * @note 一批的处置只有「收下」与「没收下」两种，没有第三种「部分收下」：部分收下意味着出口
     *       自己知道丢了哪几条，而计数在 Tracer 这一侧，拿不到那份明细就只能整批算丢。
     *       失败的批次不重试——「什么时候再试」是出口自己的状态，编排器这边只把失败计一次数。
     */
    class ASYN_NET_API SpanExporter
    {
    public:
        SpanExporter()                                = default;
        virtual ~SpanExporter()                       = default;
        SpanExporter(const SpanExporter &)            = delete;
        SpanExporter &operator=(const SpanExporter &) = delete;
        SpanExporter(SpanExporter &&)                 = delete;
        SpanExporter &operator=(SpanExporter &&)      = delete;

        /**
         * @brief 送出一批节
         * @param resource 这批节的出处（服务名与版本）
         * @param spans 已收口的节，按收口顺序
         * @return true 整批已被收下（写成功或明确投递给了下游）
         * @return false 整批没能收下：Tracer 把这批计入丢弃数并继续跑，不重试也不回头
         */
        virtual bool exportSpans(const TraceResource &resource, const std::vector<SpanRecord> &spans) = 0;

        /// @brief 出口名：出现在计数与告警文本里，给运维认「是哪一路出口在丢数据」
        [[nodiscard]] virtual std::string_view exporterName() const noexcept = 0;

        /**
         * @brief 收尾：Tracer 停止时最后一次 flush 之后调用一次
         * @details 默认什么都不做。要在此关文件、断长连接的出口重写它；实现必须不抛异常
         *          （Tracer 的析构路径上抛出会直接终止进程）。
         */
        virtual void shutdown() noexcept
        {
        }
    };
} // namespace AsynGyanis::Net
