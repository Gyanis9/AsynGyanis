// TestHttpRequestId.cpp —— request-id 这一面的直接覆盖：定长文本拼装、生成器的序号与
//   前缀语义、客户端自带值的采信判定，以及 resolve() 的「不可信就不回显」。
//   此前这些规则只在 TestHttpObservability 里通过真 socket 端到端间接验到，
//   而端到端路径喂不出「65 字节」「含 TAB」「恰好 0x21/0x7E」这类边界取值。
//
//   一. 拼装：与 std::format("{}-{:016x}") 逐字节对拍，覆盖零填充、跨 nibble 边界与
//       64 位上界；前缀长度可为任意值（进程内第 65536 台及以后会自然加宽）。
//   二. 生成器：同一实例内不重复且序号递增，不同实例的前缀不同（前缀标识哪台服务器）。
//   三. 采信判定：长度上下的 64/65 与可见 ASCII 的 0x21/0x7E 两侧各一组，控制字符、
//       空白、高位字节一律拒（放行即响应拆分或日志错位）。
//   四. resolve()：合法值原样沿用、非法值改由服务器生成，绝不把对端原文回显出去。

#include "Net/Http/HttpRequestId.h"

#include "Net/Http/HttpRequest.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一条合法的客户端自带取值，用作「原样沿用」的对照组
        constexpr std::string_view kAcceptableClientRequestId = "trace-from-upstream-gateway";
    } // namespace

    TEST(HttpRequestId, FormatsSequenceIdenticallyToTheReferenceFormatString)
    {
        // 参照实现就是要替换掉的那句 std::format：逐字节相同才说明改写没有改变对外形态
        for (const std::string_view prefix: {"", "0", "0001", "deadbeef"})
        {
            for (const std::uint64_t sequenceNumber: {std::uint64_t{0},
                                                      std::uint64_t{1},
                                                      std::uint64_t{0xF},
                                                      std::uint64_t{0x10},
                                                      std::uint64_t{0xFF},
                                                      std::uint64_t{0xFFFF'FFFF},
                                                      std::uint64_t{0x1'0000'0000},
                                                      std::numeric_limits<std::uint64_t>::max()})
            {
                const std::string expected = std::format("{}-{:016x}", prefix, sequenceNumber);
                EXPECT_EQ(detail::formatRequestIdText(prefix, sequenceNumber), expected)
                        << "前缀 " << prefix << " 序号 " << sequenceNumber;
            }
        }
    }

    TEST(HttpRequestId, GeneratedIdentifiersKeepTheDocumentedShapeAndLength)
    {
        const HttpRequestIdGenerator generator;
        const std::string firstIdentifier = generator.next();

        // 形如 0001-0000000000000000：4 位十六进制前缀 + 连字符 + 16 位十六进制序号
        EXPECT_EQ(firstIdentifier.size(), 21U) << firstIdentifier;
        EXPECT_EQ(firstIdentifier[4], '-') << firstIdentifier;
        EXPECT_EQ(firstIdentifier.substr(5), "0000000000000000");
        for (const char character: firstIdentifier.substr(0, 4) + firstIdentifier.substr(5))
        {
            EXPECT_TRUE((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))
                    << "前缀里出现了非小写十六进制字符：" << firstIdentifier;
        }
    }

    TEST(HttpRequestId, SequenceAdvancesWithoutRepeatingWithinOneGenerator)
    {
        const HttpRequestIdGenerator generator;
        std::unordered_set<std::string> seenIdentifiers;

        constexpr std::size_t kIdentifierCount = 2000;
        std::string previousIdentifier = generator.next();
        seenIdentifiers.insert(previousIdentifier);
        for (std::size_t index = 1; index < kIdentifierCount; ++index)
        {
            const std::string currentIdentifier = generator.next();
            EXPECT_TRUE(seenIdentifiers.insert(currentIdentifier).second) << "重复的 request-id：" << currentIdentifier;
            // 定长十六进制的好处之一：文本序即序号序，可直接按字符串比较判断递增
            EXPECT_LT(previousIdentifier, currentIdentifier) << previousIdentifier << " 不小于 " << currentIdentifier;
            previousIdentifier = std::move(currentIdentifier);
        }
        EXPECT_EQ(seenIdentifiers.size(), kIdentifierCount);
    }

    TEST(HttpRequestId, EachGeneratorTakesADistinctProcessWidePrefix)
    {
        const HttpRequestIdGenerator firstGenerator;
        const HttpRequestIdGenerator secondGenerator;

        // 前缀必须不同，否则两台服务器的 id 会在日志里混成一条链；同一台服务器则全程一致
        EXPECT_NE(firstGenerator.next().substr(0, 4), secondGenerator.next().substr(0, 4));
        EXPECT_EQ(firstGenerator.next().substr(0, 4), firstGenerator.next().substr(0, 4));
    }

    TEST(HttpRequestId, AcceptanceJudgeSplitsAtLengthAndVisibleAsciiBoundaries)
    {
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId({}));
        // 可见 ASCII 的两个端点放行，端点外一格拒绝
        EXPECT_TRUE(HttpRequestIdGenerator::isAcceptableRequestId("!"));
        EXPECT_TRUE(HttpRequestIdGenerator::isAcceptableRequestId("~"));
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId(" "));
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId(std::string_view("\x7f", 1)));

        // 长度 64 收、65 拒：上限是「别让对端原文无限占日志与响应头」
        EXPECT_TRUE(HttpRequestIdGenerator::isAcceptableRequestId(std::string(64, 'a')));
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId(std::string(65, 'a')));

        // TAB / CR / LF / NUL / 高位字节都不许回显，也不能进日志
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId("ab\tcd"));
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId("ab\r\ncd"));
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId(std::string("ab\0cd", 5)));
        EXPECT_FALSE(HttpRequestIdGenerator::isAcceptableRequestId(std::string("ab\x80" "cd", 5)));
    }

    TEST(HttpRequestId, ResolveKeepsTrustworthyClientValuesAndReplacesTheRest)
    {
        const HttpRequestIdGenerator generator;

        HttpRequest trustedRequest;
        trustedRequest.addHeader(std::string(kRequestIdHeaderName), std::string(kAcceptableClientRequestId));
        // 上游网关的链路 id 一旦替换就断了关联，因此原样返回而不是重新生成
        EXPECT_EQ(generator.resolve(trustedRequest), kAcceptableClientRequestId);

        HttpRequest untrustedRequest;
        untrustedRequest.addHeader(std::string(kRequestIdHeaderName), std::string("bad\tvalue", 9));
        const std::string replacedIdentifier = generator.resolve(untrustedRequest);
        // 非法取值既不被回显也不被采信：拿到的是服务器自己生成的定长标识，且不含对端原文
        EXPECT_EQ(replacedIdentifier.size(), 21U) << replacedIdentifier;
        EXPECT_EQ(replacedIdentifier.find("bad"), std::string_view::npos) << "改生成的结果里不应留有对端原文";

        HttpRequest withoutHeaderRequest;
        EXPECT_EQ(generator.resolve(withoutHeaderRequest).size(), 21U);
    }

} // namespace AsynGyanis::Net
