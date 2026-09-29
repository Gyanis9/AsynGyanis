// 秘密值比较的行为面：全部走完才算相等，长度差与内嵌 NUL 都不能被当成「相同」
#include "Base/Coding/SecureCompare.h"

#include <gtest/gtest.h>

#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 钉住：相同与「最后一位不同」两种结果分别为真与假
     * @details 「最后一位不同」这条钉的是比较必须走完整个长度：一旦实现退化成strncmp 式的短路比较，
     *          前缀相同的令牌就会被判相等，而攻击者要的正是这个
     */
    TEST(SecureCompare, MatchesOnlyWhenEveryByteIsEqual)
    {
        EXPECT_TRUE(constantTimeEquals("assemble-test-token", "assemble-test-token"));
        EXPECT_FALSE(constantTimeEquals("assemble-test-token", "assemble-test-toke0"));
        EXPECT_FALSE(constantTimeEquals("a", "b"));
    }

    /**
     * @brief 钉住：前缀关系不算相等——短的那条不能因为「前面都一样」被放过
     */
    TEST(SecureCompare, PrefixIsNotAMatch)
    {
        EXPECT_FALSE(constantTimeEquals("token", "token-longer"));
        EXPECT_FALSE(constantTimeEquals("token-longer", "token"));
    }

    /**
     * @brief 钉住：空对空相等，空对非空不等
     * @details 调用方在配置层已拒绝「空令牌」，这里钉的是函数本身的边界：不能把两边都空当成不匹配，
     *          也不能让长度判断把非空写回成相等
     */
    TEST(SecureCompare, TreatsEmptySidesConsistently)
    {
        EXPECT_TRUE(constantTimeEquals({}, {}));
        EXPECT_FALSE(constantTimeEquals("", "x"));
        EXPECT_FALSE(constantTimeEquals("x", ""));
    }

    /**
     * @brief 钉住：内嵌 NUL 是内容而不是串尾
     * @details 比较走的是 string_view 而非 C 字符串：若实现退回 strcmp，「abc\0」与「abc」会被判相等，
     *          那是能凭空字节截断绕过的鉴权
     */
    TEST(SecureCompare, KeepsEmbeddedNulSignificant)
    {
        constexpr std::string_view withNul("abc\0", 4);
        constexpr std::string_view withoutNul("abc", 3);
        EXPECT_FALSE(constantTimeEquals(withNul, withoutNul));
        EXPECT_TRUE(constantTimeEquals(withNul, std::string_view("abc\0", 4)));
    }
} // namespace AsynGyanis::Base
