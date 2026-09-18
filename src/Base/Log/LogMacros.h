/**
 * @file LogMacros.h
 * @brief 日志便捷宏：源码位置采集与全局/指定日志器的输出宏
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/SourceLocation.h"
#include "Base/Log/LoggerRegistry.h"

// ============================================================================
// 源码位置采集宏
// ----------------------------------------------------------------------------
// Debug 构建（ASYN_DEBUG，由 CMake 在 Debug 配置注入）返回真实源码位置，
// Release 构建返回空位置，避免采集与输出开销。
// ============================================================================
#ifdef ASYN_DEBUG
    #define LOG_SOURCE_LOCATION() AsynGyanis::Base::SourceLocation::current()
#else
    #define LOG_SOURCE_LOCATION() AsynGyanis::Base::SourceLocation()
#endif

// ============================================================================
// 内部使用：获取日志器并记录
// ============================================================================
#define LOG_INTERNAL(logger_expression, level, message) \
    do { \
        auto &internalLogger = (logger_expression); \
        if (internalLogger.shouldLog(level)) { \
            internalLogger.log(level, (message), LOG_SOURCE_LOCATION()); \
        } \
    } while (0)

/// 使用指定日志器的宏
#define LOG_LOGGER(logger, level, message) LOG_INTERNAL(logger, level, message)

/// 以下宏使用默认根日志器输出对应等级日志
#define LOG_TRACE(message) LOG_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Trace, message)
#define LOG_DEBUG(message) LOG_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Debug, message)
#define LOG_INFO(message)  LOG_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Info,  message)
#define LOG_WARN(message)  LOG_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Warn,  message)
#define LOG_ERROR(message) LOG_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Error, message)
#define LOG_FATAL(message) LOG_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Fatal, message)

// ============================================================================
// 格式化宏（C++20 std::format，类型安全、编译期检查）
// ============================================================================
#define LOG_FORMAT_INTERNAL(logger_expression, level, format_string, ...) \
    do { \
        auto &internalLogger = (logger_expression); \
        if (internalLogger.shouldLog(level)) { \
            internalLogger.logFormat(level, LOG_SOURCE_LOCATION(), format_string, ## __VA_ARGS__); \
        } \
    } while (0)

/// 使用默认根日志器的格式化宏
#define LOG_TRACE_FMT(format_string, ...) LOG_FORMAT_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Trace, format_string, ## __VA_ARGS__)
#define LOG_DEBUG_FMT(format_string, ...) LOG_FORMAT_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Debug, format_string, ## __VA_ARGS__)
#define LOG_INFO_FMT(format_string, ...)  LOG_FORMAT_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Info,  format_string, ## __VA_ARGS__)
#define LOG_WARN_FMT(format_string, ...)  LOG_FORMAT_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Warn,  format_string, ## __VA_ARGS__)
#define LOG_ERROR_FMT(format_string, ...) LOG_FORMAT_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Error, format_string, ## __VA_ARGS__)
#define LOG_FATAL_FMT(format_string, ...) LOG_FORMAT_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Fatal, format_string, ## __VA_ARGS__)

/// 使用指定日志器的格式化宏
#define LOG_LOGGER_TRACE_FMT(logger, format_string, ...) LOG_FORMAT_INTERNAL(logger, AsynGyanis::Base::LogLevel::Trace, format_string, ## __VA_ARGS__)
#define LOG_LOGGER_DEBUG_FMT(logger, format_string, ...) LOG_FORMAT_INTERNAL(logger, AsynGyanis::Base::LogLevel::Debug, format_string, ## __VA_ARGS__)
#define LOG_LOGGER_INFO_FMT(logger, format_string, ...)  LOG_FORMAT_INTERNAL(logger, AsynGyanis::Base::LogLevel::Info,  format_string, ## __VA_ARGS__)
#define LOG_LOGGER_WARN_FMT(logger, format_string, ...)  LOG_FORMAT_INTERNAL(logger, AsynGyanis::Base::LogLevel::Warn,  format_string, ## __VA_ARGS__)
#define LOG_LOGGER_ERROR_FMT(logger, format_string, ...) LOG_FORMAT_INTERNAL(logger, AsynGyanis::Base::LogLevel::Error, format_string, ## __VA_ARGS__)
#define LOG_LOGGER_FATAL_FMT(logger, format_string, ...) LOG_FORMAT_INTERNAL(logger, AsynGyanis::Base::LogLevel::Fatal, format_string, ## __VA_ARGS__)

// ============================================================================
// 带调用栈的日志
// ----------------------------------------------------------------------------
// 原始帧在等级过滤通过后才捕获（微秒级）；符号解析推迟到 Sink 输出时，因此从事件循环
// 线程调用它们不会触发调试信息读取（见 StackTrace.h）。
// 异常日志与 LOG_ERROR_FMT / LOG_LOGGER_ERROR_FMT 同序（级别进宏名、logger 在最前）；
// 目前只提供 Error 级，需要其他级别时按同形补一条。
// ============================================================================

/// 记录当前位置的调用栈（没有异常、但想知道走到这里经过了哪些帧）
#define LOG_STACK_INTERNAL(logger_expression, level, message) \
    do { \
        auto &internalLogger = (logger_expression); \
        if (internalLogger.shouldLog(level)) { \
            internalLogger.logWithStackTrace(level, (message), LOG_SOURCE_LOCATION()); \
        } \
    } while (0)

/// 记录异常（级别见宏名）：与 LOG_*_FMT 同形，额外把异常的抛出点调用栈带进事件（非框架异常则不带栈）
#define LOG_EXCEPTION_INTERNAL(logger_expression, level, exception, format_string, ...) \
    do { \
        auto &internalLogger = (logger_expression); \
        if (internalLogger.shouldLog(level)) { \
            internalLogger.logExceptionFormat(level, (exception), LOG_SOURCE_LOCATION(), format_string, ## __VA_ARGS__); \
        } \
    } while (0)

/// 使用默认根日志器
#define LOG_STACK(level, message) LOG_STACK_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), level, message)
#define LOG_ERROR_EXCEPTION(exception, format_string, ...) LOG_EXCEPTION_INTERNAL(AsynGyanis::Base::LoggerRegistry::instance().getRootLogger(), AsynGyanis::Base::LogLevel::Error, exception, format_string, ## __VA_ARGS__)

/// 使用指定日志器
#define LOG_LOGGER_STACK(logger, level, message) LOG_STACK_INTERNAL(logger, level, message)
#define LOG_LOGGER_ERROR_EXCEPTION(logger, exception, format_string, ...) LOG_EXCEPTION_INTERNAL(logger, AsynGyanis::Base::LogLevel::Error, exception, format_string, ## __VA_ARGS__)
