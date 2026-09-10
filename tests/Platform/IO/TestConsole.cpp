/**
 * @file TestConsole.cpp
 * @brief Console 单元测试：UTF-8 输出代码页设置与 ANSI 转义能力判定
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/Console.h"

#include <gtest/gtest.h>

namespace AsynGyanis::Platform
{
    TEST(Console, EnsureUtf8OutputIsRepeatableAndSafe)
    {
        EXPECT_NO_THROW(Console::ensureUtf8Output());
        EXPECT_NO_THROW(Console::ensureUtf8Output());
    }

    TEST(Console, SupportsAnsiEscapeCodesIsStableAcrossCalls)
    {
        const bool firstResult  = Console::supportsAnsiEscapeCodes();
        const bool secondResult = Console::supportsAnsiEscapeCodes();

        // 同一进程内标准输出的目标不会变化，能力判定必须给出一致结论
        EXPECT_EQ(firstResult, secondResult);
    }

    TEST(Console, SupportsAnsiEscapeCodesDoesNotThrowWhenOutputIsCaptured)
    {
        // 测试运行时标准输出通常被 ctest 捕获为管道：必须安全返回 false 而不是崩溃
        EXPECT_NO_THROW((void)Console::supportsAnsiEscapeCodes());
    }
} // namespace AsynGyanis::Platform
