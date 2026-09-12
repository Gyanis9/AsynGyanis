/**
 * @file LogEvent.h
 * @brief 日志事件数据结构，附带时间戳与线程号生成工具
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"
#include "Platform/System/PlatformTime.h"

#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

        const auto [out, size] = std::format_to_n(buffer.data(), buffer.size(),
                                                  "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                                                  localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                                                  localTime.tm_hour, localTime.tm_min, localTime.tm_sec, millisecond.count());
        return {buffer.data(), static_cast<std::string::size_type>(out - buffer.data())};
    }

    /**
     * @brief 获取当前线程 ID 的字符串表示（按线程缓存）
     * @details 用 std::to_chars 把线程标识写成十进制并一次性构造字符串，相比
     *          std::ostringstream 少一次感知区域构造与多次内部扩容，且每个线程只算一次，
     *          热路径上仅仅是返回一个引用。
     *          取值来源是 std::hash<std::thread::id>：MSVC 与 libstdc++ 都直接返回线程的
     *          原生标识，因此文本与调试器看到的线程号一致；之所以不直接格式化
     *          std::thread::id，是因为 MSVC 在 C++20 模式下未提供 formatter<thread::id>
     *          （属 C++23 设施），而本项目禁用 C++23 设施。
     * @return const std::string& 线程 ID 字符串引用
     */
    inline const std::string &threadIdString()
    {
        thread_local const std::string kcachedThreadId = []
        {
            std::array<char, 24> buffer{};
            const auto           [out, errorCode] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                                        std::hash<std::thread::id>{}(std::this_thread::get_id()));
            return std::string(buffer.data(), static_cast<std::string::size_type>(out - buffer.data()));
        }();
        return kcachedThreadId;
    }

    /**
     * @brief 单条日志事件的完整数据
     *
     * @details 由 Logger 在写日志时构造，随指针/值语义传递给各 LogSink；
     *          事件一旦构造即不可变，异步 Sink 依赖它完成跨线程投递。
     * @note 日志器名称以 std::shared_ptr<const std::string> 共享：名字在 Logger 存活期内不变，
     *       共享后每条日志不再为此分配/拷贝一个字符串（异步 Sink 入队时也只是复制指针）。
     */
    struct LogEvent
    {
        LogLevel                             level{};      ///< 日志等级
        std::string                          timestamp{};  ///< 时间戳字符串
        std::string                          threadId{};   ///< 线程 ID 字符串
        [[no_unique_address]] SourceLocation location{};   ///< 源码位置（小对象，允许复用相邻成员的填充字节）
        std::shared_ptr<const std::string>   loggerName{}; ///< 日志器名称（与 Logger 共享同一份常量名字）
        std::string                          message{};    ///< 日志消息内容

        /**
         * @brief 默认构造
         */
        LogEvent() = default;

        /**
         * @brief 使用全量字段构造日志事件（按值接收日志器名称）
         * @details 便捷重载：把名字包成共享实例，供测试与临时构造使用；
         *          热路径请用接收 shared_ptr 的重载，避免每条日志一次字符串分配。
         * @param logLevel 日志等级
         * @param timestamp 时间戳字符串
         * @param threadId 线程 ID 字符串
         * @param sourceLocation 源码位置
         * @param loggerNameValue 日志器名称
         * @param message 日志消息
         */
        LogEvent(const LogLevel logLevel, std::string timestamp, std::string threadId, const SourceLocation &sourceLocation, std::string loggerNameValue, std::string message) :
            level(logLevel), timestamp(std::move(timestamp)), threadId(std::move(threadId)), location(sourceLocation),
            loggerName(std::make_shared<const std::string>(std::move(loggerNameValue))), message(std::move(message))
        {
        }

        /**
         * @brief 使用全量字段构造日志事件（共享日志器名称）
         * @param logLevel 日志等级
         * @param timestamp 时间戳字符串
         * @param threadId 线程 ID 字符串
         * @param sourceLocation 源码位置
         * @param loggerNameSnapshot 已共享的日志器名称，可为空表示无名字
         * @param message 日志消息
         */
        LogEvent(const LogLevel logLevel, std::string timestamp, std::string threadId, const SourceLocation &sourceLocation, std::shared_ptr<const std::string> loggerNameSnapshot,
                 std::string    message) :
            level(logLevel), timestamp(std::move(timestamp)), threadId(std::move(threadId)), location(sourceLocation), loggerName(std::move(loggerNameSnapshot)),
            message(std::move(message))
        {
        }

        /**
         * @brief 以只读视图获取日志器名称
         * @return std::string_view 名称视图；未设置名字时为空视图
         */
        [[nodiscard]] std::string_view loggerNameView() const noexcept
        {
            return loggerName ? std::string_view{*loggerName} : std::string_view{};
        }
    };
} // namespace AsynGyanis::Base
