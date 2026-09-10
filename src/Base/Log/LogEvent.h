#pragma once

#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"
#include "Platform/System/PlatformTime.h"

#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace AsynGyanis::Base
{
    /**
     * @brief 生成当前时间戳字符串（含毫秒）
     *
     * @details 本地时间转换统一走 AsynGyanis::Platform::PlatformTime::localTime()，
     *          因此 Windows 与 Linux 的转换行为一致；格式化使用
     *          thread_local 缓冲区的 std::format_to_n，避免 std::format 的堆分配。
     * @return std::string 形如 "2026-09-10 12:34:56.789" 的时间戳
     */
    inline std::string currentTimestamp()
    {
        const auto    now         = std::chrono::system_clock::now();
        const auto    timeValue   = std::chrono::system_clock::to_time_t(now);
        const auto    millisecond = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const std::tm localTime   = AsynGyanis::Platform::PlatformTime::localTime(timeValue);

        thread_local std::array<char, 32> buffer;
        const auto                        [out, size] = std::format_to_n(buffer.data(), buffer.size(),
                                                                         "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                                                                         localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                                                                         localTime.tm_hour, localTime.tm_min, localTime.tm_sec, millisecond.count());
        return {buffer.data(), static_cast<std::string::size_type>(out - buffer.data())};
    }

    /**
     * @brief 获取当前线程 ID 的字符串表示（按线程缓存）
     * @return const std::string& 线程 ID 字符串引用
     */
    inline const std::string &threadIdString()
    {
        thread_local const std::string kcached = []
        {
            std::ostringstream stream;
            stream << std::this_thread::get_id();
            return stream.str();
        }();
        return kcached;
    }

    /**
     * @brief 单条日志事件的完整数据
     *
     * @details 由 Logger 在写日志时构造，随指针/值语义传递给各 LogSink；
     *          事件一旦构造即不可变，异步 Sink 依赖它完成跨线程投递。
     */
    struct LogEvent
    {
        LogLevel       level{};      ///< 日志等级
        std::string    timestamp{};  ///< 时间戳字符串
        std::string    threadId{};   ///< 线程 ID 字符串
        SourceLocation location{};   ///< 源码位置
        std::string    loggerName{}; ///< 日志器名称
        std::string    message{};    ///< 日志消息内容

        /**
         * @brief 默认构造
         */
        LogEvent() = default;

        /**
         * @brief 使用全量字段构造日志事件
         * @param logLevel 日志等级
         * @param timestamp 时间戳字符串
         * @param threadId 线程 ID 字符串
         * @param sourceLocation 源码位置
         * @param loggerName 日志器名称
         * @param message 日志消息
         */
        LogEvent(const LogLevel logLevel, std::string timestamp, std::string threadId, const SourceLocation &sourceLocation, std::string loggerName, std::string message) :
            level(logLevel), timestamp(std::move(timestamp)), threadId(std::move(threadId)), location(sourceLocation), loggerName(std::move(loggerName)),
            message(std::move(message))
        {
        }
    };
} // namespace AsynGyanis::Base
