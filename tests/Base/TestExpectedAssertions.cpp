// ExpectedAssertions 自证：失败载荷摊平的四种形状、诊断里真的带出文案、成功分支不碰 error()

#include "ExpectedAssertions.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    /// 只有类别没有文案时用的替身类别：取值刻意不从 0 起，避免「类别=0」被读成默认构造
    enum class FakeKind
    {
        First  = 1,
        Second = 2,
    };

    /**
     * @brief 「类别 + 中文文案」这一档载荷的替身，形状与 QuicDecodeError 同构
     */
    struct TwoPartFailure
    {
        FakeKind    kind{FakeKind::First}; ///< 失败类别
        std::string message;               ///< 中文文案
    };

    /** @brief 判断文本是否以给定前缀开头 */
    bool startsWithText(const std::string &text, const std::string_view prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }

    /** @brief 判断文本里是否出现给定子串 */
    bool containsText(const std::string &text, const std::string_view fragment)
    {
        return text.find(fragment) != std::string::npos;
    }
} // namespace

// 类别与文案都要出现在同一句里：只有号没有文案，或只有文案没有号，都定位不到是哪条规则判的
TEST(ExpectedAssertions, DescribesKindAndMessage)
{
    const TwoPartFailure failure{FakeKind::Second, "伪头出现在普通字段之后"};

    EXPECT_EQ(AsynGyanis::TestSupport::describeFailurePayload(failure), "kind=2：伪头出现在普通字段之后");
}

// 裸 std::string 载荷原样交出：本仓还有 153 处这种形状，多加一层包装只会让日志更难读
TEST(ExpectedAssertions, DescribesBareStringPayloadVerbatim)
{
    const std::string reason = "证书链落盘失败：目标目录不可写";

    EXPECT_EQ(AsynGyanis::TestSupport::describeFailurePayload(reason), reason);
}

// 只有类别的载荷也要报出号，并注明没文案，免得读诊断的人以为文案被吞了
TEST(ExpectedAssertions, DescribesCategoryOnlyEnum)
{
    EXPECT_EQ(AsynGyanis::TestSupport::describeFailurePayload(FakeKind::Second), "类别=2（该载荷不带文案）");
}

// 系统失败通道的载荷要带出编号——文本由标准库按类别给，本仓只保证「号在前、文本在后」
TEST(ExpectedAssertions, DescribesErrorCode)
{
    const std::error_code code(2, std::generic_category());

    const std::string text = AsynGyanis::TestSupport::describeFailurePayload(code);

    EXPECT_TRUE(startsWithText(text, "error_code=2：")) << text;
    EXPECT_TRUE(containsText(text, code.message())) << text;
}

// 接线判据：断言判红时，文案、类别号与被断言的表达式名都要真的落到诊断里
TEST(ExpectedAssertions, ReportsPayloadTextInFailureDiagnostic)
{
    std::expected<int, TwoPartFailure> rejected{std::unexpect, TwoPartFailure{FakeKind::Second, "动态表项大于容量"}};

    EXPECT_NONFATAL_FAILURE(EXPECT_HAS_VALUE(rejected), "动态表项大于容量");
    EXPECT_NONFATAL_FAILURE(EXPECT_HAS_VALUE(rejected), "kind=2");
    EXPECT_NONFATAL_FAILURE(EXPECT_HAS_VALUE(rejected), "rejected 落进了失败分支");
}

// 成功分支不许取 error()：带值的 expected 一旦走了成功，诊断链根本不该被求值
TEST(ExpectedAssertions, LeavesSuccessfulResultUntouched)
{
    std::expected<int, TwoPartFailure> accepted{7};

    EXPECT_HAS_VALUE(accepted);
    EXPECT_EQ(*accepted, 7);
}

// optional 没有失败载荷，要说清楚「没载荷」而不是留一句空诊断
TEST(ExpectedAssertions, NamesTheMissingPayloadForOptional)
{
    std::optional<int> empty;
    std::optional<int> present{3};

    EXPECT_NONFATAL_FAILURE(EXPECT_HAS_VALUE(empty), "该结果类型不带失败载荷");
    EXPECT_HAS_VALUE(present);
}
