// JWT（RS256）纯函数层的用例：现场生成 RSA 密钥对并签名，对正反两面逐格证伪。
// 覆盖场景：
// - VerifiesAFreshlySignedToken / VerifiesClaimsWhenAllExpectationsMatch（正例，含 aud 数组形态）
// - RejectsTamperedPayload / RejectsSignatureFromAnotherKey（篡改与错键）
// - RejectsExpiredToken / RejectsNotYetValidToken（时间窗两侧，skew 边界逐点钉）
// - RejectsAlgNoneBeforeSignatureCheck / RejectsNonRs256Algorithm（alg 白名单先于验签）
// - RejectsIssuerMismatch / RejectsAudienceMismatch（声明不符）
// - RejectsMalformedTokens / Base64UrlRejectsForeignAlphabet（形状与编码拒绝面）
// - JwkRoundTripsExportedKeyMaterial（JWK 构造的公钥与原键同解，并能验真令牌）

#include "ExpectedAssertions.h"
#include "Net/Http/JwtVerifier.h"

#include <openssl/core_names.h>
#include <openssl/bn.h>
#include <openssl/evp.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using TimePoint = std::chrono::system_clock::time_point;

        [[nodiscard]] TimePoint atUnixSeconds(const std::int64_t seconds)
        {
            return TimePoint{std::chrono::seconds{seconds}};
        }

        /// 现场生成 RSA 密钥对：每个用例都真签真验，不拿写死的令牌冒充（那测不到实现）
        [[nodiscard]] std::shared_ptr<EVP_PKEY> generateRsaKeyPair()
        {
            EVP_PKEY_CTX *keygenContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
            if (keygenContext == nullptr || EVP_PKEY_keygen_init(keygenContext) != 1)
            {
                EVP_PKEY_CTX_free(keygenContext);
                return nullptr;
            }

            // 密钥位数走 OSSL_PARAM 下发：EVP_PKEY_CTX_set_rsa_keygen_bits 那一族旧 RSA 接口
            // 在 OpenSSL 3.0 已弃用（本库 /WX 下当场红）
            int        keyBits        = 2048;
            OSSL_PARAM keygenParams[] = {OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_RSA_BITS, &keyBits), OSSL_PARAM_END};
            if (EVP_PKEY_CTX_set_params(keygenContext, keygenParams) != 1)
            {
                EVP_PKEY_CTX_free(keygenContext);
                return nullptr;
            }

            EVP_PKEY *generatedKey = nullptr;
            if (EVP_PKEY_keygen(keygenContext, &generatedKey) != 1)
            {
                EVP_PKEY_CTX_free(keygenContext);
                return nullptr;
            }

            EVP_PKEY_CTX_free(keygenContext);
            return std::shared_ptr<EVP_PKEY>(generatedKey, EVP_PKEY_free);
        }

        /// 用私钥对 "header.payload" 原文签 RS256，返回签名原始字节
        [[nodiscard]] std::string signRs256(const std::string &signingInput, EVP_PKEY *privateKey)
        {
            EVP_MD_CTX *digestContext = EVP_MD_CTX_new();
            if (digestContext == nullptr || EVP_DigestSignInit(digestContext, nullptr, EVP_sha256(), nullptr, privateKey) != 1 ||
                EVP_DigestSignUpdate(digestContext, signingInput.data(), signingInput.size()) != 1)
            {
                EVP_MD_CTX_free(digestContext);
                return {};
            }

            std::size_t signatureLength = 0;
            if (EVP_DigestSignFinal(digestContext, nullptr, &signatureLength) != 1)
            {
                EVP_MD_CTX_free(digestContext);
                return {};
            }

            std::string signature(signatureLength, '\0');
            if (EVP_DigestSignFinal(digestContext, reinterpret_cast<unsigned char *>(signature.data()), &signatureLength) != 1)
            {
                EVP_MD_CTX_free(digestContext);
                return {};
            }

            signature.resize(signatureLength);
            EVP_MD_CTX_free(digestContext);
            return signature;
        }

        /// 拼一枚真令牌：头、载荷各给 JSON 文本，签名现签
        [[nodiscard]] std::string makeToken(const std::string &headerJson, const std::string &payloadJson, EVP_PKEY *privateKey)
        {
            const std::string signingInput = base64UrlEncode(headerJson) + "." + base64UrlEncode(payloadJson);
            const std::string signature    = signRs256(signingInput, privateKey);
            return signingInput + "." + base64UrlEncode(signature);
        }

        [[nodiscard]] std::string defaultHeader()
        {
            return R"({"alg":"RS256","typ":"JWT","kid":"test-key-1"})";
        }

        [[nodiscard]] std::string makePayload(const std::int64_t expiration, const std::int64_t notBefore, const std::string &audienceJson)
        {
            return R"({"iss":"https://issuer.example","aud":)" + audienceJson + R"(,"sub":"user-1","exp":)" + std::to_string(expiration) + R"(,"nbf":)" +
                   std::to_string(notBefore) + "}";
        }

        /**
         * @brief 用例夹具：密钥对与两枚参照令牌（当前有效 / 一个不存在的时刻）在套件级建一次
         */
        class JwtVerifierTest : public ::testing::Test
        {
        protected:
            static void SetUpTestSuite()
            {
                signingKey = generateRsaKeyPair();
                ASSERT_NE(signingKey, nullptr) << "RSA 密钥对生成失败，本组用例无从进行";
            }

            /// 参照时刻：2026-10-12 12:00:00 UTC，全部时间断言都以它为基准手算差值
            static constexpr std::int64_t kNowSeconds = 1791547200;
            static constexpr std::int64_t kOneHour    = 3600;

            static inline std::shared_ptr<EVP_PKEY> signingKey;
        };
    } // namespace

    TEST_F(JwtVerifierTest, VerifiesAFreshlySignedToken)
    {
        const std::string token = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());

        const std::optional<std::string> reason = verifyJwtRs256(token, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        EXPECT_FALSE(reason.has_value()) << *reason;
    }

    TEST_F(JwtVerifierTest, VerifiesClaimsWhenAllExpectationsMatch)
    {
        // aud 用数组形态：RFC 7519 §4.1.3 允许字符串或数组，两条都要能命中
        const std::string token =
                makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"(["https://other.example","https://api.example"])"), signingKey.get());

        JwtClaimExpectations expectations;
        expectations.issuer   = "https://issuer.example";
        expectations.audience = "https://api.example";

        EXPECT_FALSE(verifyJwtRs256(token, signingKey.get(), expectations, atUnixSeconds(kNowSeconds)).has_value());
    }

    TEST_F(JwtVerifierTest, RejectsTamperedPayload)
    {
        // 造「同一枚签名、被换过的载荷」要在明文上重组：token 里全是 base64 文本，直接在 token 上
        // 找明文字符是找不到的——那样写出来的用例会在「找不到」上先红，测不到验签本身
        const std::string headerJson  = defaultHeader();
        const std::string payloadJson = makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")");
        const std::string signature   = signRs256(base64UrlEncode(headerJson) + "." + base64UrlEncode(payloadJson), signingKey.get());

        std::string       tamperedPayload = payloadJson;
        const std::size_t subjectPosition = tamperedPayload.find("user-1");
        ASSERT_NE(subjectPosition, std::string::npos);
        tamperedPayload.replace(subjectPosition, 6, "user-2");

        const std::string tampered = base64UrlEncode(headerJson) + "." + base64UrlEncode(tamperedPayload) + "." + base64UrlEncode(signature);

        const std::optional<std::string> reason = verifyJwtRs256(tampered, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(reason);
        EXPECT_NE(reason->find("签名校验失败"), std::string::npos) << *reason;
    }

    TEST_F(JwtVerifierTest, RejectsSignatureFromAnotherKey)
    {
        const std::shared_ptr<EVP_PKEY> anotherKey = generateRsaKeyPair();
        ASSERT_NE(anotherKey, nullptr);

        const std::string token = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), anotherKey.get());

        // 用别人的令牌对我们的公钥：验签失败（而不是「解析失败」——形状没毛病，只是签错了键）
        const std::optional<std::string> reason = verifyJwtRs256(token, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(reason);
        EXPECT_NE(reason->find("签名校验失败"), std::string::npos) << *reason;
    }

    TEST_F(JwtVerifierTest, RejectsExpiredToken)
    {
        // exp = now - 61：偏差 60 秒已经吃完，必须拒
        const std::string expiredToken = makeToken(defaultHeader(), makePayload(kNowSeconds - 61, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());
        const std::optional<std::string> expiredReason = verifyJwtRs256(expiredToken, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(expiredReason);
        EXPECT_NE(expiredReason->find("已过期"), std::string::npos) << *expiredReason;

        // exp = now - 59：还在 60 秒偏差之内，放行——两侧各取一点，把边界钉住而不是只测「明显过期」
        const std::string withinSkewToken = makeToken(defaultHeader(), makePayload(kNowSeconds - 59, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());
        EXPECT_FALSE(verifyJwtRs256(withinSkewToken, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds)).has_value());
    }

    TEST_F(JwtVerifierTest, RejectsNotYetValidToken)
    {
        // nbf = now + 61：还没生效
        const std::string earlyToken = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds + 61, R"("https://api.example")"), signingKey.get());
        const std::optional<std::string> earlyReason = verifyJwtRs256(earlyToken, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(earlyReason);
        EXPECT_NE(earlyReason->find("尚未生效"), std::string::npos) << *earlyReason;

        // nbf = now + 59：在偏差之内，放行
        const std::string soonToken = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds + 59, R"("https://api.example")"), signingKey.get());
        EXPECT_FALSE(verifyJwtRs256(soonToken, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds)).has_value());
    }

    TEST_F(JwtVerifierTest, RejectsAlgNoneBeforeSignatureCheck)
    {
        // alg=none 的公开伪装：头改成 none、签名段清空。判据是拒绝原因点名算法而不是「签名失败」——
        // 那证明白名单在验签之前生效（否则空签名也走不到算法判定）
        const std::string signingInput =
                base64UrlEncode(R"({"alg":"none","typ":"JWT"})") + "." + base64UrlEncode(makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"));
        const std::string noneToken = signingInput + ".";

        const std::optional<std::string> reason = verifyJwtRs256(noneToken, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(reason);
        EXPECT_NE(reason->find("不是 RS256"), std::string::npos) << *reason;
    }

    TEST_F(JwtVerifierTest, RejectsNonRs256Algorithm)
    {
        // HS256 头 + 用同一枚私钥正经签出来的签名：算法不对就必须拒（公钥当 HMAC 密钥是经典混淆攻击）
        const std::string hs256Header = R"({"alg":"HS256","typ":"JWT"})";
        const std::string hs256Token  = makeToken(hs256Header, makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());

        const std::optional<std::string> reason = verifyJwtRs256(hs256Token, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(reason);
        EXPECT_NE(reason->find("不是 RS256"), std::string::npos) << *reason;
    }

    TEST_F(JwtVerifierTest, RejectsIssuerMismatch)
    {
        const std::string token = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());

        JwtClaimExpectations expectations;
        expectations.issuer                     = "https://another-issuer.example";
        const std::optional<std::string> reason = verifyJwtRs256(token, signingKey.get(), expectations, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(reason);
        EXPECT_NE(reason->find("iss"), std::string::npos) << *reason;
    }

    TEST_F(JwtVerifierTest, RejectsAudienceMismatch)
    {
        const std::string token = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());

        JwtClaimExpectations expectations;
        expectations.audience                   = "https://other-api.example";
        const std::optional<std::string> reason = verifyJwtRs256(token, signingKey.get(), expectations, atUnixSeconds(kNowSeconds));
        ASSERT_HAS_VALUE(reason);
        EXPECT_NE(reason->find("aud"), std::string::npos) << *reason;
    }

    TEST_F(JwtVerifierTest, RejectsMalformedTokens)
    {
        const std::string validToken = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());

        std::vector<std::pair<std::string, std::string>> malformedCases;
        malformedCases.emplace_back("两段", validToken.substr(0, validToken.find_last_of('.')));
        malformedCases.emplace_back("四段", validToken + ".extra");
        malformedCases.emplace_back("空串", "");
        malformedCases.emplace_back("头段含标准 base64 字符 '+'", "+" + validToken);
        malformedCases.emplace_back("头段 JSON 非法", base64UrlEncode("not json") + validToken.substr(validToken.find('.')));

        for (const auto &[name, token]: malformedCases)
        {
            const std::optional<std::string> reason = verifyJwtRs256(token, signingKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds));
            EXPECT_HAS_VALUE(reason) << name << " 竟被放行";
        }

        // 空公钥是调用方用法错误：给原因（签名失败）而不是崩
        EXPECT_TRUE(verifyJwtRs256(validToken, nullptr, JwtClaimExpectations{}, atUnixSeconds(kNowSeconds)).has_value());
    }

    TEST_F(JwtVerifierTest, Base64UrlRejectsForeignAlphabet)
    {
        // 编码：已知向量的往返（"Man" 的经典 base64 是 TWFu；base64url 同形）
        EXPECT_EQ(base64UrlEncode("Man"), "TWFu");
        EXPECT_EQ(base64UrlEncode("Ma"), "TWE");
        EXPECT_EQ(base64UrlEncode("M"), "TQ");
        EXPECT_EQ(base64UrlEncode(""), "");

        // 解码：往返对称 + 拒绝表外字符（含标准 base64 的 '+' '/' 与填充 '='）
        for (const std::string &sample: std::vector<std::string>{"", "M", "Ma", "Man", "hello world", std::string("\x00\xFF\x7F", 3)})
        {
            const std::optional<std::string> decoded = base64UrlDecode(base64UrlEncode(sample));
            ASSERT_HAS_VALUE(decoded) << sample;
            EXPECT_EQ(*decoded, sample);
        }

        EXPECT_FALSE(base64UrlDecode("TW+u").has_value());
        EXPECT_FALSE(base64UrlDecode("TW/u").has_value());
        EXPECT_FALSE(base64UrlDecode("TWFu=").has_value());
        EXPECT_FALSE(base64UrlDecode("A").has_value());
    }

    TEST_F(JwtVerifierTest, JwkRoundTripsExportedKeyMaterial)
    {
        // 把现场密钥的 n/e 按 JWK 的 base64url 形态导出，再走 rsaPublicKeyFromJwk 构造回来：
        // EVP_PKEY_eq 判两个键同解——这条链路正是「JWKS 端点 → 公钥」的缩样
        BIGNUM *modulus  = nullptr;
        BIGNUM *exponent = nullptr;
        ASSERT_EQ(EVP_PKEY_get_bn_param(signingKey.get(), OSSL_PKEY_PARAM_RSA_N, &modulus), 1);
        ASSERT_EQ(EVP_PKEY_get_bn_param(signingKey.get(), OSSL_PKEY_PARAM_RSA_E, &exponent), 1);

        std::vector<unsigned char> modulusBytes(static_cast<std::size_t>(BN_num_bytes(modulus)));
        std::vector<unsigned char> exponentBytes(static_cast<std::size_t>(BN_num_bytes(exponent)));
        BN_bn2bin(modulus, modulusBytes.data());
        BN_bn2bin(exponent, exponentBytes.data());
        BN_free(modulus);
        BN_free(exponent);

        const std::string modulusText  = base64UrlEncode(std::string_view(reinterpret_cast<const char *>(modulusBytes.data()), modulusBytes.size()));
        const std::string exponentText = base64UrlEncode(std::string_view(reinterpret_cast<const char *>(exponentBytes.data()), exponentBytes.size()));

        const std::shared_ptr<EVP_PKEY> reconstructedKey = rsaPublicKeyFromJwk(modulusText, exponentText);
        ASSERT_NE(reconstructedKey, nullptr);

        // 重建键的 n/e 逐字节复核：比 eq 更精确的判据——传参错位在这里就指名道姓
        BIGNUM *roundTripModulus  = nullptr;
        BIGNUM *roundTripExponent = nullptr;
        ASSERT_EQ(EVP_PKEY_get_bn_param(reconstructedKey.get(), OSSL_PKEY_PARAM_RSA_N, &roundTripModulus), 1);
        ASSERT_EQ(EVP_PKEY_get_bn_param(reconstructedKey.get(), OSSL_PKEY_PARAM_RSA_E, &roundTripExponent), 1);
        std::vector<unsigned char> roundTripModulusBytes(static_cast<std::size_t>(BN_num_bytes(roundTripModulus)));
        std::vector<unsigned char> roundTripExponentBytes(static_cast<std::size_t>(BN_num_bytes(roundTripExponent)));
        BN_bn2bin(roundTripModulus, roundTripModulusBytes.data());
        BN_bn2bin(roundTripExponent, roundTripExponentBytes.data());
        BN_free(roundTripModulus);
        BN_free(roundTripExponent);

        ASSERT_EQ(roundTripModulusBytes, modulusBytes) << "重建键的 n 与原键不同";
        ASSERT_EQ(roundTripExponentBytes, exponentBytes) << "重建键的 e 与原键不同";

        EXPECT_EQ(EVP_PKEY_eq(reconstructedKey.get(), signingKey.get()), 1) << "JWK 构造出的公钥与原键不同解";

        // 用重构出来的公钥验真令牌：整个「JWKS → 验签」链路的最小闭环
        const std::string token = makeToken(defaultHeader(), makePayload(kNowSeconds + kOneHour, kNowSeconds - kOneHour, R"("https://api.example")"), signingKey.get());
        EXPECT_FALSE(verifyJwtRs256(token, reconstructedKey.get(), JwtClaimExpectations{}, atUnixSeconds(kNowSeconds)).has_value());

        // 反面：n/e 非法（空、非 base64url）给空
        EXPECT_EQ(rsaPublicKeyFromJwk("", "AQAB"), nullptr);
        EXPECT_EQ(rsaPublicKeyFromJwk(modulusText, "!!!"), nullptr);
    }

} // namespace AsynGyanis::Net
