#include "Net/Http/OAuthDeviceCode.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/OAuthSupport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 8628 §3.4 的授权类型标识（逐字；冒号由表单编码器编成 %3A）
        constexpr std::string_view kDeviceCodeGrantType = "urn:ietf:params:oauth:grant-type:device_code";

        /// RFC 8628 §3.5：slow_down 时间隔必须增加 5 秒
        constexpr std::chrono::seconds kSlowDownPenalty{5};

        /// RFC 8628 §3.2：服务端没给 interval 时客户端必须用的默认值
        constexpr std::chrono::seconds kDefaultPollingInterval{5};
    } // namespace

    OAuthDeviceCodeClient::OAuthDeviceCodeClient(EventLoop &loop, OAuthDeviceCodeOptions options) : m_loop(loop), m_timer(loop), m_options(std::move(options))
    {
        if (m_options.deviceAuthorizationUrl.empty())
        {
            throw Base::InvalidArgumentException("OAuthDeviceCodeClient: deviceAuthorizationUrl 不能为空——没有端点就取不到设备码");
        }
        if (m_options.tokenUrl.empty())
        {
            throw Base::InvalidArgumentException("OAuthDeviceCodeClient: tokenUrl 不能为空——没有端点就换不到令牌");
        }
        if (m_options.clientId.empty())
        {
            throw Base::InvalidArgumentException("OAuthDeviceCodeClient: clientId 不能为空——RFC 8628 §3.1 要求公开客户端必须带它");
        }
        if (m_options.requestTimeout.count() <= 0)
        {
            throw Base::InvalidArgumentException("OAuthDeviceCodeClient: requestTimeout 必须为正");
        }
    }

    Core::Task<std::expected<OAuthDeviceAuthorization, std::string>> OAuthDeviceCodeClient::requestDeviceAuthorization()
    {
        // 表单体先落在局部串上：HttpClientRequest 的 body 是视图，须活到 send 用完为止
        std::string formBody = "client_id=" + percentEncodeFormComponent(m_options.clientId);
        if (!m_options.scope.empty())
        {
            formBody += "&scope=" + percentEncodeFormComponent(m_options.scope);
        }

        const auto sent = co_await HttpClient::send(m_loop, m_options.deviceAuthorizationUrl, makeOAuthFormPost(formBody, m_options.clientId, m_options.clientSecret),
                                                    m_options.requestTimeout);
        if (!sent.has_value())
        {
            co_return std::unexpected("设备授权请求（" + m_options.deviceAuthorizationUrl + "）失败：" + sent.error());
        }
        if (sent->statusCode < 200 || sent->statusCode >= 300)
        {
            co_return std::unexpected("设备授权端点（" + m_options.deviceAuthorizationUrl + "）回的是 HTTP " + std::to_string(sent->statusCode) + describeOauthError(sent->body));
        }

        const nlohmann::json document = nlohmann::json::parse(sent->body, nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            co_return std::unexpected("设备授权端点（" + m_options.deviceAuthorizationUrl + "）的正文不是 JSON 对象");
        }

        OAuthDeviceAuthorization authorization;

        const std::expected<std::string, std::string> deviceCode = requiredJsonString(document, "device_code");
        if (!deviceCode.has_value())
        {
            co_return std::unexpected("设备授权响应：" + deviceCode.error());
        }
        authorization.deviceCode = *deviceCode;

        const std::expected<std::string, std::string> userCode = requiredJsonString(document, "user_code");
        if (!userCode.has_value())
        {
            co_return std::unexpected("设备授权响应：" + userCode.error());
        }
        authorization.userCode = *userCode;

        const std::expected<std::string, std::string> verificationUri = requiredJsonString(document, "verification_uri");
        if (!verificationUri.has_value())
        {
            co_return std::unexpected("设备授权响应：" + verificationUri.error());
        }
        authorization.verificationUri = *verificationUri;

        const std::expected<std::chrono::seconds, std::string> expiresIn = requiredPositiveSeconds(document, "expires_in");
        if (!expiresIn.has_value())
        {
            co_return std::unexpected("设备授权响应：" + expiresIn.error());
        }
        authorization.expiresIn = *expiresIn;

        const std::expected<std::string, std::string> completeUri = optionalJsonString(document, "verification_uri_complete");
        if (!completeUri.has_value())
        {
            co_return std::unexpected("设备授权响应：" + completeUri.error());
        }
        authorization.verificationUriComplete = *completeUri;

        // interval 没给才回退默认；给了坏值（非正整数）按协议不符拒——「没给」与「给了坏值」是两回事
        const std::expected<std::chrono::seconds, std::string> interval = optionalPositiveSeconds(document, "interval", kDefaultPollingInterval);
        if (!interval.has_value())
        {
            co_return std::unexpected("设备授权响应：" + interval.error());
        }
        authorization.interval = *interval;

        co_return authorization;
    }

    Core::Task<std::expected<OAuthAccessToken, std::string>> OAuthDeviceCodeClient::waitForToken(OAuthDeviceAuthorization authorization)
    {
        // 取值校验放最前：手拼的 authorization（不经 requestDeviceAuthorization）不该把轮询带进无界状态
        if (authorization.interval.count() <= 0)
        {
            co_return std::unexpected("轮询间隔必须为正（authorization.interval=" + std::to_string(authorization.interval.count()) + " 秒）");
        }
        if (authorization.expiresIn.count() <= 0)
        {
            co_return std::unexpected("设备码有效期必须为正（authorization.expiresIn=" + std::to_string(authorization.expiresIn.count()) + " 秒）");
        }

        const auto deadline     = std::chrono::steady_clock::now() + authorization.expiresIn;
        const auto lifetimeOver = [&authorization]
        { return std::unexpected("device_code 的有效期（" + std::to_string(authorization.expiresIn.count()) + " 秒）已过，停止轮询：用户未在时限内完成授权"); };

        for (;;)
        {
            // §3.5：每次新请求之前先等满当前间隔（第一次同样等——等满在任何读法下都不越线）；
            // 等待不越过剩余有效期，醒来时已到点的这一枚注定过期的轮询不再发出去
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
            {
                co_return lifetimeOver();
            }
            co_await m_timer.waitFor(std::min(remaining, std::chrono::duration_cast<std::chrono::milliseconds>(authorization.interval)));
            if (std::chrono::steady_clock::now() >= deadline)
            {
                co_return lifetimeOver();
            }

            // 轮询一次（表单体落在局部串上，视图须活到 send 用完为止）
            std::string formBody = "grant_type=" + percentEncodeFormComponent(kDeviceCodeGrantType);
            formBody += "&device_code=" + percentEncodeFormComponent(authorization.deviceCode);
            formBody += "&client_id=" + percentEncodeFormComponent(m_options.clientId);

            const auto sent =
                    co_await HttpClient::send(m_loop, m_options.tokenUrl, makeOAuthFormPost(formBody, m_options.clientId, m_options.clientSecret), m_options.requestTimeout);
            if (!sent.has_value())
            {
                co_return std::unexpected("轮询令牌端点（" + m_options.tokenUrl + "）失败：" + sent.error());
            }

            // 2xx：拿到令牌就交回
            if (sent->statusCode >= 200 && sent->statusCode < 300)
            {
                std::expected<OAuthAccessToken, std::string> token = parseOAuthTokenResponse(sent->body);
                if (!token.has_value())
                {
                    co_return std::unexpected("令牌端点（" + m_options.tokenUrl + "）：" + token.error());
                }
                co_return std::move(*token);
            }

            // 非 2xx：按 RFC 6749 §5.2 读 error 字段定处置（§3.5 只认两种可续轮的错误码）
            const nlohmann::json                          document   = nlohmann::json::parse(sent->body, nullptr, false);
            const std::expected<std::string, std::string> errorField = optionalJsonString(document, "error");
            if (!errorField.has_value() || errorField->empty())
            {
                co_return std::unexpected("轮询令牌端点（" + m_options.tokenUrl + "）回了 HTTP " + std::to_string(sent->statusCode) +
                                          "，且正文没有 RFC 6749 §5.2 形状的 error 字段");
            }

            const std::string &errorCode = *errorField;
            if (errorCode == "authorization_pending")
            {
                // 用户还没确认：按现间隔续轮
                continue;
            }
            if (errorCode == "slow_down")
            {
                // 间隔 +5 秒，「此后所有请求」都用新值
                authorization.interval = pollingIntervalAfter(authorization.interval, errorCode);
                continue;
            }

            const std::string description = optionalJsonString(document, "error_description").value_or(std::string{});
            const std::string described   = "error=" + errorCode + (description.empty() ? std::string{} : "：" + description);
            if (errorCode == "access_denied")
            {
                co_return std::unexpected("设备授权被拒（" + described + "）");
            }
            if (errorCode == "expired_token")
            {
                co_return std::unexpected("设备码已过期（" + described + "）——需要重新发起设备授权");
            }
            co_return std::unexpected("令牌端点回了未预期的错误（" + described + "）：RFC 8628 §3.5 只有 authorization_pending 与 slow_down 可续轮，其余一律终止");
        }
    }

    std::chrono::seconds OAuthDeviceCodeClient::pollingIntervalAfter(const std::chrono::seconds currentInterval, const std::string_view errorCode) noexcept
    {
        // RFC 8628 §3.5：slow_down 时间隔必须增加 5 秒，「本请求与之后所有请求」都用新值——
        // 调用方把返回值存回当前间隔，即得「此后都用新值」
        if (errorCode == "slow_down")
        {
            return currentInterval + kSlowDownPenalty;
        }
        return currentInterval;
    }

} // namespace AsynGyanis::Net
