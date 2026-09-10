/**
 * @file ConsoleSink.h
 * @brief 控制台日志输出目标
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogSink.h"

#include <mutex>

namespace AsynGyanis::Base
{
    /**
     * @brief 将日志写入标准输出/标准错误的 Sink
     *
     * @details 构造时即选定格式化器：警告及以上等级走 std::cerr，其余走 std::cout。
     *          启用彩色但输出目标不支持 ANSI 序列时自动退回 DefaultFormatter，避免打印乱码。
     */
    class ConsoleSink : public LogSink
    {
    public:
        /**
         * @brief 构造控制台 Sink，并按需启用彩色输出
         * @param enableColor 是否启用彩色输出
         */
        explicit ConsoleSink(bool enableColor = true);

        /**
         * @brief 将日志事件写入标准输出或标准错误
         * @details 重写 LogSink::write()：先按需初始化控制台编码，再持互斥锁输出整行；
         *          Warn 及以上等级写 std::cerr，其余写 std::cout，每次调用自带换行。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

        /**
         * @brief 刷新控制台输出缓冲区
         * @details 重写 LogSink::flush()：同时刷新 std::cout 与 std::cerr，持锁避免与其他输出交错。
         */
        void flush() override;

        /**
         * @brief 动态切换控制台彩色输出能力
         * @param enabled 是否启用彩色输出
         */
        void setColorEnabled(bool enabled);

    private:
        bool       m_colorEnabled; ///< 是否启用彩色输出
        std::mutex m_mutex;        ///< 保护控制台输出的互斥锁
    };
} // namespace AsynGyanis::Base
