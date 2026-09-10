/**
 * @file LogFormatter.h
 * @brief 日志格式化器抽象接口
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogEvent.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 日志格式化器接口
     *
     * @details 把 LogEvent 渲染成一行文本，与 Sink 的落地方式（控制台/文件/异步队列）解耦。
     *          实例由 LogSink 以 std::atomic<std::shared_ptr> 持有，实现必须无共享可变状态，
     *          以允许同一格式化器被多个 Sink 并发调用。
     */
    class LogFormatter
    {
    public:
        /**
         * @brief 析构格式化器基类，保证按派生类正确销毁
         */
        virtual ~LogFormatter() = default;

        /**
         * @brief 将日志事件格式化为文本
         * @param event 日志事件
         * @return std::string 格式化后的文本（不含行尾换行符）
         */
        virtual std::string format(const LogEvent &event) = 0;
    };
} // namespace AsynGyanis::Base
