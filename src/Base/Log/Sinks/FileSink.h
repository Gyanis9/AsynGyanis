/**
 * @file FileSink.h
 * @brief 文件日志输出目标
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Sinks/LogSink.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 将日志写入文件的 Sink
     *
     * @details 默认以追加模式打开，父目录不存在时自动创建；所有写入与刷新都在互斥锁内完成。
     *          滚动文件 Sink（RollingFileSink）以「一个活动 FileSink」的形式复本类。
     */
    class FileSink : public LogSink
    {
    public:
        /**
         * @brief 构造文件 Sink，并按模式打开目标日志文件
         * @param filePath 日志文件路径
         * @param truncate 是否以截断模式打开文件，false 为追加模式
         * @throws std::runtime_error 文件无法打开
         */
        explicit FileSink(std::filesystem::path filePath, bool truncate = false);

        /**
         * @brief 析构文件 Sink 并确保缓存落盘
         * @details 重写 LogSink 的虚析构：先刷新再由成员析构关闭 ofstream，
         *          避免进程退出时丢失缓冲区内的最后若干行。
         */
        ~FileSink() override;

        /**
         * @brief 将日志事件写入文件
         * @details 重写 LogSink::write()：持锁把版式直接渲进本 Sink 的行缓冲、续上换行一次写出；
         *          文件已被关闭时静默丢弃，不向调用方抛出异常。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

        /**
         * @brief 写入一行已完成格式化的文本
         * @details 供需要自行掌握「本行字节数」的调用方（如按大小滚动的 RollingFileSink）
         *          复用本 Sink 的落盘与加锁逻辑：RollingFileSink 用它完成字节累计，
         *          从而不必每行都 flush 后 stat 一次真实文件大小。
         * @param line 已格式化的单行文本（不含换行）
         * @return std::size_t 落到磁盘上的字节数（含行尾换行，Windows 文本模式下按 "\r\n" 计）；
         *         文件未打开或流已失效时返回 0
         */
        std::size_t writeLine(std::string_view line);

        /**
         * @brief 刷新文件缓冲区
         * @details 重写 LogSink::flush()：持锁调用 ofstream::flush，文件未打开时直接返回。
         */
        void flush() override;

        /**
         * @brief 重新打开并切换输出文件路径
         * @param newPath 新日志文件路径
         */
        void reopen(const std::filesystem::path &newPath);

    private:
        /**
         * @brief 把 m_lineBuffer 里已有的正文续上换行写出，并如实报告落了多少字节
         * @details 调用方必须已持有 m_mutex。write() 与 writeLine() 共用这一段：
         *          两者只差在正文的来处（本 Sink 的格式化器 vs 调用方交来的现成文本）。
         * @return std::size_t 写入字节数（含换行）；文件未打开或流已失效时返回 0
         */
        std::size_t writePreparedLineLocked();

        std::filesystem::path m_filePath;   ///< 当前日志文件路径
        std::ofstream         m_file;       ///< 日志文件输出流
        std::string           m_lineBuffer; ///< 写入用的行缓冲：拼接换行后整行一次写出，仅 write/writeLine 在互斥锁内复用
        std::mutex            m_mutex;      ///< 保护文件写入的互斥锁
        bool                  m_hasReportedWriteFailure{false}; ///< 本轮连续写失败是否已上报（避免每条日志都写一次标准错误）
    };
} // namespace AsynGyanis::Base
