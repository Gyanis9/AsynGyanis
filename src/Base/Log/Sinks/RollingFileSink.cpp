#include "Base/Log/Sinks/RollingFileSink.h"
#include "Platform/System/PlatformTime.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 按时间滚动时，同一周期备份名冲突的最大试探次数
        constexpr int kMaximumSuffixCollisions = 1000;

        /**
         * @brief 按大小滚动前把已有备份整体向后顺移一位，为空出 1 号位
         * @param directory 日志目录
         * @param namePart 去掉扩展名的基础文件名
         * @param extensionPart 含点号的扩展名，无扩展名时为空串
         * @param maximumBackupFiles 允许保留的备份数量上限
         * @note 必须从最大序号倒序移动，正序会把后一个备份直接覆盖
         */
        void rotateSizeBackups(const std::filesystem::path &directory, const std::string &namePart, const std::string &extensionPart, const std::size_t maximumBackupFiles)
        {
            const int highestIndex = maximumBackupFiles == 0 ? 1 : static_cast<int>(maximumBackupFiles);
            for (int index = highestIndex; index >= 1; --index)
            {
                std::error_code             errorCode;
                const std::filesystem::path sourcePath = directory / std::format("{}.{}{}", namePart, index, extensionPart);
                if (!std::filesystem::exists(sourcePath, errorCode) || errorCode)
                {
                    continue;
                }
                // 顺移失败时保持原文件不动，后续 cleanupOldFiles 仍会按上限收敛
                const std::filesystem::path targetPath = directory / std::format("{}.{}{}", namePart, index + 1, extensionPart);
                std::filesystem::rename(sourcePath, targetPath, errorCode);
            }
        }
    } // namespace

    RollingFileSink::RollingFileSink(std::string  baseFilename, std::filesystem::path directory, const RollingPolicy policy, const size_t maximumSizeBytes,
                                     const size_t maximumBackupFiles) :
        m_baseFilename(std::move(baseFilename)), m_directory(std::move(directory)), m_policy(policy), m_maximumSizeBytes(maximumSizeBytes), m_maximumBackupFiles(maximumBackupFiles)
    {
        std::filesystem::create_directories(m_directory);
        if (m_policy == RollingPolicy::Daily || m_policy == RollingPolicy::Hourly)
        {
            m_currentSuffix = generateTimestampSuffix();
        }
        m_currentSink = std::make_unique<FileSink>(getCurrentFilename());
    }

    RollingFileSink::~RollingFileSink()
    {
        RollingFileSink::flush();
    }

    void RollingFileSink::write(const LogEvent &event)
    {
        std::lock_guard lock(m_mutex);
        checkAndRoll();
        if (m_currentSink)
        {
            m_currentSink->write(event);
        }
    }

    void RollingFileSink::flush()
    {
        std::lock_guard lock(m_mutex);
        if (m_currentSink)
        {
            m_currentSink->flush();
        }
    }

    void RollingFileSink::checkAndRoll()
    {
        bool shouldRoll = false;
        if (m_policy == RollingPolicy::Daily || m_policy == RollingPolicy::Hourly)
        {
            if (const std::string newSuffix = generateTimestampSuffix(); newSuffix != m_currentSuffix)
            {
                shouldRoll      = true;
                m_currentSuffix = newSuffix;
            }
        }
        if (m_policy == RollingPolicy::Size && m_currentSink)
        {
            // 先将缓冲数据落盘，file_size 才能反映真实大小
            m_currentSink->flush();
            std::error_code errorCode;
            if (const auto fileSize = std::filesystem::file_size(getCurrentFilename(), errorCode); !errorCode && fileSize >= m_maximumSizeBytes)
            {
                shouldRoll = true;
            }
        }
        if (shouldRoll)
        {
            // 先关闭当前文件（Windows 不允许重命名打开中的文件）
            m_currentSink.reset();
            const auto currentPath = getCurrentFilename();
            if (std::filesystem::exists(currentPath))
            {
                const auto  dotPosition = m_baseFilename.rfind('.');
                std::string namePart;
                std::string extensionPart;
                if (dotPosition != std::string::npos)
                {
                    namePart      = m_baseFilename.substr(0, dotPosition);
                    extensionPart = m_baseFilename.substr(dotPosition);
                } else
                {
                    namePart = m_baseFilename;
                }

                std::string backupFilename;
                if (m_policy == RollingPolicy::Size)
                {
                    // 已有备份整体向后顺移一位，空出 .1，避免序号耗尽后覆盖最旧备份
                    rotateSizeBackups(m_directory, namePart, extensionPart, m_maximumBackupFiles);
                    backupFilename = std::format("{}.1{}", namePart, extensionPart);
                } else
                {
                    backupFilename = std::format("{}.{}{}", namePart, m_currentSuffix, extensionPart);
                    // 同一周期内已有备份（例如进程重启后再次滚动）时追加序号，不覆盖历史内容
                    for (int collisionIndex = 2; std::filesystem::exists(m_directory / backupFilename) && collisionIndex <= kMaximumSuffixCollisions;
                         ++collisionIndex)
                    {
                        backupFilename = std::format("{}.{}.{}{}", namePart, m_currentSuffix, collisionIndex, extensionPart);
                    }
                }

                std::error_code renameError;
                std::filesystem::rename(currentPath, m_directory / backupFilename, renameError);
                if (renameError)
                {
                    // 重命名失败（目标目录只读、跨卷等）时清空当前文件：
                    // 否则滚动条件恒成立，活动日志会在原地无限增长
                    std::error_code truncateError;
                    std::filesystem::resize_file(currentPath, 0, truncateError);
                }
            }
            m_currentSink = std::make_unique<FileSink>(currentPath);
            cleanupOldFiles();
        }
    }

    std::filesystem::path RollingFileSink::getCurrentFilename() const
    {
        std::string filename = m_baseFilename;
        if (m_policy == RollingPolicy::Daily || m_policy == RollingPolicy::Hourly)
        {
            if (const auto dotPosition = filename.rfind('.'); dotPosition != std::string::npos)
            {
                filename.insert(dotPosition, "." + m_currentSuffix);
            } else
            {
                filename += "." + m_currentSuffix;
            }
        }
        return m_directory / filename;
    }

    std::string RollingFileSink::generateTimestampSuffix() const
    {
        const auto    now       = std::chrono::system_clock::now();
        const auto    timeValue = std::chrono::system_clock::to_time_t(now);
        const std::tm localTime = AsynGyanis::Platform::PlatformTime::localTime(timeValue);
        if (m_policy == RollingPolicy::Daily)
        {
            return std::format("{:04d}-{:02d}-{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday);
        }
        return std::format("{:04d}-{:02d}-{:02d}_{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour);
    }

    void RollingFileSink::cleanupOldFiles() const
    {
        // m_maximumBackupFiles == 0 表示不保留任何备份，备份列表仍需要构建并全部清理
        std::vector<std::filesystem::path> backupFiles;
        const auto                         dotPosition = m_baseFilename.rfind('.');
        const std::string                  namePart    = (dotPosition != std::string::npos) ? m_baseFilename.substr(0, dotPosition) : m_baseFilename;
        const std::string                  activeName  = getCurrentFilename().filename().string();

        for (std::error_code errorCode; const auto &entry: std::filesystem::directory_iterator(m_directory, errorCode))
        {
            if (errorCode)
            {
                break;
            }
            // 仅匹配 "namePart." 前缀的备份文件，且排除当前活动文件
            if (const auto filename = entry.path().filename().string(); filename != activeName && filename.rfind(namePart + ".", 0) == 0)
            {
                backupFiles.push_back(entry.path());
            }
        }
        if (backupFiles.size() > m_maximumBackupFiles)
        {
            std::ranges::sort(backupFiles,
                              [](const auto &left, const auto &right)
                              {
                                  return std::filesystem::last_write_time(left) > std::filesystem::last_write_time(right);
                              });
            for (size_t index = m_maximumBackupFiles; index < backupFiles.size(); ++index)
            {
                std::error_code removeErrorCode;
                std::filesystem::remove(backupFiles[index], removeErrorCode);
            }
        }
    }
} // namespace AsynGyanis::Base
