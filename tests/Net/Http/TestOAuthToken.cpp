// parseOAuthTokenResponse 的直测：不经 HTTP 往返，正文形状直接构造。
// 覆盖：
// - ParsesTheSuccessShapeFieldByField（字段逐项 + 可选字段缺席按空/0 交回，不是失败）
// - NamesTheMissingRequiredField（缺 access_token / 缺 token_type 各自点名）
// - RejectsWrongTypedAndEmptyValues（scope 是数字、access_token 是空串、expires_in 非正）
// - RejectsNonJsonBody（正文不是 JSON 对象）

#include "ExpectedAssertions.h"
#include "Net/Http/OAuthToken.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    TEST(OAuthTokenResponseTest, ParsesTheSuccessShapeFieldByField)
    {
        const auto token = parseOAuthTokenResponse(R"({"access_token":"at-1","token_type":"Bearer","expires_in":3600,"refresh_token":"rt-1","scope":"read"})");
        ASSERT_HAS_VALUE(token) << token.error();
        EXPECT_EQ(token->accessToken, "at-1");
        EXPECT_EQ(token->tokenType, "Bearer");
        EXPECT_EQ(token->expiresIn.count(), 3600);
        EXPECT_EQ(token->refreshToken, "rt-1");
        EXPECT_EQ(token->scope, "read");

        // 可选字段缺席：按空 / 0 交回（0 的语义是「端点未给出」），不是失败
        const auto minimal = parseOAuthTokenResponse(R"({"access_token":"at-2","token_type":"Bearer"})");
        ASSERT_HAS_VALUE(minimal) << minimal.error();
        EXPECT_TRUE(minimal->refreshToken.empty());
        EXPECT_TRUE(minimal->scope.empty());
        EXPECT_EQ(minimal->expiresIn.count(), 0);
    }

    TEST(OAuthTokenResponseTest, NamesTheMissingRequiredField)
    {
        const std::pair<const char *, const char *> cases[] = {{R"({"token_type":"Bearer"})", "access_token"}, {R"({"access_token":"at-x"})", "token_type"}};
        for (const auto &[body, expectedName]: cases)
        {
            const auto token = parseOAuthTokenResponse(body);
            ASSERT_FALSE(token.has_value()) << body;
            EXPECT_NE(token.error().find(expectedName), std::string::npos) << token.error();
        }
    }

    TEST(OAuthTokenResponseTest, RejectsWrongTypedAndEmptyValues)
    {
        // scope 是数字：类型不符，按协议不符拒并点名
        const auto wrongTyped = parseOAuthTokenResponse(R"({"access_token":"at-1","token_type":"Bearer","scope":42})");
        ASSERT_FALSE(wrongTyped.has_value());
        EXPECT_NE(wrongTyped.error().find("scope"), std::string::npos) << wrongTyped.error();

        // access_token 是空串：必填字段不可用
        const auto emptyAccessToken = parseOAuthTokenResponse(R"({"access_token":"","token_type":"Bearer"})");
        ASSERT_FALSE(emptyAccessToken.has_value());
        EXPECT_NE(emptyAccessToken.error().find("access_token"), std::string::npos) << emptyAccessToken.error();

        // expires_in 非正：给了坏值不猜成「未给出」
        const auto nonPositive = parseOAuthTokenResponse(R"({"access_token":"at-1","token_type":"Bearer","expires_in":0})");
        ASSERT_FALSE(nonPositive.has_value());
        EXPECT_NE(nonPositive.error().find("expires_in"), std::string::npos) << nonPositive.error();
    }

    TEST(OAuthTokenResponseTest, RejectsNonJsonBody)
    {
        const auto token = parseOAuthTokenResponse("<html>gateway error page</html>");
        ASSERT_FALSE(token.has_value());
        EXPECT_NE(token.error().find("JSON"), std::string::npos) << token.error();
    }

} // namespace AsynGyanis::Net
