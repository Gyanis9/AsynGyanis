/**
 * @file LogLevel.h
 * @brief 日志等级枚举及等级字符串互转工具
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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
        Info = 2,  ///< 信息级别
        Warn = 3,  ///< 警告级别
        Error = 4, ///< 错误级别
        Fatal = 5, ///< 致命错误级别
        Off = 6    ///< 关闭全部日志输出
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

    /**
     * @brief 将字符串解析为日志等级（大小写敏感）
     * @details 无法识别的取值回落为 Info 并往 std::cerr 打一条中文诊断：配置里写错一个等级
     *          不该让进程起不来，但容错必须可见。不经日志系统是因为本函数在日志就绪前
     *          （配置加载阶段）就可能被调用，且日志本身也依赖等级解析。
     * @param levelString 等级字符串，如 "INFO"
     * @return LogLevel 解析结果，未知字符串回退为 LogLevel::Info
     */
    inline LogLevel logLevelFromString(const std::string_view levelString)
    {
        if (levelString == "TRACE")
            return LogLevel::Trace;
        if (levelString == "DEBUG")
            return LogLevel::Debug;
        if (levelString == "INFO")
            return LogLevel::Info;
        if (levelString == "WARN")
            return LogLevel::Warn;
        if (levelString == "ERROR")
            return LogLevel::Error;
        if (levelString == "FATAL")
            return LogLevel::Fatal;
        if (levelString == "OFF")
            return LogLevel::Off;

        std::cerr << "日志等级：无法识别 '" << levelString
                << "'，已回落为 INFO（可用取值：TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF）" << '\n';
        return LogLevel::Info;
    }
} // namespace AsynGyanis::Base
