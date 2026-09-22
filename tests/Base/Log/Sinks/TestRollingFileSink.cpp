// RollingFileSink 单元测试：按大小滚动、按时间滚动的命名、备份上限与析构安全性

#include "Base/Log/Sinks/RollingFileSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <system_error>
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
         * @brief 构造字段齐备、各字段取值固定的日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "rolling message")
        {
            return {
                    level, TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789), "tid-223344",
                    SourceLocation("rolling_fixture.cpp", 9137, "rollingTestFunction"),
                    "rolling_logger", std::move(message)
            };
        }

        /**
         * @brief 读取文本文件全部内容并统一换行符
         * @details 文件类 Sink 以文本模式写入，Windows 上会把 '\n' 变成 "\r\n"，
         *          故按二进制读取后归一化换行，保证跨平台行数统计一致。
         * @return 无法打开文件时返回空串
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
         */
        std::size_t countLines(const fs::path &filePath)
        {
            const std::string content = readWholeFile(filePath);
            return static_cast<std::size_t>(std::ranges::count(content, '\n'));
        }

        /**
         * @brief 收集目录中文件名匹配正则的全部文件路径
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
        // 最新一条事件必须落在活动文件里，而不是被滚动进备份
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

    TEST(RollingFileSink, CleanupRemovesOldestBackupsByWriteTime)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_OldestBackup");
        const fs::path                        directory = temporaryDirectory.path();

        // 活动文件预置内容已超过阈值：构造后的第一次写入即触发滚动，从而走到清理逻辑
        ASSERT_TRUE(temporaryDirectory.writeFile("cap.log", std::string(256, 'x')));
        ASSERT_TRUE(temporaryDirectory.writeFile("cap.1.log", "oldest_backup\n"));
        ASSERT_TRUE(temporaryDirectory.writeFile("cap.2.log", "middle_backup\n"));

        // 把两份备份的最后写入时间拉开，使「最旧」有唯一解：
        // 排序必须在读好时间戳之后进行，比较器里再读时间戳出错时会抛异常（sort 比较器抛出是未定义行为）
        const auto      baseTime = fs::file_time_type::clock::now();
        std::error_code timeError;
        fs::last_write_time(directory / "cap.1.log", baseTime - std::chrono::minutes(10), timeError);
        ASSERT_FALSE(timeError);
        fs::last_write_time(directory / "cap.2.log", baseTime - std::chrono::minutes(5), timeError);
        ASSERT_FALSE(timeError);

        RollingFileSink sink("cap.log", directory, RollingPolicy::Size, 128, 2);
        sink.write(makeEvent(LogLevel::Info, "trigger_roll"));
        sink.flush();

        // 顺移后三份备份按时间从新到旧为 cap.1(本次滚动) → cap.3(原 cap.2) → cap.2(原 cap.1)，
        // 上限 2 表示最旧的那份必须被删除
        EXPECT_TRUE(fs::exists(directory / "cap.1.log"));
        EXPECT_TRUE(fs::exists(directory / "cap.3.log"));
        EXPECT_FALSE(fs::exists(directory / "cap.2.log")) << "清理应按最后写入时间删除最旧的备份";
        EXPECT_EQ(collectFilesMatching(directory, R"(cap\.\d+\.log)").size(), 2u);
    }

    /**
     * @brief 清理要认得出带时间戳的备份名（周期策略留下的那些），同时不误删同前缀的无关文件
     * @details 周期策略的备份名形如 app.log.2026-09-15 或 app.log.2026-09-15_07，时间戳里带
     *          '-' 与 '_'。备份名判定若只认数字与点，这类备份就永远进不了清理名单——
     *          max_backup 形同虚设，日志目录无界增长。用例用大小策略触发清理：
     *          清理只看备份名形态，与触发它的是哪种策略无关
     */
    TEST(RollingFileSink, TimeStampedBackupsAreRecognizedByCleanup)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_TimeBackups");
        const fs::path                        directory = temporaryDirectory.path();

        ASSERT_TRUE(temporaryDirectory.writeFile("mixed.2026-09-14.log", "oldest_backup\n"));
        ASSERT_TRUE(temporaryDirectory.writeFile("mixed.2026-09-15_07.log", "newer_backup\n"));
        // 同前缀但非备份的无关文件：清理必须放过它，误删就是数据丢失
        ASSERT_TRUE(temporaryDirectory.writeFile("mixed.audit.log", "unrelated\n"));

        // 把两份备份的最后写入时间拉开，使「最旧」有唯一解（两份都是刚写入的，时间本就相同）
        const auto      baseTime = fs::file_time_type::clock::now();
        std::error_code timeError;
        fs::last_write_time(directory / "mixed.2026-09-14.log", baseTime - std::chrono::minutes(10), timeError);
        ASSERT_FALSE(timeError);
        fs::last_write_time(directory / "mixed.2026-09-15_07.log", baseTime - std::chrono::minutes(5), timeError);
        ASSERT_FALSE(timeError);

        // 活动文件预置内容已超过阈值：构造后的第一次写入即触发滚动，从而走到清理逻辑
        ASSERT_TRUE(temporaryDirectory.writeFile("mixed.log", std::string(256, 'x')));
        RollingFileSink sink("mixed.log", directory, RollingPolicy::Size, 128, 1);
        sink.write(makeEvent(LogLevel::Info, "trigger_roll"));
        sink.flush();

        // 上限 1 只留得下本次滚动产生的那份，两份时间戳备份都必须被清掉
        EXPECT_FALSE(fs::exists(directory / "mixed.2026-09-14.log")) << "带时间戳的备份没有被清理";
        EXPECT_FALSE(fs::exists(directory / "mixed.2026-09-15_07.log")) << "带下划线的时间戳同样要认得出";
        EXPECT_TRUE(fs::exists(directory / "mixed.1.log")) << "本次滚动产生的备份不该被清掉";
        EXPECT_TRUE(fs::exists(directory / "mixed.audit.log")) << "同前缀的无关文件被误删了";
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

        EXPECT_EQ(totalLines, static_cast<std::size_t>(kthreadCount) * kwritesPerThread);
    }

    /**
     * @brief 活动文件一度打不开时，故障撤掉后日志必须恢复生产
     * @details 钉的是「一次重开失败即永久停产」：滚动先把活动 Sink 置空，重开再失败时它就留在
     *          空指针上，而按大小的滚动判据又要求活动 Sink 非空——再没有别的触发点，之后每一行
     *          都被无声丢弃直到进程退出
     */
    TEST(RollingFileSink, ResumesWritingAfterTheActiveFileFailedToReopen)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_Reopen");
        const fs::path                        logDirectory = temporaryDirectory.path();
        const fs::path                        activePath   = logDirectory / "back.log";

        // 故障源要能在「活动文件正被 Sink 开着」时就位，因此不能用删目录那类手法：
        // 滚动会先关闭句柄再重开，名字到那时又自由了。这里两件事叠加：
        //   1) 活动文件只读 -> 重开的写打开必失败；
        //   2) 1 号与 2 号备份位各放一个非空目录 -> 滚动既挪不动 1 号位、也重命名不进 1 号位，
        //      于是那份只读文件被留在原地，只读位就真的挡住了重开
        constexpr std::size_t kzeroBackupCount = 0;
        RollingFileSink       sink("back.log", logDirectory, RollingPolicy::Size, 1, kzeroBackupCount);
        sink.write(makeEvent(LogLevel::Info, "before_outage_payload"));
        sink.flush();
        ASSERT_NE(readWholeFile(activePath).find("before_outage_payload"), std::string::npos)
                << "基线：第一行就要落进活动文件";

        for (const std::string blockerName: {"back.1.log", "back.2.log"})
        {
            ASSERT_NO_THROW(fs::create_directory(logDirectory / blockerName));
            ASSERT_NO_THROW(fs::create_directory(logDirectory / blockerName / "inner"));
        }
        ASSERT_NO_THROW(fs::permissions(activePath,
                                        fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                                        fs::perm_options::replace));
        {
            // 探针：只读位挡不住写打开的平台（容器里以 root 跑）没有可用的故障源，如实跳过
            const std::ofstream probe(activePath, std::ios::out | std::ios::app);
            if (probe.is_open())
            {
                GTEST_SKIP() << "本平台以当前身份运行时，只读位不阻止写打开，无法注入重开失败";
            }
        }

        // 阈值 1 字节 -> 这一行必然进入滚动，进而走到重开并抛出
        EXPECT_THROW(sink.write(makeEvent(LogLevel::Info, "lost_during_outage_payload")), std::exception);

        // 撤掉故障：只读位解除即可（备份位那两个目录留着不影响本行的落盘位置断言）
        ASSERT_NO_THROW(fs::permissions(activePath, fs::perms::all, fs::perm_options::add));
        EXPECT_NO_THROW(sink.write(makeEvent(LogLevel::Info, "after_outage_payload")));
        sink.flush();

        EXPECT_NE(readWholeFile(activePath).find("after_outage_payload"), std::string::npos)
                << "重开失败一次之后本 Sink 永久停产：故障撤掉后的日志再也没落过盘";
    }

    /**
     * @brief 备份数填成超出 int 的天文数字时，编号备份仍要逐级顺移而不是被盖掉
     * @details 钉的是顺移序号的类型：它一度被转成 int，3e9 这类取值回绕成负数后整个顺移循环
     *          一步不跑，随后的重命名直接覆盖 1 号备份——「保留 N 份」实际只剩 1 份，
     *          且丢掉的是最旧那一份的历史内容
     */
    TEST(RollingFileSink, ShiftsEveryBackupWhenTheLimitWouldWrapAroundAsInt)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Rolling_WrapLimit");
        const fs::path                        logDirectory = temporaryDirectory.path();

        // 3e9 仍在 size_t 范围内，但按 int 解释是 -1294967296：顺移循环的起点直接小于 1
        RollingFileSink sink("wrap.log", logDirectory, RollingPolicy::Size, 1, 3000000000ULL);

        sink.write(makeEvent(LogLevel::Info, "first_generation_payload"));
        sink.write(makeEvent(LogLevel::Info, "second_generation_payload"));
        sink.write(makeEvent(LogLevel::Info, "third_generation_payload"));
        sink.flush();

        // 阈值 1 字节 ⇒ 每行都滚动：三代内容应分别落在 2 号备份、1 号备份与活动文件里
        const fs::path secondBackup = logDirectory / "wrap.2.log";
        const fs::path firstBackup  = logDirectory / "wrap.1.log";
        ASSERT_TRUE(fs::exists(secondBackup)) << "顺移一步没跑：2 号备份根本没被生成";
        EXPECT_NE(readWholeFile(secondBackup).find("first_generation_payload"), std::string::npos)
                << "最旧的备份被就地覆盖：备份顺移没有跑到";
        EXPECT_NE(readWholeFile(firstBackup).find("second_generation_payload"), std::string::npos);
        EXPECT_NE(readWholeFile(logDirectory / "wrap.log").find("third_generation_payload"), std::string::npos);
    }
} // namespace AsynGyanis::Base
