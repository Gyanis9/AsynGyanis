/**
 * @file TestFileSystem.cpp
 * @brief FileSystem 单元测试：UTF-8 字符串到路径对象的跨平台构造
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/FileSystem/FileSystem.h"

#include "Platform/System/TextEncoding.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    TEST(FileSystem, PathFromUtf8HandlesEmptyInput)
    {
        EXPECT_TRUE(FileSystem::pathFromUtf8("").empty());
    }

    TEST(FileSystem, PathFromUtf8KeepsAsciiPathUnchanged)
    {
        const std::string asciiPath = "logs/app.log";

        EXPECT_EQ(FileSystem::pathFromUtf8(asciiPath), std::filesystem::path(asciiPath));
    }

    TEST(FileSystem, PathFromUtf8NativeFormMatchesPlatformEncoding)
    {
        const std::string utf8Path = "配置/日志.txt";

        const std::filesystem::path converted = FileSystem::pathFromUtf8(utf8Path);

#if ASYN_PLATFORM_WIN32
        // Windows 原生路径是 UTF-16，必须等价于先做 UTF-8→UTF-16 转换的结果
        EXPECT_EQ(converted.native(), TextEncoding::toWideString(utf8Path));
#else
        // POSIX 原生路径就是原始字节序列
        EXPECT_EQ(converted.native(), utf8Path);
#endif
    }

    TEST(FileSystem, FileCreatedWithNonAsciiNameIsReachableAgain)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileSystem_Utf8");

        // 以 UTF-8 字符串作为唯一来源，避免经 path::string() 按 ANSI 代码页丢字
        const std::string       fileName   = "用户设置.json";
        const std::filesystem::path targetPath = temporaryDirectory.path() / FileSystem::pathFromUtf8(fileName);

        const std::string content = R"({"language": "zh-CN"})";
        {
            std::ofstream file(targetPath, std::ios::out | std::ios::trunc);
            ASSERT_TRUE(file.is_open());
            file << content;
        }

        // 用同一 UTF-8 文件名重建必须指向同一个文件
        const std::filesystem::path rebuiltPath = temporaryDirectory.path() / FileSystem::pathFromUtf8(fileName);
        ASSERT_TRUE(std::filesystem::exists(rebuiltPath));

        std::ifstream     reader(rebuiltPath);
        const std::string readBack((std::istreambuf_iterator<char>(reader)), std::istreambuf_iterator<char>());
        EXPECT_EQ(readBack, content);
    }

    TEST(FileSystem, PathFromUtf8PreservesSeparatorsForJoining)
    {
        const std::filesystem::path directory = FileSystem::pathFromUtf8("logs");
        const std::filesystem::path fileName  = FileSystem::pathFromUtf8("应用.log");

        const std::filesystem::path joined = directory / fileName;

        EXPECT_EQ(joined.filename(), fileName);
        EXPECT_EQ(joined.parent_path(), directory);
    }
} // namespace AsynGyanis::Platform
