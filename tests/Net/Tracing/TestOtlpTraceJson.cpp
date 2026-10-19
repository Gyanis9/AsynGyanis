// OTLP/HTTP JSON 渲染的用例：只判正文形状，不涉及任何传输
//
// 把编码单独拿出来测是为了让「出口失联」这类故障不必靠起一个假 Collector 才能发现：
// 这里逐字段读回 JSON，改坏了渲染当场就红。

#include "Net/Tracing/OtlpTraceJson.h"

#include "Base/Config/ConfigValue.h"
#include "ExpectedAssertions.h"
#include "Net/Tracing/Span.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using AsynGyanis::Base::parseConfigValue;
    using AsynGyanis::Net::formatOtlpTracesJson;
    using AsynGyanis::Net::SpanAttribute;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::SpanRecord;
    using AsynGyanis::Net::SpanStatusCode;
    using AsynGyanis::Net::TraceResource;
    using AsynGyanis::Net::TestSupport::jsonNumberOf;
    using AsynGyanis::Net::TestSupport::jsonTextOf;
    using AsynGyanis::Net::TestSupport::makeRecord;

    constexpr std::string_view kRootTraceId = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view kRootSpanId  = "1111222233334444";
    constexpr std::string_view kChildSpanId = "aaaabbbbccccdddd";
    /// makeRecord() 里那个起点在两个平台的墙钟刻度下都能精确表示，因此可以逐位断言
    constexpr std::string_view kStartNanoText = "1700000000123456700";
    constexpr std::string_view kEndNanoText   = "1700000002623456700";

    /// 造一份出处固定的 resource
    TraceResource makeResource(const std::string &version = {})
    {
        return TraceResource{.serviceName = "otlp-test-service", .serviceVersion = version};
    }
} // namespace

/**
 * @brief 一批两节（根 + 子）渲染成 Collector 认的形状：字段名、标识形态、时间与维度的取值类型都不许走样
 */
TEST(OtlpTraceJson, RendersTheOtlpJsonShapeOfABatch)
{
    SpanRecord root = makeRecord("root-operation", kRootTraceId, kRootSpanId);
    root.kind       = SpanKind::Server;

    SpanRecord child    = makeRecord("child-operation", kRootTraceId, kChildSpanId, kRootSpanId);
    child.kind          = SpanKind::Client;
    child.status        = SpanStatusCode::Error;
    child.statusMessage = "上游超时";
    child.attributes.push_back(SpanAttribute{.key = "http.request.method", .value = std::string{"GET"}});
    child.attributes.push_back(SpanAttribute{.key = "http.response.status_code", .value = std::int64_t{504}});
    child.droppedAttributeCount = 3U;

    const std::string text   = formatOtlpTracesJson(makeResource(), {root, child});
    const auto        parsed = parseConfigValue(text);
    ASSERT_HAS_VALUE(parsed) << "渲染结果不是合法 JSON：" << text;

    const ConfigValue &resourceSpan = parsed->at("resourceSpans").at(0);
    const ConfigValue &attributes   = resourceSpan.at("resource").at("attributes");
    ASSERT_EQ(attributes.size(), 1U);
    EXPECT_EQ(jsonTextOf(attributes.at(0).at("key")), "service.name");
    EXPECT_EQ(jsonTextOf(attributes.at(0).at("value").at("stringValue")), "otlp-test-service");

    const ConfigValue &scopeSpans = resourceSpan.at("scopeSpans").at(0);
    EXPECT_EQ(jsonTextOf(scopeSpans.at("scope").at("name")), AsynGyanis::Net::kTracingScopeName);

    const ConfigValue &spans = scopeSpans.at("spans");
    ASSERT_EQ(spans.size(), 2U);

    const ConfigValue &rootSpan = spans.at(0);
    EXPECT_EQ(jsonTextOf(rootSpan.at("traceId")), kRootTraceId);
    EXPECT_EQ(jsonTextOf(rootSpan.at("spanId")), kRootSpanId);
    EXPECT_FALSE(rootSpan.contains("parentSpanId")) << "根节不带上一节：protojson 对缺省值是省略字段，空串会被判成非法标识";
    EXPECT_EQ(jsonTextOf(rootSpan.at("name")), "root-operation");
    // 枚举取数值发，且与 OTLP 的编号对齐（UNSPECIFIED 占 0，Internal 才是 1）
    EXPECT_EQ(jsonNumberOf(rootSpan.at("kind")), 2);
    EXPECT_EQ(jsonTextOf(rootSpan.at("startTimeUnixNano")), kStartNanoText) << "64 位时间要发成字符串，与各语言 SDK 一致";
    EXPECT_EQ(jsonTextOf(rootSpan.at("endTimeUnixNano")), kEndNanoText);
    EXPECT_EQ(jsonNumberOf(rootSpan.at("status").at("code")), 0);
    EXPECT_FALSE(rootSpan.at("status").contains("message"));

    const ConfigValue &childSpan = spans.at(1);
    EXPECT_EQ(jsonTextOf(childSpan.at("parentSpanId")), kRootSpanId);
    EXPECT_EQ(jsonNumberOf(childSpan.at("kind")), 3);
    EXPECT_EQ(jsonNumberOf(childSpan.at("status").at("code")), 2);
    EXPECT_EQ(jsonTextOf(childSpan.at("status").at("message")), "上游超时");
    EXPECT_EQ(jsonNumberOf(childSpan.at("droppedAttributesCount")), 3);

    const ConfigValue &renderedAttributes = childSpan.at("attributes");
    ASSERT_EQ(renderedAttributes.size(), 2U);
    EXPECT_EQ(jsonTextOf(renderedAttributes.at(0).at("key")), "http.request.method");
    EXPECT_EQ(jsonTextOf(renderedAttributes.at(0).at("value").at("stringValue")), "GET");
    EXPECT_EQ(jsonTextOf(renderedAttributes.at(1).at("value").at("intValue")), "504") << "整数值发成字符串形态的 intValue";
}

/**
 * @brief 取值进不了 JSON（正文含非法 UTF-8）时交回空串，而不是硬凑一份坏正文
 */
TEST(OtlpTraceJson, RefusesToRenderAValueThatCannotEnterJson)
{
    SpanRecord        poisoned = makeRecord(std::string_view{"bad\xffname"}, kRootTraceId, kRootSpanId);
    const std::string text     = formatOtlpTracesJson(makeResource(), {poisoned});
    EXPECT_TRUE(text.empty()) << "含非法字节的正文不该交出来：整批发不出去由出口计一次失败";
}

/**
 * @brief 空批也要交出结构完整的正文：调用方据此判「渲染这一路是通的」
 */
TEST(OtlpTraceJson, EmptyBatchStillRendersTheResourceShell)
{
    const auto parsed = parseConfigValue(formatOtlpTracesJson(makeResource(), {}));
    ASSERT_HAS_VALUE(parsed);
    EXPECT_EQ(parsed->at("resourceSpans").at(0).at("scopeSpans").at(0).at("spans").size(), 0U);
    // 版本没配时不带 service.version；配了就出现在 resource 属性里
    const ConfigValue &plainAttributes = parsed->at("resourceSpans").at(0).at("resource").at("attributes");
    ASSERT_EQ(plainAttributes.size(), 1U);

    const auto versioned = parseConfigValue(formatOtlpTracesJson(makeResource("1.2.3"), {}));
    ASSERT_HAS_VALUE(versioned);
    const ConfigValue &versionedAttributes = versioned->at("resourceSpans").at(0).at("resource").at("attributes");
    ASSERT_EQ(versionedAttributes.size(), 2U);
    EXPECT_EQ(jsonTextOf(versionedAttributes.at(1).at("key")), "service.version");
    EXPECT_EQ(jsonTextOf(versionedAttributes.at(1).at("value").at("stringValue")), "1.2.3");
}

/**
 * @brief 随机属性：几百组必须被转义的操作名、维度键值与服务名都要按原样待在正文里
 * @details span 的 name 就是路由模式或调用方给的路径，属性值里会装 URL 与被截断的报错原文——
 *          这一族形状靠手写用例钉不住。渲染器漏转义任何一个字符，要么整份正文解析失败，
 *          要么成员变形，两种都在这里露出来。
 * @note 固定种子是为了失败可复放。这一族留在 gtest 侧，不进 fuzz-net.sh 那份闭包：
 *       标识文本定义在 Span.cpp 里，而它牵进 Core::CoroutinePool 整条依赖链。
 */
TEST(OtlpTraceJson, RandomTrickyTextsRoundTripVerbatim)
{
    // 整片取用而不是按字节挑：按字节会把「示」这类多字节序列拆开，造出的正是渲染器应当拒绝的
    // 非法 UTF-8——那是驱动自己的错，不是被测代码的错
    static constexpr std::array<std::string_view, 12> kTrickyPieces = {"\"", "\\", "\r", "\n", "\t", "\x01", "\x1f", "\x7f", " ", "a", "示", "例"};

    std::mt19937                               generator(20261019U);
    std::uniform_int_distribution<std::size_t> charPick(0U, kTrickyPieces.size() - 1U);
    const auto                                 makeText = [&](const std::size_t maximumLength) -> std::string
    {
        std::uniform_int_distribution<std::size_t> lengthPick(1U, maximumLength);
        std::string                                text;
        for (std::size_t count = lengthPick(generator); count > 0U; --count)
        {
            text += kTrickyPieces[charPick(generator)];
        }
        return text;
    };

    for (std::size_t round = 0; round < 250U; ++round)
    {
        const TraceResource resource{.serviceName = makeText(24U), .serviceVersion = makeText(8U)};
        const std::string   spanName       = makeText(48U);
        const std::string   attributeKey   = makeText(16U);
        const std::string   attributeValue = makeText(48U);

        SpanRecord record = makeRecord(spanName, kRootTraceId, kRootSpanId);
        record.attributes = {SpanAttribute{.key = attributeKey, .value = attributeValue}};

        const std::string label  = "第 " + std::to_string(round) + " 轮";
        const std::string body   = formatOtlpTracesJson(resource, {record});
        const auto        parsed = parseConfigValue(body);
        if (!parsed.has_value())
        {
            FAIL() << label << "：渲染结果不是合法 JSON，正文=" << body;
            return;
        }

        const ConfigValue &spans = parsed->at("resourceSpans").at(0).at("scopeSpans").at(0).at("spans");
        ASSERT_EQ(spans.size(), 1U) << label;
        EXPECT_EQ(jsonTextOf(spans.at(0).at("name")), spanName) << label;

        const ConfigValue &spanAttributes = spans.at(0).at("attributes");
        ASSERT_EQ(spanAttributes.size(), 1U) << label;
        EXPECT_EQ(jsonTextOf(spanAttributes.at(0).at("key")), attributeKey) << label;
        EXPECT_EQ(jsonTextOf(spanAttributes.at(0).at("value").at("stringValue")), attributeValue) << label;

        const ConfigValue &resourceAttributes = parsed->at("resourceSpans").at(0).at("resource").at("attributes");
        ASSERT_GE(resourceAttributes.size(), 1U) << label;
        EXPECT_EQ(jsonTextOf(resourceAttributes.at(0).at("value").at("stringValue")), resource.serviceName) << label;
    }
}
