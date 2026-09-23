/**
 * @file ConsoleSink.h
 * @brief 控制台日志输出目标
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Sinks/LogSink.h"

#include <mutex>
#include <string>

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
         * @details 重写 LogSink::write()：持互斥锁把整行一次性写出（拼接复用成员缓冲，稳态不取堆）；
         *          Warn 及以上等级写 std::cerr，其余写 std::cout，每次调用自带换行，
         *          且返回时该行已刷新落地（重定向到文件或管道时缓冲不会把它扣住）。
         *          控制台编码由构造函数切一次，不在每行重复设置。
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
         * @details 与构造函数走同一条 formatter 选择路径（applyFormatter），
         *          因此配置里的 color 开关与运行期切换行为完全一致。
         * @param enabled 是否启用彩色输出
         */
        void setColorEnabled(bool enabled);

    private:
        /**
         * @brief 按当前颜色开关与终端能力选择 formatter
         * @details 这是 m_colorEnabled 的唯一消费点：终端不支持 ANSI 序列时退回纯文本 formatter，
         *          避免把转义序列打成乱码。调用方必须已持有 m_mutex。
         */
        void applyFormatter();

        bool       m_colorEnabled; ///< 是否启用彩色输出（唯一真相源：构造与运行期切换都写它，applyFormatter 读它）
        std::mutex m_mutex;        ///< 保护控制台输出与 formatter 切换的互斥锁

        /// 复用的行缓冲：格式化结果容量恰等于长度，直接给它追加换行必然再取一块堆并整行搬一次。
        /// 与 FileSink 的 m_lineBuffer 同一套路——留容量，稳态下拼行不碰堆。由 m_mutex 保护
        std::string m_lineBuffer;
    };
} // namespace AsynGyanis::Base
