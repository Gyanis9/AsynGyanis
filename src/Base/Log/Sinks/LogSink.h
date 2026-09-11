/**
 * @file LogSink.h
 * @brief 日志输出目标抽象基类（等级过滤与格式化器持有）
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogEvent.h"
#include "Base/Log/Formatters/LogFormatter.h"
#include "Base/Log/LogLevel.h"

#include <atomic>
#include <memory>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 日志输出目标基类
     *
     * @details 统一「等级过滤 + 格式化 + 落地」三件事：等级与格式化器均由原子成员持有，
     *          可在运行期安全替换；落地方式由派生类（控制台/文件/滚动文件/异步包装）实现。
     * @note Logger 会持有多个 Sink 并在每次写日志前调用 shouldLog() 预筛。
     */
    class LogSink
    {
    public:
        /**
         * @brief 析构 Sink 基类，保证通过基类指针删除时正确派发到派生类析构
         */
        virtual ~LogSink() = default;

        /**
         * @brief 输出一条日志事件
         * @param event 日志事件
         * @note 实现方需自行保证多线程调用安全
         */
        virtual void write(const LogEvent &event) = 0;

        /**
         * @brief 刷新输出缓冲
         * @note 实现方应尽量阻塞到数据落盘后再返回
         */
        virtual void flush() = 0;

        /**
         * @brief 设置 Sink 的最小日志级别
         * @param level 目标日志级别
         */
        void setLevel(LogLevel level);

        /**
         * @brief 获取 Sink 当前日志级别
         * @return LogLevel 当前日志级别
         */
        [[nodiscard]] LogLevel getLevel() const;

        /**
         * @brief 判断给定日志级别是否允许输出
         * @details 阈值为 LogLevel::Off 时表示关闭全部日志输出，任何级别都不放行。
         * @param level 待判断级别
         * @return bool 满足级别阈值时返回 true
         */
        [[nodiscard]] bool shouldLog(LogLevel level) const;

        /**
         * @brief 设置自定义格式化器
         * @param formatter 格式化器对象所有权
         */
        void setFormatter(std::unique_ptr<LogFormatter> formatter);

    protected:
        /**
         * @brief 使用当前格式化器将事件转换为文本
         * @param event 日志事件
         * @return std::string 格式化文本；未设置格式化器时回退到 DefaultFormatter
         */
        std::string formatEvent(const LogEvent &event) const;

    private:
        std::atomic<LogLevel>                       m_level{LogLevel::Trace}; ///< 当前最小日志级别
        std::atomic<std::shared_ptr<LogFormatter> > m_formatter;              ///< 原子 shared_ptr，store/load 保证线程安全的读写
    };
} // namespace AsynGyanis::Base
