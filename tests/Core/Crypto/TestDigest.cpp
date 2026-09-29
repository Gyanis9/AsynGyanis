// 摘要与 HMAC 单元测试：公开测试向量、密钥按长度取的性质，以及十六进制输出形状
#include "Core/Crypto/Digest.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Core::Digest
{
    /**
     * @brief SHA-1 与 SHA-256 的公开向量、两种 HMAC 的常见引用向量
     * @details 四个 MAC 值都另拿两份独立实现（Node 的 crypto 与 Python 的 hmac/hashlib）对过：
     *          靠记忆写摘要向量是危险的，同一组 key/data 换一种 HMAC 算法就是完全不同的串，
     *          记错算法时这条用例只会以「代码算错了」的形式误导人
     */
    TEST(DigestTest, MatchesPublishedTestVectors)
    {
        EXPECT_EQ(toHex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
        EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT_EQ(toHex(hmacSha1("key", "The quick brown fox jumps over the lazy dog")), "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9");
        EXPECT_EQ(hmacSha256Hex("key", "The quick brown fox jumps over the lazy dog"), "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    }

    /**
     * @brief 空输入也有确定摘要，不能与「算不出来」混为一谈
     */
    TEST(DigestTest, EmptyInputHasDefinedDigest)
    {
        EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT_EQ(toHex(hmacSha1("", "")), "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d");
        EXPECT_EQ(toHex(hmacSha256("", "")), "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    }

    /**
     * @brief 密钥按长度取而不是按零终止取：含 NUL 的密钥要影响结果
     * @details 把密钥当字符串处理会静默截断长度，签出来的 MAC 看起来正常却与对端算的不一样，
     *          那种错在联调阶段极难定位。两条 HMAC 都要钉住：外部服务的签名密钥常是
     *          「密钥 + 固定后缀」的形状（如阿里云 RPC 的 secret&），截断一次就全线验签失败
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

        // HMAC-SHA-1 同一组形状，期望值来自独立实现而非本仓库
        const std::string keyWithNul{"key\0x", 5};
        EXPECT_EQ(toHex(hmacSha1(keyWithNul, "data")), "c8b92dbf9b179283d19f79d7feeefffc5af518eb");
        EXPECT_NE(toHex(hmacSha1(keyWithNul, "data")), toHex(hmacSha1("key", "data")));
        EXPECT_EQ(toHex(hmacSha1("key", "data")), "104152c5bfdca07bc633eebd46199f0255c9f49d");
    }

    /**
     * @brief 数据里的 NUL 参与摘要，不会被当成串尾
     */
    TEST(DigestTest, DataIsTakenByLengthNotTerminator)
    {
        const std::string withNul{"abc\0def", 7};
        EXPECT_NE(sha256Hex(withNul), sha256Hex("abc"));
        EXPECT_NE(sha256Hex(withNul), sha256Hex(std::string_view{"abc\0", 4}));

        // HMAC-SHA-1 的输入侧同样按长度取：这份期望值的 data 是 "a\0b"（3 字节）
        EXPECT_EQ(toHex(hmacSha1(std::string{"data\0", 5}, std::string{"a\0b", 3})), "ffc24e28e200384bc3b80bbe138c3a6bad50a1ad");
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
