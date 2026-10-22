/**
 * @file OtlpJsonSupport.h
 * @brief OTLP 三个信号共用的 JSON 映射助手（protojson 形状）
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Config/ConfigValue.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net::Telemetry
{
    /**
     * @brief 一段文本折成 OTLP 的 AnyValue（stringValue 那一支）
     * @param text 原始文本
     * @return Base::ConfigValue 形如 {"stringValue":"…"}
     */
    [[nodiscard]] inline Base::ConfigValue textValueOf(const std::string_view text)
    {
        Base::ConfigValue value = Base::ConfigValue::object();
        value["stringValue"]    = text;
        return value;
    }

    /**
     * @brief 一个键值对折成 OTLP 的 KeyValue
     * @param key 维度名
     * @param value 已折成 AnyValue 的取值
     * @return Base::ConfigValue 形如 {"key":"…","value":{…}}
     */
    [[nodiscard]] inline Base::ConfigValue keyValueOf(const std::string_view key, Base::ConfigValue value)
    {
        Base::ConfigValue entry = Base::ConfigValue::object();
        entry["key"]            = key;
        entry["value"]          = std::move(value);
        return entry;
    }

    /**
     * @brief 墙钟时刻折成自 Unix 纪元起的纳秒文本
     * @details 用 floor 而不是 duration_cast：后者对 time_point::min 这类极端值会溢出成负数。
     * @param moment 墙钟时刻
     * @return std::string 十进制纳秒文本（protojson 的 64 位整数一律是字符串）
     */
    [[nodiscard]] inline std::string unixNanoTextOf(const std::chrono::system_clock::time_point &moment)
    {
        return std::to_string(std::chrono::floor<std::chrono::nanoseconds>(moment.time_since_epoch()).count());
    }
} // namespace AsynGyanis::Net::Telemetry
