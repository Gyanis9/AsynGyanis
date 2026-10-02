#include "Net/Acme/AcmeClient.h"

#include "Base/Coding/Base64.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogEscaping.h"
#include "Base/Log/LogMacros.h"
#include "Core/Crypto/Digest.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpDate.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 8555 §6.7 里问题类型的固定前缀，机构交回的 "type" 一定带着它
        constexpr std::string_view kProblemTypePrefix = "urn:ietf:params:acme:error:";

        /// 客户端标识的头部名（RFC 8555 §6.1 要求每条请求都带）
        constexpr std::string_view kUserAgentHeaderName = "User-Agent";

        /**
         * @brief 本框架 ACME 客户端的标识串
         * @details 不只是礼貌：Boulder 与 Pebble 对缺这条头的请求一律回 400 malformed
         *          （Pebble 的原文是 "All requests MUST include a User-Agent header"），
         *          连取目录这一步都过不去，整台状态机第一步就断。它因此必须挂在**每一条**请求上，
         *          包括不带签名的目录 GET 与 newNonce 的 HEAD。
         */
        constexpr std::string_view kAcmeUserAgentValue = "AsynGyanis ACME client (+https://github.com/Gyanis9/AsynGyanis)";

        /**
         * @brief 给一份出站请求补上客户端标识
         * @details 三处请求构造都走这里，而不是各自记得加：漏一处就等于那条通路对着 Boulder 直接 400
         */
        void applyClientIdentification(HttpClientRequest &request)
        {
            request.headers.emplace_back(std::string(kUserAgentHeaderName), std::string(kAcmeUserAgentValue));
        }

        /// 机构发的新 nonce 与新建资源的地址都在响应头里
        constexpr std::string_view kReplayNonceHeader = "Replay-Nonce";
        constexpr std::string_view kLocationHeader    = "Location";
        constexpr std::string_view kRetryAfterHeader  = "Retry-After";

        /// jose+json 是 ACME 唯一的请求正文类型（RFC 8555 §6.3），且机构会按它拒收别的写法
        constexpr std::string_view kJoseContentType = "application/jose+json";

        /// 域名标签与总长的上限（RFC 1035 §2.3.4）：超限的名字机构一定拒，本地先判省一次往返
        constexpr std::size_t kMaximumLabelLength  = 63U;
        constexpr std::size_t kMaximumDomainLength = 253U;

        /// 账户在机构侧已不存在的错误类型名（RFC 8555 §6.7），本机记的账户 URL 失效就是它
        constexpr std::string_view kAccountDoesNotExistProblem = "accountDoesNotExist";

        /**
         * @brief 折成小写 ASCII 用于头部名比对
         * @details HTTP 头部名大小写不敏感，而本框架的客户端按收到什么留什么（不做折叠），
         *          因此读 Replay-Nonce 与 Location 必须自己按大小写不敏感找
         */
        [[nodiscard]] std::string toLowerCaseAscii(const std::string_view text)
        {
            std::string lowered;
            lowered.reserve(text.size());
            for (const char character: text)
            {
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
            }
            return lowered;
        }

        [[nodiscard]] std::string findHeader(const std::vector<std::pair<std::string, std::string>> &headers, const std::string_view name)
        {
            const std::string wanted = toLowerCaseAscii(name);
            const auto        found  = std::ranges::find_if(headers, [&wanted](const auto &field) { return toLowerCaseAscii(field.first) == wanted; });
            return found == headers.end() ? std::string{} : found->second;
        }

        /**
         * @brief 读一个字符串成员，缺失或类型不对都交回空而不是抛
         */
        [[nodiscard]] std::optional<std::string> readStringMember(const Base::ConfigValue &value, const std::string_view memberName)
        {
            if (!value.is_object() || !value.contains(memberName))
            {
                return std::nullopt;
            }
            return Base::configValueAs<std::string>(value.at(memberName));
        }

        /**
         * @brief 读一个字符串数组成员（订单里的 authorizations 那一档）
         */
        [[nodiscard]] std::vector<std::string> readStringArrayMember(const Base::ConfigValue &value, const std::string_view memberName)
        {
            std::vector<std::string> collected;
            if (!value.is_object() || !value.contains(memberName) || !value.at(memberName).is_array())
            {
                return collected;
            }
            for (const Base::ConfigValue &element: value.at(memberName))
            {
                if (const auto text = Base::configValueAs<std::string>(element); text.has_value())
                {
                    collected.push_back(*text);
                }
            }
            return collected;
        }

        /**
         * @brief 从一段应答正文里提出 RFC 8555 的问题类型短名（去掉 urn 前缀）
         * @return std::optional<std::string> 没有正文、不是 JSON 对象或没有 type 时为空
         */
        [[nodiscard]] std::optional<std::string> readProblemType(const std::string &bodyText)
        {
            const auto parsed = Base::parseConfigValue(bodyText);
            if (!parsed.has_value())
            {
                return std::nullopt;
            }
            const auto type = readStringMember(*parsed, "type");
            if (!type.has_value() || type->substr(0, kProblemTypePrefix.size()) != kProblemTypePrefix)
            {
                return type;
            }
            return type->substr(kProblemTypePrefix.size());
        }

        /**
         * @brief 按 RFC 8553 §3 核一个域名标识：字母数字、连字符、下划线与点，通配只在 dns-01 下允许
         * @details 通配要 dns-01 才验得了（RFC 8738；HTTP-01 没法让机构去取 `*.example` 的令牌），
         *          因此这条判据挂在**挑了哪种挑战**上，而不是挂在配置的一个字段名上。
         *          本地直接拒并说明原因，比让机构回一条 rejectedIdentifier 更有指导性
         */
        [[nodiscard]] std::optional<std::string> describeUnusableDomainName(const std::string &domainName, const bool allowsWildcard)
        {
            if (domainName.empty())
            {
                return "域名是空串";
            }
            if (domainName.size() > kMaximumDomainLength)
            {
                return std::format("域名长度 {} 超过 {} 的上限（RFC 1035 §2.3.4）", domainName.size(), kMaximumDomainLength);
            }

            std::string_view bareName{domainName};
            if (domainName.find('*') != std::string::npos)
            {
                if (!allowsWildcard)
                {
                    return "本客户端按配置要的是 http-01，它验不了通配域名。要覆盖 *.example.com 就把自证种类改成 dns-01，"
                           "或直接申请 example.com 与所需的具名子域";
                }
                // 机构只认「* 占满最左一段标签」这一种写法，且去掉前缀之后剩余部分仍要过下面那条 LDH 规则
                if (!domainName.starts_with("*.") || domainName.find('*', 1U) != std::string::npos)
                {
                    return std::format("通配域名只接受「*.example.com」这种 * 占满最左一段标签的写法，给的是：{}", domainName);
                }
                bareName = std::string_view{domainName}.substr(2);
                if (bareName.empty())
                {
                    return std::format("通配域名「{}」在 *. 之后没有内容", domainName);
                }
            }

            std::size_t labelLength = 0;
            for (const char character: bareName)
            {
                const bool isLetterOrDigit = std::isalnum(static_cast<unsigned char>(character)) != 0;
                if (character == '.')
                {
                    // 空标签（连续两点、开头或结尾的点）在 LDH 规则里不合法
                    if (labelLength == 0 || labelLength > kMaximumLabelLength)
                    {
                        return std::format("域名「{}」里有一个长度不合规的标签（每个标签须为 1~{} 个字符）", domainName, kMaximumLabelLength);
                    }
                    labelLength = 0;
                    continue;
                }
                if (!isLetterOrDigit && character != '-' && character != '_')
                {
                    return std::format("域名「{}」含字符 '{}'，RFC 8553 的标识只允许字母、数字、连字符、下划线与点", domainName, character);
                }
                ++labelLength;
            }
            if (labelLength == 0 || labelLength > kMaximumLabelLength)
            {
                return std::format("域名「{}」的最后一个标签长度不合规（须为 1~{} 个字符）", domainName, kMaximumLabelLength);
            }
            return std::nullopt;
        }

        /**
         * @brief 用机构发的 HMAC 密钥给外部账户绑定那一段做 HS256 签名
         * @details 载荷是账户公钥的 JWK，头部是 {"alg":"HS256","kid":标识,"url":newAccount}
         *          （RFC 8555 §7.3.4）。这一段与账户密钥的 JWS 是两次独立签名：算法不同（对称密钥），
         *          签名对象也不同（机构要拿它核对「这台机构发的账户号确实在用这把密钥」）。
         */
        [[nodiscard]] std::optional<std::string> buildExternalAccountBinding(const AcmeKeyPair &accountKey, const std::string &keyIdentifier, const std::string &encodedSecret,
                                                                             const std::string &newAccountUrl)
        {
            const auto secretBytes = Base::base64UrlDecode(encodedSecret);
            if (!secretBytes.has_value())
            {
                return std::nullopt;
            }

            Base::ConfigValue protectedHeader = Base::ConfigValue::object();
            protectedHeader["alg"]            = "HS256";
            protectedHeader["kid"]            = keyIdentifier;
            protectedHeader["url"]            = newAccountUrl;
            const auto protectedText          = Base::serializeConfigValue(protectedHeader);
            if (!protectedText.has_value())
            {
                return std::nullopt;
            }

            const std::string signingInput = Base::base64UrlEncode(*protectedText) + "." + Base::base64UrlEncode(accountKey.publicJsonWebKeyText());

            unsigned char digest[EVP_MAX_MD_SIZE]{};
            unsigned int  digestLength = 0;
            if (HMAC(EVP_sha256(), secretBytes->data(), static_cast<int>(secretBytes->size()), reinterpret_cast<const unsigned char *>(signingInput.data()), signingInput.size(),
                     digest, &digestLength) == nullptr ||
                digestLength == 0)
            {
                return std::nullopt;
            }
            return Base::base64UrlEncode(*protectedText) + "." + Base::base64UrlEncode(std::string_view(accountKey.publicJsonWebKeyText())) + "." +
                   Base::base64UrlEncode(std::string_view(reinterpret_cast<const char *>(digest), digestLength));
        }
    } // namespace

    AcmeClient::AcmeClient(Core::EventLoop &loop, Configuration configuration, const AcmeKeyPair &accountKey) :
        m_loop(loop), m_configuration(std::move(configuration)), m_accountKey(accountKey)
    {
    }

    std::string_view AcmeClient::accountUrl() const noexcept
    {
        return m_accountUrl;
    }

    std::string_view AcmeClient::termsOfServiceUrl() const noexcept
    {
        return m_termsOfServiceUrl;
    }

    Core::Task<std::expected<void, AcmeError>> AcmeClient::fetchFreshNonce()
    {
        HttpClientRequest request;
        request.method = "HEAD";
        applyClientIdentification(request);
        // 取 nonce 用 HEAD 而不是 GET：RFC 8555 §6.5 明说这一步不需要正文，GET 会让机构多送一份 JSON
        auto sent = co_await HttpClient::send(m_loop, m_newNonceUrl, request, m_configuration.requestTimeout);
        if (!sent.has_value())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::Transport, std::format("向 ACME 的 newNonce 端点 {} 取 nonce 失败：{}", m_newNonceUrl, sent.error())});
        }
        std::string nonce = findHeader(sent->headers, kReplayNonceHeader);
        if (nonce.empty())
        {
            co_return std::unexpected(
                    AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("ACME 机构在 {} 的应答里没有带 {} 头（HTTP {}）。没有 nonce 就签不出任何请求，这台机构不能用",
                                                                             m_newNonceUrl, kReplayNonceHeader, sent->statusCode)});
        }
        m_noncePool.push_back(std::move(nonce));
        co_return std::expected<void, AcmeError>{};
    }

    Core::Task<std::expected<std::string, AcmeError>> AcmeClient::takeNonce()
    {
        // 机构在每次应答里都顺带发一个新 nonce，先用池里那些，省掉一次往返
        if (m_noncePool.empty())
        {
            auto fetched = co_await fetchFreshNonce();
            if (!fetched.has_value())
            {
                co_return std::unexpected(fetched.error());
            }
        }
        std::string nonce = std::move(m_noncePool.back());
        m_noncePool.pop_back();
        co_return nonce;
    }

    Core::Task<std::expected<AcmeReply, AcmeError>> AcmeClient::postSignedRequest(const std::string &url, const std::string_view payloadJson, const bool isNewAccountRequest)
    {
        for (std::size_t attempt = 0;; ++attempt)
        {
            auto nonce = co_await takeNonce();
            if (!nonce.has_value())
            {
                co_return std::unexpected(nonce.error());
            }

            Base::ConfigValue protectedHeader = Base::ConfigValue::object();
            protectedHeader["alg"]            = std::string(m_accountKey.jsonWebAlgorithmName());
            if (isNewAccountRequest)
            {
                // 只有注册那一次能带 jwk（此时还没有账户 URL 可指），其余一律 kid（RFC 8555 §6.3）
                const auto parsedKey = Base::parseConfigValue(m_accountKey.publicJsonWebKeyText());
                if (!parsedKey.has_value())
                {
                    co_return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, "账户公钥的 JWK 文本没能解析成 JSON，请重新生成账户密钥"});
                }
                protectedHeader["jwk"] = *parsedKey;
            } else
            {
                protectedHeader["kid"] = m_accountUrl;
            }
            protectedHeader["nonce"] = *nonce;
            protectedHeader["url"]   = url;

            const auto protectedText = Base::serializeConfigValue(protectedHeader);
            if (!protectedText.has_value())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, "JWS 的 protected 头没能序列化成 JSON"});
            }

            // POST-as-GET 的正文是空串：payload 段编出来就是空，整串形如 "protected..signature"
            const std::string protectedEncoded = Base::base64UrlEncode(*protectedText);
            const std::string payloadEncoded   = Base::base64UrlEncode(payloadJson);
            const std::string signingInput     = protectedEncoded + "." + payloadEncoded;
            auto              signature        = m_accountKey.signJsonWebSigningInput(signingInput);
            if (!signature.has_value())
            {
                co_return std::unexpected(signature.error());
            }
            // ACME 的正文是 flattened JWS JSON 序列化（RFC 7515 §7.2.2，RFC 8555 §6.3 沿用）：
            // 三段点分串是 JWS Compact，直接把它当正文发出去会被机构判成「不是 JSON」
            Base::ConfigValue envelope = Base::ConfigValue::object();
            envelope["payload"]        = payloadEncoded;
            envelope["protected"]      = protectedEncoded;
            envelope["signature"]      = *signature;
            const auto envelopeText    = Base::serializeConfigValue(envelope);
            if (!envelopeText.has_value())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, "JWS 的 flattened JSON 正文没能序列化"});
            }

            HttpClientRequest request;
            request.method      = "POST";
            request.contentType = kJoseContentType;
            applyClientIdentification(request);
            // body 是视图：这份局部串活在本协程帧里，直到 co_await 返回才析构
            request.body = *envelopeText;

            auto sent = co_await HttpClient::send(m_loop, url, request, m_configuration.requestTimeout);
            if (!sent.has_value())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::Transport, std::format("ACME 请求 {} 没拿到响应：{}", url, sent.error())});
            }

            AcmeReply reply;
            reply.statusCode  = sent->statusCode;
            reply.locationUrl = findHeader(sent->headers, kLocationHeader);
            reply.replayNonce = findHeader(sent->headers, kReplayNonceHeader);
            reply.bodyText    = std::move(sent->body);
            // RFC 8555 §6.8 要求机构回 429 时必须带 Retry-After，并且客户端必须照办：这一项以前
            // 被解析出来又当场丢掉，退避完全由本地的 renewalCheckInterval 决定，等于对端的说法不进账
            if (const auto retryAfterText = findHeader(sent->headers, kRetryAfterHeader); !retryAfterText.empty())
            {
                reply.retryAfter = parseRetryAfter(retryAfterText, std::chrono::system_clock::now());
            }
            // 每次应答都换发一个新 nonce，留着下一次用（用坏的就撞上 badNonce）
            if (!reply.replayNonce.empty())
            {
                m_noncePool.push_back(reply.replayNonce);
            }

            // badNonce 是唯一「原样重发就能好」的失败：nonce 是一次性的，重取一个再来
            if (reply.statusCode == 400 && readProblemType(reply.bodyText) == "badNonce" && attempt < m_configuration.maximumNonceRetries)
            {
                continue;
            }
            co_return reply;
        }
    }

    AcmeError AcmeClient::describeFailure(const AcmeReply &reply, const std::string_view step)
    {
        std::string message = std::format("ACME 在「{}」这一步没有得到成功应答（HTTP {}）。", step, reply.statusCode);
        if (const auto parsed = Base::parseConfigValue(reply.bodyText); parsed.has_value())
        {
            if (const auto type = readStringMember(*parsed, "type"); type.has_value())
            {
                message += std::format("机构给的类型：{}。", *type);
            }
            if (const auto detail = readStringMember(*parsed, "detail"); detail.has_value())
            {
                // 机构给的自由文本要折过再进消息：这条消息最终由 `LOG_ERROR_FMT("AcmeCertificateManager: {}", ...)`
                // 落到日志行上，而 problem document 里的 detail 合法地可以带 `\n`（JSON 里写成 `\\n`，解析回来就是
                // 真换行）——一个不守规矩或被劫持的机构因此能在本进程的日志里伪造记录。折法与 h3 的错误串同源
                message += std::format("机构原文：{}。", Base::escapeForLog(*detail, 512));
            }
        } else if (!reply.bodyText.empty())
        {
            message += "机构的正文不是 JSON，读不出它为什么拒。";
        }

        if (reply.statusCode == 429 || reply.statusCode == 503)
        {
            message += "这一档要退避后再试而不是改配置：先查这一小时内的下单次数与并发数。";
            if (reply.retryAfter.has_value())
            {
                message += std::format("机构要求的等待时间是 {} 秒，已按它设置下一次尝试。", reply.retryAfter->count());
            }
            // 把机构的说法带上去：管理器据此决定退避门槛，而 RateLimited 的注释一直承诺的是这一档
            return AcmeError{AcmeErrorKind::RateLimited, std::move(message), reply.retryAfter};
        }
        message += "按上面的类型改对应的那一项（域名、联系邮箱、服务条款或账户状态）；原样重试只会再吃一次同样的失败。";
        return AcmeError{AcmeErrorKind::RejectedByAuthority, std::move(message)};
    }

    Core::Task<std::expected<void, AcmeError>> AcmeClient::prepareAccount(std::optional<std::string> persistedAccountUrl)
    {
        // 校验排在这里而不是构造函数：配置不合法与网络失败要从同一条 expected 通道交回，
        // 而构造函数没有错误通道
        if (m_configuration.directoryUrl.empty())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "ACME 配置里没有机构的目录 URL。填 directoryUrl，例如 Let's Encrypt 的 "
                                                                                     "https://acme-v02.api.letsencrypt.org/directory"});
        }
        const bool hasExternalAccountBinding = !m_configuration.externalAccountKeyId.empty() && !m_configuration.externalAccountKeySecret.empty();
        if (hasExternalAccountBinding == false && (!m_configuration.externalAccountKeyId.empty() || !m_configuration.externalAccountKeySecret.empty()))
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "外部账户绑定要标识与 HMAC 密钥两条一起给，这里只给了一条。"
                                                                                     "两条都有才成立，缺一条机构会直接拒掉账户注册"});
        }

        HttpClientRequest directoryRequest;
        directoryRequest.method = "GET";
        applyClientIdentification(directoryRequest);
        auto sent = co_await HttpClient::send(m_loop, m_configuration.directoryUrl, directoryRequest, m_configuration.requestTimeout);
        if (!sent.has_value())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::Transport, std::format("取 ACME 目录 {} 失败：{}", m_configuration.directoryUrl, sent.error())});
        }
        if (sent->statusCode < 200 || sent->statusCode >= 300)
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse,
                                                std::format("ACME 目录 {} 回的是 HTTP {}，不是一个可服务的机构地址", m_configuration.directoryUrl, sent->statusCode)});
        }

        const auto directory = Base::parseConfigValue(sent->body);
        if (!directory.has_value() || !directory->is_object())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("ACME 目录 {} 的正文不是 JSON 对象", m_configuration.directoryUrl)});
        }

        // 这三个端点少任何一个都没法走完一单，缺哪个就点名哪个
        struct RequiredEndpoint
        {
            std::string_view memberName;
            std::string     &destination;
            std::string_view purpose;
        };
        const RequiredEndpoint requiredEndpoints[] = {
                {"newNonce", m_newNonceUrl, "取一次性 nonce"},
                {"newAccount", m_newAccountUrl, "注册或查询账户"},
                {"newOrder", m_newOrderUrl, "下订单"},
        };
        for (const RequiredEndpoint &endpoint: requiredEndpoints)
        {
            const auto value = readStringMember(*directory, endpoint.memberName);
            if (!value.has_value() || value->empty())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("ACME 目录里没有 {} 端点（本框架要用它{}）。这台机构不是 RFC 8555 的 v2 服务，"
                                                                                                   "或者它的响应被改写过",
                                                                                                   endpoint.memberName, endpoint.purpose)});
            }
            endpoint.destination = *value;
        }

        if (directory->contains("meta") && (*directory)["meta"].is_object())
        {
            const Base::ConfigValue &meta = (*directory)["meta"];
            m_termsOfServiceUrl           = readStringMember(meta, "termsOfService").value_or(std::string{});
            if (meta.contains("externalAccountRequired") && meta.at("externalAccountRequired").is_boolean())
            {
                m_isExternalAccountRequired = meta.at("externalAccountRequired").get<bool>();
            }
        }
        // 目录应答里就带一个可用 nonce，先收下：省掉第一次的 HEAD
        if (const auto seededNonce = findHeader(sent->headers, kReplayNonceHeader); !seededNonce.empty())
        {
            m_noncePool.push_back(seededNonce);
        }

        if (m_isExternalAccountRequired && !hasExternalAccountBinding)
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, std::format("这家机构（{}）在目录里声明必须要外部账户绑定。请向机构申领账户标识与 HMAC 密钥，"
                                                                                                 "填进 externalAccountKeyId 与 externalAccountKeySecret",
                                                                                                 m_configuration.directoryUrl)});
        }

        m_accountUrl = persistedAccountUrl.value_or(std::string{});

        // 记着账户 URL 时先拿它问一次：机构那边的账户可能已被删（401 accountDoesNotExist），
        // 那属于「按 RFC 该重新建号」的情形，不该要求人工去删状态文件
        if (!m_accountUrl.empty())
        {
            auto probe = co_await postSignedRequest(m_accountUrl, {}, false);
            if (probe.has_value())
            {
                if (probe->statusCode >= 200 && probe->statusCode < 300)
                {
                    co_return std::expected<void, AcmeError>{};
                }
                if (readProblemType(probe->bodyText) == kAccountDoesNotExistProblem)
                {
                    m_accountUrl.clear();
                } else
                {
                    co_return std::unexpected(describeFailure(*probe, "查询已记录的账户"));
                }
            } else
            {
                co_return std::unexpected(probe.error());
            }
        }

        // 走到这里就是要建新账户。条款的接受是这次请求里的一个字段，因此必须在这里判
        if (!m_configuration.isTermsOfServiceAccepted)
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration,
                                                m_termsOfServiceUrl.empty() ? std::string("建 ACME 账户要先显式接受机构的服务条款（配置项 isTermsOfServiceAccepted），"
                                                                                          "而这家机构的目录里没给出条款地址。请先向机构确认条款再打开这一项")
                                                                            : std::format("建 ACME 账户要先显式接受机构的服务条款（配置项 isTermsOfServiceAccepted）。"
                                                                                          "条款地址：{}",
                                                                                          m_termsOfServiceUrl)});
        }

        Base::ConfigValue payload       = Base::ConfigValue::object();
        payload["termsOfServiceAgreed"] = true;
        if (!m_configuration.contactEmailAddress.empty())
        {
            payload["contact"] = Base::ConfigArray{m_configuration.contactEmailAddress};
        } else
        {
            // 不登记联系人在 RFC 8555 下是合法的（带 EAB 的机构往往本来就留空），因此这里不拒；
            // 但 90 天寿命的证书一旦漏续就是线上事故，而机构唯一能主动找你的通道就是这个地址。
            // 说一次，别让它静默
            LOG_WARN("AcmeClient: 账户没有登记 contactEmailAddress，机构无法在证书到期或账户异常时通知你；"
                     "请确认续期由本框架的 runRenewalLoop() 或外部监控兜住");
        }
        if (hasExternalAccountBinding)
        {
            const auto binding = buildExternalAccountBinding(m_accountKey, m_configuration.externalAccountKeyId, m_configuration.externalAccountKeySecret, m_newAccountUrl);
            if (!binding.has_value())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "外部账户绑定的 HMAC 密钥不是合法的 base64url，或签名这一步失败。"
                                                                                         "请用机构发的那份原文（不要多加换行）"});
            }
            payload["externalAccountBinding"] = *binding;
        }

        const auto payloadText = Base::serializeConfigValue(payload);
        if (!payloadText.has_value())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "账户注册的请求正文没能序列化成 JSON：检查联系邮箱里是否有非 UTF-8 字节"});
        }

        auto reply = co_await postSignedRequest(m_newAccountUrl, *payloadText, true);
        if (!reply.has_value())
        {
            co_return std::unexpected(reply.error());
        }
        if (reply->statusCode < 200 || reply->statusCode >= 300)
        {
            co_return std::unexpected(describeFailure(*reply, "注册 ACME 账户"));
        }
        // 账户 URL 只在新建时给（201）；机构按 200 回已有账户时正文里没有它，只能沿用请求的那条
        m_accountUrl = reply->locationUrl.empty() ? m_newAccountUrl : reply->locationUrl;
        co_return std::expected<void, AcmeError>{};
    }

    Core::Task<std::expected<AcmeOrder, AcmeError>> AcmeClient::createOrder(const std::vector<std::string> &domainNames)
    {
        if (domainNames.empty())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "下单至少要一个域名。请把这台服务对外的域名填进配置"});
        }
        // 通配能不能用取决于挑的是哪种挑战：dns-01 验得了，http-01 验不了
        const bool allowsWildcard = m_configuration.challengeKind == AcmeChallengeKind::Dns01;
        for (const std::string &domainName: domainNames)
        {
            if (const auto complaint = describeUnusableDomainName(domainName, allowsWildcard); complaint.has_value())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, std::format("ACME 下单被本地拦下：{}。", *complaint)});
            }
        }

        Base::ConfigValue identifiers = Base::ConfigValue::array();
        for (const std::string &domainName: domainNames)
        {
            Base::ConfigValue identifier = Base::ConfigValue::object();
            identifier["type"]           = "dns";
            identifier["value"]          = domainName;
            identifiers.push_back(std::move(identifier));
        }
        Base::ConfigValue payload = Base::ConfigValue::object();
        payload["identifiers"]    = std::move(identifiers);

        const auto payloadText = Base::serializeConfigValue(payload);
        if (!payloadText.has_value())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "下单的请求正文没能序列化成 JSON：域名里应有且只有 ASCII 合法字符"});
        }

        auto reply = co_await postSignedRequest(m_newOrderUrl, *payloadText, false);
        if (!reply.has_value())
        {
            co_return std::unexpected(reply.error());
        }
        if (reply->statusCode < 200 || reply->statusCode >= 300)
        {
            co_return std::unexpected(describeFailure(*reply, "下 ACME 订单"));
        }

        const auto order = Base::parseConfigValue(reply->bodyText);
        if (!order.has_value() || !order->is_object())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, "机构回来的订单不是 JSON 对象"});
        }

        AcmeOrder parsedOrder;
        parsedOrder.orderUrl          = reply->locationUrl;
        parsedOrder.finalizeUrl       = readStringMember(*order, "finalize").value_or(std::string{});
        parsedOrder.status            = readStringMember(*order, "status").value_or(std::string{});
        parsedOrder.certificateUrl    = readStringMember(*order, "certificate").value_or(std::string{});
        parsedOrder.authorizationUrls = readStringArrayMember(*order, "authorizations");
        if (parsedOrder.finalizeUrl.empty() || parsedOrder.authorizationUrls.empty())
        {
            co_return std::unexpected(
                    AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("机构回来的订单缺 finalize 或 authorizations（finalize 为空：{}，授权条数：{}）。"
                                                                             "没有这两条就交不出 CSR，也没有可答的挑战",
                                                                             parsedOrder.finalizeUrl.empty() ? "是" : "否", parsedOrder.authorizationUrls.size())});
        }
        co_return parsedOrder;
    }

    std::string dns01ValidationText(const std::string_view keyAuthorization)
    {
        // 与 HTTP-01 唯一的差别就是这一层摘要：SHA-256 后按 URL-safe 无填充 Base64 表示（RFC 8738 §3）
        const Core::Digest::Sha256Value digest = Core::Digest::sha256(keyAuthorization);
        return Base::base64UrlEncode(std::string_view(reinterpret_cast<const char *>(digest.data()), digest.size()));
    }

    std::string dns01RecordName(const std::string_view domainName)
    {
        // 通配符要把 "*." 去掉：*.example.com 与 example.com 是同一条 TXT，挂着 "*." 前缀去查只会查一条不存在的名字
        const std::string_view bareName = domainName.starts_with("*.") ? domainName.substr(2) : domainName;
        return "_acme-challenge." + std::string(bareName);
    }

    Core::Task<std::expected<AcmeAuthorization, AcmeError>> AcmeClient::fetchAuthorization(const std::string &authorizationUrl)
    {
        auto reply = co_await postSignedRequest(authorizationUrl, {}, false);
        if (!reply.has_value())
        {
            co_return std::unexpected(reply.error());
        }
        if (reply->statusCode < 200 || reply->statusCode >= 300)
        {
            co_return std::unexpected(describeFailure(*reply, "读取域名授权记录"));
        }

        const auto authorization = Base::parseConfigValue(reply->bodyText);
        if (!authorization.has_value() || !authorization->is_object())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("授权记录 {} 的正文不是 JSON 对象", authorizationUrl)});
        }

        AcmeAuthorization parsed;
        parsed.status = readStringMember(*authorization, "status").value_or(std::string{});
        if (authorization->contains("identifier") && (*authorization)["identifier"].is_object())
        {
            parsed.identifier = readStringMember((*authorization)["identifier"], "value").value_or(std::string{});
        }

        if (authorization->contains("challenges") && (*authorization)["challenges"].is_array())
        {
            // 只挑配置要的那一种：两种都填会让调用方自己决定发哪条给机构，而「挑了一条、答的却是
            // 另一条」机构只回一个 invalid，落到日志上看不出差在哪
            const bool                   wantsDns01 = (m_configuration.challengeKind == AcmeChallengeKind::Dns01);
            const std::string            wantedType = wantsDns01 ? "dns-01" : "http-01";
            std::string                  offeredTypes;
            std::optional<AcmeChallenge> picked;
            for (const Base::ConfigValue &challenge: (*authorization)["challenges"])
            {
                const auto type = readStringMember(challenge, "type");
                if (!type.has_value())
                {
                    continue;
                }
                offeredTypes += offeredTypes.empty() ? *type : ", " + *type;
                if (*type != wantedType)
                {
                    continue;
                }
                const auto token        = readStringMember(challenge, "token");
                const auto challengeUrl = readStringMember(challenge, "url");
                if (!token.has_value() || !challengeUrl.has_value())
                {
                    co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("域名 {} 的 {} 挑战缺 token 或 url，无法自证（机构给的类型：{}）",
                                                                                                       parsed.identifier, wantedType, offeredTypes)});
                }
                picked = AcmeChallenge{*challengeUrl, *token};
                break;
            }
            if (!picked.has_value())
            {
                co_return std::unexpected(
                        AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("机构为域名 {} 只提供了这些挑战类型：{}。本客户端按配置要的是 {}，"
                                                                                 "请换一家支持它的机构，或改配置要机构实际给的那一种",
                                                                                 parsed.identifier, offeredTypes.empty() ? "（一个都没给）" : offeredTypes, wantedType)});
            }
            if (wantsDns01)
            {
                parsed.dns01 = picked;
            } else
            {
                parsed.http01 = picked;
            }
        } else
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("域名 {} 的授权记录里没有 challenges 数组", parsed.identifier)});
        }
        co_return parsed;
    }

    Core::Task<std::expected<std::string, AcmeError>> AcmeClient::solveChallenge(const std::string &challengeUrl, const std::chrono::milliseconds pollInterval,
                                                                                 const std::chrono::milliseconds overallTimeout)
    {
        // 空 JSON 对象就是「请开始校验」的信号（RFC 8555 §7.5.1）：正文不带任何字段，
        // 但它是 payload 段而非 POST-as-GET 的空串
        const std::string triggerPayload = "{}";
        auto              triggered      = co_await postSignedRequest(challengeUrl, triggerPayload, false);
        if (!triggered.has_value())
        {
            co_return std::unexpected(triggered.error());
        }
        if (triggered->statusCode < 200 || triggered->statusCode >= 300)
        {
            co_return std::unexpected(describeFailure(*triggered, "触发自证校验"));
        }

        Core::Timer                                 timer(m_loop);
        const std::chrono::steady_clock::time_point deadline        = std::chrono::steady_clock::now() + overallTimeout;
        const auto                                  firstStatusJson = Base::parseConfigValue(triggered->bodyText);
        std::string                                 lastStatus = firstStatusJson.has_value() ? readStringMember(*firstStatusJson, "status").value_or(std::string{}) : std::string{};
        // 机构把自证失败的原因写在最后一次应答的 error 里：不带着它交回，运维就只能看到「判成 invalid」
        std::string lastBodyText = triggered->bodyText;
        for (;;)
        {
            if (lastStatus == "valid")
            {
                co_return lastStatus;
            }
            if (lastStatus == "invalid" || lastStatus == "expired" || lastStatus == "revoked" || lastStatus == "deactivated")
            {
                // 挑战走到 invalid 只有一种可能：机构去取自证时没拿到对的答案（404、连不上、
                // 或 keyAuthorization 不匹配）。含糊成「超时」会让人去查网络，而该查的是路由与端口
                std::string complaint;
                std::string complaintType;
                if (const auto body = Base::parseConfigValue(lastBodyText); body.has_value() && body->is_object() && body->contains("error") && (*body)["error"].is_object())
                {
                    // 同一条判据：机构留下的自由文本最终会被记进日志行，折过再拼（type 是 URN，
                    // 但它是同一批远程字节，没有理由单独信任）
                    complaint     = Base::escapeForLog(readStringMember((*body)["error"], "detail").value_or(std::string{}), 512);
                    complaintType = Base::escapeForLog(readStringMember((*body)["error"], "type").value_or(std::string{}), 128);
                }
                co_return std::unexpected(
                        AcmeError{AcmeErrorKind::ChallengeNotAnswered,
                                  std::format("机构把挑战判成 {}：它在自证时没能从本机取到正确答案。逐个核对——http-01 的路由是否已注册、"
                                              "机构能不能从公网访问到 80 端口、域名解析是否指向这台机器。挑战地址：{}。{}",
                                              lastStatus, challengeUrl,
                                              complaint.empty() ? "机构没留下进一步说明"
                                                                : std::format("机构给的类型：{}；原文：{}。", complaintType.empty() ? "（无）" : complaintType, complaint))});
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::Transport, std::format("自证挑战在 {} 内没给出终局（最后一次状态：{}）。机构可能仍在排队校验，"
                                                                                          "本轮按失败收口以免占住循环",
                                                                                          overallTimeout.count(), lastStatus)});
            }

            co_await timer.waitFor(pollInterval);
            auto polled = co_await postSignedRequest(challengeUrl, {}, false);
            if (!polled.has_value())
            {
                co_return std::unexpected(polled.error());
            }
            if (polled->statusCode < 200 || polled->statusCode >= 300)
            {
                co_return std::unexpected(describeFailure(*polled, "轮询自证挑战的状态"));
            }
            const auto statusJson = Base::parseConfigValue(polled->bodyText);
            lastStatus            = statusJson.has_value() ? readStringMember(*statusJson, "status").value_or(std::string{}) : std::string{};
            lastBodyText          = polled->bodyText;
        }
    }

    Core::Task<std::expected<AcmeOrder, AcmeError>> AcmeClient::finalizeOrder(const AcmeOrder &order, const std::string_view encodedRequest,
                                                                              const std::chrono::milliseconds pollInterval, const std::chrono::milliseconds overallTimeout)
    {
        Base::ConfigValue payload = Base::ConfigValue::object();
        payload["csr"]            = std::string(encodedRequest);
        const auto payloadText    = Base::serializeConfigValue(payload);
        if (!payloadText.has_value())
        {
            co_return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, "定稿订单的请求正文没能序列化成 JSON"});
        }

        auto reply = co_await postSignedRequest(order.finalizeUrl, *payloadText, false);
        if (!reply.has_value())
        {
            co_return std::unexpected(reply.error());
        }
        if (reply->statusCode < 200 || reply->statusCode >= 300)
        {
            co_return std::unexpected(describeFailure(*reply, "提交证书签名请求"));
        }

        AcmeOrder                                   finalized = order;
        Core::Timer                                 timer(m_loop);
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + overallTimeout;
        for (;;)
        {
            const auto statusJson = Base::parseConfigValue(reply->bodyText);
            if (!statusJson.has_value())
            {
                co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, "定稿之后机构回的订单不是 JSON"});
            }
            finalized.status         = readStringMember(*statusJson, "status").value_or(std::string{});
            finalized.certificateUrl = readStringMember(*statusJson, "certificate").value_or(std::string{});

            if (finalized.status == "valid")
            {
                if (finalized.certificateUrl.empty())
                {
                    co_return std::unexpected(AcmeError{AcmeErrorKind::UnexpectedResponse, "机构把订单判成 valid 却没给 certificate 地址，这一张证书取不回来"});
                }
                co_return finalized;
            }
            if (finalized.status == "invalid")
            {
                // 订单在定稿阶段被判死：多半是某条授权没走成 valid，机构会把原因写在 error 里
                std::string complaint;
                if (statusJson->contains("error") && (*statusJson)["error"].is_object())
                {
                    complaint = readStringMember((*statusJson)["error"], "detail").value_or(std::string{});
                }
                co_return std::unexpected(
                        AcmeError{AcmeErrorKind::RejectedByAuthority, std::format("机构在定稿这一步把订单判成 invalid。{}机构给的说明：{}",
                                                                                  complaint.empty() ? "" : std::format("原因：{}。", complaint), order.orderUrl)});
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                co_return std::unexpected(
                        AcmeError{AcmeErrorKind::Transport, std::format("定稿后的订单在 {} 内没有走到终局（最后一次状态：{}）", overallTimeout.count(), finalized.status)});
            }

            co_await timer.waitFor(pollInterval);
            reply = co_await postSignedRequest(order.orderUrl, {}, false);
            if (!reply.has_value())
            {
                co_return std::unexpected(reply.error());
            }
            if (reply->statusCode < 200 || reply->statusCode >= 300)
            {
                co_return std::unexpected(describeFailure(*reply, "轮询订单状态"));
            }
        }
    }

    Core::Task<std::expected<std::string, AcmeError>> AcmeClient::fetchCertificateChain(const std::string &certificateUrl)
    {
        auto reply = co_await postSignedRequest(certificateUrl, {}, false);
        if (!reply.has_value())
        {
            co_return std::unexpected(reply.error());
        }
        if (reply->statusCode < 200 || reply->statusCode >= 300)
        {
            co_return std::unexpected(describeFailure(*reply, "取证书链"));
        }
        // 这一步机构回的是 PEM 原文而不是 JSON（RFC 8555 §7.4.2.1，Content-Type 是
        // application/pem-certificate-chain）：判「像不像一份证书链」而不是判长度，
        // 半份链与一份错误页都不该被当成证书存下去
        if (reply->bodyText.find("BEGIN CERTIFICATE") == std::string::npos)
        {
            co_return std::unexpected(
                    AcmeError{AcmeErrorKind::UnexpectedResponse, std::format("机构回的证书链正文里没有 PEM 证书块（前 {} 字节：{}）。地址：{}",
                                                                             std::min<std::size_t>(200, reply->bodyText.size()), reply->bodyText.substr(0, 200), certificateUrl)});
        }
        co_return reply->bodyText;
    }
} // namespace AsynGyanis::Net
