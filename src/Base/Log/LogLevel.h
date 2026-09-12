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

// ============================================================================
// Windows ERROR 宏冲突说明
// ----------------------------------------------------------------------------
// winerror.h 会把 ERROR 定义为一个值为 0 的宏，历史上本头文件在此处书写
// #ifdef ERROR / #undef ERROR 来避免 LogLevel::ERROR 之类的标识符被展开。
// 现在该项目级清理已统一收敛到 "Platform/Platform.h"（其中已对 DELETE 与
// ERROR 两个 Windows SDK 宏执行 #undef），本头文件不再重复 #undef。
// 约定：在 Windows 上使用日志模块的编译单元必须（直接或间接）先包含
// "Platform/Platform.h"，否则会残留 windows.h 注入的 ERROR 宏污染后续代码。
// ============================================================================

namespace AsynGyanis::Base
{
    /**
     * @brief 日志等级枚举，数值越大等级越高
     *
     * @details 供 Logger 与 LogSink 做级别过滤，字符串形式统一由
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
     * @details 无法识别的取值回落到 LogLevel::Info 是刻意的容错——配置里写错一个等级
     *          不应让进程起不来，也不应改变既有调用方的返回类型（本函数恒返回 LogLevel）。
     *          但容错必须可见：未知取值会向 std::cerr 打一条中文诊断，与
     *          LoggerConfigLoader 的其它配置诊断风格一致。这里刻意不经日志系统，
     *          因为本函数可能在日志系统就绪之前（配置加载阶段）被调用，且日志本身
     *          也依赖等级解析，走日志通道会形成递归。
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
