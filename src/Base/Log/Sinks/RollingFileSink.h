/**
 * @file RollingFileSink.h
 * @brief 支持按大小或按时间滚动的文件日志输出目标
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Sinks/FileSink.h"
#include "Base/Log/Sinks/LogSink.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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
         * @brief 保留备份数的最大可用值
         *
         * @details 按大小滚动前要把已有备份整体顺移一位，代价与这个上限成正比（每个序号一次
         *          存在性探测）。不设上限的话，一个填错的配置就能让每次滚动做出上千万次目录项
         *          查询、且全程握着本 Sink 的锁——日志系统反过来把进程拖垮。构造函数自行钳制，
         *          配置侧共用同一个常量，两处解析必须一致。
         */
        static constexpr std::size_t kMaximumBackupFileCount = 4096U;

        /**
         * @brief 构造滚动文件 Sink
         * @param baseFilename 基础文件名（路径刻度）。含目录段时**只取其中的文件名段**，活动文件与
         *                     备份一律落在 directory 参数所指的那一层——两半分家时备份清不掉。
         *                     由 UTF-8 配置文本进来时要先经 `Platform::FileSystem::pathFromUtf8`，
         *                     直接交窄串会在 Windows 上过一遍本地代码页
         * @param directory 日志目录
         * @param policy 滚动策略
         * @param maximumSizeBytes 按大小滚动时的阈值（字节）
         * @param maximumBackupFiles 最大保留备份文件数，超过 kMaximumBackupFileCount 时按该上限钳制
         */
        RollingFileSink(std::filesystem::path baseFilename,
                        std::filesystem::path directory,
                        RollingPolicy         policy,
                        size_t                maximumSizeBytes   = 10 * 1024 * 1024,
                        size_t                maximumBackupFiles = 10);

        /**
         * @brief 析构滚动文件 Sink 并刷新残留数据
         * @details 重写 LogSink 的虚析构：先刷新活动文件再释放，活动 Sink 的 unique_ptr
         *          随后自动析构并二次关闭文件，保证退出时不丢日志。
         */
        ~RollingFileSink() override;

        /**
         * @brief 写入日志前执行滚动检查
         * @details 重写 LogSink::write()：在互斥锁内先做滚动判定，再委派给活动 FileSink，
         *          因此滚动与写入之间不会出现文件名切换竞态。上一次重开活动文件失败时，
         *          这里按重试节奏再开一次，否则本 Sink 会不打一声招呼地永久停产。
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
         * @details 判据全部是内存计数/时间比较，不再每写一行就 flush + stat 真实文件大小：
         *          按大小用累计写入字节数，按时间用缓存的下一个周期边界时刻。
         */
        void checkAndRoll();

        /**
         * @brief 获取当前活动日志文件完整路径
         * @return std::filesystem::path 活动日志文件路径
         */
        [[nodiscard]] std::filesystem::path getCurrentFilename() const;

        /**
         * @brief 取当前时刻所属周期的文件名后缀
         * @details 换算在 `Detail::rollingPeriodSuffix()`，本成员只负责带上本 Sink 的策略
         * @return std::string 时间后缀字符串
         */
        [[nodiscard]] std::string generateTimestampSuffix() const;

        /**
         * @brief 计算当前时间之后、下一个需要检查滚动的时间点
         * @details 换算在 `Detail::nextRollingPeriodBoundary()`，本成员只负责带上本 Sink 的策略；
         *          用本地时间的时分秒推算到下一个整点（Hourly）或整日（Daily），
         *          因此正常写入路径上既不做本地时间转换也不做字符串格式化。
         * @param timeValue 当前时间（time_t）
         * @return std::time_t 严格晚于 timeValue 的下一个周期边界
         */
        [[nodiscard]] std::time_t nextPeriodBoundary(std::time_t timeValue) const noexcept;

        /**
         * @brief 重新打开活动文件并重置按大小的字节累计
         * @details 新建活动文件后需要把累计值重置为「文件当前真实大小」——
         *          追加模式下目标文件可能已存在（如进程重启），此时累计值必须从既有大小起算
         */
        void reopenActiveFile();

        /**
         * @brief 清理超出保留上限的历史备份文件
         */
        void cleanupOldFiles() const;

        std::filesystem::path m_baseFilename;       ///< 基础文件名（路径刻度：备份名与活动名都按它拼）
        std::filesystem::path m_directory;          ///< 日志目录
        RollingPolicy         m_policy;             ///< 滚动策略
        size_t                m_maximumSizeBytes;   ///< 按大小滚动阈值（字节）
        size_t                m_maximumBackupFiles; ///< 最大备份文件数

        std::unique_ptr<FileSink> m_currentSink;   ///< 当前活动文件 Sink
        std::string               m_currentSuffix; ///< 当前时间后缀（按时间滚动时使用）
        std::mutex                m_mutex;         ///< 保护滚动逻辑与上述计数的互斥锁

        /// 活动文件累计写入字节数（按大小滚动的判据）：
        /// 用自增计数替代「每行 flush + file_size」两次系统调用
        std::uintmax_t m_bytesInCurrentFile = 0;

        /// 下一个需要检查滚动的时间点（按时间滚动的判据）：
        /// 每行只做一次 time_t 比较，跨过边界才做本地时间转换与后缀格式化
        std::time_t m_nextPeriodBoundary = 0;

        /// 下一次允许重试重开活动文件的时刻（steady_clock）：
        /// 取默认构造值即「无冷却」，因此首次重开失败后下一行就立刻再试，不必等一个间隔
        std::chrono::steady_clock::time_point m_nextReopenAttempt{};

        /// 复用的行缓冲：本 Sink 用自己的 formatter 渲染到这里，再把视图交给活动文件的落盘逻辑，
        /// 稳态下拼一行不取堆（格式化器造结果串那两次省掉了）。由 m_mutex 保护
        std::string m_lineBuffer;
    };
} // namespace AsynGyanis::Base
