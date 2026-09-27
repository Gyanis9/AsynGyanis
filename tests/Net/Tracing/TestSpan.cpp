// Span 自身的形状：定长标识的截断规则，与「上下文缺席时的默认值」是什么行为

#include "Net/Tracing/Span.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace
{
    using AsynGyanis::Net::Span;
    using AsynGyanis::Net::SpanIdentity;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::SpanStatusCode;

    /// 造一个定长段标识：内容取自入参文本，剩余格补 NUL
    template<std::size_t Size>
    std::array<char, Size> fixedTextOf(const std::string_view text)
    {
        std::array<char, Size> target{};
        static_cast<void>(std::ranges::copy(text, target.begin()));
        return target;
    }
} // namespace

/**
 * @brief 上下文缺席时的默认值就是一个非记录的替身：写什么都不会留下痕迹，也不会崩
 */
TEST(Span, DefaultSpanIsANonRecordingPlaceholder)
{
    Span span;
    EXPECT_FALSE(span.isRecording());
    EXPECT_TRUE(span.name().empty());

    span.setName("ignored");
    span.setAttribute("ignored", std::string_view{"value"});
    span.setAttribute("ignored-int", std::int64_t{7});
    span.setStatus(SpanStatusCode::Error, "ignored");
    EXPECT_TRUE(span.name().empty()) << "非记录的替身不该接住改名";
    span.finish();
    span.finish();

    EXPECT_FALSE(span.identity().hasParent());
    EXPECT_EQ(span.identity().traceIdText(), std::string(AsynGyanis::Net::kTraceIdHexDigitCount, '\0'));
    const auto identifiers = span.identifiers();
    EXPECT_FALSE(identifiers.isSampled());
    EXPECT_EQ(identifiers.version, 0U);
}

/**
 * @brief 段标识的文本按「首字节是不是 NUL」决定有没有上一节：定长数组没有空值，靠的就是这个哨兵
 * @details 渲染侧（OTLP 与文件两条出口）都按这条决定写不写 parentSpanId，
 *          所以这条规则必须钉在用例里，而不是散在两个出口各自的判法里。
 */
TEST(Span, IdentityParentTextDependsOnTheFirstByteSentinel)
{
    SpanIdentity root;
    root.traceId = fixedTextOf<AsynGyanis::Net::kTraceIdHexDigitCount + 1U>("0123456789abcdef0123456789abcdef");
    root.spanId  = fixedTextOf<AsynGyanis::Net::kSpanIdHexDigitCount + 1U>("0123456789abcdef");
    EXPECT_FALSE(root.hasParent());
    EXPECT_EQ(root.parentSpanIdText().size(), 0U);
    EXPECT_EQ(root.traceIdText(), "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(root.spanIdText(), "0123456789abcdef");

    SpanIdentity child = root;
    child.parentSpanId = fixedTextOf<AsynGyanis::Net::kSpanIdHexDigitCount + 1U>("fedcba9876543210");
    EXPECT_TRUE(child.hasParent());
    EXPECT_EQ(child.parentSpanIdText(), "fedcba9876543210");
    // 上一节存在与否不影响本节自己的标识
    EXPECT_EQ(child.spanIdText(), root.spanIdText());
}
