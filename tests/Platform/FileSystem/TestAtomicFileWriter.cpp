/**
 * @file TestAtomicFileWriter.cpp
 * @brief AtomicFileWriter 单元测试：原子写、父目录创建、失败路径与残留清理
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/FileSystem/AtomicFileWriter.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "用户设置.json";

        std::string error;
        ASSERT_TRUE(AtomicFileWriter::writeText(targetPath, "{\"语言\": \"简体中文\"}", {}, &error)) << error;
        EXPECT_NE(readWholeFile(targetPath).find("简体中文"), std::string::npos);
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
        EXPECT_NO_THROW((void)AtomicFileWriter::writeText(blockingFile / "child.txt", "y", {}));
        EXPECT_FALSE(AtomicFileWriter::writeText(blockingFile / "child.txt", "y", {}));
    }
} // namespace AsynGyanis::Platform
