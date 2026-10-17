/**
 * @file OtlpLogJson.h
 * @brief 把一批日志记录渲染成 OTLP/HTTP 的 JSON 正文
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Log/LogLevel.h"
#include "Net/Tracing/SpanExporter.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /// OTLP 日志正文里 instrumentationScope 的名字
    inline constexpr std::string_view kLoggingScopeName = "AsynGyanis.Logger";

    /**
     * @brief 一条待导出的日志记录（上下文已在产生线程上摘下来）
     * @details 不能在导出线程上再读当前上下文：那条线程没有请求作用域，读到的只会是空。
     *          因此 Sink::write() 在调用线程上把上下文快照放进本结构，导出线程只做渲染与发送。
     */
    struct ASYN_NET_API OtlpLogRecord
    {
        Base::LogLevel                                   level{Base::LogLevel::Info}; ///< 日志等级
        std::chrono::system_clock::time_point            timestamp{};                 ///< 事件时刻
        std::string                                      message{};                   ///< 正文
        std::string                                      loggerName{};                ///< 日志器名（空表示根）
        std::string                                      threadId{};                  ///< 线程号文本
        std::vector<std::pair<std::string, std::string>> attributes{};                ///< 上下文键值（trace_id/span_id/request_id 等）
    };

    /**
     * @brief 把一批记录渲染成 `POST /v1/logs` 的 JSON 正文
     * @details 形状按 OTLP proto3 的 JSON mapping：resourceLogs → scopeLogs → logRecords；
     *          64 位时间写成字符串（与 span/metrics 两条出口同一口径）。severityText 给文本、
     *          severityNumber 给 OTLP 的数值档（TRACE=1/DEBUG=5/INFO=9/WARN=13/ERROR=17/FATAL=21）。
     * @param resource 这批日志的出处（服务名与版本）
     * @param records 记录，按产生顺序
     * @return std::string 请求正文；空串表示有一条记录进不了 JSON（非法 UTF-8）
     */
    [[nodiscard]] ASYN_NET_API std::string formatOtlpLogsJson(const TraceResource &resource, const std::vector<OtlpLogRecord> &records);
} // namespace AsynGyanis::Net
