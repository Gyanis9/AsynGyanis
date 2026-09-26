/**
 * @file TestTraceContext.cpp
 * @brief W3C Trace Context 的用例：traceparent 的接受/拒绝矩阵、生成规则、tracestate 的条目管理，
 *        以及请求侧 setHeader 的覆盖语义
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 * @details 三条新判据都按突变法证过非恒绿（临时改坏实现→转红→复原→转绿，2026-09-24）：
 *          · 关掉 overwriteOrAppend 的同名收拢 → OverwriteCollapsesTheDuplicateRecordsOfTheSameName
 *            与 TraceContextMiddleware.RestartsTraceWhenTheHeaderAppearsTwice 同时红；
 *          · 把 trace-id/parent-id 的全零判定改成永不成立 → RejectsMalformedValues 红；
 *          · 让中间件「合法也重写」 → 六条管道用例红，其中 PassesAnUnknownVersionWithExtraFields…
 *            专门盯「重渲染会丢掉看不懂的尾字段」这一类自以为干净的实现。
 */

#include "Net/Http/TraceContext.h"

#include "AllocationProbe.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 规范 §3.2 里的示例取值：钉它是因为各家实现都拿它当对照，抄错一个字符就会跨实现错位
        constexpr std::string_view kCanonicalTraceparent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

        /// 规范示例里的 trace-id 与 parent-id
        constexpr std::string_view kCanonicalTraceId = "4bf92f3577b34da6a3ce929d0e0e4736";
        constexpr std::string_view kCanonicalSpanId  = "00f067aa0ba902b7";

        /// 把一段文本按「替换一个字符」的方式改坏：用于逐条钉拒绝分支，而不必手写十几条串
        [[nodiscard]] std::string corruptedByReplacing(std::string_view source, const std::size_t offset, const char replacement)
        {
            std::string text(source);
            text[offset] = replacement;
            return text;
        }
    } // namespace

    // ============================================================================
    // traceparent：接受侧
    // ============================================================================

    TEST(Traceparent, ParsesTheSpecExampleVerbatim)
    {
        const std::optional<TraceIdentifiers> identifiers = Traceparent::parse(kCanonicalTraceparent);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_EQ(identifiers->traceIdText(), kCanonicalTraceId);
        EXPECT_EQ(identifiers->parentIdText(), kCanonicalSpanId);
        EXPECT_EQ(identifiers->version, 0U);
        EXPECT_EQ(identifiers->flags, 0x01U);
        EXPECT_TRUE(identifiers->isSampled());

        // 数组必须是自终止的 C 串：调用方会把它当 char* 打日志
        EXPECT_EQ(identifiers->traceId[kTraceIdHexDigitCount], '\0');
        EXPECT_EQ(identifiers->parentId[kSpanIdHexDigitCount], '\0');
    }

    TEST(Traceparent, KeepsUndefinedTraceFlagBitsAndOnlyReadsSampledFromItsOwnBit)
    {
        // flags 取 "0a"：采样位为 0，另一位按规范「原样传递、收到时忽略」
        const std::string                     unsampledWithOtherBits = corruptedByReplacing(kCanonicalTraceparent, 54U, 'a');
        const std::optional<TraceIdentifiers> identifiers            = Traceparent::parse(unsampledWithOtherBits);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_EQ(identifiers->flags, 0x0AU);
        EXPECT_FALSE(identifiers->isSampled());
    }

    TEST(Traceparent, AcceptsFutureVersionWithAdditionalDashSeparatedFields)
    {
        // 版本 01 并多带一个附加字段：本实现不认识它的含义，但必须整条采信（§3.2.2）
        const std::string                     future      = "01-" + std::string(kCanonicalTraceId) + "-" + std::string(kCanonicalSpanId) + "-01-extra";
        const std::optional<TraceIdentifiers> identifiers = Traceparent::parse(future);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_EQ(identifiers->version, 1U);
        EXPECT_EQ(identifiers->traceIdText(), kCanonicalTraceId);
    }

    TEST(Traceparent, RenderRoundTripsTheVersionZeroValueByteForByte)
    {
        const std::optional<TraceIdentifiers> identifiers = Traceparent::parse(kCanonicalTraceparent);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_EQ(Traceparent::value(*identifiers), kCanonicalTraceparent);

        // 复用缓冲那条要给出同一串：容量随首条留下，稳态不碰堆
        std::string reused = "a-very-long-previous-value-that-must-be-overwritten-exactly";
        Traceparent::renderInto(reused, *identifiers);
        EXPECT_EQ(reused, kCanonicalTraceparent);
    }

    // ============================================================================
    // traceparent：拒绝侧——每一条都是「猜一个解释就会出错」的形状
    // ============================================================================

    TEST(Traceparent, RejectsMalformedValues)
    {
        const std::array<std::string_view, 11> rejected{
                "",                                                          // 空
                "00-4bf92f3577b34da6a3ce929d0e0e473-00f067aa0ba902b7-01",    // trace-id 少一位
                "00-4bf92f3577b34da6a3ce929d0e0e47366-00f067aa0ba902b7-01",  // trace-id 多一位（截到 55 字节后分隔符错位）
                "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",   // ff 是哨兵，不是版本
                "00-000000000000000000000000000000000-00f067aa0ba902b7-01",  // 全零 trace-id
                "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",   // 全零 parent-id
                "00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",   // 分隔符不是 '-'
                "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01",   // 大写十六进制：ABNF 只收小写
                "00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01",   // 非十六进制字符
                "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01x",  // 版本 0 之后多一个字符
                "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01,x", // 多条同名头部被折成一条带逗号的取值
        };
        for (const std::string_view candidate: rejected)
        {
            EXPECT_FALSE(Traceparent::parse(candidate).has_value()) << "本该拒绝的取值被采信了：" << candidate;
        }
    }

    TEST(Traceparent, RejectsTruncatedValuesAtEveryLengthBelowTheFixedForm)
    {
        for (std::size_t length = 0; length < kTraceparentCanonicalLength; ++length)
        {
            EXPECT_FALSE(Traceparent::parse(kCanonicalTraceparent.substr(0, length)).has_value()) << "截到 " << length << " 字节仍被采信，定长校验没起作用";
        }
    }

    // ============================================================================
    // 生成侧
    // ============================================================================

    TEST(Traceparent, GeneratesDistinctParseableIdentifiers)
    {
        constexpr std::size_t           kSampleCount = 2000U;
        std::unordered_set<std::string> seenTraceIds;
        seenTraceIds.reserve(kSampleCount);
        for (std::size_t index = 0; index < kSampleCount; ++index)
        {
            const TraceIdentifiers                generated = Traceparent::generate(true);
            const std::optional<TraceIdentifiers> reparsed  = Traceparent::parse(Traceparent::value(generated));
            ASSERT_TRUE(reparsed.has_value()) << "自己生成的字段解析不回来：渲染与校验口径分叉了";
            EXPECT_EQ(reparsed->version, 0U);
            EXPECT_TRUE(reparsed->isSampled());
            EXPECT_EQ(generated.traceId, reparsed->traceId);
            seenTraceIds.insert(std::string(generated.traceIdText()));
        }
        // 2000 条 128 位随机标识撞重的概率可以忽略：这里掉一条就说明随机源在原地打转
        EXPECT_EQ(seenTraceIds.size(), kSampleCount);
    }

    TEST(Traceparent, GeneratedUnsampledContextOnlyDiffersInTheSampledBit)
    {
        const TraceIdentifiers unsampled = Traceparent::generate(false);
        EXPECT_FALSE(unsampled.isSampled());
        EXPECT_EQ(unsampled.flags, 0U);

        // setSampled 只碰那一位：未定义的位必须留着
        TraceIdentifiers withOtherBits = unsampled;
        withOtherBits.flags            = 0x08U;
        withOtherBits.setSampled(true);
        EXPECT_EQ(withOtherBits.flags, 0x09U);
        withOtherBits.setSampled(false);
        EXPECT_EQ(withOtherBits.flags, 0x08U);
    }

    // ============================================================================
    // tracestate
    // ============================================================================

    TEST(TraceState, ParsesMembersIgnoringWhitespaceAroundSeparators)
    {
        const std::optional<TraceState> state = TraceState::parse(" roto=abc ,  a=b=c , vendor-1.key=v ");
        ASSERT_TRUE(state.has_value());
        ASSERT_EQ(state->entryCount(), 3U);
        EXPECT_EQ(state->entries()[0].key, "roto");
        EXPECT_EQ(state->entries()[0].value, "abc");
        // 值里的等号不参与切分：只按第一个 '=' 分名值，否则带 base64 取值的条目会被切碎
        EXPECT_EQ(state->entries()[1].key, "a");
        EXPECT_EQ(state->entries()[1].value, "b=c");
        EXPECT_EQ(state->entries()[2].key, "vendor-1.key");
    }

    TEST(TraceState, TreatsBlankFieldValueAsAbsentRatherThanMalformed)
    {
        const std::optional<TraceState> emptyState = TraceState::parse("");
        ASSERT_TRUE(emptyState.has_value());
        EXPECT_EQ(emptyState->entryCount(), 0U);

        const std::optional<TraceState> whitespaceState = TraceState::parse("   ");
        ASSERT_TRUE(whitespaceState.has_value());
        EXPECT_EQ(whitespaceState->entryCount(), 0U);
    }

    TEST(TraceState, RejectsMalformedListsWholesale)
    {
        const std::array<std::string_view, 7> rejected{
                "roto=abc,,other=1", // 多一个逗号留下空成员
                ",roto=abc",         // 开头就是分隔符
                "roto",              // 缺等号
                "=abc",              // 键为空
                "ROTO=abc",          // 键含大写：规范里键是小写
                "congo=abc",         // 点名作废的键
                "roto=abc,roto=xyz", // 重复键：整条判废，不留「一半可信」
        };
        for (const std::string_view candidate: rejected)
        {
            EXPECT_FALSE(TraceState::parse(candidate).has_value()) << "本该整条判废的 tracestate 被接受了：" << candidate;
        }
    }

    TEST(TraceState, UpsertMovesOwnKeyToFrontAndTruncatesToTheLimit)
    {
        std::optional<TraceState> parsed = TraceState::parse("a=1,b=2,c=3");
        ASSERT_TRUE(parsed.has_value());
        TraceState state = std::move(*parsed);

        ASSERT_TRUE(state.upsertFront("b", "fresh"));
        ASSERT_EQ(state.entryCount(), 3U);
        EXPECT_EQ(state.entries()[0].key, "b");
        EXPECT_EQ(state.entries()[0].value, "fresh");
        // 后面的顺序原样保留：规范只要求把自己的挪到最前，不得重排别人的相对次序
        EXPECT_EQ(state.entries()[1].key, "a");
        EXPECT_EQ(state.entries()[2].key, "c");

        for (std::size_t index = 0; index < 40U; ++index)
        {
            ASSERT_TRUE(state.upsertFront("k" + std::to_string(index), "v"));
        }
        EXPECT_EQ(state.entryCount(), kMaximumTraceStateEntryCount);
        EXPECT_EQ(state.entries().front().key, "k39");

        // 非法键或值都不改表：调用方拿到 false 才能自己决定是「不加」还是「拒绝请求」
        const std::size_t countBefore = state.entryCount();
        EXPECT_FALSE(state.upsertFront("BadKey", "v"));
        EXPECT_FALSE(state.upsertFront("ok", "带逗号,的值"));
        EXPECT_EQ(state.entryCount(), countBefore);
    }

    TEST(TraceState, KeepsParsedEntriesAboveTheLimitUntilWriting)
    {
        std::string longList;
        for (std::size_t index = 0; index < 40U; ++index)
        {
            if (index != 0U)
            {
                longList += ',';
            }
            longList += "k" + std::to_string(index) + "=v";
        }
        const std::optional<TraceState> parsed = TraceState::parse(longList);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->entryCount(), 40U) << "条目过多不是畸形：规范许可「收到太多就删几条」";

        std::string rendered;
        parsed->renderInto(rendered);
        EXPECT_EQ(std::ranges::count(rendered, ','), static_cast<std::ptrdiff_t>(kMaximumTraceStateEntryCount - 1U));
        // 解析保留的是上游的到达顺序，截断删的是尾部：最前面那批（刚写下的上游系统）必须留下
        EXPECT_TRUE(rendered.starts_with("k0=v")) << rendered;
        EXPECT_EQ(rendered.find("k32="), std::string::npos) << "尾部条目没被删掉：截断上限失效";

        std::string truncatedToTwo;
        parsed->renderInto(truncatedToTwo, 2U);
        EXPECT_EQ(truncatedToTwo, "k0=v,k1=v");
    }

    namespace
    {
        /**
         * @brief 按到达顺序取出请求的头部名（同名多条各占一个位置）
         * @param request 待读的请求
         * @return std::vector<std::string> 头部名列表，顺序即权威记录顺序
         */
        std::vector<std::string> collectHeaderNames(const HttpRequest &request)
        {
            std::vector<std::string> names;
            request.forEachHeaderField([&names](const std::string_view name, const std::string_view) { names.push_back(std::string(name)); });
            return names;
        }
    } // namespace

    // ============================================================================
    // 读侧与请求头部覆盖
    // ============================================================================

    TEST(TraceContextExtraction, ReadsTheHeaderValueAsTheSingleSourceOfTruth)
    {
        HttpRequest request;
        EXPECT_FALSE(extractTraceContext(request).has_value()) << "没带 traceparent 的请求应当不在任何链路里";

        ASSERT_TRUE(request.setHeader(kTraceparentHeaderName, kCanonicalTraceparent));
        const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
        ASSERT_TRUE(identifiers.has_value());
        EXPECT_EQ(identifiers->traceIdText(), kCanonicalTraceId);

        static_cast<void>(request.setHeader(kTraceparentHeaderName, "not-a-traceparent"));
        EXPECT_FALSE(extractTraceContext(request).has_value());
    }

    TEST(HttpRequestSetHeader, OverwritesInPlaceAndKeepsFieldOrder)
    {
        HttpRequest request;
        request.addHeader("accept", "*/*");
        request.addHeader(kTraceparentHeaderName, "00-00000000000000000000000000000000-00f067aa0ba902b7-01");
        request.addHeader("user-agent", "probe");

        ASSERT_TRUE(request.setHeader(kTraceparentHeaderName, kCanonicalTraceparent));

        // 覆盖而不是追加：字段条数不变，位置也不变，否则序列化顺序会随改写次数漂移
        const std::vector<std::string> names = collectHeaderNames(request);
        ASSERT_EQ(names.size(), 3U);
        EXPECT_EQ(names[1], "traceparent");
        EXPECT_EQ(request.getHeader(kTraceparentHeaderName).value_or(""), kCanonicalTraceparent);
    }

    TEST(HttpRequestSetHeader, RefusesUnsafeNamesAndValuesWithoutTouchingStoredState)
    {
        HttpRequest request;
        request.addHeader("accept", "text/plain");

        EXPECT_FALSE(request.setHeader("bad name", "v"));
        EXPECT_FALSE(request.setHeader("bad:name", "v"));
        EXPECT_FALSE(request.setHeader("", "v"));
        EXPECT_FALSE(request.setHeader("x", "value\r\ninjected: yes"));
        EXPECT_FALSE(request.setHeader("x", std::string_view{"nul\0byte", 8U}));

        EXPECT_FALSE(request.hasHeader("x"));
        EXPECT_EQ(request.getHeader("accept").value_or(""), "text/plain");
        EXPECT_EQ(collectHeaderNames(request).size(), 1U) << "拒写必须一字不改：半途写入会留下改到一半的头部";
    }

    TEST(HttpRequestSetHeader, AppendsForRepeatableHeadersLikeTheResponseSide)
    {
        HttpRequest request;
        ASSERT_TRUE(request.setHeader("set-cookie", "a=1"));
        ASSERT_TRUE(request.setHeader("set-cookie", "b=2"));
        EXPECT_EQ(request.headerValues("set-cookie").size(), 2U);
    }

    // ============================================================================
    // 分配台账：被跟踪的请求不该为「读一次上下文」付堆
    // ============================================================================

    TEST(TraceContextAllocations, ReadingAndRenderingStayAllocationFreeInSteadyState)
    {
        // 生成 → 渲染 → 写回头部 → 读回上下文，正是 traceContextMiddleware 归一化一条请求要跑的形状。
        // 每轮先 reset()：保活连接上「上一条报文收完、下一条进来」才是这个形状的真实节奏，
        // 请求自己的头部缓冲与 thread_local 渲染串都跨报文留着容量
        const auto normalizeOnce = []()
        {
            thread_local HttpRequest request;
            request.reset();
            static_cast<void>(request.setHeader(kTraceparentHeaderName, kCanonicalTraceparent));
            const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
            thread_local std::string              scratch;
            Traceparent::renderInto(scratch, identifiers.value_or(Traceparent::generate(false)));
            static_cast<void>(request.setHeader(kTraceparentHeaderName, scratch));
            return identifiers.has_value() ? 1U : 0U;
        };

        // 暖身：thread_local 的首块缓冲与请求容器的首次扩容是一次性成本，稳态判据不能把它算进去
        for (std::size_t index = 0; index < 64U; ++index)
        {
            static_cast<void>(normalizeOnce());
        }

        const auto profile = AsynGyanis::TestSupport::measureOperations(AsynGyanis::TestSupport::kMeasurementIterations, normalizeOnce);

        // 与 request-id 同一口径：字段字节写进请求自己的缓冲，读回来不拷一份
        EXPECT_EQ(profile.totalAllocations, 0U) << "读一次上下文或渲染一条字段就碰堆，说明复用缓冲的路子被改坏了";
    }
} // namespace AsynGyanis::Net
