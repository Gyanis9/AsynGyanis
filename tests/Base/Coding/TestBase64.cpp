// Base64 单元测试：RFC 4648 的标准测试向量、严格解码的几条拒绝判据与往返一致性
#include "Base/Coding/Base64.h"

#include <gtest/gtest.h>

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
} // namespace AsynGyanis::Base
