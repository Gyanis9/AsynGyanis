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

#include "Database/Redis/RedisConnection.h"

#include <cstdint>
#include <utility>

#ifdef DATABASE_HAS_REDIS
#include "Platform/Platform.h"
#endif

namespace AsynGyanis::Database
{
    namespace Detail
    {
        // makeTimeoutParts() 在 RedisConnection.cpp 里给出线定义：公开头只放声明，Release 下
        // 编译器不会为一个未被本翻译单元引用的 inline 函数留下弱符号，测试因此会在链接期找不到它

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
