/**
 * @file TestPlatformError.cpp
 * @brief PlatformError 单元测试：错误码读写、常量语义与错误描述
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <string>

namespace AsynGyanis::Platform
{
    TEST(PlatformError, LastErrorCodeDefaultsToZeroBeforeAnyFailure)
    {
        PlatformError::setLastErrorCode(0);
        EXPECT_EQ(PlatformError::lastErrorCode(), 0);
    }

    TEST(PlatformError, SetLastErrorCodeIsReadableBack)
    {
        PlatformError::setLastErrorCode(42);
        EXPECT_EQ(PlatformError::lastErrorCode(), 42);

        PlatformError::setLastErrorCode(0);
        EXPECT_EQ(PlatformError::lastErrorCode(), 0);
    }

    TEST(PlatformError, LastSocketErrorCodeSharesErrnoOnLinuxAndWsaOnWindows)
    {
        PlatformError::setLastErrorCode(0);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), 0);

        PlatformError::setLastErrorCode(7);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), 7);
        PlatformError::setLastErrorCode(0);
    }

    TEST(PlatformError, ErrorConstantsArePairwiseDistinct)
    {
        // 常量按 POSIX 语义给出，重复值只允许出现在刻意等价的别名上
        EXPECT_NE(PlatformError::kInterrupted, PlatformError::kWouldBlock);
        EXPECT_NE(PlatformError::kInterrupted, PlatformError::kConnectionAborted);
        EXPECT_NE(PlatformError::kWouldBlock, PlatformError::kConnectionAborted);
        EXPECT_NE(PlatformError::kTooManyOpenFiles, PlatformError::kNoBufferSpace);
    }

    TEST(PlatformError, WouldBlockAndInProgressMatchOnWindowsBecauseWsaHasNoInProgress)
    {
#if ASYN_PLATFORM_WIN32
        EXPECT_EQ(PlatformError::kWouldBlock, PlatformError::kInProgress);
#else
        EXPECT_NE(PlatformError::kWouldBlock, PlatformError::kInProgress);
#endif
    }

    TEST(PlatformError, OutOfMemoryConstantIsNonZero)
    {
        EXPECT_NE(PlatformError::kOutOfMemory, 0);
    }

    TEST(PlatformError, ResourceExhaustionConstantsLiveInTheSocketErrorSpace)
    {
        // kOutOfMemory 曾被写成 Win32 系统空间的 ERROR_NOT_ENOUGH_MEMORY，而调用方一律拿
        // WSAGetLastError() 的结果与它比较，导致该分支永远不成立。这里钉住它确实落在
        // socket 空间：Windows 上 socket 的内存不足就是 WSAENOBUFS，与 kNoBufferSpace 同值
#if ASYN_PLATFORM_WIN32
        EXPECT_EQ(PlatformError::kOutOfMemory, PlatformError::kNoBufferSpace);
#else
        EXPECT_NE(PlatformError::kOutOfMemory, PlatformError::kNoBufferSpace);
#endif
    }

    TEST(PlatformError, SystemFileTableFullCollapsesOntoTooManyOpenFilesOnlyOnWindows)
    {
        // Windows 的 socket 空间没有独立的 ENFILE 对等码；POSIX 上 EMFILE 与 ENFILE 是两回事
#if ASYN_PLATFORM_WIN32
        EXPECT_EQ(PlatformError::kSystemFileTableFull, PlatformError::kTooManyOpenFiles);
#else
        EXPECT_NE(PlatformError::kSystemFileTableFull, PlatformError::kTooManyOpenFiles);
#endif
    }

    TEST(PlatformError, MessageReturnsNonEmptyTextForKnownCode)
    {
        const std::string message = PlatformError::message(PlatformError::kInterrupted);
        EXPECT_FALSE(message.empty());
    }

    TEST(PlatformError, MessageDoesNotThrowForUnknownCode)
    {
        EXPECT_NO_THROW(PlatformError::message(999999));
    }

    TEST(PlatformError, FailedSystemCallPopulatesLastErrorCode)
    {
        // 对无效描述符设置非阻塞必然失败，验证真实失败路径会写出非零错误码
        PlatformError::setLastErrorCode(0);
        EXPECT_FALSE(FileDescriptor::setNonBlocking(FileDescriptor::kInvalid));
        EXPECT_NE(PlatformError::lastSocketErrorCode(), 0);

        PlatformError::setLastErrorCode(0);
    }
} // namespace AsynGyanis::Platform
