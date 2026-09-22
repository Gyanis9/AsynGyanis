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
     *          实例由 LogSink 以 std::atomic<std::shared_ptr> 持有，实现不得有**跨线程共享**的可变状态，
     *          以允许同一格式化器被多个 Sink 并发调用；线程局域的复用缓冲不在此列（JsonFormatter
     *          就靠它省掉每条重建字段对象的开销），但实现必须在自己的类注释里写明留了什么、代价是什么。
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

        /**
         * @brief 把日志事件渲染进调用方的缓冲，稳态下不取堆
         * @details 默认实现转调 format() 再整份追加，因此不比 format() 更省、也不会更差，
         *          自定义格式化器无需改动即可走通 Sink 的行缓冲。自带版式的两个格式化器都覆写了
         *          它：std::format 交回一个 std::string 要取两次堆，而直接写进留有容量的缓冲是零次。
         * @param out 目标缓冲；不清空，本次文本追加在其现有内容之后
         * @param event 日志事件
         */
        virtual void formatInto(std::string &out, const LogEvent &event)
        {
            out.append(format(event));
        }
    };
} // namespace AsynGyanis::Base
