/**
 * @file OtlpTraceJson.h
 * @brief 把一批链路节渲染成 OTLP/HTTP 的 JSON 正文
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Tracing/SpanExporter.h"

#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /// 出口正文里 instrumentationScope 的名字：报「是哪一套设施写的」，与 service.name 分开
    inline constexpr std::string_view kTracingScopeName = "AsynGyanis";

    /**
     * @brief 把一批节渲染成 OTLP/HTTP 的请求正文（`POST /v1/traces`，Content-Type: application/json）
     * @details 走 protobuf 的 JSON mapping 而不是 protobuf 本身：本引擎不引入 protobuf 依赖，
     *          而 Collector 系（Jaeger、Grafana Alloy、Tempo）都认这一份编码。64 位时间一律写成**字符串**，
     *          与各语言 OTel SDK 的输出一致——数字形态虽然也在解码器的接受范围内，但不是各家默认发的形状。
     * @param resource 这批节的出处（服务名与版本）
     * @param spans 已收口的节，按收口顺序；空批也渲染出结构完整的正文
     * @return std::string 可直接作为请求正文的 JSON；空串表示有一个取值进不了 JSON（正文含非法 UTF-8）
     * @note 返回空串时整批都发不出去，不只是那一条：出口侧要按「整批没收」计一次失败。
     *       一份 OTLP 文档只有一个正文，坏一条就换整批是这套编码的既有形状，不去偷偷修那条坏数据
     * @see OtlpHttpSpanExporter
     */
    [[nodiscard]] ASYN_NET_API std::string formatOtlpTracesJson(const TraceResource &resource, const std::vector<SpanRecord> &spans);
} // namespace AsynGyanis::Net
