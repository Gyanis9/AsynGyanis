// Base64 单元测试：RFC 4648 的标准测试向量、严格解码的几条拒绝判据与往返一致性
#include "Base/Coding/Base64.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief RFC 4648 §9.1 的标准向量逐个对上
     */
    TEST(Base64Test, EncodesEveryRfc4648Vector)
    {
        EXPECT_EQ(base64Encode(""), "");
        EXPECT_EQ(base64Encode("f"), "Zg==");
        EXPECT_EQ(base64Encode("fo"), "Zm8=");
        EXPECT_EQ(base64Encode("foo"), "Zm9v");
        EXPECT_EQ(base64Encode("foob"), "Zm9vYg==");
        EXPECT_EQ(base64Encode("fooba"), "Zm9vYmE=");
        EXPECT_EQ(base64Encode("foobar"), "Zm9vYmFy");
    }

    /**
     * @brief 任意二进制都能往返：中间含 NUL、含 0xFF 都不该被当成零终止字符串处理
     */
    TEST(Base64Test, RoundTripsArbitraryBinary)
    {
        const std::string binary{'\x00', '\x01', '\xFF', '\x7F', '\x00', '='};

        const std::optional<std::string> decoded = base64Decode(base64Encode(binary));
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, binary);
    }

    /**
     * @brief 严格解码的拒绝判据：长度、字母表外字符、填充位置、非规范填充位
     * @details 放过任何一种，同一个字节串就有了多种「合法」写法，而它们会在比较凭据时被判成不相等
     */
    TEST(Base64Test, DecodingRejectsNonCanonicalEncodings)
    {
        EXPECT_FALSE(base64Decode("").has_value()) << "空文本按非法处理：调用点都是长度固定的凭据";
        EXPECT_FALSE(base64Decode("Zm9vY").has_value()) << "长度不是 4 的倍数";
        EXPECT_FALSE(base64Decode("Zm9v*").has_value()) << "字母表之外的字符";
        EXPECT_FALSE(base64Decode("Z=m9").has_value()) << "填充符出现在中间";
        EXPECT_FALSE(base64Decode("Zm=9").has_value()) << "填充位非 0（'9' 落在了不该有信息的低位上）";
        EXPECT_FALSE(base64Decode("Zm9vYm9v===").has_value()) << "至多两个填充符";

        // 规范写法则收下：同一份字节的规范编码只有一个
        const std::optional<std::string> canonical = base64Decode("Zm9vYmFy");
        ASSERT_TRUE(canonical.has_value());
        EXPECT_EQ(*canonical, "foobar");
    }

    /**
     * @brief 解码结果按长度算，不按零终止算：含 NUL 的凭据要能整份拿回来
     */
    TEST(Base64Test, DecodedLengthIncludesEmbeddedNulBytes)
    {
        const std::optional<std::string> decoded = base64Decode("AAEC");
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->size(), 3U);
        EXPECT_EQ((*decoded)[0], '\0');
        EXPECT_EQ((*decoded)[1], '\x01');
        EXPECT_EQ((*decoded)[2], '\x02');
    }

    /**
     * @brief URL-safe 无填充字母表的逐个向量，含两处只有这套写法才会显形的字符替换
     * @details 期望值取自 RFC 4648 §10 的样例字节（0xFB 0xFF 0xFE 这类把 '+' '/' 顶出来的组合），
     *          并用另一份独立实现（Python 的 urlsafe_b64encode）逐条核对过——本函数的字母表写错一位，
     *          这里就要红，而 JWS 的凭据比对恰好是「差一个字符就不相等」。
     */
    TEST(Base64UrlTest, EncodesEveryRfc4648UrlSafeVector)
    {
        EXPECT_EQ(base64UrlEncode(""), "");
        EXPECT_EQ(base64UrlEncode("f"), "Zg");
        EXPECT_EQ(base64UrlEncode("fo"), "Zm8");
        EXPECT_EQ(base64UrlEncode("foo"), "Zm9v");
        EXPECT_EQ(base64UrlEncode("foobar"), "Zm9vYmFy");

        // 0xFB 0xFF 0xFE：标准字母表编出 "+//+"，这套写法必须交回 "-__-"
        const std::string standardAlphabetProbe{'\xFB', '\xFF', '\xFE'};
        const std::string crossedAlphabetProbe{'\xFB', '\xFF', '\xBF'};
        EXPECT_EQ(base64UrlEncode(standardAlphabetProbe), "-__-");
        EXPECT_EQ(base64UrlEncode(crossedAlphabetProbe), "-_-_");

        // 无填充：末组缺的字节数直接反映在长度上，不得出现 '='
        EXPECT_EQ(base64UrlEncode("fooba"), "Zm9vYmE");
        EXPECT_EQ(base64UrlEncode("foob"), "Zm9vYg");
        EXPECT_EQ(base64UrlEncode("foob").find('='), std::string_view::npos);
    }

    /**
     * @brief 同一份字节的两种写法只在字母表与填充两处不同，位布局一致
     * @details 钉住「两套编码是同一个算法换了 64 个字符、去掉填充」这一点：若 URL-safe 那侧自己
     *          重排了分组，往返就会与标准侧分歧，而这种分歧在凭据比对里表现为「明明同一把密钥却判不等」。
     */
    TEST(Base64UrlTest, BothAlphabetsEncodeTheSameBitLayout)
    {
        const std::string bytes{'\x00', '\xFF', '\x7F', '\x00', '='};

        const std::string standard = base64Encode(bytes);
        const std::string urlSafe  = base64UrlEncode(bytes);
        EXPECT_EQ(urlSafe, "AP9_AD0");

        // 把标准写法的 '+' '/' 换成 '-' '_'、去掉末尾 '='，就必须与 URL-safe 写法逐字节相等
        std::string rebased = standard;
        std::ranges::replace(rebased, '+', '-');
        std::ranges::replace(rebased, '/', '_');
        rebased.erase(std::ranges::find(rebased, '='), rebased.end());
        EXPECT_EQ(rebased, urlSafe);

        // 两侧各自解回同一份字节：编码可换字母表，解码后的字节不许换
        const std::optional<std::string> fromStandard = base64Decode(standard);
        const std::optional<std::string> fromUrlSafe  = base64UrlDecode(urlSafe);
        ASSERT_TRUE(fromStandard.has_value());
        ASSERT_TRUE(fromUrlSafe.has_value());
        EXPECT_EQ(*fromStandard, bytes);
        EXPECT_EQ(*fromUrlSafe, bytes);
    }

    /**
     * @brief URL-safe 解码的拒绝判据：标准字母表的字符、填充符、凑不出字节的长度、非规范填充位
     * @details '+' '/' 与 '=' 在 JWT 与 ACME 里都是「另一套写法的产物」，收下就等于让同一份凭据有
     *          两种被认可的形态；长度 % 4 == 1 那一格在 RFC 4648 的表里不存在，收下只能靠猜。
     */
    TEST(Base64UrlTest, DecodingRejectsStandardAlphabetPaddingAndImpossibleLengths)
    {
        EXPECT_FALSE(base64UrlDecode("").has_value()) << "空文本判非法，但它确实是 0 字节的合法编码：调用方要自己先判空";
        EXPECT_FALSE(base64UrlDecode("+//+").has_value()) << "标准字母表的 '+' '/' 不收";
        EXPECT_FALSE(base64UrlDecode("Zg==").has_value()) << "无填充写法里的 '=' 一律算多余字符";
        EXPECT_FALSE(base64UrlDecode("Zm9vY").has_value()) << "长度 % 4 == 1，凑不出整字节";
        EXPECT_FALSE(base64UrlDecode("AP9_AD1").has_value()) << "末组留了 2 位空闲位且被置位（'1' 的低 2 位非 0）";

        // 规范写法则收下，含 '-' '_' 那两个只属于本字母表的字符
        const std::string                urlAlphabetBytes{'\xFB', '\xFF', '\xFE'};
        const std::optional<std::string> decoded = base64UrlDecode("-__-");
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, urlAlphabetBytes);

        const std::optional<std::string> fiveBytes = base64UrlDecode("AP9_AD0");
        ASSERT_TRUE(fiveBytes.has_value());
        EXPECT_EQ(fiveBytes->size(), 5U);
    }

    /**
     * @brief 任意二进制（含 NUL 与 0xFF）在 URL-safe 这套写法里也能整份往返
     */
    TEST(Base64UrlTest, RoundTripsArbitraryBinary)
    {
        std::string binary;
        for (int value = 0; value < 256; ++value)
        {
            binary.push_back(static_cast<char>(value));
        }

        const std::optional<std::string> decoded = base64UrlDecode(base64UrlEncode(binary));
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, binary);
    }
} // namespace AsynGyanis::Base
