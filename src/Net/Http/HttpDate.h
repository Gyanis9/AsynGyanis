/**
 * @file HttpDate.h
 * @brief HTTP 日期（IMF-fixdate）的格式化与解析
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
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
    [[nodiscard]] ASYN_NET_API std::string formatHttpDate(std::chrono::system_clock::time_point time);

    /**
     * @brief 把时间点写成 HTTP 日期，产物落在调用方给的定长缓冲里
     * @details 与返回 std::string 的那条共用同一份拼装，区别只在不碰堆：29 字节超出小串内联缓冲，
     *          按值交出文本就是每次静态文件请求一份白付的分配，而调用方只需要一个活到 setHeader
     *          把内容拷走之前的视图。长度写在类型里（std::span 的定长切片），不存在「缓冲给小了
     *          静默截断」这种形状。
     * @param time 待格式化的时间点，按 UTC 解释
     * @param buffer 恰好 29 字节的输出缓冲
     * @return 指向 buffer 的定长视图，生命周期由调用方的缓冲决定
     */
    [[nodiscard]] ASYN_NET_API std::string_view formatHttpDate(std::chrono::system_clock::time_point time, std::span<char, kHttpDateTextLength> buffer) noexcept;

    /**
     * @brief 取「此刻」的 HTTP 日期文本，按整秒缓存
     * @details Date 头每条响应都要写一份，而文本精度只到秒：同一秒内重复折算纯属浪费。缓存是
     *          thread_local 的，各事件循环线程自己刷新，因此不需要锁；时钟被往回调时秒数不相等，
     *          会照常重算而不会继续发未来的那一秒。
     * @warning 返回的视图指向本线程内部缓冲，下一次调用即失效——要跨调用持有必须立刻拷走
     * @return 定长 29 字节的 IMF-fixdate 文本
     */
    [[nodiscard]] ASYN_NET_API std::string_view currentHttpDateText();

    /**
     * @brief 解析 HTTP 日期文本
     * @details 认 RFC 9110 §5.6.7 的三种格式：首选的 IMF-fixdate，以及该节要求收端保持兼容的两种
     *          过时格式——RFC 850（`Sunday, 06-Nov-94 08:49:37 GMT`，两位年份按「0..69 记 2000 年代、
     *          70..99 记 1900 年代」折叠，不按「离现在最近的世纪」解释，否则同一个头在不同时刻会解析
     *          出不同结果，而缓存验证器要的是可复现的判据）与 asctime（`Sun Nov  6 08:49:37 1994`，
     *          日右对齐补空格、格式里没有时区段故按 GMT 处理）。
     * @details 格式、名称或字段取值不合法时返回空 optional，不抛异常——外部输入不能靠异常否定整个请求。
     * @param text 待解析文本，允许首尾空白
     * @return 解析出的时间点；无法解析时为空 optional
     * @note 星期名只校验是否为合法名称之一，不要求它与日期字段自洽（RFC 允许收端忽略）；
     *       RFC 850 的星期全称与三字母缩写都收
     * @note 名称逐字区分大小写：HTTP 日期的 ABNF 已把大小写固定下来，放宽只会放过真正畸形的头
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text);

    /**
     * @brief 解析 `Retry-After` 的取值，统一折成「还要等多久」
     *
     * @details RFC 9110 §10.2.3 允许这个头写成两种形状：秒数的相对延迟（`Retry-After: 3600`），
     *          或一个绝对的 HTTP-date（`Retry-After: Wed, 21 Oct 2026 07:28:00 GMT`）。本框架的
     *          服务端在 429 与 503 上发的是前者（限流中间件按窗口算、在途预算按一秒算），而机构的
     *          应答两种都可能出现——只认一种就等于在对端说实话时装作没听见。
     * @details 绝对时刻按「与 `now` 的差」交回，已经过去的折成 0 秒（= 立刻可重试），不做「负数」
     *          这种第三种状态：调用方要的是一个可用于定时器的正数。
     * @details 解析不出来时返回空 optional 而不是 0：**0 的含义是「现在就再试一次」**，把一条读不懂
     *          的头折成 0 等于在限流现场把对端的警告丢掉并加速撞上去。
     * @param text 头部原文，允许首尾空白
     * @param now 换算绝对日期时的参照时刻（由调用方给，便于直测）
     * @return std::optional<std::chrono::seconds> 要等待的秒数；无法解析时为空
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::chrono::seconds> parseRetryAfter(std::string_view text, std::chrono::system_clock::time_point now);
} // namespace AsynGyanis::Net
