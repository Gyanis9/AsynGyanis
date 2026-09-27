/**
 * @file HttpTracingMiddleware.h
 * @brief 给每条入站请求开一节 HTTP 服务端链路的中间件
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Http/Middleware.h"
#include "Net/Tracing/Tracer.h"

#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 创建「一条请求一节」的服务端链路中间件
     *
     * @details 放在 traceContextMiddleware **之后**：它读的是请求上那条 traceparent，而归一化
     *          （缺则生成、畸形则重开）正是那一步做的。顺序反了也不会出错，只是这一节会跟着
     *          上游的原始写法走。
     * @details 它做三件事：按上游的采样位决定记不记；把 traceparent 的段标识改成本节的标识
     *          （于是处理器与出站客户端从头部读到的上下文就是它们的直接上级）；在下游跑完之后
     *          把状态码、路径与方法写成维度，并按 5xx 判 Error。
     *
     * @note 改写头部这一步只在 traceparent 是版本 0 时做：W3C §3.5 要求看不懂的更高版本原样转发，
     *       而改段标识就等于按自己理解的格式重排别人的字段。此时下游那一跳仍以本端为上级这件事
     *       在头部里体现不出来——是规范的取舍，不是这里的遗漏。
     * @note 响应上不写任何链路头部：traceparent/tracestate 是请求侧的传播字段，规范没有定义响应形态
     *       （与 traceContextMiddleware 同一口径）。
     *
     * @param tracer 链路编排器；nullptr 是配置错误，当场抛出而不是静默不记
     * @return MiddlewareFunc 可直接 addMiddleware 的中间件
     * @throws Base::LogicException tracer 为空
     * @see traceContextMiddleware(), Tracer::startSpan()
     */
    [[nodiscard]] MiddlewareFunc tracingSpanMiddleware(std::shared_ptr<Tracer> tracer);
} // namespace AsynGyanis::Net
