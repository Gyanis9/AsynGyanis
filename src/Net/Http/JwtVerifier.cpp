#include "Net/Http/JwtVerifier.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// base64url 的字母表（RFC 4648 §5：标准表的 '+' '/' 换成 '-' '_'）
        constexpr std::string_view kBase64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

        /// 解码查表：非字母表字节给 0xFF（含 '=' 与标准 base64 的 '+' '/'——JWT 段不带填充也不带这两个）
        [[nodiscard]] constexpr std::array<std::uint8_t, 256> makeDecodeTable() noexcept
        {
            std::array<std::uint8_t, 256> decodeTable{};
            for (std::size_t index = 0; index < decodeTable.size(); ++index)
            {
                decodeTable[index] = 0xFFU;
            }
            for (std::size_t index = 0; index < kBase64UrlAlphabet.size(); ++index)
            {
                decodeTable[static_cast<std::uint8_t>(kBase64UrlAlphabet[index])] = static_cast<std::uint8_t>(index);
            }
            return decodeTable;
        }

        constexpr std::array<std::uint8_t, 256> kBase64UrlDecodeTable = makeDecodeTable();

        /**
         * @brief 把载荷里的时间声明按 NumericDate（秒）读出来
         * @details 规范允许 exp/nbf 出现为整数；JSON 里是浮点（少见但合法）时按截断取整——
         *          真实的签发方不会这么写，但拒绝它会比「容忍一下」更容易把合法令牌挡在门外
         * @param payload 载荷对象
         * @param key 声明名（"exp" 或 "nbf"）
         * @return std::optional<std::int64_t> 秒数；声明缺失或不是数值时给空
         */
        [[nodiscard]] std::optional<std::int64_t> readNumericDate(const nlohmann::json &payload, const std::string_view key)
        {
            const auto iterator = payload.find(key);
            if (iterator == payload.end())
            {
                return std::nullopt;
            }

            if (iterator->is_number_integer())
            {
                return iterator->get<std::int64_t>();
            }
            if (iterator->is_number_float())
            {
                return static_cast<std::int64_t>(iterator->get<double>());
            }
            return std::nullopt;
        }

        [[nodiscard]] std::int64_t toUnixSeconds(const std::chrono::system_clock::time_point moment) noexcept
        {
            return std::chrono::duration_cast<std::chrono::seconds>(moment.time_since_epoch()).count();
        }
    } // namespace

    std::string base64UrlEncode(const std::string_view bytes)
    {
        std::string encoded;
        encoded.reserve((bytes.size() + 2) / 3 * 4);

        // 逐三字节组出四符号；末组按剩余 1/2 字节各出 2/3 个符号（不补 '='）
        std::size_t byteIndex = 0;
        while (byteIndex + 3 <= bytes.size())
        {
            const std::uint32_t group = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex])) << 16) |
                                        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex + 1])) << 8) |
                                        static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex + 2]));
            encoded.push_back(kBase64UrlAlphabet[(group >> 18) & 0x3FU]);
            encoded.push_back(kBase64UrlAlphabet[(group >> 12) & 0x3FU]);
            encoded.push_back(kBase64UrlAlphabet[(group >> 6) & 0x3FU]);
            encoded.push_back(kBase64UrlAlphabet[group & 0x3FU]);
            byteIndex += 3;
        }

        const std::size_t remaining = bytes.size() - byteIndex;
        if (remaining == 1)
        {
            const std::uint32_t group = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex])) << 16;
            encoded.push_back(kBase64UrlAlphabet[(group >> 18) & 0x3FU]);
            encoded.push_back(kBase64UrlAlphabet[(group >> 12) & 0x3FU]);
        } else if (remaining == 2)
        {
            const std::uint32_t group = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex])) << 16) |
                                        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[byteIndex + 1])) << 8);
            encoded.push_back(kBase64UrlAlphabet[(group >> 18) & 0x3FU]);
            encoded.push_back(kBase64UrlAlphabet[(group >> 12) & 0x3FU]);
            encoded.push_back(kBase64UrlAlphabet[(group >> 6) & 0x3FU]);
        }

        return encoded;
    }

    std::optional<std::string> base64UrlDecode(const std::string_view text)
    {
        // 单个符号（1 个字符）凑不出一个字节：长度模 4 为 1 的输入结构上就不合法
        if (text.size() % 4 == 1)
        {
            return std::nullopt;
        }

        std::string decoded;
        decoded.reserve(text.size() / 4 * 3 + 2);

        std::uint32_t accumulator = 0;
        int           symbolCount = 0;
        for (const char character: text)
        {
            const std::uint8_t symbol = kBase64UrlDecodeTable[static_cast<std::uint8_t>(character)];
            if (symbol == 0xFFU)
            {
                // 非法字符（含 '=' 填充与标准 base64 的 '+' '/'）：JWT 段不该出现，一律拒
                return std::nullopt;
            }

            accumulator = (accumulator << 6) | symbol;
            ++symbolCount;
            if (symbolCount == 4)
            {
                decoded.push_back(static_cast<char>((accumulator >> 16) & 0xFFU));
                decoded.push_back(static_cast<char>((accumulator >> 8) & 0xFFU));
                decoded.push_back(static_cast<char>(accumulator & 0xFFU));
                accumulator = 0;
                symbolCount = 0;
            }
        }

        // 收尾的 2/3 个符号各自出 1/2 字节；1 个符号的残余在上面的长度检查里已被拒
        if (symbolCount == 2)
        {
            decoded.push_back(static_cast<char>((accumulator >> 4) & 0xFFU));
        } else if (symbolCount == 3)
        {
            decoded.push_back(static_cast<char>((accumulator >> 10) & 0xFFU));
            decoded.push_back(static_cast<char>((accumulator >> 2) & 0xFFU));
        }

        return decoded;
    }

    std::optional<ParsedJwt> parseJwt(const std::string_view token)
    {
        // 三段：头.载荷.签名。签名段可以为空（alg:none 的形态），但那不归本层拦——白名单在验签入口
        const std::size_t firstDot  = token.find('.');
        const std::size_t secondDot = firstDot == std::string_view::npos ? std::string_view::npos : token.find('.', firstDot + 1);
        if (firstDot == std::string_view::npos || secondDot == std::string_view::npos || token.find('.', secondDot + 1) != std::string_view::npos)
        {
            return std::nullopt;
        }

        const std::string_view headerSegment    = token.substr(0, firstDot);
        const std::string_view payloadSegment   = token.substr(firstDot + 1, secondDot - firstDot - 1);
        const std::string_view signatureSegment = token.substr(secondDot + 1);

        const std::optional<std::string> headerBytes    = base64UrlDecode(headerSegment);
        const std::optional<std::string> payloadBytes   = base64UrlDecode(payloadSegment);
        const std::optional<std::string> signatureBytes = base64UrlDecode(signatureSegment);
        if (!headerBytes.has_value() || !payloadBytes.has_value() || !signatureBytes.has_value())
        {
            return std::nullopt;
        }

        // 头的 JSON 解析只为「形状对不对」把关；载荷同样——两者都可能不是对象（如顶层数组），一律拒
        const nlohmann::json headerDocument  = nlohmann::json::parse(*headerBytes, nullptr, false);
        const nlohmann::json payloadDocument = nlohmann::json::parse(*payloadBytes, nullptr, false);
        if (headerDocument.is_discarded() || payloadDocument.is_discarded() || !headerDocument.is_object() || !payloadDocument.is_object())
        {
            return std::nullopt;
        }

        ParsedJwt parsed;
        parsed.headerJson   = *headerBytes;
        parsed.payloadJson  = *payloadBytes;
        parsed.signature    = *signatureBytes;
        parsed.signingInput = std::string(token.substr(0, secondDot));
        return parsed;
    }

    std::shared_ptr<EVP_PKEY> rsaPublicKeyFromJwk(const std::string_view modulusN, const std::string_view exponentE)
    {
        const std::optional<std::string> modulusBytes  = base64UrlDecode(modulusN);
        const std::optional<std::string> exponentBytes = base64UrlDecode(exponentE);
        if (!modulusBytes.has_value() || !exponentBytes.has_value() || modulusBytes->empty() || exponentBytes->empty())
        {
            return nullptr;
        }

        // n/e 从大端字节转成 BIGNUM，再经 OSSL_PARAM_BLD 交给 fromdata：直接手拼 OSSL_PARAM 时
        // 「UNSIGNED_INTEGER 的 data 该放字节缓冲还是 BIGNUM 指针」极易写错（实测把缓冲当指针解，
        // 读出来的键与输入毫无关系），BLD 是设计给 BIGNUM 的正门、没有这层歧义。
        // 走 fromdata 而不是 RSA_new / RSA_set0_key / EVP_PKEY_assign_RSA：那一族旧 RSA API
        // 在 OpenSSL 3.0 已整体标记弃用（本库 /WX 下当场红），fromdata 是它的正路
        BIGNUM *modulus  = BN_bin2bn(reinterpret_cast<const unsigned char *>(modulusBytes->data()), static_cast<int>(modulusBytes->size()), nullptr);
        BIGNUM *exponent = BN_bin2bn(reinterpret_cast<const unsigned char *>(exponentBytes->data()), static_cast<int>(exponentBytes->size()), nullptr);
        if (modulus == nullptr || exponent == nullptr)
        {
            BN_free(modulus);
            BN_free(exponent);
            return nullptr;
        }

        OSSL_PARAM_BLD *parameterBuilder = OSSL_PARAM_BLD_new();
        if (parameterBuilder == nullptr || OSSL_PARAM_BLD_push_BN(parameterBuilder, OSSL_PKEY_PARAM_RSA_N, modulus) != 1 ||
            OSSL_PARAM_BLD_push_BN(parameterBuilder, OSSL_PKEY_PARAM_RSA_E, exponent) != 1)
        {
            OSSL_PARAM_BLD_free(parameterBuilder);
            BN_free(modulus);
            BN_free(exponent);
            return nullptr;
        }

        OSSL_PARAM   *keyParameters   = OSSL_PARAM_BLD_to_param(parameterBuilder);
        EVP_PKEY_CTX *fromDataContext = keyParameters != nullptr ? EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr) : nullptr;
        EVP_PKEY     *publicKey       = nullptr;
        const bool    isBuilt         = fromDataContext != nullptr && EVP_PKEY_fromdata_init(fromDataContext) == 1 &&
                                        EVP_PKEY_fromdata(fromDataContext, &publicKey, EVP_PKEY_PUBLIC_KEY, keyParameters) == 1;

        OSSL_PARAM_free(keyParameters);
        EVP_PKEY_CTX_free(fromDataContext);
        OSSL_PARAM_BLD_free(parameterBuilder);
        BN_free(modulus);
        BN_free(exponent);

        if (!isBuilt || publicKey == nullptr)
        {
            EVP_PKEY_free(publicKey);
            return nullptr;
        }

        return std::shared_ptr<EVP_PKEY>(publicKey, EVP_PKEY_free);
    }

    bool verifyRs256(const ParsedJwt &parsed, EVP_PKEY *const publicKey)
    {
        if (publicKey == nullptr)
        {
            return false;
        }

        EVP_MD_CTX *digestContext = EVP_MD_CTX_new();
        if (digestContext == nullptr)
        {
            return false;
        }

        const bool isInitialized = EVP_DigestVerifyInit(digestContext, nullptr, EVP_sha256(), nullptr, publicKey) == 1 &&
                                   EVP_DigestVerifyUpdate(digestContext, parsed.signingInput.data(), parsed.signingInput.size()) == 1 &&
                                   EVP_DigestVerifyFinal(digestContext, reinterpret_cast<const unsigned char *>(parsed.signature.data()), parsed.signature.size()) == 1;

        EVP_MD_CTX_free(digestContext);
        return isInitialized;
    }

    std::optional<std::string> validateJwtClaims(const std::string_view payloadJson, const JwtClaimExpectations &expectations, const std::chrono::system_clock::time_point now)
    {
        const nlohmann::json payload = nlohmann::json::parse(payloadJson, nullptr, false);
        if (payload.is_discarded() || !payload.is_object())
        {
            return "JWT 载荷不是 JSON 对象，无法校验声明";
        }

        const std::int64_t nowSeconds = toUnixSeconds(now);
        const auto         skew       = expectations.clockSkew.count();

        // exp：到期之后（含偏差）一律拒——两边都不给宽限的写法（now < exp + skew 才放行）
        if (const std::optional<std::int64_t> expiration = readNumericDate(payload, "exp"); expiration.has_value())
        {
            if (nowSeconds >= *expiration + skew)
            {
                return "JWT 已过期（exp 已过，允许偏差 " + std::to_string(skew) + " 秒）";
            }
        }

        // nbf：生效之前（含偏差）一律拒
        if (const std::optional<std::int64_t> notBefore = readNumericDate(payload, "nbf"); notBefore.has_value())
        {
            if (nowSeconds + skew < *notBefore)
            {
                return "JWT 尚未生效（nbf 还没到，允许偏差 " + std::to_string(skew) + " 秒）";
            }
        }

        // iss：期望给出时必须精确相等
        if (!expectations.issuer.empty())
        {
            const auto issuer = payload.find("iss");
            if (issuer == payload.end() || !issuer->is_string() || issuer->get<std::string>() != expectations.issuer)
            {
                return "JWT 的签发者（iss）与期望不符";
            }
        }

        // aud：载荷里是字符串时精确相等；是数组时含于其中即命中（RFC 7519 §4.1.3 两种形态都合法）
        if (!expectations.audience.empty())
        {
            const auto audience = payload.find("aud");
            bool       matched  = false;
            if (audience != payload.end())
            {
                if (audience->is_string())
                {
                    matched = audience->get<std::string>() == expectations.audience;
                } else if (audience->is_array())
                {
                    for (const auto &item: *audience)
                    {
                        if (item.is_string() && item.get<std::string>() == expectations.audience)
                        {
                            matched = true;
                            break;
                        }
                    }
                }
            }
            if (!matched)
            {
                return "JWT 的受众（aud）与期望不符";
            }
        }

        return std::nullopt;
    }

    std::optional<std::string> verifyJwtRs256(const std::string_view token, EVP_PKEY *const publicKey, const JwtClaimExpectations &expectations,
                                              const std::chrono::system_clock::time_point now)
    {
        const std::optional<ParsedJwt> parsed = parseJwt(token);
        if (!parsed.has_value())
        {
            return "JWT 形状或编码非法（需要三段 base64url、头与载荷为 JSON 对象）";
        }

        // alg 白名单：只认 RS256。none 与 HS256 都必须在这里被拒——前者是无签名的公开伪装，
        // 后者的「公钥当 HMAC 密钥」是经典混淆攻击，放过任何一条都等于验签形同虚设
        const nlohmann::json header = nlohmann::json::parse(parsed->headerJson, nullptr, false);
        if (header.is_discarded() || !header.is_object())
        {
            return "JWT 头部不是 JSON 对象";
        }
        const auto algorithm = header.find("alg");
        if (algorithm == header.end() || !algorithm->is_string() || algorithm->get<std::string>() != "RS256")
        {
            return "JWT 的签名算法不是 RS256（本件只认 RS256，alg=none 与 HS256 一律拒）";
        }

        if (!verifyRs256(*parsed, publicKey))
        {
            return "JWT 签名校验失败（公钥不匹配或令牌被篡改）";
        }

        return validateJwtClaims(parsed->payloadJson, expectations, now);
    }

} // namespace AsynGyanis::Net
