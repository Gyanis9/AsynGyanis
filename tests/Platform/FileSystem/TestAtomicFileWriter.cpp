// AtomicFileWriter 单元测试：原子写、父目录创建、失败路径与残留清理
#include "Platform/FileSystem/AtomicFileWriter.h"

#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 读取文本文件全部内容
         * @param filePath 目标文件路径
         * @return std::string 文件内容，无法打开时返回空串
         */
        std::string readWholeFile(const std::filesystem::path &filePath)
        {
            std::ifstream file(filePath, std::ios::in | std::ios::binary);
            if (!file.is_open())
            {
                return {};
            }
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }
    } // namespace

    TEST(AtomicFileWriter, WritesNewFileWithExpectedContent)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Basic");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "settings.json";

        std::string error;
        const bool  succeeded = AtomicFileWriter::writeText(targetPath, "{ \"a\": 1 }\n", {}, &error);

        EXPECT_TRUE(succeeded) << error;
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(readWholeFile(targetPath), "{ \"a\": 1 }\n");
    }

    TEST(AtomicFileWriter, CreatesMissingParentDirectories)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Parent");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "nested" / "deeper" / "out.txt";

        std::string error;
        EXPECT_TRUE(AtomicFileWriter::writeText(targetPath, "deep", {}, &error)) << error;
        EXPECT_TRUE(std::filesystem::is_regular_file(targetPath));
    }

    TEST(AtomicFileWriter, OverwritesExistingFileCompletely)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Overwrite");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "repeat.txt";

        std::string error;
        ASSERT_TRUE(AtomicFileWriter::writeText(targetPath, "第一版内容比较长的一段文本", {}, &error)) << error;
        ASSERT_TRUE(AtomicFileWriter::writeText(targetPath, "short", {}, &error)) << error;

        // 截断语义：新内容必须完整替换旧内容而不是追加
        EXPECT_EQ(readWholeFile(targetPath), "short");
    }

    TEST(AtomicFileWriter, NoTemporaryFileRemainsAfterSuccessfulWrite)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Residue");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "clean.txt";

        std::string error;
        ASSERT_TRUE(AtomicFileWriter::writeText(targetPath, "content", {}, &error)) << error;

        EXPECT_FALSE(std::filesystem::exists(targetPath.string() + ".tmp"));
    }

    TEST(AtomicFileWriter, EmptyContentProducesEmptyFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Empty");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "empty.txt";

        std::string error;
        EXPECT_TRUE(AtomicFileWriter::writeText(targetPath, "", {}, &error)) << error;
        EXPECT_TRUE(std::filesystem::exists(targetPath));
        EXPECT_EQ(readWholeFile(targetPath), "");
    }

    TEST(AtomicFileWriter, Utf8FileNameIsWrittenAndReadable)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Utf8");
        // 目标名按 UTF-8 交给 path：直接拿窄字面量去拼，Windows 上会先经本地代码页解释一遍，那样这条
        // 用例测的就只是「同一个错名字自洽」——落盘的目录项其实不是要的那个
        const std::filesystem::path targetPath = temporaryDirectory.path() / FileSystem::pathFromUtf8("用户设置.json");

        std::string error;
        ASSERT_TRUE(AtomicFileWriter::writeText(targetPath, "{\"语言\": \"简体中文\"}", {}, &error)) << error;
        EXPECT_NE(readWholeFile(targetPath).find("简体中文"), std::string::npos);
        EXPECT_EQ(FileSystem::utf8FromPath(targetPath.filename()), "用户设置.json") << "落盘的目录项不是请求的那个名字";
    }

    /**
     * @brief 钉住：名字落在本地代码页之外的目标写得出，失败通道仍然只有 error 出参
     * @details Windows 上本层此前拿 `path::string()` 拼临时文件名，泰文名在 ACP 936 下直接抛出
     *          system_error（实测），而这层对外承诺的是「失败以 bool + error 表达」。名字取泰文而不
     *          取中文：中文在中文语境的 Windows 上能被代码页原样往返，那条路在这里测不出东西。
     */
    TEST(AtomicFileWriter, TargetNameOutsideLocalCodePageIsWrittenWithoutThrowing)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_OutsideCodePage");
        const std::string                     targetNameUtf8 = std::string("\xE0\xB8\x81\xE0\xB8\x82") + ".json";
        const std::filesystem::path           targetPath     = temporaryDirectory.path() / FileSystem::pathFromUtf8(targetNameUtf8);

        std::string error;
        bool        succeeded = false;
        EXPECT_NO_THROW(succeeded = AtomicFileWriter::writeText(targetPath, "{\"th\": true}", {}, &error));
        EXPECT_TRUE(succeeded) << error;
        EXPECT_EQ(readWholeFile(targetPath), "{\"th\": true}");

        // 目录里只该留下目标本身：临时名过一遍代码页会落到另一个名字上，那种残留没人清
        std::vector<std::string> entryNames;
        for (const auto &entry: std::filesystem::directory_iterator(temporaryDirectory.path()))
        {
            entryNames.push_back(FileSystem::utf8FromPath(entry.path().filename()));
        }
        EXPECT_EQ(entryNames, std::vector<std::string>{targetNameUtf8}) << "落盘的名字与请求的不是同一个，或多出一份临时文件";
    }

    TEST(AtomicFileWriter, LargeContentIsWrittenWithoutTruncation)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Large");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "large.txt";

        const std::string largeContent(1024 * 1024, 'x');

        std::string error;
        ASSERT_TRUE(AtomicFileWriter::writeText(targetPath, largeContent, {}, &error)) << error;

        EXPECT_EQ(std::filesystem::file_size(targetPath), largeContent.size());
    }

    TEST(AtomicFileWriter, FailureKeepsOriginalFileIntactAndReportsReason)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_Failure");

        // 先创建一个普通文件，再把它当作父目录使用，创建目录必然失败
        const std::filesystem::path blockingFile = temporaryDirectory.path() / "blocker";
        std::string                 error;
        ASSERT_TRUE(AtomicFileWriter::writeText(blockingFile, "original", {}, &error)) << error;

        const std::filesystem::path invalidTarget = blockingFile / "child.txt";
        error.clear();

        EXPECT_FALSE(AtomicFileWriter::writeText(invalidTarget, "should not be written", {}, &error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(readWholeFile(blockingFile), "original");
    }

    TEST(AtomicFileWriter, NullErrorPointerIsAcceptedOnFailure)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_NullError");
        const std::filesystem::path           blockingFile = temporaryDirectory.path() / "blocker2";

        ASSERT_TRUE(AtomicFileWriter::writeText(blockingFile, "x", {}));

        // 不传错误输出参数时失败也不应崩溃
        EXPECT_NO_THROW((void) AtomicFileWriter::writeText(blockingFile / "child.txt", "y", {}));
        EXPECT_FALSE(AtomicFileWriter::writeText(blockingFile / "child.txt", "y", {}));
    }

    /**
     * @brief 钉住：临时文件名带进程号，跨进程并发写同一目标不会撞进同一个 .tmp
     * @details 只有进程内计数器时，两个进程各自从 0 开始算出同一个 `.tmp.0`，于是两个写者
     *          交叉写同一份临时文件、再把夹杂内容 rename 成目标——「各写各的临时文件」恰好在
     *          跨进程发布这条路上失效。这里从失败路径的回显里读名字：把父目录位置摆一个普通
     *          文件，打开临时文件必然失败，报错文案带出完整的临时路径。
     * @note 钉的是「名字里有进程号」这一条足以分开跨进程的性质；两个真实进程同时写这种
     *       端到端形状要起子进程且时序敏感，不放进本用例。
     */
    TEST(AtomicFileWriter, NamesTheTemporaryFileAfterTheCurrentProcess)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_TempName");
        const std::filesystem::path           blockingFile = temporaryDirectory.path() / "blocker3";

        std::string error;
        ASSERT_TRUE(AtomicFileWriter::writeText(blockingFile, "x", {}, &error)) << error;

        error.clear();
        EXPECT_FALSE(AtomicFileWriter::writeText(blockingFile / "child.txt", "y", {}, &error));
        const std::string expectedMarker = ".tmp." + std::to_string(ProcessInfo::currentProcessId());
        EXPECT_NE(error.find(expectedMarker), std::string::npos) << "临时名里没有进程号，跨进程并发写会共用同一个 .tmp：" << error;
    }
    /**
     * @brief 钉住：替换失败时交出的是替换那一步的原因，而不是清理临时文件的结果
     * @details 清理若复用同一个 error_code，remove() 成功会把它清空，交出去的文案就成了
     *          「替换失败：Success」——调用方与运维据此永远判断不出真因。对照文本取自测试
     *          自己的一次同形 rename，不依赖实现内部变量。
     */
    TEST(AtomicFileWriter, RenameFailureReportsTheRenameReasonNotTheCleanup)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("AtomicWriter_RenameReason");

        std::string                 error;
        const std::filesystem::path blockedDirectory = temporaryDirectory.path() / "blocked-dir";
        std::filesystem::create_directories(blockedDirectory);
        // 非空目录才让「文件改名成目录」稳定失败：空目录在个别平台上会被直接换过来
        ASSERT_TRUE(AtomicFileWriter::writeText(blockedDirectory / "occupant.txt", "x", {}, &error)) << error;

        // 独立判据：自己复现一次同样的 rename，取内核给的原因文本
        const std::filesystem::path probeSource = temporaryDirectory.path() / "probe-source.txt";
        ASSERT_TRUE(AtomicFileWriter::writeText(probeSource, "probe", {}, &error)) << error;
        std::error_code probeError;
        std::filesystem::rename(probeSource, blockedDirectory, probeError);
        ASSERT_TRUE(static_cast<bool>(probeError)) << "对照的 rename 没有失败，本用例失去判据";
        std::filesystem::remove(probeSource);

        error.clear();
        EXPECT_FALSE(AtomicFileWriter::writeText(blockedDirectory, "should not be published", {}, &error));
        EXPECT_NE(error.find("替换"), std::string::npos) << "失败没有落在替换那一步：" << error;
        EXPECT_NE(error.find(probeError.message()), std::string::npos) << "报错应当带上替换那一步的原因「" << probeError.message() << "」，实际：" << error;

        // 清理照旧要发生：残留的 .tmp 会让下一次发布的判据被污染
        std::size_t temporaryResidueCount = 0;
        for (const auto &entry: std::filesystem::directory_iterator(temporaryDirectory.path()))
        {
            if (entry.path().filename().string().find(".tmp.") != std::string::npos)
            {
                ++temporaryResidueCount;
            }
        }
        EXPECT_EQ(temporaryResidueCount, 0U) << "失败的发布留下了没清掉的临时文件";
    }
} // namespace AsynGyanis::Platform
