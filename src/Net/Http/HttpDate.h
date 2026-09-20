/**
 * @file HttpDate.h
 * @brief HTTP 日期（IMF-fixdate）的格式化与解析
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /// IMF-fixdate 的固定字节数："Sun, 06 Nov 1994 08:49:37 GMT" 恰为 29
    inline constexpr std::size_t kHttpDateTextLength = 29;

    /**
     * @brief 把时间点格式化为 HTTP 日期（IMF-fixdate）
     * @details 产出形如 "Sun, 06 Nov 1994 08:49:37 GMT" 的 ASCII 文本：固定 GMT、英文三字母
     *          星期与月份、两位日、四位年，符合 RFC 9110 §5.6.7。结果不受 locale 影响。
     * @param time 待格式化的时间点，按 UTC 解释
     * @return IMF-fixdate 文本
     */
    [[nodiscard]] std::string formatHttpDate(std::chrono::system_clock::time_point time);

    /**
     * @brief 取「此刻」的 HTTP 日期文本，按整秒缓存
     * @details Date 头每条响应都要写一份，而文本精度只到秒：同一秒内重复折算纯属浪费。缓存是
     *          thread_local 的，各事件循环线程自己刷新，因此不需要锁；时钟被往回调时秒数不相等，
     *          会照常重算而不会继续发未来的那一秒。
     * @warning 返回的视图指向本线程内部缓冲，下一次调用即失效——要跨调用持有必须立刻拷走
     * @return 定长 29 字节的 IMF-fixdate 文本
     */
    [[nodiscard]] std::string_view currentHttpDateText();

    /**
     * @brief 解析 HTTP 日期文本
     * @details 支持 RFC 9110 §5.6.7 的 IMF-fixdate；格式、星期名、月份名或字段取值不合法时
     *          返回空 optional，不抛异常——外部输入不能靠异常否定整个请求。
     * @param text 待解析文本，允许首尾空白
     * @return 解析出的时间点；无法解析时为空 optional
     * @note 星期名只校验是否为七个合法缩写之一，不要求它与日期字段自洽（RFC 允许收端忽略）
     */
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text);
} // namespace AsynGyanis::Net
