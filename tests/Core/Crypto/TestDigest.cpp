// 摘要与 HMAC 单元测试：公开测试向量、密钥按长度取的性质，以及十六进制输出形状
#include "Core/Crypto/Digest.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Core::Digest
{
    /**
     * @brief SHA-1 与 SHA-256 的公开向量、HMAC-SHA-256 的常见引用向量
     * @details 三个 MAC 值都另拿一份独立实现（Python 的 hmac/hashlib）对过：靠记忆写摘要向量是危险的
     *          ——HMAC-SHA-1 与 HMAC-SHA-256 对同一组 key/data 的前 8 个字符都是 f7bc83f4，
     *          记错算法时这条用例只会以「代码算错了」的形式误导人
     */
    TEST(DigestTest, MatchesPublishedTestVectors)
    {
        EXPECT_EQ(toHex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
        EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT_EQ(hmacSha256Hex("key", "The quick brown fox jumps over the lazy dog"), "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    }

    /**
     * @brief 空输入也有确定摘要，不能与「算不出来」混为一谈
     */
    TEST(DigestTest, EmptyInputHasDefinedDigest)
    {
        EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT_EQ(toHex(hmacSha256("", "")), "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    }

    /**
     * @brief 密钥按长度取而不是按零终止取：含 NUL 的密钥要影响结果
     * @details 把密钥当字符串处理会静默截断长度，签出来的 MAC 看起来正常却与对端算的不一样，
     *          那种错在联调阶段极难定位
     */
    TEST(DigestTest, KeyIsTakenByLengthNotTerminator)
    {
        const std::string withNul{"ab\0cd", 5};
        const std::string truncated{"ab", 2};

        const Sha256Value fullMac  = hmacSha256(withNul, "data");
        const Sha256Value shortMac = hmacSha256(truncated, "data");
        EXPECT_NE(toHex(fullMac), toHex(shortMac)) << "密钥在 NUL 处被截断了，两条 MAC 不该相同";

        // 同样长度、不同字节即不同密钥：数据敏感性与长度敏感性都要在
        const std::string sameLengthDifferent{"ab\0ce", 5};
        EXPECT_NE(toHex(hmacSha256(sameLengthDifferent, "data")), toHex(fullMac));
    }

    /**
     * @brief 数据里的 NUL 参与摘要，不会被当成串尾
     */
    TEST(DigestTest, DataIsTakenByLengthNotTerminator)
    {
        const std::string withNul{"abc\0def", 7};
        EXPECT_NE(sha256Hex(withNul), sha256Hex("abc"));
        EXPECT_NE(sha256Hex(withNul), sha256Hex(std::string_view{"abc\0", 4}));
    }

    /**
     * @brief 十六进制输出恒为小写、长度双倍
     */
    TEST(DigestTest, HexOutputIsLowercaseAndDoubleLength)
    {
        const std::string hex = sha256Hex("x");
        EXPECT_EQ(hex.size(), kSha256Length * 2U);
        for (const char character: hex)
        {
            const bool isDigit    = character >= '0' && character <= '9';
            const bool isLowerHex = character >= 'a' && character <= 'f';
            EXPECT_TRUE(isDigit || isLowerHex) << "出现了非小写十六进制字符：" << character;
        }
    }
} // namespace AsynGyanis::Core::Digest
