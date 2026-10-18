// 精确十进制承载类型的单元测试：解析文法、标度、标度整数构造、默认值语义与在统一值里的位置。
// 覆盖场景：
// - ParsesSignedIntegerAndFractionForms / KeepsLeadingZerosAndLongDigitRunsVerbatim
// - RejectsFormsThatAreNotDecimalLiterals / RejectsEmptyAndSignOnlyTexts
// - DefaultConstructionIsZeroNotMissingValue / ReportsScaleFromTheCarryingText
// - BuildsFromScaledIntegerWithoutDividing / ScaledIntegerZeroKeepsScaleAndDropsTheSign
// - EqualityJudgesTextNotNumber / LastAlternativeIndexComesFromTheVariantItself
// 全部用例不触驱动、不碰网络：Decimal 是纯值类型，这一格里要钉的是「文本进来什么样、出去什么样」，
// 以及它作为 DatabaseValue 新备选的位置契约（既有备选的下标是已发布的契约，只能往后追加）。

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Common/Decimal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace AsynGyanis::Database
{
    namespace
    {
        /**
         * @brief 取一段文本解析出的十进制，解析不出来就直接让用例失败
         * @param text 十进制文本
         * @return std::string 承载文本（与传入逐字符相同才有意义，故这里断言一次）
         */
        std::string parsedText(const std::string_view text)
        {
            const auto parsed = Decimal::tryParse(text);
            EXPECT_TRUE(parsed.has_value()) << "应当被接受为十进制文本：" << text;
            return parsed.has_value() ? parsed->text() : std::string{};
        }
    } // namespace

    TEST(Decimal, ParsesSignedIntegerAndFractionForms)
    {
        // 三个形状各一条：纯整数、带小数、带符号——这三档就是引擎会交回的全部形态
        EXPECT_EQ(parsedText("5"), "5");
        EXPECT_EQ(parsedText("5.25"), "5.25");
        EXPECT_EQ(parsedText("-0.5"), "-0.5");
    }

    TEST(Decimal, KeepsLeadingZerosAndLongDigitRunsVerbatim)
    {
        // 不归一化：尾零与前导零都原样留着。「读回来逐字符相同」这条判据靠的就是不做任何改写
        EXPECT_EQ(parsedText("0.1000"), "0.1000");
        EXPECT_EQ(parsedText("007.50"), "007.50");

        // 40 位数字的金额：int64 与 double 都装不下，文本承载没有位宽上限（引擎那侧的限制归引擎）
        const std::string longAmount = "1234567890123456789012345678901234567890.1234";
        EXPECT_EQ(parsedText(longAmount), longAmount);
    }

    TEST(Decimal, RejectsFormsThatAreNotDecimalLiterals)
    {
        // 每一条都有自己的理由，见 Decimal::tryParse 的注释：指数是浮点的表达法、下划线是字面量的糖、
        // 空白属于「谁去 trim」的争论、缺位的写法（".5"/"1."）在引擎侧各有不同含义
        for (const std::string_view rejected: {"1e3", "1E-3", "1_000", " 1", "1 ", "1.2.3", ".5", "1.", "NaN", "abc", "1,000", "0x10"})
        {
            EXPECT_FALSE(Decimal::tryParse(rejected).has_value()) << "不该被接受为十进制文本：" << rejected;
        }
    }

    TEST(Decimal, RejectsEmptyAndSignOnlyTexts)
    {
        EXPECT_FALSE(Decimal::tryParse("").has_value());
        EXPECT_FALSE(Decimal::tryParse("-").has_value());
        // 加号一律不收：引擎不会这样交回，收了只是把同一个数变成两种写法
        EXPECT_FALSE(Decimal::tryParse("+1").has_value());
    }

    TEST(Decimal, DefaultConstructionIsZeroNotMissingValue)
    {
        // 实体结构体默认构造时每个成员都得有自己的值，而 NULL 在本库有正当表达（空的 std::optional）
        const Decimal zero;
        EXPECT_EQ(zero.text(), "0");
        EXPECT_EQ(zero.scale(), 0U);
    }

    TEST(Decimal, ReportsScaleFromTheCarryingText)
    {
        EXPECT_EQ(Decimal::tryParse("5")->scale(), 0U);
        EXPECT_EQ(Decimal::tryParse("5.0")->scale(), 1U);
        EXPECT_EQ(Decimal::tryParse("0.005")->scale(), 3U);
        EXPECT_EQ(Decimal::tryParse("-1.2345")->scale(), 4U);
    }

    TEST(Decimal, BuildsFromScaledIntegerWithoutDividing)
    {
        // 5 / 1000 用二进制浮点算是 0.004999999999999999...，这里只在文本里插小数点
        EXPECT_EQ(Decimal::fromScaledInteger(5, 3).text(), "0.005");
        EXPECT_EQ(Decimal::fromScaledInteger(-5, 3).text(), "-0.005");
        EXPECT_EQ(Decimal::fromScaledInteger(1234, 2).text(), "12.34");
        EXPECT_EQ(Decimal::fromScaledInteger(100, 2).text(), "1.00");
        EXPECT_EQ(Decimal::fromScaledInteger(5, 0).text(), "5");
    }

    TEST(Decimal, ScaledIntegerZeroKeepsScaleAndDropsTheSign)
    {
        // 负零不存在：写成 "-0.000" 只是把符号留在文本里骗人，而标度要留着（它就是列定义的那份精度）
        EXPECT_EQ(Decimal::fromScaledInteger(0, 3).text(), "0.000");
        EXPECT_EQ(Decimal::fromScaledInteger(-0, 3).text(), "0.000");

        // INT64_MIN 取幅度：在有符号域里对它取负会溢出，而它本身是一个合法金额
        EXPECT_EQ(Decimal::fromScaledInteger(std::numeric_limits<std::int64_t>::min(), 0).text(), "-9223372036854775808");
        EXPECT_EQ(Decimal::fromScaledInteger(std::numeric_limits<std::int64_t>::min(), 2).text(), "-92233720368547758.08");
    }

    TEST(Decimal, EqualityJudgesTextNotNumber)
    {
        // 这是有意取舍而不是没做完：判成相等就得归一化文本，而归一化会改掉引擎原样交回的写法
        EXPECT_EQ(Decimal::tryParse("1.0").value(), Decimal::tryParse("1.0").value());
        EXPECT_NE(Decimal::tryParse("1.0").value(), Decimal::tryParse("1.00").value());
        EXPECT_NE(Decimal::tryParse("0.1").value(), Decimal{});
    }

    TEST(Decimal, LastAlternativeIndexComesFromTheVariantItself)
    {
        const DatabaseValue bytes{BinaryBytes{0x01}};
        const DatabaseValue decimal{Decimal::tryParse("0.1000").value()};

        // 「新增备选一律追加在末尾」是已发布的契约：既有下标不能动。这里现取 variant 的规模比对，
        // 不在测试里再抄一个数字，否则加一个备选就要同时改两处
        EXPECT_GT(decimal.index(), bytes.index());
        EXPECT_EQ(decimal.index(), std::variant_size_v<DatabaseValue> - 1);
        EXPECT_TRUE(std::holds_alternative<Decimal>(decimal));
    }

} // namespace AsynGyanis::Database
