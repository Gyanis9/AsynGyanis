#include "Net/Http/OAuthIntrospection.h"

#include "Base/Coding/Base64.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/OAuthSupport.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace AsynGyanis::Net
{
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
        introspection.active = activeField->get<bool>();

        // 可选文本字段逐个读：缺失是缺省（空串），类型不符是协议不符——不静默按缺省、也不抛异常
        const std::pair<const char *, std::string *> optionalTextFields[] = {{"scope", &introspection.scope},
                                                                             {"client_id", &introspection.clientId},
                                                                             {"username", &introspection.username},
                                                                             {"sub", &introspection.subject},
                                                                             {"token_type", &introspection.tokenType}};
        for (const auto &[fieldName, target]: optionalTextFields)
        {
            std::expected<std::string, std::string> field = optionalJsonString(document, fieldName);
            if (!field.has_value())
            {
                co_return std::unexpected("自省响应：" + field.error());
            }
            *target = std::move(*field);
        }

        // exp 是可选整数：给了但类型不对同样按协议不符拒——0 的语义是「端点未给出」，不让错类型借道
        if (const auto expiration = document.find("exp"); expiration != document.end())
        {
            if (!expiration->is_number_integer())
            {
                co_return std::unexpected("自省响应：字段「exp」不是整数（RFC 7662 §2.2 的 exp 是 Number）");
            }
            introspection.expiresAt = expiration->get<std::int64_t>();
        }

        co_return introspection;
    }

} // namespace AsynGyanis::Net
