/**
 * @file FileSink.h
 * @brief 文件日志输出目标
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogSink.h"

#include <filesystem>
#include <fstream>
#include <mutex>

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
         * @details 重写 LogSink::write()：持锁格式化并追加换行；文件已被关闭时静默丢弃，
         *          不向调用方抛出异常。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

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
        std::filesystem::path m_filePath; ///< 当前日志文件路径
        std::ofstream         m_file;     ///< 日志文件输出流
        std::mutex            m_mutex;    ///< 保护文件写入的互斥锁
    };
} // namespace AsynGyanis::Base
