// Hkdf 单元测试：RFC 5869 的抽取、扩展与一次派生，全部按独立实现的现算值钉
//
// 期望值的来源与被测实现零共享：一份是手写的 RFC §2.2/§2.3 构造（Python 的 hmac），另一份是
// Node 的 crypto.hkdfSync；两者在 132 字节的 SHA-384 多轮扩展上也逐字一致。RFC 5869 附录 A.3
// （无盐、无 info）那一条另与规范正文对过。摘要向量的纪律沿用 TestDigest.cpp 那一份说明。

#include "Core/Crypto/Digest.h"
#include "Core/Crypto/Hkdf.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "AssertionSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Core::Hkdf
{
    namespace
    {
        /// "0b" 重复 22 次：RFC 5869 附录各例用的那份输入密钥材料
        const std::vector<std::uint8_t> &sharedInputKeyMaterial()
        {
            static const std::vector<std::uint8_t> ikm(22U, 0x0bU);
            return ikm;
        }

        /// 把十六进制文本解成字节，测试里的向量都按文本写，读起来才对得上规范
        std::vector<std::uint8_t> fromHex(const std::string_view text)
        {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(text.size() / 2U);
            for (std::size_t index = 0; index + 1U < text.size() + 1U; index += 2U)
            {
                bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(text.substr(index, 2U)), nullptr, 16)));
            }
            return bytes;
        }
        /// 把一段十六进制重复 n 次：测试里的盐与 info 都是「一个前缀 + 一段重复」的形状
        std::string repeatHex(const std::string_view unit, const std::size_t times)
        {
            std::string text;
            text.reserve(unit.size() * times);
            for (std::size_t index = 0; index < times; ++index)
            {
                text += unit;
            }
            return text;
        }
    } // namespace

    TEST(HkdfTest, ExtractMatchesTheIndependentlyComputedVector)
    {
        // 盐 000102...0c（13 字节）、IKM 22×0x0b，SHA-256：PRK 长度就是哈希长度
        const std::vector<std::uint8_t> salt = fromHex("000102030405060708090a0b0c");
        const std::vector<std::uint8_t> prk  = extract(Digest::Algorithm::Sha256, salt, sharedInputKeyMaterial());

        EXPECT_EQ(Digest::toHex(prk), "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
        EXPECT_EQ(prk.size(), Digest::byteLength(Digest::Algorithm::Sha256));
    }

    TEST(HkdfTest, ExpandMatchesTheIndependentlyComputedVector)
    {
        const std::vector<std::uint8_t> prk  = fromHex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
        const std::vector<std::uint8_t> info = fromHex("f0f1f2f3f4f5f6f7f8f9");

        const std::vector<std::uint8_t> output = expand(Digest::Algorithm::Sha256, prk, info, 42U);
        EXPECT_EQ(Digest::toHex(output), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    }

    TEST(HkdfTest, DeriveEqualsExtractFollowedByExpand)
    {
        const std::vector<std::uint8_t> salt = fromHex("000102030405060708090a0b0c");
        const std::vector<std::uint8_t> info = fromHex("f0f1f2f3f4f5f6f7f8f9");

        const std::vector<std::uint8_t> derived = derive(Digest::Algorithm::Sha256, salt, sharedInputKeyMaterial(), info, 42U);
        EXPECT_EQ(Digest::toHex(derived), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

        // 分两步走的同一组输入要交回同一份输出：否则「调用方自己串起来」与「用 derive」就是两种密钥
        const std::vector<std::uint8_t> stepwise = expand(Digest::Algorithm::Sha256, extract(Digest::Algorithm::Sha256, salt, sharedInputKeyMaterial()), info, 42U);
        EXPECT_EQ(stepwise, derived);
    }

    TEST(HkdfTest, AbsentSaltAndInfoFallBackToTheRfcDefaults)
    {
        // RFC 5869 附录 A.3：不给盐（按 Hash.length 个零处理）、不给 info，输出与规范正文逐字一致
        const std::vector<std::uint8_t> output = derive(Digest::Algorithm::Sha256, {}, sharedInputKeyMaterial(), {}, 42U);
        EXPECT_EQ(Digest::toHex(output), "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
    }

    TEST(HkdfTest, Sha384SpansSeveralExpandBlocksExactly)
    {
        // 132 字节 = ⌈132/48⌉ = 3 轮扩展：多轮拼接的边界最容易错在最后一段该截多少
        const std::vector<std::uint8_t> salt = fromHex("f4" + repeatHex("00", 47U));
        const std::vector<std::uint8_t> info = fromHex("09" + repeatHex("f0", 39U));

        const std::vector<std::uint8_t> output = derive(Digest::Algorithm::Sha384, salt, sharedInputKeyMaterial(), info, 132U);
        EXPECT_EQ(output.size(), 132U);
        EXPECT_EQ(Digest::toHex(output), "69c0ce46c6c7db722d683cefc577a563ca87c3b0ef590cf291d44db4ed1d9d68ea43377bb8b99f727594f70840660538bc69b7"
                                         "dd0b549a169549e910009ffa071edd91653f318315eb7741132e58e8d1cc1acf430c72a1adfcdd07a0bad0a18e3e8e7db482ce"
                                         "26f69e17200ee62cd1cd306c22219ad67242b9869e6fc8db8dd883a01437");
    }

    TEST(HkdfTest, RefusesOutputLongerThanTheStandardAllows)
    {
        const std::vector<std::uint8_t> prk(32U, 0x2aU);

        // 255 × Hash.length 之内是合法的（这里正好取到边界），越一格就是调用方算错了额度
        EXPECT_EQ(expand(Digest::Algorithm::Sha256, prk, {}, 255U * Digest::byteLength(Digest::Algorithm::Sha256)).size(), 8160U);
        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>(
                [&] { static_cast<void>(expand(Digest::Algorithm::Sha256, prk, {}, 255U * Digest::byteLength(Digest::Algorithm::Sha256) + 1U)); }, "超过所选哈希的上限",
                "超长输出");
    }

    TEST(HkdfTest, RefusesAnEmptyExpandKeyInsteadOfDerivingFromZeros)
    {
        // 空密钥扩展出来的是一份确定的、谁都能重算的「密钥」：把它当派生结果用等于没有派生
        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([&] { static_cast<void>(expand(Digest::Algorithm::Sha256, {}, {}, 32U)); }, "伪随机密钥为空",
                                                                               "空 prk");
    }

    TEST(HkdfTest, SaltAndInfoAreTakenByLengthNotTerminator)
    {
        // 含 NUL 的上下文绑定串要影响结果：按零终止取就会把 info 截断，两边算出不同的密钥却看不出来
        const std::vector<std::uint8_t> salt(16U, 0x11U);
        const std::vector<std::uint8_t> infoWithNul{0x61U, 0x00U, 0x62U};
        const std::vector<std::uint8_t> infoTruncated{0x61U};

        const std::vector<std::uint8_t> withNul   = derive(Digest::Algorithm::Sha256, salt, sharedInputKeyMaterial(), infoWithNul, 32U);
        const std::vector<std::uint8_t> shortInfo = derive(Digest::Algorithm::Sha256, salt, sharedInputKeyMaterial(), infoTruncated, 32U);
        EXPECT_NE(withNul, shortInfo);

        // 盐也按长度取：含 NUL 的盐与被截断的盐必须是两份不同的盐（注：全零的短盐与全零的长盐
        // 在 HMAC 里会补到同一个分组，那是 HMAC 的构造而不是截断，不能拿它当「按长度取」的反例）
        const std::vector<std::uint8_t> saltWithNul{0x61U, 0x00U, 0x62U};
        const std::vector<std::uint8_t> saltTruncated{0x61U};
        const std::vector<std::uint8_t> fromNulSalt   = derive(Digest::Algorithm::Sha256, saltWithNul, sharedInputKeyMaterial(), infoWithNul, 32U);
        const std::vector<std::uint8_t> fromShortSalt = derive(Digest::Algorithm::Sha256, saltTruncated, sharedInputKeyMaterial(), infoWithNul, 32U);
        EXPECT_NE(fromNulSalt, fromShortSalt);
    }
} // namespace AsynGyanis::Core::Hkdf
