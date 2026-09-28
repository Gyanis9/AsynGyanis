#include "AcmeStubAuthority.h"

#include "Base/Coding/Base64.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net::TestSupport
{
    namespace
    {
        /// 各类资源的固定路径前缀，拼 URL 与注册路由都按这几个常量
        constexpr std::string_view kNewNoncePath      = "/acme/new-nonce";
        constexpr std::string_view kNewAccountPath    = "/acme/new-account";
        constexpr std::string_view kNewOrderPath      = "/acme/new-order";
        constexpr std::string_view kAccountPath       = "/acme/acct/";
        constexpr std::string_view kOrderPath         = "/acme/order/";
        constexpr std::string_view kFinalizeSuffix    = "/finalize";
        constexpr std::string_view kAuthorizationPath = "/acme/authz/";
        constexpr std::string_view kChallengePath     = "/acme/chall/";
        constexpr std::string_view kCertificatePath   = "/acme/cert/";

        /// HTTP-01 令牌的固定位置（RFC 8555 §8.3）
        constexpr std::string_view kChallengeDirectory = "/.well-known/acme-challenge/";

        /// 桩自报的条款地址：只为让「没接受条款就不许建号」这条判据有东西可指
        constexpr std::string_view kTermsOfServiceUrl = "https://stub.invalid/acme/terms";

        /// 取一次自证令牌的时限：桩与应答方在同一条循环上，正常应当是毫秒级
        constexpr std::chrono::milliseconds kValidationTimeout{2000};

        /// 一段随机字节 → base64url：nonce 与令牌都靠它造
        [[nodiscard]] std::string makeOpaqueToken(const std::string &salt)
        {
            // 桩不需要密码学强度的不可预测性，需要的是同一轮里不重复：把 salt 与一个全局计数混进摘要，
            // 两次调用绝不可能撞出同一个串
            static std::atomic<std::size_t> counter{0};
            const std::size_t               sequence = counter.fetch_add(1);
            const std::string               material = std::format("{}#{}", salt, sequence);

            std::array<unsigned char, 32> digest{};
            EVP_Digest(material.data(), material.size(), digest.data(), nullptr, EVP_sha256(), nullptr);
            return Base::base64UrlEncode(std::string_view(reinterpret_cast<const char *>(digest.data()), 16));
        }

        /// SHA-256 后 base64url，桩自己那份指纹算法（与被测实现手写的成员顺序无关）
        [[nodiscard]] std::string sha256Base64Url(const std::string &text)
        {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int                               digestLength = 0;
            if (EVP_Digest(text.data(), text.size(), digest.data(), &digestLength, EVP_sha256(), nullptr) != 1)
            {
                return {};
            }
            return Base::base64UrlEncode(std::string_view(reinterpret_cast<const char *>(digest.data()), digestLength));
        }

        /// 取一个字符串成员，缺失或类型不对交回空
        [[nodiscard]] std::string textOf(const Base::ConfigValue &value, const std::string_view name)
        {
            if (!value.is_object() || !value.contains(name))
            {
                return {};
            }
            return Base::configValueAs<std::string>(value.at(name)).value_or(std::string{});
        }
    } // namespace

    AcmeStubAuthority::AcmeStubAuthority(Core::EventLoop &loop, Settings settings) :
        m_loop(loop), m_settings(std::move(settings)), m_validationAuthority(m_settings.validationAuthority)
    {
        auto minted = mintTestCertificateAuthority("AsynGyanis ACME 桩签发机构");
        if (minted.has_value())
        {
            m_authority = std::move(*minted);
        }
    }

    AcmeStubAuthority::~AcmeStubAuthority() = default;

    bool AcmeStubAuthority::start(const std::uint16_t port)
    {
        // CA 没造出来（内存不足一类）就别让后面的请求去解引用空的签发密钥
        if (!m_authority.privateKey || !m_authority.certificate)
        {
            return false;
        }

        m_server       = std::make_unique<HttpServer>(m_loop, Core::InetAddress::localhost(port));
        Router &router = m_server->router();
        router.get("/directory", [this](HttpRequest &request, HttpResponse &response) { return handleDirectory(request, response); });
        router.get(kNewNoncePath.data(), [this](HttpRequest &request, HttpResponse &response) { return handleNewNonce(request, response); });
        router.post(kNewAccountPath.data(), [this](HttpRequest &request, HttpResponse &response) { return handleNewAccount(request, response); });
        router.post(kNewOrderPath.data(), [this](HttpRequest &request, HttpResponse &response) { return handleNewOrder(request, response); });
        router.post(std::string(kAccountPath) + ":id", [this](HttpRequest &request, HttpResponse &response) { return handleAccount(request, response); });
        router.post(std::string(kAuthorizationPath) + ":id", [this](HttpRequest &request, HttpResponse &response) { return handleAuthorization(request, response); });
        router.post(std::string(kChallengePath) + ":id", [this](HttpRequest &request, HttpResponse &response) { return handleChallenge(request, response); });
        router.post(std::string(kOrderPath) + ":id", [this](HttpRequest &request, HttpResponse &response) { return handleOrder(request, response); });
        router.post(std::string(kOrderPath) + ":id/finalize", [this](HttpRequest &request, HttpResponse &response) { return handleFinalize(request, response); });
        router.post(std::string(kCertificatePath) + ":id", [this](HttpRequest &request, HttpResponse &response) { return handleCertificate(request, response); });

        // start() 交回的是服务器主协程本身：必须把它投进循环并让帧活到循环停下，
        // 否则监听建不起来，端口会一直是 0
        m_startTask = m_server->start();
        m_loop.scheduler().schedule(m_startTask->handle());
        // 端口要到接受协程真跑起来才可见，因此这里不把端口当「起没起起来」的判据：
        // CA 造出来了、协程也投出去了，就是成功；用例在循环上等端口发布
        return true;
    }

    void AcmeStubAuthority::stop()
    {
        // 只收监听：接受循环的协程帧要等循环自己把它跑到收尾再放，
        // 在这里销毁一个还挂在定时器上的帧就是与那次恢复抢同一份内存
        if (m_server)
        {
            m_server->stop();
        }
    }

    std::uint16_t AcmeStubAuthority::port() const noexcept
    {
        // 绑定发生在 start() 投递的接受协程里，内核分配的端口要等循环转起来才发布：
        // 这里每次都现问，桩里的 URL 因此只在循环上拼才对
        return m_server ? m_server->listeningPort() : 0;
    }

    std::string AcmeStubAuthority::directoryUrl() const
    {
        return port() == 0 ? std::string{} : std::format("http://127.0.0.1:{}/directory", port());
    }

    const AcmeStubAuthority::Evidence &AcmeStubAuthority::evidence() const noexcept
    {
        return m_evidence;
    }

    void AcmeStubAuthority::setValidationAuthority(std::string authority)
    {
        m_validationAuthority = std::move(authority);
    }

    std::string AcmeStubAuthority::unknownAccountUrl() const
    {
        return std::format("http://127.0.0.1:{}/acme/acct/999999", port());
    }

    std::string AcmeStubAuthority::issueNonce()
    {
        std::string nonce = makeOpaqueToken("nonce");
        m_outstandingNonces.insert(nonce);
        return nonce;
    }

    void AcmeStubAuthority::writeProblem(HttpResponse &response, const int statusCode, const std::string_view problemType, const std::string_view detail)
    {
        Base::ConfigValue body = Base::ConfigValue::object();
        body["type"]           = std::format("urn:ietf:params:acme:error:{}", problemType);
        body["detail"]         = std::string(detail);
        writeJson(response, statusCode, body);
    }

    void AcmeStubAuthority::writeJson(HttpResponse &response, const int statusCode, const Base::ConfigValue &body, const std::string_view location)
    {
        const std::string text = Base::serializeConfigValue(body).value_or("{}");
        response.setStatus(statusCode);
        response.setHeader("content-type", "application/json");
        // 每次应答都顺带发一个新 nonce（RFC 8555 §6.5），客户端因此不必每次都先 HEAD 一次
        response.setHeader("replay-nonce", issueNonce());
        if (!location.empty())
        {
            response.setHeader("location", location);
        }
        response.setBody(text);
    }

    std::optional<std::string> AcmeStubAuthority::canonicalJsonWebKeyText(const std::string_view jwkText)
    {
        const auto parsed = Base::parseConfigValue(jwkText);
        if (!parsed.has_value() || !parsed->is_object())
        {
            return std::nullopt;
        }
        const auto kind = parsed->contains("kty") ? Base::configValueAs<std::string>(parsed->at("kty")) : std::nullopt;
        if (!kind.has_value())
        {
            return std::nullopt;
        }

        // 只留 RFC 7518 的必需成员，再交给 JSON 序列化按键排序输出：这条规范化路径与实现侧
        // 「手写成员顺序」是两份代码，两边算出的指纹必须相等，否则自证串永远配不上
        Base::ConfigValue canonical = Base::ConfigValue::object();
        if (*kind == "RSA")
        {
            if (!parsed->contains("e") || !parsed->contains("kty") || !parsed->contains("n"))
            {
                return std::nullopt;
            }
            canonical["e"]   = parsed->at("e");
            canonical["kty"] = parsed->at("kty");
            canonical["n"]   = parsed->at("n");
        } else if (*kind == "EC")
        {
            if (!parsed->contains("crv") || !parsed->contains("kty") || !parsed->contains("x") || !parsed->contains("y"))
            {
                return std::nullopt;
            }
            canonical["crv"] = parsed->at("crv");
            canonical["kty"] = parsed->at("kty");
            canonical["x"]   = parsed->at("x");
            canonical["y"]   = parsed->at("y");
        } else
        {
            return std::nullopt;
        }
        return Base::serializeConfigValue(canonical);
    }

    std::optional<AcmeStubAuthority::AuthenticatedJws> AcmeStubAuthority::authenticate(const std::string_view bodyText, const std::string &requestUrl, HttpResponse &failure)
    {
        const auto envelope = Base::parseConfigValue(bodyText);
        if (!envelope.has_value() || !envelope->is_object())
        {
            writeProblem(failure, 400, "malformed", "请求正文不是一份 JWS 的 JSON");
            return std::nullopt;
        }
        const std::string protectedSegment = textOf(*envelope, "protected");
        const std::string payloadSegment   = textOf(*envelope, "payload");
        const std::string signatureSegment = textOf(*envelope, "signature");
        if (protectedSegment.empty() || signatureSegment.empty())
        {
            writeProblem(failure, 400, "malformed", "JWS 缺 protected 或 signature 段");
            return std::nullopt;
        }

        const auto protectedBytes = Base::base64UrlDecode(protectedSegment);
        const auto protectedJson  = protectedBytes.has_value() ? Base::parseConfigValue(*protectedBytes) : std::nullopt;
        if (!protectedJson.has_value() || !protectedJson->is_object())
        {
            writeProblem(failure, 400, "malformed", "protected 段解不开或不是 JSON 对象");
            return std::nullopt;
        }

        ParsedJws jws;
        jws.protectedText = *protectedBytes;
        jws.algorithm     = textOf(*protectedJson, "alg");
        jws.nonce         = textOf(*protectedJson, "nonce");
        jws.url           = textOf(*protectedJson, "url");
        jws.kid           = textOf(*protectedJson, "kid");
        if (protectedJson->contains("jwk") && (*protectedJson)["jwk"].is_object())
        {
            // 头里的 jwk 用**序列化后**的文本记账：验签要靠它重建公钥，注册后的账户也要留着它
            jws.jwkText = Base::serializeConfigValue((*protectedJson)["jwk"]).value_or(std::string{});
        }
        if (const auto payloadBytes = Base::base64UrlDecode(payloadSegment); payloadBytes.has_value())
        {
            jws.payloadText = *payloadBytes;
        } else if (!payloadSegment.empty())
        {
            writeProblem(failure, 400, "malformed", "payload 段不是合法的 base64url");
            return std::nullopt;
        }
        jws.signingInput = protectedSegment + "." + payloadSegment;
        jws.signature    = signatureSegment;

        // 头里的 url 与实际请求的 URL 必须逐字相同（RFC 8555 §6.2）：不一致意味着一份签名被拿去
        // 打另一个端点，桩照此拒掉就等于替真机构把这道门守住
        if (jws.url != requestUrl)
        {
            writeProblem(failure, 400, "malformed", std::format("JWS 头里的 url 与本次请求的 URL 不一致：{} 对 {}", jws.url, requestUrl));
            return std::nullopt;
        }

        if (jws.algorithm != "RS256" && jws.algorithm != "ES256")
        {
            writeProblem(failure, 400, "badalg", std::format("桩只验得起 RS256 与 ES256，收到的是 {}", jws.algorithm));
            return std::nullopt;
        }

        // POST-as-GET（payload 段为空）必须用 kid：RFC 8555 §6.3 明写它得用已建好的账户身份
        if (payloadSegment.empty() && !jws.jwkText.empty())
        {
            writeProblem(failure, 400, "malformed", "POST-as-GET 不能带 jwk，必须用账户的 kid");
            return std::nullopt;
        }

        // 公钥来源：jwk（只有注册那次）或 kid 指向的账户记录。两条都是**从 JWK 重建**，
        // 与被测实现写出 JWK 的那条路是两份代码
        std::unique_ptr<EVP_PKEY, Net::detail::EvpKeyDeleter> publicKey;
        Account                                               account;
        if (!jws.jwkText.empty())
        {
            ++m_evidence.jwkBearingRequestCount;
            publicKey = publicKeyFromJsonWebKeyText(jws.jwkText);
            // 桩按自己那份规范化算指纹：它与实现侧手写的成员顺序无关，两边一旦分叉，
            // 自证串就配不上，而这条分叉只能在这里被抓出来
            const auto canonical = canonicalJsonWebKeyText(jws.jwkText);
            if (!canonical.has_value())
            {
                writeProblem(failure, 400, "malformed", "jwk 里缺该 kty 的必需成员，桩无法规范化");
                return std::nullopt;
            }
            account.jwkText    = jws.jwkText;
            account.thumbprint = sha256Base64Url(*canonical);
        } else
        {
            const auto found = std::ranges::find(m_accounts, jws.kid, &Account::url);
            if (found == m_accounts.end())
            {
                writeProblem(failure, 401, "accountDoesNotExist", std::format("桩没有 {} 这个账户的记录", jws.kid));
                return std::nullopt;
            }
            account   = *found;
            publicKey = publicKeyFromJsonWebKeyText(account.jwkText);
        }
        if (!publicKey)
        {
            writeProblem(failure, 400, "malformed", "按交出的 JWK 重建公钥失败");
            return std::nullopt;
        }

        const AcmeKeyAlgorithm algorithm = jws.algorithm == "RS256" ? AcmeKeyAlgorithm::Rs256 : AcmeKeyAlgorithm::Es256;
        if (!verifyJsonWebSignature(*publicKey, algorithm, jws.signingInput, jws.signature))
        {
            ++m_evidence.rejectedSignatureCount;
            writeProblem(failure, 401, "malformed", "JWS 签名验不过");
            return std::nullopt;
        }
        ++m_evidence.verifiedSignatureCount;

        const auto nonceUsed = m_outstandingNonces.extract(jws.nonce);
        if (nonceUsed.empty())
        {
            writeProblem(failure, 400, "badNonce", std::format("nonce {} 没用过或已用过", jws.nonce));
            ++m_evidence.badNonceReplyCount;
            return std::nullopt;
        }
        // 故障排在签名与 nonce 都已核过之后：这样注入出来的 badNonce 是「本来会成功的一次」，
        // 用例断言客户端重发之后拿到成功，才排除了「因为别的原因失败」这种解释
        if (m_evidence.badNonceReplyCount < m_settings.badNonceFaults)
        {
            ++m_evidence.badNonceReplyCount;
            writeProblem(failure, 400, "badNonce", "桩按故障注入拒掉这一次合法的 nonce");
            return std::nullopt;
        }

        return AuthenticatedJws{std::move(account), std::move(jws.payloadText), std::move(jws.nonce), std::move(jws)};
    }

    Core::Task<void> AcmeStubAuthority::handleDirectory(HttpRequest &, HttpResponse &response)
    {
        const std::string root = std::format("http://127.0.0.1:{}", port());

        Base::ConfigValue body = Base::ConfigValue::object();
        body["newNonce"]       = root + std::string(kNewNoncePath);
        body["newAccount"]     = root + std::string(kNewAccountPath);
        body["newOrder"]       = root + std::string(kNewOrderPath);
        Base::ConfigValue meta = Base::ConfigValue::object();
        if (m_settings.isTermsOfServicePublished)
        {
            meta["termsOfService"] = std::string(kTermsOfServiceUrl);
        }
        if (m_settings.isExternalAccountRequired)
        {
            meta["externalAccountRequired"] = true;
        }
        body["meta"] = std::move(meta);
        writeJson(response, 200, body);
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleNewNonce(HttpRequest &, HttpResponse &response)
    {
        response.setStatus(200);
        response.setHeader("content-type", "text/plain");
        response.setHeader("replay-nonce", issueNonce());
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleNewAccount(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }

        const auto payload = Base::parseConfigValue(auth->payloadText);
        if (!payload.has_value() || !payload->is_object())
        {
            writeProblem(response, 400, "malformed", "账户注册的正文不是 JSON 对象");
            co_return;
        }

        // 条款：桩发布了条款地址就要求注册请求显式同意（RFC 8555 §7.3）
        if (m_settings.isTermsOfServicePublished &&
            (!payload->contains("termsOfServiceAgreed") || !payload->at("termsOfServiceAgreed").is_boolean() || !payload->at("termsOfServiceAgreed").get<bool>()))
        {
            writeProblem(response, 403, "agreementRequired", "本桩要求注册前接受服务条款");
            co_return;
        }

        // 外部账户绑定：验那段 JWS 是不是用桩配的 HMAC 密钥、按桩配的 kid 签在账户 JWK 上
        if (m_settings.isExternalAccountRequired)
        {
            if (!payload->contains("externalAccountBinding") || !(*payload)["externalAccountBinding"].is_string())
            {
                writeProblem(response, 400, "externalAccountRequired", "本桩要求外部账户绑定，注册正文里没有 externalAccountBinding");
                co_return;
            }
            const auto bindingText = (*payload)["externalAccountBinding"].get<std::string>();
            const auto dotFirst    = bindingText.find('.');
            const auto dotSecond   = bindingText.find('.', dotFirst == std::string::npos ? dotFirst : dotFirst + 1);
            if (dotFirst == std::string::npos || dotSecond == std::string::npos)
            {
                writeProblem(response, 400, "malformed", "externalAccountBinding 不是三段式的 JWS");
                co_return;
            }
            const std::string bindingProtectedSegment = bindingText.substr(0, dotFirst);
            const std::string bindingPayloadSegment   = bindingText.substr(dotFirst + 1, dotSecond - dotFirst - 1);
            const std::string bindingSignatureSegment = bindingText.substr(dotSecond + 1);
            const auto        bindingProtectedBytes   = Base::base64UrlDecode(bindingProtectedSegment);
            const auto        bindingProtectedJson    = bindingProtectedBytes.has_value() ? Base::parseConfigValue(*bindingProtectedBytes) : std::nullopt;
            if (!bindingProtectedJson.has_value())
            {
                writeProblem(response, 400, "malformed", "externalAccountBinding 的 protected 段解不开");
                co_return;
            }
            if (textOf(*bindingProtectedJson, "alg") != "HS256" || textOf(*bindingProtectedJson, "kid") != m_settings.externalAccountKeyId ||
                textOf(*bindingProtectedJson, "url") != requestUrl)
            {
                writeProblem(response, 401, "unauthorized", "externalAccountBinding 的 alg、kid 或 url 与桩的配置不符");
                co_return;
            }
            const auto secretBytes = Base::base64UrlDecode(m_settings.externalAccountKeySecret);
            if (!secretBytes.has_value())
            {
                writeProblem(response, 500, "urn:ietf:params:acme:error:serverInternal", "桩自己的 HMAC 密钥配置不合法");
                co_return;
            }
            const std::string                          bindingSigningInput = bindingProtectedSegment + "." + bindingPayloadSegment;
            std::array<unsigned char, EVP_MAX_MD_SIZE> expected{};
            unsigned int                               expectedLength = 0;
            if (HMAC(EVP_sha256(), secretBytes->data(), static_cast<int>(secretBytes->size()), reinterpret_cast<const unsigned char *>(bindingSigningInput.data()),
                     bindingSigningInput.size(), expected.data(), &expectedLength) == nullptr)
            {
                writeProblem(response, 500, "serverInternal", "桩算 HMAC 失败");
                co_return;
            }
            const auto        actualSignatureBytes = Base::base64UrlDecode(bindingSignatureSegment);
            const std::string expectedText(reinterpret_cast<const char *>(expected.data()), expectedLength);
            if (!actualSignatureBytes.has_value() || *actualSignatureBytes != expectedText)
            {
                writeProblem(response, 401, "unauthorized", "externalAccountBinding 的 HMAC 签名验不过");
                co_return;
            }
            // 载荷必须是账户公钥的 JWK 本身：签对了对象才算绑定
            if (Base::base64UrlEncode(auth->jws.jwkText) != bindingPayloadSegment)
            {
                writeProblem(response, 401, "unauthorized", "externalAccountBinding 的载荷不是这次注册所用的账户 JWK");
                co_return;
            }
        }

        // 同一个 JWK 重复注册按 RFC 交回既有账户（200 而不是 409）：客户端换进程重启时就是走这条路
        if (const auto found = std::ranges::find(m_accounts, auth->jws.jwkText, &Account::jwkText); found != m_accounts.end())
        {
            Base::ConfigValue body = Base::ConfigValue::object();
            body["status"]         = "valid";
            writeJson(response, 200, body, found->url);
            co_return;
        }

        Account created            = std::move(auth->account);
        created.url                = std::format("http://127.0.0.1:{}{}{}", port(), kAccountPath, ++m_sequence);
        m_evidence.firstAccountUrl = created.url;
        m_accounts.push_back(created);

        Base::ConfigValue body = Base::ConfigValue::object();
        body["status"]         = "valid";
        if (payload->contains("contact"))
        {
            body["contact"] = payload->at("contact");
        }
        writeJson(response, 201, body, created.url);
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleNewOrder(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        if (m_settings.isOrderRateLimited)
        {
            response.setStatus(429);
            response.setHeader("retry-after", "3600");
            response.setHeader("replay-nonce", issueNonce());
            response.setBody("桩按故障注入拒绝这一单");
            co_return;
        }

        const auto payload = Base::parseConfigValue(auth->payloadText);
        if (!payload.has_value() || !payload->is_object() || !payload->contains("identifiers") || !(*payload)["identifiers"].is_array())
        {
            writeProblem(response, 400, "malformed", "订单正文里没有 identifiers 数组");
            co_return;
        }

        Order order;
        order.url         = std::format("http://127.0.0.1:{}{}{}", port(), kOrderPath, ++m_sequence);
        order.accountUrl  = auth->account.url;
        order.status      = "pending";
        order.finalizeUrl = order.url + std::string(kFinalizeSuffix);

        for (const Base::ConfigValue &identifier: (*payload)["identifiers"])
        {
            if (textOf(identifier, "type") != "dns")
            {
                writeProblem(response, 400, "unsupportedIdentifierType", "桩只认 dns 类型的标识");
                co_return;
            }
            const std::string domainName = textOf(identifier, "value");
            if (std::ranges::find(m_settings.rejectedDomainNames, domainName) != m_settings.rejectedDomainNames.end())
            {
                writeProblem(response, 400, "rejectedIdentifier", std::format("桩按故障注入拒绝域名 {}", domainName));
                co_return;
            }

            const std::size_t authorizationSequence = ++m_sequence;
            Authorization     authorization;
            authorization.url          = std::format("http://127.0.0.1:{}{}{}", port(), kAuthorizationPath, authorizationSequence);
            authorization.identifier   = domainName;
            authorization.status       = "pending";
            authorization.challengeUrl = std::format("http://127.0.0.1:{}{}{}", port(), kChallengePath, authorizationSequence);

            Challenge challenge;
            challenge.url                      = authorization.challengeUrl;
            challenge.token                    = makeOpaqueToken(domainName);
            challenge.status                   = "pending";
            challenge.remainingProcessingPolls = m_settings.challengeProcessingPolls;
            order.authorizationUrls.push_back(authorization.url);
            m_challenges[challenge.url]         = std::move(challenge);
            m_authorizations[authorization.url] = std::move(authorization);
        }
        if (order.authorizationUrls.empty())
        {
            writeProblem(response, 400, "malformed", "identifiers 是空数组，桩没法给自证留位置");
            co_return;
        }

        ++m_evidence.issuedOrderCount;
        m_orders[order.url] = order;
        writeJson(response, 201, orderBody(order), order.url);
        co_return;
    }

    Base::ConfigValue AcmeStubAuthority::orderBody(const Order &order) const
    {
        Base::ConfigValue body = Base::ConfigValue::object();
        body["status"]         = order.status;
        body["finalize"]       = order.finalizeUrl;
        body["authorizations"] = Base::ConfigValue::array();
        body["identifiers"]    = Base::ConfigValue::array();
        for (const std::string &authorizationUrl: order.authorizationUrls)
        {
            body["authorizations"].push_back(authorizationUrl);
            const auto found = m_authorizations.find(authorizationUrl);
            if (found == m_authorizations.end())
            {
                continue;
            }
            Base::ConfigValue identifier = Base::ConfigValue::object();
            identifier["type"]           = "dns";
            identifier["value"]          = found->second.identifier;
            body["identifiers"].push_back(std::move(identifier));
        }
        if (!order.certificateUrl.empty())
        {
            body["certificate"] = order.certificateUrl;
        }
        return body;
    }

    Core::Task<void> AcmeStubAuthority::handleAccount(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        Base::ConfigValue body = Base::ConfigValue::object();
        body["status"]         = "valid";
        writeJson(response, 200, body);
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleAuthorization(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        const auto found = m_authorizations.find(requestUrl);
        if (found == m_authorizations.end())
        {
            writeProblem(response, 404, "malformed", "桩没有这条授权");
            co_return;
        }

        const auto challenge = m_challenges.find(found->second.challengeUrl);
        if (challenge == m_challenges.end())
        {
            writeProblem(response, 404, "malformed", "这条授权指向的挑战不存在");
            co_return;
        }

        Base::ConfigValue challengeJson = Base::ConfigValue::object();
        challengeJson["type"]           = m_settings.offeredChallengeType;
        challengeJson["url"]            = challenge->second.url;
        // http-01 才有 token；别的类型给 token 反而会把客户端引到一条它答不了的挑战上
        if (m_settings.offeredChallengeType == "http-01")
        {
            challengeJson["token"] = challenge->second.token;
        }
        Base::ConfigValue identifierJson = Base::ConfigValue::object();
        identifierJson["type"]           = "dns";
        identifierJson["value"]          = found->second.identifier;

        Base::ConfigValue challenges = Base::ConfigValue::array();
        challenges.push_back(std::move(challengeJson));

        Base::ConfigValue body = Base::ConfigValue::object();
        body["status"]         = found->second.status;
        body["identifier"]     = std::move(identifierJson);
        body["challenges"]     = std::move(challenges);
        writeJson(response, 200, body);
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleChallenge(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        const auto found = m_challenges.find(requestUrl);
        if (found == m_challenges.end())
        {
            writeProblem(response, 404, "malformed", "桩没有这条挑战");
            co_return;
        }

        Challenge &challenge = found->second;
        const bool isTrigger = !auth->payloadText.empty();
        if (isTrigger)
        {
            if (challenge.remainingProcessingPolls > 0)
            {
                // 报 processing 而不去取令牌：客户端必须真在轮询才拿得到终局，
                // 一次就 valid 的桩测不出「不轮询就直接定稿」这种退化
                challenge.status = "processing";
            } else
            {
                co_await validateHttp01(challenge, auth->account);
            }
        } else if (challenge.status == "processing")
        {
            if (challenge.remainingProcessingPolls > 1)
            {
                --challenge.remainingProcessingPolls;
            } else
            {
                co_await validateHttp01(challenge, auth->account);
            }
        }

        // 授权随挑战一起走：定稿那一步要看的是授权的状态，因此按「哪条授权指向这条挑战」找回去
        for (auto &[authorizationUrl, authorization]: m_authorizations)
        {
            static_cast<void>(authorizationUrl);
            if (authorization.challengeUrl == challenge.url)
            {
                authorization.status = challenge.status;
            }
        }

        Base::ConfigValue body = Base::ConfigValue::object();
        body["type"]           = m_settings.offeredChallengeType;
        body["url"]            = challenge.url;
        body["token"]          = challenge.token;
        body["status"]         = challenge.status;
        if (!challenge.detail.empty())
        {
            Base::ConfigValue errorJson = Base::ConfigValue::object();
            errorJson["type"]           = "urn:ietf:params:acme:error:connection";
            errorJson["detail"]         = challenge.detail;
            body["error"]               = std::move(errorJson);
        }
        writeJson(response, 200, body);
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::validateHttp01(Challenge &challenge, const Account &account)
    {
        challenge.isValidated  = true;
        const std::string path = std::string(kChallengeDirectory) + challenge.token;
        const std::string url  = std::format("http://{}{}", m_validationAuthority, path);

        auto fetched = co_await HttpClient::get(m_loop, url, kValidationTimeout);
        ++m_evidence.challengeFetchCount;
        m_evidence.lastChallengeFetchPath = path;
        if (fetched == nullptr)
        {
            challenge.status = "invalid";
            challenge.detail = std::format("桩取 {} 时一条应答都没拿到（连接失败或超时）", url);
            co_return;
        }
        m_evidence.lastFetchedBody = fetched->body;
        if (fetched->statusCode != 200)
        {
            challenge.status = "invalid";
            challenge.detail = std::format("桩取 {} 得到 HTTP {}，不是 200", url, fetched->statusCode);
            co_return;
        }

        // 期望正文由桩自己那份规范化算出：与被测实现手写的成员顺序无关
        const std::string expectedKeyAuthorization = challenge.token + "." + account.thumbprint;
        if (fetched->body != expectedKeyAuthorization)
        {
            challenge.status = "invalid";
            challenge.detail = std::format("令牌正文与期望的 keyAuthorization 不符。桩期望 {}，读到的是 {}", expectedKeyAuthorization, fetched->body);
            co_return;
        }
        challenge.status = "valid";
        challenge.detail.clear();
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleOrder(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        const auto found = m_orders.find(requestUrl);
        if (found == m_orders.end())
        {
            writeProblem(response, 404, "malformed", "桩没有这张订单");
            co_return;
        }
        writeJson(response, 200, orderBody(found->second));
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleFinalize(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        const std::string id         = request.param("id").value_or(std::string{});
        const std::string orderUrl   = std::format("http://127.0.0.1:{}{}{}", port(), kOrderPath, id);
        const auto        foundOrder = m_orders.find(orderUrl);
        if (foundOrder == m_orders.end())
        {
            writeProblem(response, 404, "malformed", "桩没有这张订单");
            co_return;
        }

        // 授权没全走成 valid 就定稿：真机构会 403，桩照做——这条判据钉的是「客户端确实把每条自证都做成了」
        for (const std::string &authorizationUrl: foundOrder->second.authorizationUrls)
        {
            const auto authorization = m_authorizations.find(authorizationUrl);
            if (authorization == m_authorizations.end() || authorization->second.status != "valid")
            {
                writeProblem(response, 403, "unauthorized",
                             std::format("域名 {} 的授权还没走成 valid", authorization == m_authorizations.end() ? "?" : authorization->second.identifier));
                co_return;
            }
        }

        const auto payload = Base::parseConfigValue(auth->payloadText);
        if (!payload.has_value() || !payload->is_object() || !payload->contains("csr"))
        {
            writeProblem(response, 400, "malformed", "定稿正文里没有 csr");
            co_return;
        }
        const std::string encodedRequest = textOf(*payload, "csr");
        const auto        issued         = issueCertificatePemFromRequest(m_authority, encodedRequest, m_certificateSerial++, m_settings.certificateValidDays);
        if (!issued.has_value())
        {
            writeProblem(response, 400, "malformed", "桩解不开这份 CSR，或它没有 SAN");
            co_return;
        }

        Order &order                         = foundOrder->second;
        order.status                         = "valid";
        order.certificateUrl                 = std::format("http://127.0.0.1:{}{}{}", port(), kCertificatePath, ++m_sequence);
        m_certificates[order.certificateUrl] = std::move(*issued);
        m_evidence.issuedCertificateCount++;
        m_evidence.issuedCertificatePem = m_certificates[order.certificateUrl];
        writeJson(response, 200, orderBody(order));
        co_return;
    }

    Core::Task<void> AcmeStubAuthority::handleCertificate(HttpRequest &request, HttpResponse &response)
    {
        const std::string requestUrl = std::format("http://127.0.0.1:{}{}", port(), std::string(request.path()));
        const auto        auth       = authenticate(request.body(), requestUrl, response);
        if (!auth.has_value())
        {
            co_return;
        }
        const auto found = m_certificates.find(requestUrl);
        if (found == m_certificates.end())
        {
            writeProblem(response, 404, "malformed", "桩没有这张证书");
            co_return;
        }
        // 证书这一步回的是 PEM 正文本身，不是 JSON（RFC 8555 §7.4.2.1）
        response.setStatus(200);
        response.setHeader("content-type", "application/pem-certificate-chain");
        response.setHeader("replay-nonce", issueNonce());
        response.setBody(found->second);
        co_return;
    }
} // namespace AsynGyanis::Net::TestSupport
