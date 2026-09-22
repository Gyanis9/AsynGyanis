/**
 * @file LogEvent.h
 * @brief 日志事件数据结构，附带线程号快照工具
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/StackTrace.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

#include <array>
#include <charconv>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Base
{
    /**
     * @brief 日志事件携带的时刻类型（墙上时钟）
     * @details 事件只带时刻、不带文本：文本由各格式化器在 Sink 写入线程上就地渲染，
     *          因此产生日志的线程（通常是事件循环线程）不为一行日志取一块堆。
     * @note 取自 system_clock，会随时钟校时而跳变；日志按到达次序落地，不靠本字段排序
     */
    using TimestampMoment = std::chrono::system_clock::time_point;

    /**
     * @brief 取当前线程 ID 字符串的共享快照（按线程缓存一次）
     * @details 取 std::hash<std::thread::id>：MSVC 与 libstdc++ 都返回线程的原生标识，
     *          文本与调试器看到的线程号一致。线程内只构造一次，事件构造只复制指针，
     *          因此每行日志既不重复格式化、也不产生堆分配；事件若活过本线程（异步 Sink
     *          队列），快照由 shared_ptr 保活。
     * @return const std::shared_ptr<const std::string>& 本线程的 ID 快照
     */
    inline const std::shared_ptr<const std::string> &threadIdString()
    {
        thread_local const std::shared_ptr<const std::string> kcachedThreadId = []
        {
            std::array<char, 24> buffer{};
            const auto           [out, errorCode] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                                        std::hash<std::thread::id>{}(std::this_thread::get_id()));
            return std::make_shared<const std::string>(buffer.data(), static_cast<std::string::size_type>(out - buffer.data()));
        }();
        return kcachedThreadId;
    }

    /**
     * @brief 单条日志事件的完整数据
     *
     * @details 由 Logger 构造后随指针/值语义传给各 LogSink，构造完即不可变，
     *          异步 Sink 依赖这一点完成跨线程投递。时刻、调用栈都按原始值随事件走，
     *          文本渲染推迟到各 Sink 的输出时（见 TimestampText.h 与 StackTrace.h）。
     * @note 日志器名称与线程 ID 都以 std::shared_ptr<const std::string> 共享：两者在各自
     *       宿主（Logger / 线程）内都不变，因此每条日志不为其分配/拷贝字符串，
     *       异步 Sink 入队也只复制指针。
     * @note 调用栈以**原始帧**随事件传递，符号解析由各 Sink 在输出时进行（见 StackTrace.h）：
     *       异步 Sink 上解析落在工作线程，事件循环线程不为它付出调试信息读取的开销。
     */
    struct LogEvent
    {
        LogLevel                             level{};             ///< 日志等级
        TimestampMoment                      timestamp{};         ///< 事件发生的时刻；文本由格式化器就地渲染，见 TimestampText.h
        std::shared_ptr<const std::string>   threadId{};          ///< 线程 ID 快照（按线程共享，事件只存指针）
        [[no_unique_address]] SourceLocation location{};          ///< 源码位置（小对象，允许复用相邻成员的填充字节）
        std::shared_ptr<const std::string>   loggerName{};        ///< 日志器名称（与 Logger 共享同一份常量名字）
        std::string                          message{};           ///< 日志消息内容
        CapturedStackTrace                   stackTrace{};        ///< 调用栈原始帧；空表示本条日志不带栈

        LogEvent() = default;

        /**
         * @brief 使用全量字段构造日志事件（按值接收消息等文本）
         * @details 便捷重载：线程 ID 与日志器名称在此包成共享实例，热路径请用接收
         *          shared_ptr 的重载，避免每行日志两次字符串分配。
         * @param logLevel 日志等级
         * @param timestampMoment 事件发生的时刻
         * @param threadIdValue 线程 ID 文本
         * @param sourceLocation 源码位置
         * @param loggerNameValue 日志器名称
         * @param message 日志消息
         */
        LogEvent(const LogLevel logLevel, const TimestampMoment timestampMoment, std::string threadIdValue,
                 const SourceLocation &sourceLocation, std::string loggerNameValue, std::string message) :
            level(logLevel), timestamp(timestampMoment),
            threadId(std::make_shared<const std::string>(std::move(threadIdValue))), location(sourceLocation),
            loggerName(std::make_shared<const std::string>(std::move(loggerNameValue))), message(std::move(message))
        {
        }

        /**
         * @brief 使用全量字段构造日志事件（共享线程 ID 与日志器名称）
         * @param logLevel 日志等级
         * @param timestampMoment 事件发生的时刻
         * @param threadIdSnapshot 已共享的线程 ID 快照，可为空表示无 ID
         * @param sourceLocation 源码位置
         * @param loggerNameSnapshot 已共享的日志器名称，可为空表示无名字
         * @param message 日志消息
         */
        LogEvent(const LogLevel logLevel, const TimestampMoment timestampMoment, std::shared_ptr<const std::string> threadIdSnapshot,
                 const SourceLocation &sourceLocation, std::shared_ptr<const std::string> loggerNameSnapshot, std::string message) :
            level(logLevel), timestamp(timestampMoment), threadId(std::move(threadIdSnapshot)), location(sourceLocation), loggerName(std::move(loggerNameSnapshot)),
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

        /**
         * @brief 以只读视图获取线程 ID
         * @return std::string_view 线程 ID 视图；未设置时为空视图
         */
        [[nodiscard]] std::string_view threadIdView() const noexcept
        {
            return threadId ? std::string_view{*threadId} : std::string_view{};
        }
    };
} // namespace AsynGyanis::Base
