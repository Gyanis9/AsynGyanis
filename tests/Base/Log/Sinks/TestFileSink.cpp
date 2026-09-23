// FileSink 单元测试：父目录创建、追加/截断语义、reopen 切换路径与落盘完整性

#include "Base/Log/Sinks/FileSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
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
#include "Platform/Platform.h"

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
         * @brief 按二进制读回文件全部内容，不动任何字节
         * @details 与 readWholeFile 相对：那条归一化换行以便跨平台逐行断言，这一条用来钉落盘字节本身。
         */
        std::string readRawFile(const fs::path &filePath)
        {
            std::ifstream file(filePath, std::ios::in | std::ios::binary);
            if (!file.is_open())
            {
                return {};
            }
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{}};
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

    /**
     * @brief writeLine 报回的字节数必须等于文件真实增长——按大小滚动的阈值直接累加它
     * @details RollingFileSink::write 把这个返回值累加进「本文件已写字节」并据此滚动，
     *          少算就等于活动文件系统性超出上限才滚。差值出在 Windows 的文本模式：
     *          每个换行落到磁盘上是 "\r\n"，带调用栈的行每帧还各有一个换行。
     *          本用例钉的是跨平台的不变量（报数 == 磁盘增长），因此在 Linux 上改动前后都绿，
     *          只有 Windows 侧能证伪——缺陷本身就只在那一侧
     */
    TEST(FileSink, WriteLineReportsBytesActuallyLandedOnDisk)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileSink_ByteAccounting");
        const fs::path                        logPath = temporaryDirectory.path() / "accounting.log";

        FileSink sink(logPath);
        const std::size_t singleLineBytes = sink.writeLine("x");
        const std::size_t multiLineBytes  = sink.writeLine("a\nb\nc");
        sink.flush();

        EXPECT_EQ(fs::file_size(logPath), singleLineBytes + multiLineBytes)
                << "报回的字节数与磁盘增长不一致：滚动阈值会被低估，"
                << "实测报 " << singleLineBytes + multiLineBytes << " 而文件有 " << fs::file_size(logPath) << " 字节";
    }

    /**
     * @brief 落盘字节逐字钉住：Windows 每个换行都是 "\r\n"，其余平台是 "\n"
     * @details FileSink 现在按二进制打开、由自己补行尾，「与文本模式逐字相同」不再由流实现兜底，
     *          只能由用例钉。行内另有换行的形态（带调用栈的行每帧一个）必须逐个补，
     *          只补行尾那一个、或整段忘了补，本用例在 Windows 上转红。
     */
    TEST(FileSink, WritesPlatformNativeLineEndingsToDisk)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileSink_LineEndings");
        const fs::path                        logPath = temporaryDirectory.path() / "line-endings.log";

        FileSink sink(logPath);
        static_cast<void>(sink.writeLine("single"));
        static_cast<void>(sink.writeLine("a\nb\nc"));
        sink.flush();

        const std::string landed = readRawFile(logPath);
#if ASYN_PLATFORM_WIN32
        EXPECT_EQ(landed, "single\r\na\r\nb\r\nc\r\n") << "Windows 上每个换行前都要有一个 '\\r'";
#else
        EXPECT_EQ(landed, "single\na\nb\nc\n") << "POSIX 上行尾就是单个换行，不该多出 '\\r'";
#endif
    }
#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 字节其实没落盘时，flush 也要把那一声报出来
     * @details /dev/full 是 POSIX 上确定性的 ENOSPC 来源：打开成功，短写入先进流缓冲，
     *          于是 writeLine 照样报「写成了几个字节」，真正把字节推出去的是这一次刷新。
     *          刷完不看流状态，「Fatal 那条已经落盘」就等于没人核过——打完就 abort 的调用方
     *          丢了最后一条日志，而现场全静默。
     * @note Windows 上没有等价的「写必失败」设备，本例只在 POSIX 侧跑
     */
    TEST(FileSinkWriteFailure, FlushReportsWhatTheBufferedWriteCouldNot)
    {
        std::ostringstream captured;
        std::size_t        bufferedByteCount = 0;
        {
            const TestSupport::ScopedStreamRedirect redirect(std::cerr, captured.rdbuf());
            FileSink                                sink{fs::path("/dev/full")};
            bufferedByteCount = sink.writeLine("the last line before abort");
            sink.flush();
            sink.flush();
        }

        const std::string diagnostic = captured.str();
        EXPECT_GT(bufferedByteCount, 0U) << "本例的前提是「短写入先落进流缓冲」，报了 0 就说明走的是另一条失败路径";
        EXPECT_NE(diagnostic.find("写日志失败"), std::string::npos) << "刷不出去这件事一个字都没报";
        EXPECT_NE(diagnostic.find("/dev/full"), std::string::npos) << "诊断里没点名是哪个目标写不下去";
        // 两次 flush 只许报一条：磁盘故障期间每条日志都往标准错误写一遍就成了噪声
        EXPECT_EQ(std::ranges::count(diagnostic, '\n'), 1);
    }
    /**
     * @brief reopen 换到能写的路径就恢复落盘，并把一次性上报重新武装
     * @details 诊断文案里承诺「重新打开该文件后恢复」，这句承诺得有人验：换流之后旧流上的 badbit
     *          随它一起消失，新流要真写得进去；而「连续失败只报一次」的那个开关若不在 reopen 里复位，
     *          下一次再坏掉就一个字都不报——磁盘反复出问题的现场会被读成「只坏过一次」。
     * @note 只在 POSIX 侧跑：Windows 没有 /dev/full 这类「打开必成功、写必失败」的设备
     */
    TEST(FileSinkWriteFailure, ReopenRecoversWritingAndRearmsTheDiagnostic)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileSink_ReopenRecovery");
        const fs::path                        recoveredPath = temporaryDirectory.path() / "recovered.log";

        std::ostringstream captured;
        const auto         countReports = [&captured]
        {
            const std::string text = captured.str();
            std::size_t       count = 0U;
            for (std::string::size_type position = text.find("写日志失败");
                 position != std::string::npos;
                 position        = text.find("写日志失败", position + 1U))
            {
                ++count;
            }
            return count;
        };

        std::size_t recoveredWriteByteCount = 0U;
        std::size_t writeAfterSecondFailure = 0U;
        std::string landedText;
        {
            const TestSupport::ScopedStreamRedirect redirect(std::cerr, captured.rdbuf());
            FileSink                                sink{fs::path("/dev/full")};
            static_cast<void>(sink.writeLine("lost before recovery"));
            sink.flush();
            EXPECT_EQ(countReports(), 1U) << "第一次故障的基线没立住，后面的「第二次也报」无从判起";

            sink.reopen(recoveredPath);
            recoveredWriteByteCount = sink.writeLine("written after recovery");
            sink.flush();
            landedText = readWholeFile(recoveredPath);

            // 再换回写不下去的设备：这一条要的是「第二次故障也出声」。注意第一次写进坏设备的返回值
            // 仍然报字节数——正文还躺在流缓冲里，此刻无从知道会失败（能知道的只有下一次同步）；
            // 因此判据取「同步之后再来一条」，那一条必须如实报 0
            sink.reopen(fs::path("/dev/full"));
            static_cast<void>(sink.writeLine("lost again"));
            sink.flush();
            writeAfterSecondFailure = sink.writeLine("and again");
            EXPECT_EQ(countReports(), 2U) << "reopen 之后又坏掉却不再报：一次性开关没被重新武装";

            // 上一条走的是「中间有过成功写」的复位；这里再走一格「坏设备直接换坏设备」，且第一笔写
            // 就超出流缓冲（同步就失败，不给「缓冲里看着成功」的机会）：此时只有 reopen 自己复位开关
            // 才出得来第三声——短写入会被随后那次成功入缓冲顺手复位，量不到这条规矩
            sink.reopen(fs::path("/dev/full"));
            static_cast<void>(sink.writeLine(std::string(8192U, 'x')));
            sink.flush();
            EXPECT_EQ(countReports(), 3U) << "坏到坏之间第一笔写就失败，reopen 没复位开关就再也听不到下文";
        }

        EXPECT_GT(recoveredWriteByteCount, 0U) << "换到能写的路径后仍报 0 字节，说明恢复没发生";
        EXPECT_TRUE(landedText.find("written after recovery") != std::string::npos) << "换流后的正文没落到新文件";
        EXPECT_EQ(writeAfterSecondFailure, 0U) << "流已失效之后还在报「写成功了」";
    }
#endif
} // namespace AsynGyanis::Base
