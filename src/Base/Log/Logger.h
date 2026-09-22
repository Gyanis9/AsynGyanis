/**
 * @file Logger.h
 * @brief 日志器类：等级过滤与 Sink 分发
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/ExceptionStackTrace.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/SourceLocation.h"

#include <atomic>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 日志器实例
     *
     * @details 每个日志器拥有自己的名字、等级过滤与一组 Sink；线程安全，但通常经
     *          LoggerRegistry 获取，由注册表保证创建过程的线程安全。
     * @note 单个 Sink 抛出的异常不会中断其余 Sink 的写入（记一行标准错误）。
     */
    class Logger
    {
    public:
        /**
         * @brief 构造日志器实例
         * @param name 日志器名称
         */
        explicit Logger(std::string name);

        /**
         * @brief 析构日志器并释放所有 Sink
         */
        ~Logger();

        Logger(const Logger &) = delete;

        Logger &operator=(const Logger &) = delete;

        Logger(Logger &&) = delete;

        Logger &operator=(Logger &&) = delete;

        /**
         * @brief 按当前日志级别过滤后写入日志事件
         * @param level 本次日志级别
         * @param message 日志消息内容
         * @param location 源码位置信息
         */
        void log(LogLevel level, std::string_view message, const SourceLocation &location = SourceLocation::current()) const;

        /**
         * @brief 记录一条带调用栈的日志（栈在等级过滤通过后于此处捕获）
         * @details 用于「没有异常但想知道走到这里经过了哪些帧」的排障。符号解析推迟到 Sink
         *          输出时进行，因此事件循环线程上调用它不会触发调试信息读取。
         * @param level 本次日志级别
         * @param message 日志消息内容
         * @param location 源码位置信息
         */
        void logWithStackTrace(LogLevel level, std::string_view message, const SourceLocation &location = SourceLocation::current()) const;

        /**
         * @brief 记录一条带指定调用栈的日志（用于携带异常抛出点的栈）
         * @param level 本次日志级别
         * @param message 日志消息内容
         * @param stackTrace 调用栈原始帧（如 `exception.stackTrace()`）
         * @param location 源码位置信息
         */
        void logWithStackTrace(LogLevel level, std::string_view message, CapturedStackTrace stackTrace,
                               const SourceLocation &location = SourceLocation::current()) const;

        /**
         * @brief 使用 std::format 格式化日志消息并记录
         * @details 格式串非法时不抛给业务方，降级为一条 Error 日志并带上格式串原文。
         * @tparam Args 格式化参数类型
         * @param level 本次日志级别
         * @param location 源码位置信息
         * @param formatString std::format 格式串
         * @param arguments 格式化参数
         */
        template<typename... Args>
        void logFormat(const LogLevel level, const SourceLocation &location, std::string_view formatString, Args &&... arguments) const
        {
            if (!shouldLog(level))
            {
                return;
            }
            writeFormattedEvent(level, location, {}, formatString, std::forward<Args>(arguments)...);
        }

        /**
         * @brief 使用 std::format 格式化日志消息并记录，同时附上异常的抛出点调用栈
         * @details 与 logFormat 同形；异常来自框架异常家族（Base::Exception / LogicException /
         *          InvalidArgumentException）时带上它在构造时捕获的抛出点栈，其余异常照常记录、
         *          只是没有栈（打印点的栈没有诊断价值，不凭空补采）。
         * @tparam Args 格式化参数类型
         * @param level 本次日志级别
         * @param exception 触发本次记录的异常
         * @param location 源码位置信息
         * @param formatString std::format 格式串（通常用 `{}` 接收 exception.what()）
         * @param arguments 格式化参数
         */
        template<typename... Args>
        void logExceptionFormat(const LogLevel level, const std::exception &exception, const SourceLocation &location,
                                std::string_view formatString, Args &&... arguments) const
        {
            if (!shouldLog(level))
            {
                return;
            }
            CapturedStackTrace stackTrace;
            if (const CapturedStackTrace *captured = tryStackTrace(exception); captured != nullptr)
            {
                stackTrace = *captured;
            }
            writeFormattedEvent(level, location, std::move(stackTrace), formatString, std::forward<Args>(arguments)...);
        }

        /**
         * @brief 向日志器追加一个输出 Sink
         * @param sink 待接管所有权的 Sink
         */
        void addSink(std::unique_ptr<LogSink> sink);

        /**
         * @brief 清空所有已注册 Sink
         */
        void clearSinks();

        /**
         * @brief 设置日志器最低输出级别
         * @param level 目标日志级别
         */
        void setLevel(LogLevel level);

        /**
         * @brief 获取当前日志器级别
         * @return LogLevel 当前日志级别
         */
        [[nodiscard]] LogLevel getLevel() const;

        /**
         * @brief 获取日志器名称
         * @return const std::string& 日志器名称引用
         */
        [[nodiscard]] const std::string &name() const;

        /**
         * @brief 刷新所有 Sink 的缓冲区
         */
        void flush() const;

        /**
         * @brief 判断指定级别是否满足输出条件
         * @details 阈值为 LogLevel::Off 时表示关闭全部日志输出，任何级别都不放行。
         * @param level 待判断日志级别
         * @return bool 当级别不低于当前阈值时返回 true
         */
        [[nodiscard]] bool shouldLog(LogLevel level) const;

    private:
        /**
         * @brief Sink 列表的不可变快照
         *
         * @details 读侧原子加载后整轮遍历，无需加锁；写侧（addSink/clearSinks）构造新快照
         *          整体替换，旧快照由仍在遍历它的线程共同持有，因此不会写入已释放的 Sink。
         */
        struct SinkSnapshot
        {
            std::vector<std::shared_ptr<LogSink> > sinks; ///< 该代快照持有的 Sink 列表
        };

        /**
         * @brief 构造不含任何 Sink 的空快照
         * @return std::shared_ptr<const SinkSnapshot> 只读空快照
         */
        [[nodiscard]] static std::shared_ptr<const SinkSnapshot> emptySnapshot();

        /**
         * @brief 将日志事件分发到全部可用 Sink
         * @details 先原子加载快照再遍历，因此并发 clearSinks() 不会让遍历撞上已释放的 Sink。
         *          最后一个愿意收的 Sink 拿到事件本体（move），其余各拿一份只读引用：
         *          异步队列那条路因此不必再为每行日志复制一份消息体。
         * @param event 已构造好的日志事件；本函数可能交出它的内容，调用方此后不得再读
         */
        void writeToSinks(LogEvent &event) const;

        /**
         * @brief 渲染格式串并分发事件，格式化失败时降级为带格式串原文的 Error 日志
         * @details logFormat 与 logExceptionFormat 共用；降级路径与正常路径携带同一份调用栈。
         * @tparam Args 格式化参数类型
         * @param level 本次日志级别
         * @param location 源码位置信息
         * @param stackTrace 调用栈原始帧；空表示本条日志不带栈
         * @param formatString std::format 格式串
         * @param arguments 格式化参数
         */
        template<typename... Args>
        void writeFormattedEvent(const LogLevel level, const SourceLocation &location, CapturedStackTrace stackTrace,
                                 const std::string_view formatString, Args &&... arguments) const
        {
            try
            {
                writeEvent(level, std::vformat(formatString, std::make_format_args(arguments...)), location, std::move(stackTrace));
            } catch (const std::format_error &formatError)
            {
                writeEvent(LogLevel::Error,
                           std::format("日志格式化错误：{} [format='{}']", formatError.what(), formatString),
                           location, std::move(stackTrace));
            }
        }

        /**
         * @brief 以「已持有消息体与调用栈」的形式构造并分发日志事件
         * @details 消息体按值接收并移入事件，使 std::vformat 的结果零拷贝交给事件；
         *          这是 log / logFormat / logWithStackTrace 共用的内部入口。
         * @param level 本次日志级别
         * @param message 已格式化好的日志消息
         * @param location 源码位置信息
         * @param stackTrace 调用栈原始帧；空表示本条日志不带栈
         */
        void writeEvent(LogLevel level, std::string message, const SourceLocation &location, CapturedStackTrace stackTrace = {}) const;

        /// 日志器名称：以共享常量字符串持有，事件构造时只复制指针不再拷贝文本
        std::shared_ptr<const std::string>                m_name;
        std::atomic<LogLevel>                             m_level{LogLevel::Trace};         ///< 当前日志级别
        std::atomic<std::shared_ptr<const SinkSnapshot> > m_sinksSnapshot{emptySnapshot()}; ///< 读路径无锁的 Sink 快照
        std::mutex                                        m_sinksWriteMutex;                ///< 仅用于串行化替换快照的写者，读者不会触碰
    };
} // namespace AsynGyanis::Base
