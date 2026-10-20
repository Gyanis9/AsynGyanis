#include "Net/Http/JwksKeyProvider.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/OAuthSupport.h"
#include "Net/Http/OAuthError.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace AsynGyanis::Net
{
    JwksKeyProvider::JwksKeyProvider(EventLoop &loop, JwksKeyProviderOptions options) : m_loop(loop), m_timer(loop), m_options(std::move(options))
    {
    }

    JwksKeyProvider::~JwksKeyProvider()
    {
        // 后台帧由成员持有：这里交给 Core::Task 的析构收束。帧此刻要么已结束、要么挂起在
        // 定时/IO 等待上——挂起中的帧可安全销毁，帧内的等待器随帧一起撤销
    }

    std::shared_ptr<JwksKeyProvider> JwksKeyProvider::start(EventLoop &loop, JwksKeyProviderOptions options)
    {
        if (options.jwksUrl.empty())
        {
            throw Base::InvalidArgumentException("JwksKeyProvider: jwksUrl 不能为空——没有端点就没有可拉取的密钥");
        }
        if (options.refreshInterval.count() <= 0)
        {
            throw Base::InvalidArgumentException("JwksKeyProvider: refreshInterval 必须为正——非正的轮换周期会把拉取变成无间隔循环");
        }
        if (options.requestTimeout.count() <= 0)
        {
            throw Base::InvalidArgumentException("JwksKeyProvider: requestTimeout 必须为正");
        }
        if (options.refreshRequestSlice.count() <= 0)
        {
            throw Base::InvalidArgumentException("JwksKeyProvider: refreshRequestSlice 必须为正——零或负的分片会让周期等待立即返回，把轮换变成忙等");
        }

        std::shared_ptr<JwksKeyProvider> provider(new JwksKeyProvider(loop, std::move(options)));

        // 起后台循环并把首次恢复投到所属循环：惰性协程的首次恢复必须发生在循环线程上，
        // scheduleRemote 允许调用方在任意线程（启动路径通常在主线程）
        provider->m_refreshTask.emplace(provider->refreshLoop());
        provider->m_loop.scheduler().scheduleRemote(provider->m_refreshTask->handle());
        return provider;
    }

    void JwksKeyProvider::recordFailure(std::string reason)
    {
        const std::lock_guard<std::mutex> guard(m_errorMutex);
        m_lastError = std::move(reason);
    }

    Core::Task<void> JwksKeyProvider::fetchOnce()
    {
        HttpClientRequest request;
        request.method = "GET";

        auto sent = co_await HttpClient::send(m_loop, m_options.jwksUrl, request, m_options.requestTimeout);
        if (!sent.has_value())
        {
            recordFailure("拉取 JWKS（" + m_options.jwksUrl + "）失败：" + sent.error());
            co_return;
        }
        if (sent->statusCode < 200 || sent->statusCode >= 300)
        {
            recordFailure("JWKS 端点（" + m_options.jwksUrl + "）回的是 HTTP " + std::to_string(sent->statusCode));
            co_return;
        }

        const nlohmann::json document = nlohmann::json::parse(sent->body, nullptr, false);
        if (document.is_discarded() || !document.is_object() || !document.contains("keys") || !document["keys"].is_array())
        {
            recordFailure("JWKS 端点（" + m_options.jwksUrl + "）的正文不是带 keys 数组的 JSON 对象");
            co_return;
        }

        auto parsedKeys = std::make_shared<KeySet>();
        for (const auto &key: document["keys"])
        {
            // 只收 RSA 公钥（本件配套 RS256 验签）；其余 kty 静默跳过属于「还有别的键类型」的常态，
            // 但若整份文档一条 RSA 都没有，下面会以「解析出 0 条」失败——不放过空壳文档
            if (!key.is_object())
            {
                continue;
            }
            const OAuthExpected<std::string> keyType = optionalJsonString(key, "kty");
            if (!keyType.has_value() || *keyType != "RSA")
            {
                continue;
            }
            // n / e 类型不符的键跳过（与不认识的 kty 同处置）：单条坏记录不该让整次轮换判失败，
            // 更不该把读字段的异常带进这条后台协程——它死了没有任何人会重来
            const OAuthExpected<std::string> modulus  = optionalJsonString(key, "n");
            const OAuthExpected<std::string> exponent = optionalJsonString(key, "e");
            if (!modulus.has_value() || !exponent.has_value())
            {
                continue;
            }
            const std::string         kid       = optionalJsonString(key, "kid").value_or(std::string{});
            std::shared_ptr<EVP_PKEY> publicKey = rsaPublicKeyFromJwk(*modulus, *exponent);
            if (publicKey == nullptr)
            {
                continue;
            }
            // kid 缺失的键挂在空串下：头里也不带 kid 的令牌会走到「遍历全部键」那一路
            parsedKeys->insert_or_assign(kid, std::move(publicKey));
        }

        if (parsedKeys->empty())
        {
            recordFailure("JWKS 端点（" + m_options.jwksUrl + "）里没有可用的 RSA 公钥");
            co_return;
        }

        // 整份原子换入：读侧要么看到旧快照、要么看到新快照，不会看到半份
        m_keys.store(std::move(parsedKeys));
        recordFailure("");
    }

    Core::Task<void> JwksKeyProvider::refreshLoop()
    {
        for (;;)
        {
            co_await fetchOnce();

            // 等待一个周期，但按 refreshRequestSlice 分片醒来：未知 kid 的提前刷新请求以分片为上限，
            // 否则等 300 秒的周期会让「签发方刚轮换、我们还没跟上」的窗口拖满整个周期
            auto waited = std::chrono::milliseconds{0};
            while (waited < m_options.refreshInterval)
            {
                if (m_refreshRequested.exchange(false))
                {
                    break;
                }
                co_await m_timer.waitFor(m_options.refreshRequestSlice);
                waited += m_options.refreshRequestSlice;
            }
        }
    }

    std::optional<std::string> JwksKeyProvider::verify(const std::string_view token, const JwtClaimExpectations &expectations,
                                                       const std::chrono::system_clock::time_point now) const
    {
        const std::optional<ParsedJwt> parsed = parseJwt(token);
        if (!parsed.has_value())
        {
            return "JWT 形状或编码非法（需要三段 base64url、头与载荷为 JSON 对象）";
        }

        // 算法白名单与纯层同一条纪律：只认 RS256（none/HS256 在验签之前就拒）；
        // alg 类型不符按「不是 RS256」拒——读取口不抛异常，同一条判据接住
        const nlohmann::json header = nlohmann::json::parse(parsed->headerJson, nullptr, false);
        if (header.is_discarded() || !header.is_object() || optionalJsonString(header, "alg").value_or(std::string{}) != "RS256")
        {
            return "JWT 的签名算法不是 RS256（本件只认 RS256，alg=none 与 HS256 一律拒）";
        }

        const std::shared_ptr<const KeySet> keys = m_keys.load();
        if (keys == nullptr)
        {
            return "JWKS 尚未成功拉取过（首次拉取还没完成或一直失败），无法验签";
        }

        // 读 kid 也走不抛的读取口：字段存在但类型不是字符串时 value() 会抛 type_error，
        // 与上面 alg 同一套判据——按「没带 kid」处理，落到遍历键集那一路
        const std::string kid = optionalJsonString(header, "kid").value_or(std::string{});
        if (!kid.empty())
        {
            const auto key = keys->find(kid);
            if (key == keys->end())
            {
                // 轮换的先导信号：请求后台提前刷新一次，本次仍按现有键集拒绝（不阻塞本请求）
                m_refreshRequested.store(true);
                return "JWT 的 kid「" + kid + "」不在当前密钥集里（已请求提前刷新，稍后重试）";
            }
            if (!verifyRs256(*parsed, key->second.get()))
            {
                return "JWT 签名校验失败（公钥不匹配或令牌被篡改）";
            }
            return validateJwtClaims(parsed->payloadJson, expectations, now);
        }

        // 头里没带 kid（单键服务常见）：遍历当前全部键，任一验过即过；都不行为才拒
        for (const auto &[keyId, publicKey]: *keys)
        {
            if (verifyRs256(*parsed, publicKey.get()))
            {
                return validateJwtClaims(parsed->payloadJson, expectations, now);
            }
        }
        return "JWT 没有带 kid，且当前密钥集里的任何公钥都验不过它的签名";
    }

    std::size_t JwksKeyProvider::cachedKeyCount() const noexcept
    {
        const std::shared_ptr<const KeySet> keys = m_keys.load();
        return keys != nullptr ? keys->size() : 0;
    }

    std::string JwksKeyProvider::lastError() const
    {
        const std::lock_guard<std::mutex> guard(m_errorMutex);
        return m_lastError;
    }

} // namespace AsynGyanis::Net
