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
#include <ostream>
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
         *          流写入失败不抛异常，只置 badbit 并让后续每行都成空操作，因此写完要看流状态：
         *          本次故障的第一条会经另一条标准流出声。
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

        /**
         * @brief 流已失效时出一行诊断，同一次故障只出一次
         * @details 控制台没有「重新打开」这条路，因此诊断之后仍会持续静默——但静默得有话，
         *          否则「日志不打了」与「没有日志」在现场分不开。报给另一条标准流：本 Sink 就是
         *          日志出口，拿根日志器报自己等于让 write() 递归回来。调用方必须已持有 m_mutex。
         * @param stream 刚写过的那条流
         * @param hasReported 该流的「本次故障是否已报」标记，成功写一次就重新武装
         * @param streamLabel 流名，只用于文案定位（stdout / stderr）
         */
        static void reportStreamFailureOnceLocked(std::ostream &stream, bool &hasReported, const char *streamLabel);

        bool       m_colorEnabled;                     ///< 是否启用彩色输出（唯一真相源：构造与运行期切换都写它，applyFormatter 读它）
        bool       m_hasReportedStdoutFailure = false; ///< std::cout 本次故障是否已报（由 m_mutex 保护）
        bool       m_hasReportedStderrFailure = false; ///< std::cerr 本次故障是否已报（由 m_mutex 保护）
        std::mutex m_mutex;                            ///< 保护控制台输出与 formatter 切换的互斥锁

        /// 复用的行缓冲：格式化结果容量恰等于长度，直接给它追加换行必然再取一块堆并整行搬一次。
        /// 与 FileSink 的 m_lineBuffer 同一套路——留容量，稳态下拼行不碰堆。由 m_mutex 保护
        std::string m_lineBuffer;
    };
} // namespace AsynGyanis::Base
