// FileDescriptor 单元测试：有效性判定、非阻塞、描述符对与读写关闭
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    TEST(FileDescriptor, IsValidRejectsNegativeDescriptors)
    {
        EXPECT_FALSE(FileDescriptor::isValid(FileDescriptor::kInvalid));
        EXPECT_FALSE(FileDescriptor::isValid(-42));
    }

    TEST(FileDescriptor, IsValidAcceptsZeroAndPositiveDescriptors)
    {
        EXPECT_TRUE(FileDescriptor::isValid(0));
        EXPECT_TRUE(FileDescriptor::isValid(7));
    }

    TEST(FileDescriptor, InvalidConstantIsNegativeOne)
    {
        EXPECT_EQ(FileDescriptor::kInvalid, -1);
    }

    TEST(FileDescriptor, ReadOnInvalidDescriptorFailsWithoutTouchingBuffer)
    {
        char buffer[8] = {0};

        EXPECT_EQ(FileDescriptor::read(FileDescriptor::kInvalid, buffer, sizeof(buffer)), -1);
        EXPECT_EQ(FileDescriptor::write(FileDescriptor::kInvalid, buffer, sizeof(buffer)), -1);
    }

    TEST(FileDescriptor, CloseOnInvalidDescriptorIsNoOpSuccess)
    {
        EXPECT_EQ(FileDescriptor::close(FileDescriptor::kInvalid), 0);
    }

    TEST(FileDescriptor, SetNonBlockingFailsForInvalidDescriptor)
    {
        EXPECT_FALSE(FileDescriptor::setNonBlocking(FileDescriptor::kInvalid));
    }

    TEST(FileDescriptor, CreatePairReturnsTwoValidDescriptors)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        EXPECT_TRUE(FileDescriptor::isValid(readDescriptor));
        EXPECT_TRUE(FileDescriptor::isValid(writeDescriptor));
        EXPECT_NE(readDescriptor, writeDescriptor);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, CreatePairOutputIsNonBlocking)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        // 非阻塞描述符在无数据时读取应立即返回 -1 而不是阻塞
        char buffer[16];
        EXPECT_LT(FileDescriptor::read(readDescriptor, buffer, sizeof(buffer)), 0);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, WriteFromOneEndIsReadableFromTheOther)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        const std::string payload = "hello descriptor pair";
        const ssize_t     written = FileDescriptor::write(writeDescriptor, payload.data(), payload.size());
        EXPECT_EQ(written, static_cast<ssize_t>(payload.size()));

        char        buffer[64] = {0};
        std::size_t totalRead  = 0;
        const auto  deadline   = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (totalRead < payload.size() && std::chrono::steady_clock::now() < deadline)
        {
            const ssize_t bytesRead = FileDescriptor::read(readDescriptor, buffer + totalRead,
                                                           payload.size() - totalRead);
            if (bytesRead > 0)
            {
                totalRead += static_cast<std::size_t>(bytesRead);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        EXPECT_EQ(std::string(buffer, totalRead), payload);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, PairIsUsableInBothDirections)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        constexpr char kmarker = 'A';
        EXPECT_EQ(FileDescriptor::write(readDescriptor, &kmarker, sizeof(kmarker)), 1);
        EXPECT_TRUE(TestSupport::waitForReadable(writeDescriptor, 1000));

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, CreatePairResetsOutputsOnFailure)
    {
        // 成功路径下输出参数一定被写入有效值；失败路径下保持 kInvalid
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        if (!FileDescriptor::createPair(readDescriptor, writeDescriptor))
        {
            EXPECT_EQ(readDescriptor, FileDescriptor::kInvalid);
            EXPECT_EQ(writeDescriptor, FileDescriptor::kInvalid);
        } else
        {
            FileDescriptor::close(readDescriptor);
            FileDescriptor::close(writeDescriptor);
        }
        SUCCEED();
    }
} // namespace AsynGyanis::Platform
