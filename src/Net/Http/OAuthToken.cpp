#include "Net/Http/OAuthToken.h"

#include "Net/Http/OAuthSupport.h"
#include "Net/Http/OAuthError.h"

#include <nlohmann/json.hpp>

namespace AsynGyanis::Net
{
    OAuthExpected<OAuthAccessToken> parseOAuthTokenResponse(const std::string_view responseBody)
    {
        const nlohmann::json document = nlohmann::json::parse(responseBody, nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            return std::unexpected(OAuthError{OAuthErrorKind::MalformedResponse, "令牌响应的正文不是 JSON 对象"});
        }

        OAuthAccessToken token;

        const OAuthExpected<std::string> accessToken = requiredJsonString(document, "access_token");
        if (!accessToken.has_value())
        {
            return std::unexpected(OAuthError{accessToken.error().kind, "令牌响应：" + accessToken.error().message});
        }
        token.accessToken = *accessToken;

        const OAuthExpected<std::string> tokenType = requiredJsonString(document, "token_type");
        if (!tokenType.has_value())
        {
            return std::unexpected(OAuthError{tokenType.error().kind, "令牌响应：" + tokenType.error().message});
        }
        token.tokenType = *tokenType;

        const OAuthExpected<std::string> refreshToken = optionalJsonString(document, "refresh_token");
        if (!refreshToken.has_value())
        {
            return std::unexpected(OAuthError{refreshToken.error().kind, "令牌响应：" + refreshToken.error().message});
        }
        token.refreshToken = *refreshToken;

        const OAuthExpected<std::string> grantedScope = optionalJsonString(document, "scope");
        if (!grantedScope.has_value())
        {
            return std::unexpected(OAuthError{grantedScope.error().kind, "令牌响应：" + grantedScope.error().message});
        }
        token.scope = *grantedScope;

        // expires_in 是 RECOMMENDED：没给按 0（「端点未给出」），给了坏值按协议不符拒
        const OAuthExpected<std::chrono::seconds> expiresIn = optionalPositiveSeconds(document, "expires_in", std::chrono::seconds{0});
        if (!expiresIn.has_value())
        {
            return std::unexpected(OAuthError{expiresIn.error().kind, "令牌响应：" + expiresIn.error().message});
        }
        token.expiresIn = *expiresIn;

        return token;
    }

} // namespace AsynGyanis::Net
