// FileSystem 单元测试：UTF-8 字符串到路径对象的跨平台构造
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

    TEST(FileSystem, Utf8FromPathRoundTripsPathFromUtf8)
    {
        // 成对关系必须闭合：配置里读到的 UTF-8 文本绕 path 一圈回来还是同一段字节，
        // 否则「把路径打进日志与报错文案」这条通道会把名字写错
        for (const std::string &utf8Path: {std::string{"logs/app.log"}, std::string{"配置/日志.txt"}, std::string{}})
        {
            EXPECT_EQ(FileSystem::utf8FromPath(FileSystem::pathFromUtf8(utf8Path)), utf8Path);
        }
    }

    /**
     * @brief 落在本地代码页之外的路径转文本时不抛异常
     * @details Windows 上 `path::string()` 对这类字符直接抛 std::system_error，而路径文本多数只出现在
     *          日志与报错文案里——诊断通道本身不该成为新的故障点。POSIX 上窄串就是原生刻度，
     *          这一条只有 Windows 侧能证伪
     */
    TEST(FileSystem, Utf8FromPathDescribesNameOutsideCodePage)
    {
        const std::string       utf8Path = "🐳-鲸.log";
        const std::filesystem::path path = FileSystem::pathFromUtf8(utf8Path);

        std::string described;
        EXPECT_NO_THROW(described = FileSystem::utf8FromPath(path));
        EXPECT_EQ(described, utf8Path);
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
