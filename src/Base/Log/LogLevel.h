/**
 * @file LogLevel.h
 * @brief 日志等级枚举及等级字符串互转工具
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

// Windows 上 winerror.h 会把 ERROR 定义成值为 0 的宏，污染 LogLevel::ERROR 这类标识符；
// 清理统一收敛在 "Platform/Platform.h"（对 ERROR 与 DELETE 执行 #undef），
// 因此日志模块的使用方必须（直接或间接）先包含它。

namespace AsynGyanis::Base
{
    /**
     * @brief 日志等级枚举，数值越大等级越高
     *
     * @details 供 Logger 与 LogSink 做级别过滤；字符串形式统一经
     *          logLevelToString()/logLevelFromString() 互转。
     */
    enum class LogLevel : std::uint8_t
    {
        Trace = 0, ///< 跟踪级别（最详细）
        Debug = 1, ///< 调试级别
        Info  = 2, ///< 信息级别
        Warn  = 3, ///< 警告级别
        Error = 4, ///< 错误级别
        Fatal = 5, ///< 致命错误级别
        Off   = 6  ///< 关闭全部日志输出
    };

    /**
     * @brief 判断一条日志是否通过等级阈值过滤
     * @details Off 在两侧都不放行：作为阈值表示关闭全部输出，作为消息等级是用错了枚举——它不是
     *          可记录的等级，放行会落出一行等级文本为 "?????" 的记录（静默变形比拒绝更难查）。
     *          数值超出已知范围的等级仍放行，让用错枚举的现场留在日志里而不是静默消失。
     * @param threshold 过滤器当前的最低等级（Logger 或 Sink 的级别）
     * @param level 待判断的消息等级
     * @return bool 本次消息应当被记录时返回 true
     */
    [[nodiscard]] inline constexpr bool logLevelPassesFilter(const LogLevel threshold, const LogLevel level) noexcept
    {
        return level != LogLevel::Off && level >= threshold && threshold != LogLevel::Off;
    }

    /**
     * @brief 将日志等级转换为固定宽度字符串（5 字符对齐）
     * @param level 日志等级
     * @return const char* 等级名称字符串，未知等级返回 "?????"
     */
    inline const char *logLevelToString(const LogLevel level) noexcept
    {
        switch (level)
        {
            case LogLevel::Trace:
                return "TRACE";
            case LogLevel::Debug:
                return "DEBUG";
            case LogLevel::Info:
                return "INFO ";
            case LogLevel::Warn:
                return "WARN ";
            case LogLevel::Error:
                return "ERROR";
            case LogLevel::Fatal:
                return "FATAL";
            default:
                return "?????";
        }
    }

    namespace detail
    {
        /**
         * @brief 把一个字节折成 ASCII 小写，非大写字母原样交回
         * @details 刻意不用 `std::tolower`：它查的是当前 C locale 的转换表，土耳其语环境下
         *          `'I'` 折出的不是 `'i'`，"INFO" 会在那样的进程里解不出来。等级标签只有 ASCII
         *          字母，按码位区间折叠就够，且因此可在常量表达式里用
         * @param character 待折叠的字节
         * @return char 折叠结果
         */
        [[nodiscard]] inline constexpr char toAsciiLowercase(const char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        }

        /**
         * @brief 按「不区分 ASCII 大小写」比较一个等级标签与它的规范写法
         * @details 两侧都折叠，故调用方可以照 `logLevelToString()` 的规范大写形式写标签；
         *          长度先判不等即返回，因此非 ASCII 字节（UTF-8/GBK 的中文标签）只会配不上，不会被折叠成别的标签
         * @param label 待判断的标签
         * @param canonicalLabel 规范写法（已知等级名）
         * @return bool 两者在不区分大小写的意义下相等时返回 true
         */
        [[nodiscard]] inline constexpr bool logLevelLabelEquals(const std::string_view label, const std::string_view canonicalLabel) noexcept
        {
            if (label.size() != canonicalLabel.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < label.size(); ++index)
            {
                if (toAsciiLowercase(label[index]) != toAsciiLowercase(canonicalLabel[index]))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace detail

    /**
     * @brief 将字符串解析为日志等级（不区分 ASCII 大小写）
     * @details 大小写都收是因为日志配置里其余的取值全是小写（`type: file`、`policy: size`、
     *          `overflow_policy: drop_oldest`），于是 `level: error` 是最自然的写法；把它判成不认识
     *          就回落为 INFO，等于「只想留错误日志」的配置实际在打全量 INFO。无法识别的取值仍回落为
     *          Info 并往 std::cerr 打一条中文诊断：配置里写错一个等级不该让进程起不来，但容错必须可见。
     *          不经日志系统是因为本函数在日志就绪前（配置加载阶段）就可能被调用，且日志本身也依赖等级解析。
     * @param levelString 等级字符串，如 "INFO"、"info"、"Info"
     * @return LogLevel 解析结果，未知字符串回退为 LogLevel::Info
     */
    inline LogLevel logLevelFromString(const std::string_view levelString)
    {
        using detail::logLevelLabelEquals;

        if (logLevelLabelEquals(levelString, "TRACE"))
            return LogLevel::Trace;
        if (logLevelLabelEquals(levelString, "DEBUG"))
            return LogLevel::Debug;
        if (logLevelLabelEquals(levelString, "INFO"))
            return LogLevel::Info;
        if (logLevelLabelEquals(levelString, "WARN"))
            return LogLevel::Warn;
        if (logLevelLabelEquals(levelString, "ERROR"))
            return LogLevel::Error;
        if (logLevelLabelEquals(levelString, "FATAL"))
            return LogLevel::Fatal;
        if (logLevelLabelEquals(levelString, "OFF"))
            return LogLevel::Off;

        std::cerr << "日志等级：无法识别 '" << levelString << "'，已回落为 INFO（可用取值：TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF，不区分大小写）" << '\n';
        return LogLevel::Info;
    }
} // namespace AsynGyanis::Base
