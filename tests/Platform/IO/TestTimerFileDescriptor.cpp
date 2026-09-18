// TimerFileDescriptor 单元测试：到期唤醒、取消、排空与非正值语义
#include "Platform/IO/TimerFileDescriptor.h"

#include <gtest/gtest.h>

#include <chrono>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    TEST(TimerFileDescriptor, NewlyConstructedTimerIsValid)
    {
        const TimerFileDescriptor timer;

        EXPECT_TRUE(timer.isValid());
        EXPECT_GE(timer.fileDescriptor(), 0);
    }

    TEST(TimerFileDescriptor, ArmExpiresAndMakesDescriptorReadable)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(20)));

        EXPECT_TRUE(TestSupport::waitForReadable(timer.fileDescriptor(), 1000));
    }

    TEST(TimerFileDescriptor, DrainMakesDescriptorUnreadableAgain)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(20)));
        ASSERT_TRUE(TestSupport::waitForReadable(timer.fileDescriptor(), 1000));

        timer.drain();
        EXPECT_FALSE(TestSupport::waitForReadable(timer.fileDescriptor(), 100));
    }

    TEST(TimerFileDescriptor, CancelPreventsPendingExpiry)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(50)));
        timer.cancel();

        EXPECT_FALSE(TestSupport::waitForReadable(timer.fileDescriptor(), 200));
    }

    TEST(TimerFileDescriptor, ArmWithZeroDurationCancelsInsteadOfFiring)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(20)));
        // 非正数时长按「取消」处理：它不需要任何内核登记，因此必须报成功
        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(0)));

        EXPECT_FALSE(TestSupport::waitForReadable(timer.fileDescriptor(), 200));
    }

    TEST(TimerFileDescriptor, ArmWithNegativeDurationCancelsInsteadOfFiring)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(20)));
        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(-100)));

        EXPECT_FALSE(TestSupport::waitForReadable(timer.fileDescriptor(), 200));
    }

    TEST(TimerFileDescriptor, ReArmOverridesPreviousDeadline)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        // 先设一个很早的截止时间再立刻覆盖为更晚，早到期的通知不应出现
        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(10)));
        ASSERT_TRUE(timer.arm(std::chrono::milliseconds(5000)));

        EXPECT_FALSE(TestSupport::waitForReadable(timer.fileDescriptor(), 100));

        timer.cancel();
    }

    TEST(TimerFileDescriptor, TimerFiresRepeatedlyAcrossArmCycles)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        for (int roundIndex = 0; roundIndex < 3; ++roundIndex)
        {
            ASSERT_TRUE(timer.arm(std::chrono::milliseconds(10))) << "第 " << roundIndex << " 轮武装失败";
            ASSERT_TRUE(TestSupport::waitForReadable(timer.fileDescriptor(), 1000))
                    << "第 " << roundIndex << " 轮定时器未到期";
            timer.drain();
        }
    }

    TEST(TimerFileDescriptor, CancelWithoutArmIsSafe)
    {
        TimerFileDescriptor timer;
        ASSERT_TRUE(timer.isValid());

        EXPECT_NO_THROW(timer.cancel());
        EXPECT_NO_THROW(timer.drain());
    }
} // namespace AsynGyanis::Platform
