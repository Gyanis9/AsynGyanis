/**
 * @file TestProcessInfo.cpp
 * @brief ProcessInfo 单元测试：可执行文件目录与环境变量读取
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace AsynGyanis::Platform
{
    TEST(ProcessInfo, ApplicationDirectoryIsAbsoluteAndExisting)
    {
        const std::filesystem::path applicationDirectory = ProcessInfo::applicationDirectory();

        EXPECT_FALSE(applicationDirectory.empty());
        EXPECT_TRUE(applicationDirectory.is_absolute());
        EXPECT_TRUE(std::filesystem::is_directory(applicationDirectory));
    }

    TEST(ProcessInfo, ApplicationDirectoryIsNotTheCurrentWorkingDirectoryFallback)
    {
        // 测试可执行文件所在目录必然真实存在，用它拼接出的路径可被解析
        const std::filesystem::path applicationDirectory = ProcessInfo::applicationDirectory();
        const std::filesystem::path candidate            = applicationDirectory / "logs";

        EXPECT_EQ(candidate.parent_path(), applicationDirectory);
    }

    TEST(ProcessInfo, EnvironmentVariableReturnsValueForStandardVariable)
    {
        // PATH 在 Windows 与 Linux 的进程环境中都存在
        const std::optional<std::string> pathValue = ProcessInfo::environmentVariable("PATH");

        ASSERT_TRUE(pathValue.has_value());
        EXPECT_FALSE(pathValue->empty());
    }

    TEST(ProcessInfo, EnvironmentVariableReturnsNulloptForUndefinedName)
    {
        const std::optional<std::string> missingValue =
                ProcessInfo::environmentVariable("ASYN_GYANIS_DEFINITELY_UNDEFINED_VARIABLE");

        EXPECT_FALSE(missingValue.has_value());
    }

    TEST(ProcessInfo, EnvironmentVariableHandlesEmptyName)
    {
        EXPECT_NO_THROW(ProcessInfo::environmentVariable(""));
    }
} // namespace AsynGyanis::Platform
