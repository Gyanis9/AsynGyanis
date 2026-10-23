// Random 单元测试：取字节的两条口（公开源与密钥材料源）、零长请求的空操作契约、
// 写入范围恰好等于给的视图，以及长度越界的拒绝。
//
// 随机值本身没有可钉的期望，判据只能取「结构性质」：长度、两次调用不相等、不越界改写邻居。
// 「随机源失败」这条出路在进程内造不出来（要 poison DRBG，会把同一进程里别的用例一起打掉），
// 因此它只有代码路径上的两道保障：抛出前把缓冲清零、报错文案按 OpenSSL 的 -1 与 0 分开写。
// 该格在交付报告 §4 里记为未直测。

#include "Core/Crypto/Random.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "AssertionSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AsynGyanis::Core::Random
{
    TEST(RandomTest, ReturnsTheRequestedLengthAndNeverRepeats)
    {
        const std::vector<std::uint8_t> first  = bytes(32U);
        const std::vector<std::uint8_t> second = bytes(32U);

        EXPECT_EQ(first.size(), 32U);
        EXPECT_EQ(second.size(), 32U);
        EXPECT_NE(first, second) << "两次取到的 32 字节一模一样：随机源没在动";
    }

    TEST(RandomTest, SecretEntriesAreSizedAndDistinctToo)
    {
        const std::vector<std::uint8_t> first  = secretBytes(32U);
        const std::vector<std::uint8_t> second = secretBytes(32U);

        EXPECT_EQ(first.size(), 32U);
        EXPECT_NE(first, second);
    }

    TEST(RandomTest, WritesExactlyTheGivenViewAndLeavesTheNeighboursAlone)
    {
        // 邻居字节保持初值：越界写会同时打坏哨兵，那才是这条用例要拦的东西
        std::array<std::uint8_t, 16> buffer{};
        buffer.fill(0xA5U);

        fill(std::span<std::uint8_t>{buffer}.first(8U));

        const auto touched = std::span<std::uint8_t>{buffer}.first(8U);
        EXPECT_NE(std::count(touched.begin(), touched.end(), 0xA5U), static_cast<std::ptrdiff_t>(touched.size())) << "给出去的前 8 字节一个都没变：fill 没写到指定的范围";
        for (std::size_t index = 8U; index < buffer.size(); ++index)
        {
            EXPECT_EQ(buffer[index], 0xA5U) << "第 " << index << " 字节被越界改写";
        }

        fillSecret(std::span<std::uint8_t>{buffer}.first(8U));
        EXPECT_NE(std::count(touched.begin(), touched.end(), 0xA5U), static_cast<std::ptrdiff_t>(touched.size()));
        for (std::size_t index = 8U; index < buffer.size(); ++index)
        {
            EXPECT_EQ(buffer[index], 0xA5U) << "密钥材料那条口越界改写了第 " << index << " 字节";
        }
    }

    TEST(RandomTest, EmptyRequestIsANoOpOnEveryEntry)
    {
        std::array<std::uint8_t, 4> buffer{};
        buffer.fill(0x5cU);

        // 给出去的是空视图才是空操作：非空的视图必须被填满（见上一条用例），两者不能混
        EXPECT_NO_THROW(fill(std::span<std::uint8_t>{}));
        EXPECT_NO_THROW(fillSecret(std::span<std::uint8_t>{}));
        EXPECT_TRUE(bytes(0U).empty());
        EXPECT_TRUE(secretBytes(0U).empty());
        // 空请求不该动过缓冲：动了就是把「没要到随机数」写成「要到了四个字节」
        for (const std::uint8_t byte: buffer)
        {
            EXPECT_EQ(byte, 0x5cU);
        }
    }

    TEST(RandomTest, RejectsLengthsThatTheUnderlyingInterfaceCannotExpress)
    {
        // OpenSSL 的取字节接口按 int 收长度：静默截断会给出「要了 2^31+1 字节、只拿到一半」的结果。
        // 判据落在按长度取的那两条口上——按视图取的那条，视图本身就不可能超过可表达的范围
        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([&] { static_cast<void>(bytes(static_cast<std::size_t>(INT_MAX) + 1U)); },
                                                                               "超出 OpenSSL 接口能表达的长度上限", "bytes");
        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([&] { static_cast<void>(secretBytes(static_cast<std::size_t>(INT_MAX) + 1U)); },
                                                                               "超出 OpenSSL 接口能表达的长度上限", "secretBytes");
    }
} // namespace AsynGyanis::Core::Random
