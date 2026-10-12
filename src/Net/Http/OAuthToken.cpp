#include "Net/Http/OAuthToken.h"

#include "Net/Http/OAuthSupport.h"

#include <nlohmann/json.hpp>

namespace AsynGyanis::Net
{
    std::expected<OAuthAccessToken, std::string> parseOAuthTokenResponse(const std::string_view responseBody)
    {
        const nlohmann::json document = nlohmann::json::parse(responseBody, nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            return std::unexpected("令牌响应的正文不是 JSON 对象");
        }

        OAuthAccessToken token;

        const std::expected<std::string, std::string> accessToken = requiredJsonString(document, "access_token");
        if (!accessToken.has_value())
        {
            return std::unexpected("令牌响应：" + accessToken.error());
        }
        token.accessToken = *accessToken;

        const std::expected<std::string, std::string> tokenType = requiredJsonString(document, "token_type");
        if (!tokenType.has_value())
        {
            return std::unexpected("令牌响应：" + tokenType.error());
        }
        token.tokenType = *tokenType;

        const std::expected<std::string, std::string> refreshToken = optionalJsonString(document, "refresh_token");
        if (!refreshToken.has_value())
        {
            return std::unexpected("令牌响应：" + refreshToken.error());
        }
        token.refreshToken = *refreshToken;

        const std::expected<std::string, std::string> grantedScope = optionalJsonString(document, "scope");
        if (!grantedScope.has_value())
        {
            return std::unexpected("令牌响应：" + grantedScope.error());
        }
        token.scope = *grantedScope;

        // expires_in 是 RECOMMENDED：没给按 0（「端点未给出」），给了坏值按协议不符拒
        const std::expected<std::chrono::seconds, std::string> expiresIn = optionalPositiveSeconds(document, "expires_in", std::chrono::seconds{0});
        if (!expiresIn.has_value())
        {
            return std::unexpected("令牌响应：" + expiresIn.error());
        }
        token.expiresIn = *expiresIn;

        return token;
    }

} // namespace AsynGyanis::Net
