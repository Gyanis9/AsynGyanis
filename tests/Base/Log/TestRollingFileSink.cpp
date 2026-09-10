/**
 * @file TestRollingFileSink.cpp
 * @brief RollingFileSink 单元测试：按大小滚动、按时间滚动的命名、备份上限与析构安全性
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/RollingFileSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

#include "BaseTestSupport.h"

namespace AsynGyanis::Base
{
    namespace
    {
        namespace fs = std::filesystem;

        /**
         * @brief 构造字段齐备的日志事件
         * @param level 日志等级
         * @param message 日志消息
         * @return LogEvent 日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "rolling message")
        {
            return {
                    level, "2026-09-10 12:34:56.789", "tid-223344",
                    SourceLocation("rolling_fixture.cpp", 9137, "rollingTestFunction"),
                    "rolling_logger", std::move(message)
            };
        }

        /**
         * @brief 读取文本文件全部内容并统一换行符
         * @details 文件类 Sink 以文本模式写入，Windows 上会把 '\n' 变成 "\r\n"，
         *          故按二进制读取后归一化换行，保证跨平台行数统计一致。
         * @param filePath 目标文件路径
         * @return std::string 文件内容，无法打开时返回空串
         */
        std::string readWholeFile(const fs::path &filePath)
        {
            std::ifstream file(filePath, std::ios::in | std::ios::binary);
            if (!file.is_open())
            {
                return {};
            }
            std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{}};

            for (std::string::size_type position = content.find("\r\n"); position != std::string::npos;
                 position                        = content.find("\r\n", position))
            {
                content.erase(position, 1);
            }
            return content;
        }

        /**
         * @brief 统计单个文件的换行数量
         * @param filePath 目标文件路径
         * @return std::size_t 行数
         */
        std::size_t countLines(const fs::path &filePath)
        {
            const std::string content = readWholeFile(filePath);
            return static_cast<std::size_t>(std::ranges::count(content, '\n'));
        }

        /**
         * @brief 收集目录中文件名匹配正则的全部文件路径
         * @param directory 待扫描目录
         * @param pattern 文件名词法正则
         * @return std::vector<fs::path> 匹配到的文件路径
         */
        std::vector<fs::path> collectFilesMatching(const fs::path &directory, const std::string &pattern)
        {
            const std::regex      nameRegex(pattern);
            std::vector<fs::path> matched;

            for (std::error_code error; const fs::directory_entry &entry: fs::directory_iterator(directory, error))
            {
                if (error)
                {
                    break;
                }
                if (!entry.is_regular_file())
                {
                    continue;
                }
                if (const std::string filename = entry.path().filename().string(); std::regex_match(filename, nameRegex))
                {
                    matched.push_back(entry.path());
                }
            }
            return matched;
        }

        /**
         * @brief 写入指定数量的日志事件并刷新
         * @param sink 目标滚动 Sink
         * @param eventCount 事件数量
         * @param tokenPrefix 消息前缀，便于在文件中定位
         */
        void writeEvents(RollingFileSink &sink, const int eventCount, const std::string &tokenPrefix)
        {
            for (int index = 0; index < eventCount; ++index)
            {
                sink.write(makeEvent(LogLevel::Info, tokenPrefix + "_" + std::to_string(index) + "_padding_payload"));
            }
            sink.flush();
        }
    } // namespace

    // ============================================================================
    // 策略枚举
    // ============================================================================

    TEST(RollingFileSink, RollingPolicyKeepsStableUnderlyingValues)
    {
        EXPECT_EQ(static_cast<int>(RollingPolicy::Size), 0);
        EXPECT_EQ(static_cast<int>(RollingPolicy::Daily), 1);
        EXPECT_EQ(static_cast<int>(RollingPolicy::Hourly), 2);
    }

    // ============================================================================
    // 按大小滚动
    // ============================================================================

    TEST(RollingFileSink, SizePolicyConstructorCreatesActiveFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_SizeCreate");

        RollingFileSink sink("app.log", temporaryDirectory.path(), RollingPolicy::Size, 1024 * 1024, 5);

        EXPECT_TRUE(fs::is_regular_file(temporaryDirectory.path() / "app.log"));
    }

    TEST(RollingFileSink, SizePolicyWriteLandsInActiveFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_SizeWrite");
        const fs::path                        activePath = temporaryDirectory.path() / "quiet.log";

        RollingFileSink sink("quiet.log", temporaryDirectory.path(), RollingPolicy::Size, 10 * 1024 * 1024, 5);
        sink.write(makeEvent(LogLevel::Info, "rolling_write_check"));
        sink.flush();

        EXPECT_TRUE(fs::exists(activePath));
        EXPECT_NE(readWholeFile(activePath).find("rolling_write_check"), std::string::npos);
        // 未触发阈值时不应产生任何备份
        EXPECT_TRUE(collectFilesMatching(temporaryDirectory.path(), R"(quiet\.\d+\.log)").empty());
    }

    TEST(RollingFileSink, SizePolicyCreatesNumberedBackups)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("Rolling_SizeRoll");
        const fs::path                  activePath = temporaryDirectory.path() / "sized.log";

        RollingFileSink sink("sized.log", temporaryDirectory.path(), RollingPolicy::Size, 1024, 20);
        writeEvents(sink, 40, "sized_payload");

        ASSERT_TRUE(fs::exists(activePath));
        const std::vector<fs::path> backups = collectFilesMatching(temporaryDirectory.path(), R"(sized\.\d+\.log)");
        EXPECT_GE(backups.size(), 1u) << "size threshold 1KB should have rolled at least once";
        for (const fs::path &backup: backups)
        {
            EXPECT_GT(fs::file_size(backup), 0u) << backup;
        }
        // 活动文件与备份合起来应包含全部已写事件
        EXPECT_NE(readWholeFile(activePath).find("sized_payload_39"), std::string::npos);
    }

    TEST(RollingFileSink, SizePolicyPreservesEveryWrittenLine)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_SizeTotal");
        constexpr int                         keventCount = 100;

        // 备份上限远大于本次滚动次数，cleanupOldFiles 不会删除任何文件，可校验总量守恒
        RollingFileSink sink("total.log", temporaryDirectory.path(), RollingPolicy::Size, 256, 60);
        writeEvents(sink, keventCount, "total_payload");
        sink.flush();

        std::size_t totalLines = 0;
        for (const fs::path &file: collectFilesMatching(temporaryDirectory.path(), R"(total(\.\d+)?\.log)"))
        {
            totalLines += countLines(file);
        }

        EXPECT_EQ(totalLines, static_cast<std::size_t>(keventCount));
    }

    TEST(RollingFileSink, MaximumBackupFilesCapIsEnforced)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_SizeCap");
        constexpr std::size_t                 kmaximumBackupFiles = 2;

        RollingFileSink sink("capped.log", temporaryDirectory.path(), RollingPolicy::Size, 512, kmaximumBackupFiles);
        writeEvents(sink, 60, "capped_payload");

        const std::vector<fs::path> backups = collectFilesMatching(temporaryDirectory.path(), R"(capped\.\d+\.log)");
        EXPECT_LE(backups.size(), kmaximumBackupFiles) << "backup files exceed the configured cap";
        EXPECT_GE(backups.size(), 1u) << "rolling should have produced backups";
        EXPECT_TRUE(fs::exists(temporaryDirectory.path() / "capped.log"));
    }

    TEST(RollingFileSink, ZeroMaximumBackupFilesStillWritesLogs)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_ZeroCap");

        RollingFileSink sink("nobackup.log", temporaryDirectory.path(), RollingPolicy::Size, 512, 0);
        EXPECT_NO_THROW(writeEvents(sink, 20, "nobackup_payload"));

        EXPECT_TRUE(fs::exists(temporaryDirectory.path() / "nobackup.log"));
        // 上限为 0 表示不保留备份；容忍极端情况下删除被占用导致的残留
        EXPECT_LE(collectFilesMatching(temporaryDirectory.path(), R"(nobackup\.\d+\.log)").size(), 1u);
    }

    // ============================================================================
    // 按时间滚动
    // ============================================================================

    TEST(RollingFileSink, DailyPolicyUsesDateSuffixedFileName)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("Rolling_Daily");

        RollingFileSink sink("daily.log", temporaryDirectory.path(), RollingPolicy::Daily, 10 * 1024 * 1024, 5);
        EXPECT_NO_THROW(sink.write(makeEvent(LogLevel::Info, "daily_check")));
        sink.flush();

        const std::vector<fs::path> activeFiles = collectFilesMatching(temporaryDirectory.path(),
                                                                       R"(daily\.\d{4}-\d{2}-\d{2}\.log)");
        ASSERT_EQ(activeFiles.size(), 1u);
        EXPECT_NE(readWholeFile(activeFiles.front()).find("daily_check"), std::string::npos);
        // 时间后缀不应出现在基础文件名上
        EXPECT_FALSE(fs::exists(temporaryDirectory.path() / "daily.log"));
    }

    TEST(RollingFileSink, HourlyPolicyUsesDateHourSuffixedFileName)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("Rolling_Hourly");

        RollingFileSink sink("hourly.log", temporaryDirectory.path(), RollingPolicy::Hourly, 10 * 1024 * 1024, 5);
        EXPECT_NO_THROW(sink.write(makeEvent(LogLevel::Info, "hourly_check")));
        sink.flush();

        const std::vector<fs::path> activeFiles = collectFilesMatching(temporaryDirectory.path(),
                                                                       R"(hourly\.\d{4}-\d{2}-\d{2}_\d{2}\.log)");
        ASSERT_EQ(activeFiles.size(), 1u);
        EXPECT_NE(readWholeFile(activeFiles.front()).find("hourly_check"), std::string::npos);
    }

    TEST(RollingFileSink, DailyPolicyKeepsWritingIntoSameActiveFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_DailySame");

        RollingFileSink sink("stable.log", temporaryDirectory.path(), RollingPolicy::Daily, 10 * 1024 * 1024, 5);
        writeEvents(sink, 6, "stable_payload");

        const std::vector<fs::path> activeFiles = collectFilesMatching(temporaryDirectory.path(),
                                                                       R"(stable\.\d{4}-\d{2}-\d{2}\.log)");
        ASSERT_EQ(activeFiles.size(), 1u);
        EXPECT_EQ(countLines(activeFiles.front()), 6u);
    }

    TEST(RollingFileSink, ExtensionlessBaseFilenameAlsoRolls)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_NoExtension");
        const fs::path                        activePath = temporaryDirectory.path() / "plainlog";

        RollingFileSink sink("plainlog", temporaryDirectory.path(), RollingPolicy::Size, 512, 5);
        writeEvents(sink, 20, "plain_payload");

        EXPECT_TRUE(fs::exists(activePath));
        EXPECT_GE(collectFilesMatching(temporaryDirectory.path(), R"(plainlog\.\d+)").size(), 1u);
    }

    TEST(RollingFileSink, TimePolicyWithoutExtensionUsesSuffixedName)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_TimeNoExtension");

        RollingFileSink sink("plaindaily", temporaryDirectory.path(), RollingPolicy::Daily, 1024 * 1024, 5);
        sink.write(makeEvent(LogLevel::Info, "plain_daily_check"));
        sink.flush();

        EXPECT_EQ(collectFilesMatching(temporaryDirectory.path(), R"(plaindaily\.\d{4}-\d{2}-\d{2}$)").size(), 1u);
    }

    // ============================================================================
    // 生命周期与并发
    // ============================================================================

    TEST(RollingFileSink, FlushWithoutAnyRollDoesNotThrow)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_Flush");

        RollingFileSink sink("flush.log", temporaryDirectory.path(), RollingPolicy::Size, 1024 * 1024, 5);

        EXPECT_NO_THROW(sink.flush());
    }

    TEST(RollingFileSink, DestructionWithBackupsIsSafe)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_Destroy");

        EXPECT_NO_THROW(
                {
                RollingFileSink sink("destroy.log", temporaryDirectory.path(), RollingPolicy::Size, 512, 3);
                writeEvents(sink, 30, "destroy_payload");
                });

        EXPECT_TRUE(fs::exists(temporaryDirectory.path() / "destroy.log"));
    }

    TEST(RollingFileSink, ConcurrentWritesStayWithinWrittenLineCount)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_Concurrent");
        constexpr int                         kthreadCount     = 4;
        constexpr int                         kwritesPerThread = 25;
        std::vector<std::thread>              threads;

        // 备份上限足够大，滚动不会触发清理，因此行数必须等于全部写入次数
        RollingFileSink sink("shared.log", temporaryDirectory.path(), RollingPolicy::Size, 1024, 60);
        threads.reserve(kthreadCount);
        for (int index = 0; index < kthreadCount; ++index)
        {
            threads.emplace_back([&sink, index]
            {
                for (int inner = 0; inner < kwritesPerThread; ++inner)
                {
                    sink.write(makeEvent(LogLevel::Info,
                                         "shared" + std::to_string(index) + "_" + std::to_string(inner)));
                }
            });
        }
        for (std::thread &thread: threads)
        {
            thread.join();
        }
        sink.flush();

        std::size_t totalLines = 0;
        for (const fs::path &file: collectFilesMatching(temporaryDirectory.path(), R"(shared(\.\d+)?\.log)"))
        {
            totalLines += countLines(file);
        }

        // 备份上限足够大，因此行数必须等于全部写入次数
        EXPECT_EQ(totalLines, static_cast<std::size_t>(kthreadCount) * kwritesPerThread);
    }
} // namespace AsynGyanis::Base
