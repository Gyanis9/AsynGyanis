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

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（Windows）：长度大到会回绕成小正数时，读写按失败收口而不是「搬了 0 字节」
     * @details recv/send 的长度形参是 int。刻意取 4 GiB 这个形状：它回绕之后是**正数 0**，
     *          于是系统调用正常返回 0 —— 读侧与「对端关闭/暂无数据」同形，写侧看起来是「一个字节都没
     *          发出去但没报错」，两种都不会被调用方察觉。取 INT_MAX+1 那种回绕成负数的形状反而测不出
     *          问题（系统调用自己就会报错），所以这里要量的正是「回绕后仍合法」那一类。
     *          POSIX 侧长度形参本就是 size_t、不存在这次回绕，因此用例只在 Windows 编译。
     */
    TEST(FileDescriptor, WrappingLengthFailsInsteadOfQuietlyTransferringNothing)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        constexpr std::size_t wrappingLength = static_cast<std::size_t>(1) << 32;
        char                  buffer[1]      = {};

        EXPECT_EQ(FileDescriptor::read(readDescriptor, buffer, wrappingLength), -1)
                << "长度回绕成 0 会安静地什么也没读，而 0 在本接口里另有「对端关闭」的含义";
        EXPECT_EQ(FileDescriptor::write(writeDescriptor, buffer, wrappingLength), -1)
                << "写侧同样的回绕会报成「发送了 0 字节」，调用方以为已经发完";

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }
#endif
} // namespace AsynGyanis::Platform
