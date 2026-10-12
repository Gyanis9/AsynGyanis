#include "Net/Http/OAuthIntrospection.h"

#include "Base/Coding/Base64.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Client/HttpClient.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// form-urlencoded 的百分号编码：未经保留字符原样、其余按 %XX；空格编 %20（不用 '+'，免与真值里的加号混淆）
        [[nodiscard]] std::string percentEncodeFormComponent(const std::string_view text)
        {
            constexpr char kHexDigits[] = "0123456789ABCDEF";

            std::string encoded;
            encoded.reserve(text.size());
            for (const char character: text)
            {
                const unsigned char byte = static_cast<unsigned char>(character);
                const bool          isUnreserved =
                        (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
                if (isUnreserved)
                {
                    encoded.push_back(character);
                    continue;
                }
                encoded.push_back('%');
                encoded.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
                encoded.push_back(kHexDigits[byte & 0x0FU]);
            }
            return encoded;
        }
    } // namespace

    OAuthIntrospectionClient::OAuthIntrospectionClient(EventLoop &loop, OAuthIntrospectionOptions options) : m_loop(loop), m_options(std::move(options))
    {
        if (m_options.introspectionUrl.empty())
        {
            throw Base::InvalidArgumentException("OAuthIntrospectionClient: introspectionUrl 不能为空——没有端点就无从自省");
        }
        if (m_options.requestTimeout.count() <= 0)
        {
            throw Base::InvalidArgumentException("OAuthIntrospectionClient: requestTimeout 必须为正");
        }
    }

    Core::Task<std::expected<OAuthTokenIntrospection, std::string>> OAuthIntrospectionClient::introspect(const std::string_view token)
    {
        // HttpClientRequest 的 body / contentType 是 string_view：拼接出来的表单体必须先落在
        // 局部串上（协程帧持有它，活到 send 用完为止），直接赋一个临时 string 就是悬垂视图
        const std::string formBody = "token=" + percentEncodeFormComponent(token) + "&token_type_hint=access_token";

        HttpClientRequest request;
        request.method      = "POST";
        request.contentType = "application/x-www-form-urlencoded";
        request.body        = formBody;

        // 凭据走 Basic（RFC 6749 §2.3.1）：client_id 作用户名、client_secret 作口令；
        // 未给 clientId 时不发认证头（公开客户端/网关代管的部署）
        if (!m_options.clientId.empty())
        {
            request.headers.emplace_back("authorization", "Basic " + Base::base64Encode(m_options.clientId + ":" + m_options.clientSecret));
        }

        auto sent = co_await HttpClient::send(m_loop, m_options.introspectionUrl, request, m_options.requestTimeout);
        if (!sent.has_value())
        {
            co_return std::unexpected("自省请求（" + m_options.introspectionUrl + "）失败：" + sent.error());
        }
        if (sent->statusCode < 200 || sent->statusCode >= 300)
        {
            co_return std::unexpected("自省端点（" + m_options.introspectionUrl + "）回的是 HTTP " + std::to_string(sent->statusCode) +
                                      "；按 RFC 7662 无效令牌也应以 200 + active:false 回答，非 2xx 属协议不符");
        }

        const nlohmann::json document = nlohmann::json::parse(sent->body, nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            co_return std::unexpected("自省端点（" + m_options.introspectionUrl + "）的正文不是 JSON 对象");
        }

        // active 是 RFC 7662 §2.2 的必填字段：缺了不能猜成 false——「端点没回答」与「回答不活跃」是两件事
        const auto activeField = document.find("active");
        if (activeField == document.end() || !activeField->is_boolean())
        {
            co_return std::unexpected("自省响应的 active 字段缺失或不是布尔（RFC 7662 §2.2 的必填字段）");
        }

        OAuthTokenIntrospection introspection;
        introspection.active    = activeField->get<bool>();
        introspection.scope     = document.value("scope", std::string{});
        introspection.clientId  = document.value("client_id", std::string{});
        introspection.username  = document.value("username", std::string{});
        introspection.subject   = document.value("sub", std::string{});
        introspection.tokenType = document.value("token_type", std::string{});
        if (const auto expiration = document.find("exp"); expiration != document.end() && expiration->is_number_integer())
        {
            introspection.expiresAt = expiration->get<std::int64_t>();
        }

        co_return introspection;
    }

} // namespace AsynGyanis::Net
