/**
 * @file RollingFileSink.h
 * @brief 支持按大小或按时间滚动的文件日志输出目标
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/FileSink.h"
#include "Base/Log/LogSink.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 滚动策略枚举
     */
    enum class RollingPolicy
    {
        Size,  ///< 按文件大小滚动
        Daily, ///< 按天滚动
        Hourly ///< 按小时滚动
    };

    /**
     * @brief 支持按大小/时间滚动的文件 Sink
     *
     * @details 内部始终持有一个活动 FileSink；写日志前先按策略判断是否需要滚动，
     *          滚动时关闭活动文件、重命名为备份并新建活动文件，随后清理超出保留上限的旧备份。
     * @note 按时间滚动时文件名会插入时间后缀，备份命名规则为「主名.后缀.扩展名」。
     */
    class RollingFileSink : public LogSink
    {
    public:
        /**
         * @brief 构造滚动文件 Sink
         * @param baseFilename 基础文件名
         * @param directory 日志目录
         * @param policy 滚动策略
         * @param maximumSizeBytes 按大小滚动时的阈值（字节）
         * @param maximumBackupFiles 最大保留备份文件数
         */
        RollingFileSink(std::string baseFilename, std::filesystem::path directory, RollingPolicy policy, size_t maximumSizeBytes = 10 * 1024 * 1024,
                        size_t      maximumBackupFiles                                                                           = 10);

        /**
         * @brief 析构滚动文件 Sink 并刷新残留数据
         * @details 重写 LogSink 的虚析构：先刷新活动文件再释放，活动 Sink 的 unique_ptr
         *          随后自动析构并二次关闭文件，保证退出时不丢日志。
         */
        ~RollingFileSink() override;

        /**
         * @brief 写入日志前执行滚动检查
         * @details 重写 LogSink::write()：在互斥锁内先做滚动判定，再委派给活动 FileSink，
         *          因此滚动与写入之间不会出现文件名切换竞态。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

        /**
         * @brief 刷新当前活动文件缓冲
         * @details 重写 LogSink::flush()：持锁转发给活动 FileSink；活动 Sink 为空时静默返回。
         */
        void flush() override;

    private:
        /**
         * @brief 根据策略判断并执行日志文件滚动
         */
        void checkAndRoll();

        /**
         * @brief 获取当前活动日志文件完整路径
         * @return std::filesystem::path 活动日志文件路径
         */
        [[nodiscard]] std::filesystem::path getCurrentFilename() const;

        /**
         * @brief 生成按天或按小时滚动时的时间后缀
         * @return std::string 时间后缀字符串
         */
        [[nodiscard]] std::string generateTimestampSuffix() const;

        /**
         * @brief 清理超出保留上限的历史备份文件
         */
        void cleanupOldFiles() const;

        std::string           m_baseFilename;       ///< 基础文件名
        std::filesystem::path m_directory;          ///< 日志目录
        RollingPolicy         m_policy;             ///< 滚动策略
        size_t                m_maximumSizeBytes;   ///< 按大小滚动阈值（字节）
        size_t                m_maximumBackupFiles; ///< 最大备份文件数

        std::unique_ptr<FileSink> m_currentSink;   ///< 当前活动文件 Sink
        std::string               m_currentSuffix; ///< 当前时间后缀（按时间滚动时使用）
        std::mutex                m_mutex;         ///< 保护滚动逻辑的互斥锁
    };
} // namespace AsynGyanis::Base
