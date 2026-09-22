// FileSink 单元测试：父目录创建、追加/截断语义、reopen 切换路径与落盘完整性

#include "Base/Log/Sinks/FileSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/Formatters/LogFormatter.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

#include "BaseTestSupport.h"

namespace AsynGyanis::Base
{
    namespace
    {
        namespace fs = std::filesystem;

        /**
         * @brief 前缀标记格式化器桩，用于验证 Sink 使用了当前格式化器
         */
        class MarkerFormatter final : public LogFormatter
        {
        public:
            /**
             * @brief 使用给定前缀构造格式化器
             */
            explicit MarkerFormatter(std::string prefix) :
                m_prefix(std::move(prefix))
            {
            }

            /**
             * @brief 输出「前缀:消息」
             * @details 重写 LogFormatter::format()：版式极简，便于在文件内容中直接检索前缀。
             */
            std::string format(const LogEvent &event) override
            {
                return m_prefix + ":" + event.message;
            }

        private:
            std::string m_prefix; ///< 输出前缀标记
        };

        /**
         * @brief 构造字段齐备、各字段取值固定的日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "file message")
        {
            return {
                    level, TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789), "tid-990011",
                    SourceLocation("file_sink_fixture.cpp", 7301, "fileSinkTestFunction"),
                    "file_logger", std::move(message)
            };
        }

        /**
         * @brief 读取文本文件全部内容并统一换行符
         * @details FileSink 以文本模式写文件，Windows 上会把 '\n' 变成 "\r\n"，
         *          因此这里按二进制读取后再归一化换行，保证跨平台的逐行断言一致。
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
         * @brief 统计文件中的换行数量
         */
        std::size_t countLines(const fs::path &filePath)
        {
            const std::string content = readWholeFile(filePath);
            return static_cast<std::size_t>(std::ranges::count(content, '\n'));
        }

        /**
         * @brief 判断文件内容是否包含子串
         */
        bool fileContains(const fs::path &filePath, const std::string &needle)
        {
            return readWholeFile(filePath).find(needle) != std::string::npos;
        }
    } // namespace

    // ============================================================================
    // 构造与目录创建
    // ============================================================================

    TEST(FileSink, ConstructionCreatesTargetFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Create");
        const fs::path                        logPath = temporaryDirectory.path() / "app.log";

        FileSink sink(logPath);

        EXPECT_TRUE(fs::is_regular_file(logPath));
    }

    TEST(FileSink, ConstructionCreatesMissingParentDirectories)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Parent");
        const fs::path                  logPath = temporaryDirectory.path() / "nested" / "deeper" / "app.log";

        FileSink sink(logPath);

        ASSERT_TRUE(fs::is_directory(logPath.parent_path()));
        EXPECT_TRUE(fs::is_regular_file(logPath));
    }

    TEST(FileSink, ConstructionReportsChineseErrorWhenParentDirectoryCannotBeCreated)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileSink_BadParent");
        // 父路径被一个普通文件占住：create_directories 必然失败。
        // 这里必须抛出带中文诊断的 runtime_error，而不是让 std::filesystem_error 直接逃逸
        ASSERT_TRUE(temporaryDirectory.writeFile("blocker", "not a directory"));

        try
        {
            FileSink sink(temporaryDirectory.path() / "blocker" / "app.log");
            FAIL() << "父目录不可创建时应抛出异常";
        } catch (const std::runtime_error &error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find("无法打开日志文件"), std::string::npos) << message;
            EXPECT_NE(message.find("创建目录失败"), std::string::npos) << message;
        }
    }

    TEST(FileSink, Utf8FileNameIsAccepted)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Utf8");
        const fs::path                  logPath = temporaryDirectory.path() / "日志文件.log";

        FileSink sink(logPath);
        sink.write(makeEvent(LogLevel::Info, "中文日志内容"));
        sink.flush();

        ASSERT_TRUE(fs::exists(logPath));
        EXPECT_TRUE(fileContains(logPath, "中文日志内容")) << readWholeFile(logPath);
    }

    // ============================================================================
    // 写入与刷新
    // ============================================================================

    TEST(FileSink, WriteAppendsNewlineTerminatedLine)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Write");
        const fs::path                  logPath = temporaryDirectory.path() / "lines.log";

        FileSink sink(logPath);
        sink.write(makeEvent(LogLevel::Info, "line_one"));
        sink.write(makeEvent(LogLevel::Warn, "line_two"));
        sink.write(makeEvent(LogLevel::Error, "line_three"));
        sink.flush();

        const std::string content = readWholeFile(logPath);
        ASSERT_FALSE(content.empty());
        EXPECT_EQ(countLines(logPath), 3u);
        EXPECT_EQ(content.back(), '\n');
        EXPECT_TRUE(content.find("line_one") < content.find("line_two")) << content;
        EXPECT_TRUE(content.find("line_two") < content.find("line_three")) << content;
        EXPECT_TRUE(fileContains(logPath, "WARN ")) << content;
    }

    TEST(FileSink, WrittenLineMatchesDefaultFormatterOutput)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Format");
        const fs::path                  logPath = temporaryDirectory.path() / "format.log";
        const LogEvent                  event   = makeEvent(LogLevel::Info, "layout check");
        DefaultFormatter                expectedFormatter;

        FileSink sink(logPath);
        sink.write(event);
        sink.flush();

        EXPECT_EQ(readWholeFile(logPath), expectedFormatter.format(event) + "\n");
    }

    TEST(FileSink, InjectedFormatterShapesFileContent)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Formatter");
        const fs::path                  logPath = temporaryDirectory.path() / "marker.log";

        FileSink sink(logPath);
        sink.setFormatter(std::make_unique<MarkerFormatter>("FILE-MARK"));
        sink.write(makeEvent(LogLevel::Info, "marked"));
        sink.flush();

        EXPECT_EQ(readWholeFile(logPath), "FILE-MARK:marked\n");
    }

    TEST(FileSink, FlushWithoutWritesProducesEmptyFile)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Empty");
        const fs::path                  logPath = temporaryDirectory.path() / "empty.log";

        FileSink sink(logPath);
        EXPECT_NO_THROW(sink.flush());

        ASSERT_TRUE(fs::exists(logPath));
        EXPECT_EQ(fs::file_size(logPath), 0u);
    }

    TEST(FileSink, DestructionLeavesCompleteContentOnDisk)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Destructor");
        const fs::path                  logPath = temporaryDirectory.path() / "destroy.log";

        {
            FileSink sink(logPath);
            for (int index = 0; index < 20; ++index)
            {
                sink.write(makeEvent(LogLevel::Info, "pending_" + std::to_string(index)));
            }
            // 故意不调用 flush()，依赖析构刷新
        }

        EXPECT_EQ(countLines(logPath), 20u);
        EXPECT_TRUE(fileContains(logPath, "pending_19")) << readWholeFile(logPath);
    }

    // ============================================================================
    // 截断与追加模式
    // ============================================================================

    TEST(FileSink, TruncateModeDiscardsPreviousContent)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Truncate");
        const fs::path                  logPath = temporaryDirectory.path() / "trunc.log";

        {
            FileSink sink(logPath, true);
            sink.write(makeEvent(LogLevel::Info, "first_run_only"));
        }
        {
            FileSink sink(logPath, true);
            sink.write(makeEvent(LogLevel::Info, "second_run_only"));
        }

        EXPECT_TRUE(fileContains(logPath, "second_run_only")) << readWholeFile(logPath);
        EXPECT_FALSE(fileContains(logPath, "first_run_only")) << readWholeFile(logPath);
        EXPECT_EQ(countLines(logPath), 1u);
    }

    TEST(FileSink, AppendModeKeepsPreviousContent)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Append");
        const fs::path                  logPath = temporaryDirectory.path() / "append.log";

        {
            FileSink sink(logPath, true);
            sink.write(makeEvent(LogLevel::Info, "seeded_line"));
        }
        {
            FileSink sink(logPath, false);
            sink.write(makeEvent(LogLevel::Info, "appended_line"));
        }

        const std::string content = readWholeFile(logPath);
        EXPECT_TRUE(fileContains(logPath, "seeded_line")) << content;
        EXPECT_TRUE(fileContains(logPath, "appended_line")) << content;
        EXPECT_TRUE(content.find("seeded_line") < content.find("appended_line")) << content;
        EXPECT_EQ(countLines(logPath), 2u);
    }

    TEST(FileSink, DefaultModeKeepsPreexistingContent)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_DefaultMode");
        const fs::path                  logPath = temporaryDirectory.path() / "default.log";
        ASSERT_TRUE(temporaryDirectory.writeFile("default.log", "already on disk\n"));

        FileSink sink(logPath);
        sink.write(makeEvent(LogLevel::Info, "new line"));
        sink.flush();

        const std::string content = readWholeFile(logPath);
        EXPECT_TRUE(content.find("already on disk") < content.find("new line")) << content;
    }

    // ============================================================================
    // reopen
    // ============================================================================

    TEST(FileSink, ReopenSwitchesTargetFile)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Reopen");
        const fs::path                  firstPath  = temporaryDirectory.path() / "first.log";
        const fs::path                  secondPath = temporaryDirectory.path() / "second.log";

        FileSink sink(firstPath);
        sink.write(makeEvent(LogLevel::Info, "message_in_first"));
        sink.flush();

        sink.reopen(secondPath);
        sink.write(makeEvent(LogLevel::Info, "message_in_second"));
        sink.flush();

        ASSERT_TRUE(fs::exists(secondPath));
        EXPECT_TRUE(fileContains(firstPath, "message_in_first")) << readWholeFile(firstPath);
        EXPECT_FALSE(fileContains(firstPath, "message_in_second")) << readWholeFile(firstPath);
        EXPECT_TRUE(fileContains(secondPath, "message_in_second")) << readWholeFile(secondPath);
        EXPECT_FALSE(fileContains(secondPath, "message_in_first")) << readWholeFile(secondPath);
    }

    TEST(FileSink, ReopenCreatesMissingParentDirectories)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_ReopenDir");
        const fs::path                  rotatedPath = temporaryDirectory.path() / "archive" / "run" / "moved.log";

        FileSink sink(temporaryDirectory.path() / "start.log");
        sink.reopen(rotatedPath);
        sink.write(makeEvent(LogLevel::Info, "after reopen"));
        sink.flush();

        ASSERT_TRUE(fs::is_directory(rotatedPath.parent_path()));
        EXPECT_TRUE(fileContains(rotatedPath, "after reopen")) << readWholeFile(rotatedPath);
    }

    TEST(FileSink, ReopenKeepsAppendingToExistingFile)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_ReopenAppend");
        const fs::path                  logPath = temporaryDirectory.path() / "same.log";

        FileSink sink(logPath);
        sink.write(makeEvent(LogLevel::Info, "before_reopen"));
        sink.flush();
        sink.reopen(logPath);
        sink.write(makeEvent(LogLevel::Info, "after_reopen"));
        sink.flush();

        EXPECT_TRUE(fileContains(logPath, "before_reopen")) << readWholeFile(logPath);
        EXPECT_TRUE(fileContains(logPath, "after_reopen")) << readWholeFile(logPath);
        EXPECT_EQ(countLines(logPath), 2u);
    }

    // ============================================================================
    // 压力与并发
    // ============================================================================

    TEST(FileSink, RapidWritesPreserveEveryLine)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Stress");
        const fs::path                  logPath     = temporaryDirectory.path() / "stress.log";
        constexpr int                   keventCount = 5000;

        FileSink sink(logPath);
        for (int index = 0; index < keventCount; ++index)
        {
            sink.write(makeEvent(LogLevel::Info, "stress_msg_" + std::to_string(index)));
        }
        sink.flush();

        EXPECT_EQ(countLines(logPath), static_cast<std::size_t>(keventCount));
        EXPECT_TRUE(fileContains(logPath, "stress_msg_0")) << readWholeFile(logPath);
        EXPECT_TRUE(fileContains(logPath, "stress_msg_" + std::to_string(keventCount - 1)));
    }

    TEST(FileSink, ConcurrentWritesProduceCompleteLines)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSink_Concurrent");
        const fs::path                  logPath          = temporaryDirectory.path() / "concurrent.log";
        constexpr int                   kthreadCount     = 4;
        constexpr int                   kwritesPerThread = 100;
        std::vector<std::thread>        threads;

        FileSink sink(logPath);
        threads.reserve(kthreadCount);
        for (int index = 0; index < kthreadCount; ++index)
        {
            threads.emplace_back([&sink, index]
            {
                for (int inner = 0; inner < kwritesPerThread; ++inner)
                {
                    sink.write(makeEvent(LogLevel::Info,
                                         "worker" + std::to_string(index) + "_entry" + std::to_string(inner)));
                }
            });
        }
        for (std::thread &thread: threads)
        {
            thread.join();
        }
        sink.flush();

        const std::string content = readWholeFile(logPath);
        EXPECT_EQ(countLines(logPath), static_cast<std::size_t>(kthreadCount) * kwritesPerThread);
        for (int index = 0; index < kthreadCount; ++index)
        {
            for (int inner = 0; inner < kwritesPerThread; ++inner)
            {
                const std::string token = "worker" + std::to_string(index) + "_entry" + std::to_string(inner);
                EXPECT_TRUE(content.find(token) != std::string::npos) << token;
            }
        }
    }
} // namespace AsynGyanis::Base
