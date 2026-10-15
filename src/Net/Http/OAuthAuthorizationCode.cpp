#include "Net/Http/OAuthAuthorizationCode.h"

#include "Base/Coding/Base64.h"
#include "Core/Crypto/Random.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/OAuthSupport.h"

#include <openssl/evp.h>

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 随机材料的字节数：32 字节编成 base64url 恰好 43 个字符（RFC 7636 §4.1 对 verifier 的下限，合法）
        constexpr std::size_t kRandomTokenBytes = 32U;

        /// 取一段随机 token（base64url 编码，43 字符）：密码学随机源只在熵源坏掉时失败——对 PKCE 的
        /// verifier 与 state 来说，「取不到就换个弱随机」等于把这两步的安全意义整个拿掉，宁可如实失败
        [[nodiscard]] std::expected<std::string, std::string> makeRandomToken()
        {
            // PKCE 的 verifier 与 state 都是密钥材料，走私有 DRBG 那一侧；「取不到就换个弱随机」
            // 等于把这两步的安全意义整个拿掉，宁可如实失败并把原因带回去
            try
            {
                const std::vector<std::uint8_t> bytes = Core::Random::secretBytes(kRandomTokenBytes);
                return Base::base64UrlEncode(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            } catch (const Base::Exception &failure)
            {
                return std::unexpected(std::format("密码学随机源取不到 {} 字节，无法生成 PKCE verifier 与 state。原因：{}", kRandomTokenBytes, failure.what()));
            }
        }

        /// SHA-256 摘要（PKCE 的 S256 变换用）
        [[nodiscard]] std::optional<std::string> sha256Digest(const std::string_view bytes)
        {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int                               digestLength = 0;
            if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digestLength, EVP_sha256(), nullptr) != 1)
            {
                return std::nullopt;
            }
            return std::string(reinterpret_cast<const char *>(digest.data()), digestLength);
        }
    } // namespace

    OAuthAuthorizationCodeClient::OAuthAuthorizationCodeClient(EventLoop &loop, OAuthAuthorizationCodeOptions options) : m_loop(loop), m_options(std::move(options))
    {
        if (m_options.authorizationUrl.empty())
        {
            throw Base::InvalidArgumentException("OAuthAuthorizationCodeClient: authorizationUrl 不能为空——没有端点就组不出授权 URL");
        }
        if (m_options.tokenUrl.empty())
        {
            throw Base::InvalidArgumentException("OAuthAuthorizationCodeClient: tokenUrl 不能为空——没有端点就换不到令牌");
        }
        if (m_options.clientId.empty())
        {
            throw Base::InvalidArgumentException("OAuthAuthorizationCodeClient: clientId 不能为空——授权请求必须带它（RFC 6749 §4.1.1）");
        }
        if (m_options.redirectUri.empty())
        {
            throw Base::InvalidArgumentException("OAuthAuthorizationCodeClient: redirectUri 不能为空——授权请求与换取时都要原样带上它");
        }
        if (m_options.requestTimeout.count() <= 0)
        {
            throw Base::InvalidArgumentException("OAuthAuthorizationCodeClient: requestTimeout 必须为正");
        }
    }

    std::expected<OAuthAuthorizationRequest, std::string> OAuthAuthorizationCodeClient::beginAuthorization() const
    {
        std::expected<std::string, std::string> verifier = makeRandomToken();
        if (!verifier.has_value())
        {
            return std::unexpected(verifier.error());
        }
        std::expected<std::string, std::string> state = makeRandomToken();
        if (!state.has_value())
        {
            return std::unexpected(state.error());
        }
        const std::expected<std::string, std::string> challenge = computePkceChallenge(*verifier);
        if (!challenge.has_value())
        {
            return std::unexpected(challenge.error());
        }

        // 授权端点本来带查询串时接 '&'：直接补 '?' 会让原参数与 response_type 叠成一个畸形键
        std::string url = m_options.authorizationUrl;
        url += url.find('?') == std::string::npos ? '?' : '&';
        url += "response_type=code";
        url += "&client_id=" + percentEncodeFormComponent(m_options.clientId);
        url += "&redirect_uri=" + percentEncodeFormComponent(m_options.redirectUri);
        if (!m_options.scope.empty())
        {
            url += "&scope=" + percentEncodeFormComponent(m_options.scope);
        }
        url += "&state=" + *state;
        url += "&code_challenge=" + *challenge;
        url += "&code_challenge_method=S256";

        return OAuthAuthorizationRequest{.url = std::move(url), .state = std::move(*state), .verifier = std::move(*verifier)};
    }

    std::expected<OAuthAuthorizationCallback, std::string> OAuthAuthorizationCodeClient::parseCallback(const std::string_view callback)
    {
        // 接受整条回调 URL 或裸查询串：取第一个 '?' 之后的那段；没有 '?' 时把整段当查询串
        // （调用方可能已经把 URL 拆过了）。片段（'#' 及其后）不属于一次 HTTP 请求，剥掉
        std::string_view query = callback;
        if (const std::size_t question = callback.find('?'); question != std::string_view::npos)
        {
            query = callback.substr(question + 1);
        }
        if (const std::size_t fragment = query.find('#'); fragment != std::string_view::npos)
        {
            query = query.substr(0, fragment);
        }

        // 解码复用服务端入站那一份（'+' 表示空格、%XX 按字节）：两处各写一版必然在边角上分叉
        const std::unordered_map<std::string, std::string> fields    = HttpRequest::parseUrlEncoded(query);
        const auto                                         findField = [&fields](const char *name) -> std::string
        {
            const auto iterator = fields.find(name);
            return iterator != fields.end() ? iterator->second : std::string{};
        };

        OAuthAuthorizationCallback parsed;
        parsed.code             = findField("code");
        parsed.state            = findField("state");
        parsed.error            = findField("error");
        parsed.errorDescription = findField("error_description");

        // 对端带回 error 就算一次完整回答（换取阶段会带着它拒）；两者都没有则是「不是授权回调的形状」
        if (!parsed.error.empty())
        {
            return parsed;
        }
        if (parsed.code.empty())
        {
            return std::unexpected("回调里既没有 code 也没有 error：不是一次授权响应的形状");
        }
        return parsed;
    }

    std::expected<std::string, std::string> OAuthAuthorizationCodeClient::computePkceChallenge(const std::string_view codeVerifier)
    {
        const std::optional<std::string> digest = sha256Digest(codeVerifier);
        if (!digest.has_value())
        {
            return std::unexpected("PKCE 的 SHA-256 计算失败（EVP_Digest 未成功）");
        }
        // S256：BASE64URL-ENCODE(SHA256(ASCII(code_verifier)))，无填充（RFC 7636 §4.2）
        return Base::base64UrlEncode(*digest);
    }

    Core::Task<std::expected<OAuthAccessToken, std::string>> OAuthAuthorizationCodeClient::exchangeCode(OAuthAuthorizationRequest request, OAuthAuthorizationCallback callback)
    {
        // 两道本地判都在接网之前：回调带 error 就按对端原因拒；state 不符按 CSRF 嫌疑拒（RFC 6749 §10.12）
        if (!callback.error.empty())
        {
            const std::string described = "error=" + callback.error + (callback.errorDescription.empty() ? std::string{} : "：" + callback.errorDescription);
            co_return std::unexpected("授权被拒（" + described + "）");
        }
        if (callback.state != request.state)
        {
            co_return std::unexpected("回调的 state 与本次授权请求的不一致（RFC 6749 §10.12 要求逐字核对）：可能是 CSRF 或串了两次授权，已拒绝换取");
        }
        if (callback.code.empty())
        {
            co_return std::unexpected("回调里没有授权码（code），无从换取");
        }
        if (request.verifier.empty())
        {
            co_return std::unexpected("缺少 PKCE verifier：换取授权码必须带上发起授权请求时生成的那一枚");
        }

        // 表单体落在局部串上：HttpClientRequest 的 body 是视图，须活到 send 用完为止；
        // redirect_uri 与授权请求里发出的那一份同源（同一份配置），§4.1.3 的「逐字相同」由此成立
        std::string formBody = "grant_type=authorization_code";
        formBody += "&code=" + percentEncodeFormComponent(callback.code);
        formBody += "&redirect_uri=" + percentEncodeFormComponent(m_options.redirectUri);
        formBody += "&client_id=" + percentEncodeFormComponent(m_options.clientId);
        formBody += "&code_verifier=" + percentEncodeFormComponent(request.verifier);

        const auto sent = co_await HttpClient::send(m_loop, m_options.tokenUrl, makeOAuthFormPost(formBody, m_options.clientId, m_options.clientSecret), m_options.requestTimeout);
        if (!sent.has_value())
        {
            co_return std::unexpected("令牌请求（" + m_options.tokenUrl + "）失败：" + sent.error());
        }
        if (sent->statusCode < 200 || sent->statusCode >= 300)
        {
            co_return std::unexpected("令牌端点（" + m_options.tokenUrl + "）回的是 HTTP " + std::to_string(sent->statusCode) + describeOauthError(sent->body));
        }

        std::expected<OAuthAccessToken, std::string> token = parseOAuthTokenResponse(sent->body);
        if (!token.has_value())
        {
            co_return std::unexpected("令牌端点（" + m_options.tokenUrl + "）：" + token.error());
        }
        co_return std::move(*token);
    }

} // namespace AsynGyanis::Net
