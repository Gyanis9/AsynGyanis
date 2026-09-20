// TestHttpHeaderRules.cpp —— 头字段字符集判定的覆盖：tchar 与字段值两张表按字节穷尽比对，
//   再用整段判定与解析器端到端各钉一层。这些判定是 h1/h2/h3 三条承载共用的闸门，
//   判错的后果不是「慢」而是「放行本该拒绝的报文」，因此每个字节取值都要有断言。
//
//   一. 穷尽比对：全部 256 个字节取值逐个判，参照实现独立照 RFC 文本写死（不复用生产表），
//       两边不一致即为缺陷；高位字节（0x80-0xFF）单独强调——std::isalnum 一类 locale 敏感的
//       判定在 GBK/latin-1 区域设置下会把它们算成字母数字，穷尽比对正是那种漂移的探测器；
//   二. 边界：0x08/0x09/0x0A 与 0x1F/0x20、0x7E/0x7F/0x80 三对相邻取值分成合法与非法两侧；
//   三. 整段判定：空文本按「未被反例否决」为真；非法字节落在开头、中间、末尾都要被抓到，
//       且含 NUL 的文本按长度判定而不是当成零终止串（否则 NUL 之后的非法字节会被静默忽略）；
//   四. 接线：解析器与响应序列化确实走这两张表，而不只是表本身正确。

#include "Net/Http/HttpHeaderRules.h"

#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/ParseStatus.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief tchar 的参照实现：照 RFC 9110 §5.1 的 tchar = ALPHA / DIGIT / "!" "#" ... "~" 直写
         * @param byte 字节取值，0-255
         * @return true 表示该字节属于 tchar
         */
        bool referenceIsTokenCharacter(const int byte) noexcept
        {
            constexpr std::string_view kSeparators = "!#$%&'*+-.^_`|~";
            const bool isAlphanumeric = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
                                        (byte >= 'A' && byte <= 'Z');
            return isAlphanumeric || kSeparators.find(static_cast<char>(byte)) != std::string_view::npos;
        }

        /**
         * @brief 字段值字符集的参照实现：照 RFC 9110 §5.6.2 的 field-vchar = VCHAR / obs-text 加 HTAB 直写
         * @param byte 字节取值，0-255
         * @return true 表示该字节可以出现在字段值里
         */
        bool referenceIsFieldValueCharacter(const int byte) noexcept
        {
            return byte == '\t' || (byte >= 0x20 && byte <= 0x7E) || byte >= 0x80;
        }

        /// 把字节取值拼成一段文本，用于整段判定的位置敏感性用例
        std::string makeByteText(const std::vector<unsigned char> &bytes)
        {
            std::string text;
            text.reserve(bytes.size());
            for (const unsigned char byte: bytes)
            {
                text.push_back(static_cast<char>(byte));
            }
            return text;
        }
    } // namespace

    TEST(HttpHeaderRules, TokenCharacterSetMatchesRfcDefinitionForEveryByteValue)
    {
        for (int byte = 0; byte <= 0xFF; ++byte)
        {
            // 逐字节给出取值：只报「哪个字节两侧判定不一致」的定位信息，否则失败输出无从判断
            EXPECT_EQ(isTokenCharacter(static_cast<unsigned char>(byte)), referenceIsTokenCharacter(byte))
                    << "字节 0x" << std::hex << byte;
        }
    }

    TEST(HttpHeaderRules, FieldValueCharacterSetMatchesRfcDefinitionForEveryByteValue)
    {
        for (int byte = 0; byte <= 0xFF; ++byte)
        {
            EXPECT_EQ(isFieldValueCharacter(static_cast<unsigned char>(byte)), referenceIsFieldValueCharacter(byte))
                    << "字节 0x" << std::hex << byte;
        }
    }

    TEST(HttpHeaderRules, TokenCharacterSetKeepsLocaleDependentBytesOut)
    {
        // 这三个取值正是 std::isalnum 在 GBK / latin-1 等区域设置下会误判为字母数字的区间
        EXPECT_FALSE(isTokenCharacter(0x00));
        EXPECT_FALSE(isTokenCharacter(0x7F));
        EXPECT_FALSE(isTokenCharacter(0xA1));
        EXPECT_FALSE(isTokenCharacter(0xFE));
        // 而字段值侧 obs-text 是 RFC 明确放行的接收范围
        EXPECT_TRUE(isFieldValueCharacter(0x80));
        EXPECT_TRUE(isFieldValueCharacter(0xA1));
        EXPECT_TRUE(isFieldValueCharacter(0xFF));
    }

    TEST(HttpHeaderRules, CharacterSetsSplitAtAdjacentBoundaryValues)
    {
        // 0x08 与 0x0A 是控制字符、0x09（HTAB）放行：三个相邻取值把两类判定的边界钉死
        EXPECT_FALSE(isTokenCharacter(0x08));
        EXPECT_FALSE(isTokenCharacter('\t'));
        EXPECT_FALSE(isTokenCharacter(0x0A));
        EXPECT_FALSE(isFieldValueCharacter(0x08));
        EXPECT_TRUE(isFieldValueCharacter('\t'));
        EXPECT_FALSE(isFieldValueCharacter(0x0A));

        // SP 不是 tchar 但属于字段值；DEL 两边都不属于；0x7E（~）既是 tchar 也是可见 ASCII
        EXPECT_FALSE(isTokenCharacter(' '));
        EXPECT_TRUE(isFieldValueCharacter(' '));
        EXPECT_TRUE(isTokenCharacter(0x7E));
        EXPECT_TRUE(isFieldValueCharacter(0x7E));
        EXPECT_FALSE(isFieldValueCharacter(0x7F));
        EXPECT_FALSE(isFieldValueCharacter('\r'));
        EXPECT_FALSE(isFieldValueCharacter('\n'));
    }

    TEST(HttpHeaderRules, ContainsOnlyHelpersTreatEmptyTextAsNotRefuted)
    {
        // 空文本没有反例可否决，因此为真；是否允许为空由各调用方的非空要求判定
        EXPECT_TRUE(containsOnlyTokenCharacters({}));
        EXPECT_TRUE(containsOnlyFieldValueCharacters({}));
    }

    TEST(HttpHeaderRules, ContainsOnlyTokenCharactersFindsIllegalByteAtEveryPosition)
    {
        constexpr unsigned char kLegal = 'a';
        constexpr unsigned char kIllegal = ' ';
        // 非法字节放在头、中、尾三个位置各判一次：只扫前缀或提前退出的实现会漏掉后两种
        EXPECT_FALSE(containsOnlyTokenCharacters(makeByteText({kIllegal, kLegal, kLegal})));
        EXPECT_FALSE(containsOnlyTokenCharacters(makeByteText({kLegal, kIllegal, kLegal})));
        EXPECT_FALSE(containsOnlyTokenCharacters(makeByteText({kLegal, kLegal, kIllegal})));
        EXPECT_TRUE(containsOnlyTokenCharacters(makeByteText({kLegal, kLegal, kLegal})));
    }

    TEST(HttpHeaderRules, ContainsOnlyHelpersJudgeEmbeddedNullByLengthNotTermination)
    {
        // NUL 之后紧跟非法字节：按零终止判定的话会在这条文本上静默放行
        const std::string_view tokenWithNull{"ab\0\x01" "cd", 6};
        const std::string_view valueWithCrLf{"ab\0\r\n" "cd", 7};
        EXPECT_FALSE(containsOnlyTokenCharacters(tokenWithNull));
        EXPECT_FALSE(containsOnlyFieldValueCharacters(valueWithCrLf));
    }

    TEST(HttpHeaderRules, ContainsOnlyHelpersHandleTextsLongerThanTheTablesFastPath)
    {
        // 长文本跨过各种分块边界（16/32/64 一类），将来换成向量化实现也要保住这两条
        const std::string longLegalName(200, 'x');
        EXPECT_TRUE(containsOnlyTokenCharacters(longLegalName));
        EXPECT_TRUE(containsOnlyFieldValueCharacters(longLegalName));

        const std::string longNameWithTrailingTab = longLegalName + '\t';
        EXPECT_FALSE(containsOnlyTokenCharacters(longNameWithTrailingTab));
        const std::string longValueWithTrailingBell = longLegalName + '\x07';
        EXPECT_FALSE(containsOnlyFieldValueCharacters(longValueWithTrailingBell));
    }

    TEST(HttpHeaderRules, ParserAcceptsObsTextInHeaderValueAndRejectsControlBytes)
    {
        const auto parseHeaderLine = [](const std::string &headerLine)
        {
            // headerLine 自带结尾 CRLF，再补一条空 CRLF 便是头部块的终止空行：
            // 多留一个 CRLF 会让整条报文收齐后还剩字节，正向断言会被误判成失败
            const std::string request = "GET / HTTP/1.1\r\n" + headerLine + "\r\n";
            HttpParser parser;
            const ParseStatus status = parser.parse(request.data(), request.size());
            return status == ParseStatus::Done && parser.consumedByteCount() == request.size();
        };

        // obs-text 放行：非 ASCII 的自定义头部值（如中文文件名）是真实流量
        EXPECT_TRUE(parseHeaderLine("X-Title: 文件\r\n"));
        // HTAB 允许出现在值中间
        EXPECT_TRUE(parseHeaderLine("X-Spaced: a\tb\r\n"));
        // 值里夹 LF/CR/NUL/DEL 一律拒绝：每一个都能自己截断头部块。
        // 含 NUL 的字面量必须按字节数构造，长度写小了会在 NUL 之前就把值截断，测出假通过
        EXPECT_FALSE(parseHeaderLine("X-Bad: a\x01" "b\r\n"));
        EXPECT_FALSE(parseHeaderLine("X-Bad: a\x7f" "b\r\n"));
        EXPECT_FALSE(parseHeaderLine(std::string("X-Bad: a\0b", 10) + "\r\n"));
        // 名里带 DEL 或高位字节同样拒绝：tchar 只覆盖 ASCII
        EXPECT_FALSE(parseHeaderLine("X\x7f-Bad: v\r\n"));
        EXPECT_FALSE(parseHeaderLine(std::string("X\x80" "-Bad: v", 9) + "\r\n"));
    }

    TEST(HttpHeaderRules, ResponseRejectsHeaderNamesOutsideTheSharedTokenSet)
    {
        HttpResponse response;
        response.setHeader("X-Good", "value");
        // 高位字节不是 tchar：请求侧收不进来，响应侧也不许发出去，两侧必须同一张表
        EXPECT_FALSE(response.setHeader(std::string("X\x81" "-Bad", 6), "value"));
        EXPECT_FALSE(response.setHeader("X Bad", "value"));
        // 值侧的 CR/LF 是响应拆分的最小形态
        EXPECT_FALSE(response.setHeader("X-Bad", std::string("a\r\nSet-Cookie: x=1", 18)));
        EXPECT_TRUE(response.setHeader("X-Obs", std::string("a\x80" "b", 3)));

        const std::string serialized = response.toString();
        // 头部名入库即归一化为小写，序列化按该形态上线
        EXPECT_NE(serialized.find("x-good: value"), std::string_view::npos);
        EXPECT_EQ(serialized.find("Set-Cookie: x=1"), std::string_view::npos);
    }

} // namespace AsynGyanis::Net
