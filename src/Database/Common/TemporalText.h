/**
 * @file TemporalText.h
 * @brief 日期时间文本的解析与渲染（驱动内部共用，不对外暴露）
 * @author Gyanis
 * @date 2026-10-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 三家引擎在「文本形态的日期时间」上给的不是同一种写法，而本库的值类型方向坚持一件事：
 *          线路是文本，成员是类型，中间这一层只做认与不认，不做任何猜测性换算。这份头就是那一层。
 *
 *          读回方向接受的写法（各家实际给出的都在里面）：`YYYY-MM-DD` 加 `T` 或空格作分隔，
 *          `HH:MM` 之后 `:SS` 可省（MySQL 的 DATE 列没有时分秒）；小数秒 1~9 位，多于 6 位按
 *          微秒截断（截掉的是引擎给的超出本库承载精度的位数，不是数据）；尾部的偏移认三种：
 *          `Z`、`+HH:MM`/`-HH:MM`、`+HHMM`/`-HHMM`（PostgreSQL 两种都给）。认不出的一律交回空值
 *          由调用方报错，不猜：MySQL 的零值日期 `0000-00-00 00:00:00`（月份 0 不是合法历法）、
 *          PostgreSQL 在非法国 DateStyle 下给出的 `Thu 10-18-2026 09:20:30 PDT+0530`、
 *          以及 `infinity`/-infinity 这类没有历法对应的哨兵。
 *
 *          渲染方向按引擎给三种文本，因为「同一条 SQL 文本」在两家意味着不同的事。MySQL 的 DATETIME
 *          是字面墙上时间，不做会话时区换算，所以交出的文本不带偏移，这也正是本映射不选 TIMESTAMP
 *          列的原因（见 MySqlDialect::columnTypeName 的 Timestamp 分支）。PostgreSQL 的 timestamptz
 *          会把不带偏移的文本按会话 TimeZone 解释，所以 UTC 瞬时那一路必须自带 `+00`，否则服务端
 *          会替我们换一个 instant；墙上时间那一路相反，必须不带偏移（TIMESTAMP WITHOUT TIME ZONE
 *          收字面值）。SQLite 只有存储类，文本形态完全由本库定，因此用定宽 ISO-8601：
 *          `YYYY-MM-DDTHH:MM:SS.ffffff`，UTC 那一路补一个 `Z`。定宽的意义不只是好看——SQLite 的
 *          TEXT 亲和下比较按字典序，定宽的 ISO 文本字典序与时间序一致，这一点与十进制列不同
 *          （十进制的文本字典序不等于数值序，见 SqliteDialect 的说明）。
 *
 * @note 微秒是本库的承载精度（与三家的最高精度对齐：MySQL DATETIME(6)、PostgreSQL 的 6 位小数秒、
 *       SQLite 由我们自己写 6 位）。年份范围按 `std::chrono::year` 的 int16 全幅，不额外收窄；
 *       引擎自己装不下的年份会在写入时被服务端拒，那份原因原样交回调用方。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseValue.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Database::Detail
{
    /**
     * @brief 一段日期时间文本的解析结果
     *
     * @details 只有历法字段与（可能没有的）偏移，不含任何「这是瞬时还是墙上时间」的判断——
     *          那个判断属于列型与成员型，属于调用方。偏移用分钟表示，覆盖 ±23:59 的全部合法值。
     */
    struct ParsedDateTime
    {
        std::int64_t       year;          ///< 历年份（可为负）
        std::int32_t       month;         ///< 月，1..12
        std::int32_t       day;           ///< 日，1..31（是否合法由 year_month_day::ok() 判）
        std::int32_t       hour;          ///< 时，0..23
        std::int32_t       minute;        ///< 分，0..59
        std::int32_t       second;        ///< 秒，0..60（60 是闰秒，MySQL 会给出）
        std::int32_t       microsecond;   ///< 微秒，0..999999
        std::optional<int> offsetMinutes; ///< 文本里带的偏移；没带就是空值，而不是「等于零」
    };

    /**
     * @brief 解析一段日期时间文本
     * @param text 列值文本，按字节看，不做字符集解释
     * @return std::optional<ParsedDateTime> 认不出来时空值（调用方负责点名是哪一列）
     */
    [[nodiscard]] ASYN_DATABASE_API std::optional<ParsedDateTime> parseTemporalText(std::string_view text) noexcept;

    /**
     * @brief 把 UTC 瞬时渲染成文本
     * @param instant 微秒精度的系统时钟瞬时
     * @param separator 日期与时间之间的分隔符（SQLite/Redis 用 'T'，MySQL 用空格）
     * @param withUtcMarker 是否在结尾加 `Z`（SQLite/Redis 那两档要，MySQL/PostgreSQL 不要）
     * @return std::string 形如 `2026-10-18T09:20:30.000000Z`
     * @note PostgreSQL 那一路用 renderSystemClockWithZeroOffset，因为那里需要的是 `+00` 而不是 `Z`
     *       （服务端认 `+00`；`Z` 在 timestamptz 的输入里同样合法，但两条文本分开写，
     *       日后哪一家要改形态不会牵连另一家）。
     */
    [[nodiscard]] ASYN_DATABASE_API std::string renderSystemClock(std::chrono::sys_time<std::chrono::microseconds> instant, char separator, bool withUtcMarker);

    /**
     * @brief 把 UTC 瞬时渲染成 PostgreSQL 认的带零偏移文本
     * @param instant 微秒精度的系统时钟瞬时
     * @return std::string 形如 `2026-10-18 09:20:30.000000+00`
     */
    [[nodiscard]] ASYN_DATABASE_API std::string renderSystemClockWithZeroOffset(std::chrono::sys_time<std::chrono::microseconds> instant);

    /**
     * @brief 把墙上时间渲染成文本（不带任何偏移——它本来就没有偏移这回事）
     * @param wall 微秒精度的本地时钟时刻
     * @param separator 日期与时间之间的分隔符
     * @return std::string 形如 `2026-10-18 09:20:30.000000`
     */
    [[nodiscard]] ASYN_DATABASE_API std::string renderWallClock(std::chrono::local_time<std::chrono::microseconds> wall, char separator);

    /**
     * @brief 解析结果折成 UTC 瞬时
     * @param parsed 解析结果；文本没带偏移时按 UTC 解释（这条约定是 MySQL 那一路的前提，见文件头）
     * @return std::optional<std::chrono::sys_time<std::chrono::microseconds>> 历法不成立（如 2 月 30 日）时空值
     */
    [[nodiscard]] ASYN_DATABASE_API std::optional<std::chrono::sys_time<std::chrono::microseconds>> toSystemClock(const ParsedDateTime &parsed) noexcept;

    /**
     * @brief 解析结果折成墙上时间
     * @param parsed 解析结果；文本带着偏移时空值——墙上时间没有偏移，那说明列型与成员型配错了
     * @return std::optional<std::chrono::local_time<std::chrono::microseconds>> 历法不成立或带偏移时空值
     */
    [[nodiscard]] ASYN_DATABASE_API std::optional<std::chrono::local_time<std::chrono::microseconds>> toWallClock(const ParsedDateTime &parsed) noexcept;

    /**
     * @brief 按指定引擎的文本口径渲染「日期时间参数」；不是那两档就交回空值
     *
     * @details 三个驱动绑定日期时间时唯一的不同就是分隔符与瞬时那一路尾部带什么：
     *          MySQL 用空格且什么都不带（DATETIME 按字面收，本映射把它当 UTC 字面量）、
     *          PostgreSQL 用空格且瞬时带 `+00`（timestamptz 收不带偏移的文本时是按会话 TimeZone 解释的）、
     *          SQLite/Redis 用 'T' 且瞬时带 `Z`。共用一份实现，是为了不让「同一条 SQL 值」
     *          在三处各拼一遍而慢慢漂移；墙上时间那一档恒不带后缀（它没有偏移可言）。
     *
     * @param value 待绑定的统一值
     * @param separator 日期与时间之间的分隔符
     * @param instantSuffix 瞬时那一路尾部要加的东西；墙上时间那一路忽略本参数
     * @return std::optional<std::string> 日期时间两档之一给出渲染好的文本（只在真需要时分配），
     *         其余交回空值，由调用方继续按文本/二进制等档位处理
     */
    [[nodiscard]] ASYN_DATABASE_API std::optional<std::string> renderTemporalParameterText(const DatabaseValue &value, char separator, std::string_view instantSuffix);

} // namespace AsynGyanis::Database::Detail
