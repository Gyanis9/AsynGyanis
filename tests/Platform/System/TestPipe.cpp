// Pipe 单元测试：成对描述符、读写往返、CLOEXEC 默认值与 exec 存活选项
#include "Platform/System/Pipe.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

#include "PlatformTestSupport.h"

#if !ASYN_PLATFORM_WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
#if ASYN_PLATFORM_WIN32
    TEST(Pipe, CreateReportsUnsupportedPlatform)
    {
        PlatformError::setLastErrorCode(0);

        const std::optional<Pipe> pipe = Pipe::create();

        EXPECT_FALSE(pipe.has_value());
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kNotSupported) << "Windows 上没有匿名管道通路，必须报「不支持」而不是留下含糊的失败";
    }
#else
    TEST(Pipe, CreateReturnsTwoValidDistinctEnds)
    {
        const std::optional<Pipe> pipe = Pipe::create();

        ASSERT_TRUE(pipe.has_value());
        EXPECT_TRUE(pipe->isValid());
        EXPECT_TRUE(FileDescriptor::isValid(pipe->readEnd()));
        EXPECT_TRUE(FileDescriptor::isValid(pipe->writeEnd()));
        EXPECT_NE(pipe->readEnd(), pipe->writeEnd());
    }

    TEST(Pipe, WrittenBytesAreReadableFromTheReadEnd)
    {
        std::optional<Pipe> pipe = Pipe::create();
        ASSERT_TRUE(pipe.has_value());
        ASSERT_TRUE(FileDescriptor::setNonBlocking(pipe->readEnd()));

        const std::string payload = "heartbeat";
        ASSERT_EQ(FileDescriptor::write(pipe->writeEnd(), payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

        // 非阻塞读配轮询：实现坏掉时在时限内结束，而不是把用例挂死
        std::string received;
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (received.size() < payload.size() && std::chrono::steady_clock::now() < deadline)
        {
            char          buffer[32] = {0};
            const ssize_t bytesRead  = FileDescriptor::read(pipe->readEnd(), buffer, sizeof(buffer));
            if (bytesRead > 0)
            {
                received.append(buffer, static_cast<std::size_t>(bytesRead));
            }
        }
        EXPECT_EQ(received, payload);
    }

    TEST(Pipe, BothEndsCloseOnExecByDefault)
    {
        const std::optional<Pipe> pipe = Pipe::create();
        ASSERT_TRUE(pipe.has_value());

        EXPECT_TRUE(TestSupport::isNotInheritable(pipe->readEnd())) << "读端默认应随 exec 关闭";
        EXPECT_TRUE(TestSupport::isNotInheritable(pipe->writeEnd())) << "未声明要移交的写端也必须随 exec 关闭";
    }

    TEST(Pipe, WriteEndCanBeKeptAcrossExec)
    {
        const std::optional<Pipe> pipe = Pipe::create(true);
        ASSERT_TRUE(pipe.has_value());

        EXPECT_TRUE(TestSupport::isNotInheritable(pipe->readEnd())) << "读端始终留在本进程";
        EXPECT_FALSE(TestSupport::isNotInheritable(pipe->writeEnd())) << "声明移交后写端必须能穿过 exec";
    }

    TEST(Pipe, ReleaseHandsOwnershipToTheCaller)
    {
        std::optional<Pipe> pipe = Pipe::create();
        ASSERT_TRUE(pipe.has_value());
        const int readDescriptor  = pipe->releaseReadEnd();
        const int writeDescriptor = pipe->releaseWriteEnd();
        ASSERT_TRUE(FileDescriptor::isValid(readDescriptor));
        ASSERT_TRUE(FileDescriptor::isValid(writeDescriptor));
        EXPECT_FALSE(pipe->isValid());

        pipe.reset();
        EXPECT_GE(::fcntl(readDescriptor, F_GETFD), 0) << "交出去的读端不该随 Pipe 析构被关掉";
        EXPECT_GE(::fcntl(writeDescriptor, F_GETFD), 0) << "交出去的写端不该随 Pipe 析构被关掉";

        static_cast<void>(::close(readDescriptor));
        static_cast<void>(::close(writeDescriptor));
    }
#endif
} // namespace AsynGyanis::Platform
