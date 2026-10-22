/**
 * @file RedisConnectionSupport.h
 * @brief Redis 驱动各翻译单元共用的时长换算
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 「毫秒 → 秒 + 微秒」的换算与钳位在连接、命令超时与推送读取三处共用；
 *          常量与折法只留这一份，避免各翻译单元对越界取值的处理出现分歧。
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#ifdef DATABASE_HAS_REDIS
#include "Platform/Platform.h"
#endif

namespace AsynGyanis::Database
{
    inline constexpr std::int64_t kMillisecondsPerSecond      = 1000; ///< 1 秒等于 1000 毫秒
    inline constexpr std::int64_t kMicrosecondsPerMillisecond = 1000; ///< 1 毫秒等于 1000 微秒

    namespace Detail
    {
        /**
         * @brief 把毫秒拆成 hiredis 需要的「秒 + 微秒」
         * @details 越界按「这套结构能表达的最长」钳住，而不是折回 0——0 在 hiredis 的约定里是「不设超时」，
         *          那正是调用方要长等的反面。
         * @param milliseconds 时长毫秒数
         * @return std::pair<std::int64_t, std::int64_t> 秒与微秒
         */
        inline std::pair<std::int64_t, std::int64_t> makeTimeoutParts(const std::int64_t milliseconds) noexcept
        {
            const std::int64_t clampedMilliseconds = std::clamp(milliseconds, static_cast<std::int64_t>(0), static_cast<std::int64_t>(std::numeric_limits<int>::max()));
            return {clampedMilliseconds / kMillisecondsPerSecond, (clampedMilliseconds % kMillisecondsPerSecond) * kMicrosecondsPerMillisecond};
        }

#ifdef DATABASE_HAS_REDIS
        /**
         * @brief 把毫秒换算成 hiredis 需要的 struct timeval
         * @param milliseconds 时长毫秒数，越界值由 makeTimeoutParts 钳位
         * @return struct timeval 换算结果
         */
        inline struct timeval makeTimeval(const std::int64_t milliseconds)
        {
            // tv_sec / tv_usec 的具体类型两个平台不一样（Windows 是 LONG，POSIX 是 time_t 与 suseconds_t），
            // 拿到的已是非负整数，这里只做一次落地窄化
            const auto [secondsPart, microsecondPart] = makeTimeoutParts(milliseconds);

            struct timeval timeoutValue;
            timeoutValue.tv_sec  = static_cast<decltype(timeoutValue.tv_sec)>(secondsPart);
            timeoutValue.tv_usec = static_cast<decltype(timeoutValue.tv_usec)>(microsecondPart);
            return timeoutValue;
        }
#endif
    } // namespace Detail
} // namespace AsynGyanis::Database
