/**
 * @file TimestampText.h
 * @brief 日志时间戳的文本渲染工具（历法换算按秒缓存在调用线程）
 * @author Gyanis
 * @date 2026-09-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/System/PlatformTime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <string_view>

namespace AsynGyanis::Base
{
    /// 常规渲染结果的字符数："YYYY-MM-DD HH:MM:SS.mmm"（年份四位数时的长度）
    inline constexpr std::size_t kTimestampTextLength = 23U;

    /// 历法前缀的存储上限："YYYY-MM-DD HH:MM:SS"。`tm_year` 是 int，年份最多 10 位、前缀最长 25，
    /// 取 32 留足余量，于是「前缀 + 毫秒」必然写得下，写入路径不必判界
    inline constexpr std::size_t kTimestampPrefixCapacity = 32U;

    /// 渲染缓冲的字节数：前缀上限再加尾部 ".mmm" 四位，与上面那条上限成套
    inline constexpr std::size_t kTimestampTextBufferSize = kTimestampPrefixCapacity + 4U;

    namespace detail
    {
        /**
         * @brief 历法换算的按秒缓存，挂在调用线程上
         * @details 存「到秒为止的前缀」而不是整条文本：毫秒每刻都要重写，前缀一秒内不变。
         *          前缀长度随年份位数而变，故连长度一起存，不给年份位数设上限。
         */
        struct TimestampPrefixCache
        {
            std::int64_t                               cachedSecondValue = 0; ///< 已折算成文本的那个整秒（epoch 起算）
            std::array<char, kTimestampPrefixCapacity> prefixText{};          ///< 「YYYY-MM-DD HH:MM:SS」形态的前缀
            std::size_t                                prefixLength = 0;      ///< 前缀的有效字节数
            bool                                       hasValue     = false;  ///< 本线程是否已经折算过至少一条
        };
    } // namespace detail

    /**
     * @brief 把一个时刻渲染成 `YYYY-MM-DD HH:MM:SS.mmm`（本地时间）
     * @details 历法换算按整秒缓存在调用线程：一秒内的连续渲染只折一次日历，其余只覆写尾部四位。
     *          整秒以下向下取整拆分，预 1970 的时刻因此得到「秒 + 非负毫秒」而不是负毫秒。
     * @param buffer 写入目标；返回的视图指向它，同一条语句里渲染两次会互相覆写，需各备一块
     * @param moment 要渲染的时刻
     * @return std::string_view 常规长度 kTimestampTextLength 个字符，毫秒固定三位（不足补零）
     * @note 不落堆也不抛异常：换算失败的极端时刻由 PlatformTime::localTime() 交回零值日历，
     *       渲染出的是那个零值而不是上一个缓存前缀
     */
    inline std::string_view formatTimestampText(std::array<char, kTimestampTextBufferSize> &buffer, const std::chrono::system_clock::time_point moment) noexcept
    {
        // 向下取整到整秒：朝零截断会让预 1970 的时刻偏一整秒，且下面的毫秒残差成了负数
        const auto         wholeSeconds = std::chrono::floor<std::chrono::seconds>(moment);
        const std::int64_t secondValue  = wholeSeconds.time_since_epoch().count();

        // 残差按「毫秒刻度」取，不用 moment - wholeSeconds：两个 time_point 相减要先落到彼此更细的
        // 公共单位（本平台是纳秒或 100 纳秒），那次整秒→细单位的乘法在 time_point::min()/max() 上
        // 直接溢出（UBSan 实测报在 chrono.h 的 __duration_cast_impl）。换算到毫秒是除法，极端值不会
        // 溢出。要的恰是 floor 而不是 duration_cast：后者朝零截断，落在 (-1ms, 0) 这类「不足一毫秒的
        // 负时刻」上会把这段量当成 0，残差于是等于 1000，逐位写的三位毫秒溢出成 ".:00" 这种不合版式的文本
        constexpr std::int64_t kMillisecondsPerSecond = 1'000LL;
        const std::int64_t     millisecondCount       = std::chrono::floor<std::chrono::milliseconds>(moment.time_since_epoch()).count();
        const std::int64_t     millisecondValue       = millisecondCount - secondValue * kMillisecondsPerSecond;

        thread_local detail::TimestampPrefixCache prefixCache;

        // 一秒内的事件共用同一份日历换算；跨秒（含时钟回拨）才重折一次
        if (!prefixCache.hasValue || prefixCache.cachedSecondValue != secondValue)
        {
            const std::tm localTime = Platform::PlatformTime::localTime(static_cast<std::time_t>(secondValue));

            const auto written       = std::format_to_n(prefixCache.prefixText.begin(), prefixCache.prefixText.size(), "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                                                        localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
            prefixCache.prefixLength = static_cast<std::size_t>(written.out - prefixCache.prefixText.begin());
            prefixCache.cachedSecondValue = secondValue;
            prefixCache.hasValue          = true;
        }

        // 前缀整块搬进目标缓冲；毫秒三位定长，逐位写比再走一次格式化器省掉整轮参数打包。
        // 这里走 data() 而不是迭代器：Debug 的检出式迭代器不是裸指针，尾部要按裸下标写
        std::copy_n(prefixCache.prefixText.begin(), prefixCache.prefixLength, buffer.begin());

        char *const millisecondFieldBegin = buffer.data() + prefixCache.prefixLength;
        millisecondFieldBegin[0]          = '.';
        millisecondFieldBegin[1]          = static_cast<char>('0' + millisecondValue / 100);
        millisecondFieldBegin[2]          = static_cast<char>('0' + millisecondValue % 100 / 10);
        millisecondFieldBegin[3]          = static_cast<char>('0' + millisecondValue % 10);

        return {buffer.data(), prefixCache.prefixLength + 4U};
    }
} // namespace AsynGyanis::Base
