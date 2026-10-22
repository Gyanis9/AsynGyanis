/**
 * @file HttpConfigDuration.h
 * @brief 配置里的毫秒数转时长的共用校验（HTTP 服务与 OTLP 出口共用）
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Exception/ConfigValidationException.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace AsynGyanis::Net::HttpConfig
{
    /**
     * @brief 把配置里的毫秒数转成时长，并挡住「与当前时刻相加会折回过去」的上界
     * @details 时长的底层是有符号 64 位，但那只管「装得下」：这些值最终都要与 steady_clock
     *          的当前时刻相加。按 rep 的上限放行时，越过一半上界的取值在相加那一刻折回成过去——
     *          「配得越大」于是变成「立刻到期」。因此上界取可表示范围的一半，另一半留给 now()。
     * @param milliseconds 毫秒数（调用方已确保适合本字段）
     * @param key 键路径，用于错误信息
     * @return std::chrono::milliseconds 时长
     * @throws Base::ConfigValidationException 超出可用上限
     */
    [[nodiscard]] inline std::chrono::milliseconds toMilliseconds(const std::uint64_t milliseconds, const std::string &key)
    {
        constexpr std::int64_t kMaximumUsableTimeoutMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count() / 2;
        if (milliseconds > static_cast<std::uint64_t>(kMaximumUsableTimeoutMilliseconds))
        {
            throw Base::ConfigValidationException(
                    key, std::format("超出可用的毫秒上限 {}：时限要与当前时刻相加，越过上界会折回成过去，等于「配得越大反倒立刻到期」", kMaximumUsableTimeoutMilliseconds));
        }
        return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(milliseconds));
    }
} // namespace AsynGyanis::Net::HttpConfig
