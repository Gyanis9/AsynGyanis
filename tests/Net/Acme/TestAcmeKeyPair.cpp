// ACME 密钥对象用例：JWK 与指纹的规范形态、JWS 签名的可验性、CSR 的 SAN 覆盖，以及畸形私钥的拒绝面
#include "AcmeTestSupport.h"

#include "Base/Coding/Base64.h"
#include "Base/Config/ConfigValue.h"
#include "CommonTestSupport.h"
#include "Net/Acme/AcmeKeyPair.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 7638 §3.1 的示例 JWK（已按 §4 的要求把成员排成键的字典序）与 §4.2 公布的指纹
        constexpr std::string_view kRfc7638CanonicalJsonWebKey =
                R"({"e":"AQAB","kty":"RSA","n":"0vx7agoebGcQSuuPiLJXZptN9nndrQmbXEps2aiAFbWhM78LhWx4cbbfAAtVT86zwu1RK7aPFFxuhDR1L6tSoc_BJECPebWKRXjBZCiFV4n3oknjhMstn64tZ_2W-5JsGY4Hc5n9yBXArwl93lqt7_RN5w6Cf0h4QyQ5v-65YGjQR0_FDW2QvzqY368QQMicAtaSqzs8KJZgnYb9c7d0zgdAZHzu6qMQvRL5hajrn1n91CbOpbISD08qNLyrdkt-bFTWhAI4vMQFh6WeZu0fM4lFd2NcRwr3XPksINHaQ-G_xBniIqbw0Ls1jF44-csFCur-kEgU8awapJzKnqDKgw"})";
        constexpr std::string_view kRfc7638PublishedThumbprint = "NzbLsXh8uDCcd-6MNwXF4W_7noWXFZAfHkxZsRGC9Xs";

        /// 256 位整数的 base64url 无填充长度（32 字节 → 43 字符），EC 坐标与指纹都是这个长度
        constexpr std::size_t kEncodedTwoHundredFiftySixBitLength = 43U;

        /// 2048 位模数的 base64url 无填充长度（256 字节 → 342 字符）
        constexpr std::size_t kEncodedRsaModulusLength = 342U;

        /// ES256 签名段的线上长度：R 与 S 各 32 字节
        constexpr std::size_t kEs256SignatureLength = 64U;
    } // namespace

    /**
     * @brief 一把 RSA 与一把 EC 密钥，供各用例只读复用（生成一次几百毫秒，每条用例现造会把用例时长抬高一个量级）
     */
    class AcmeKeyPairTest : public ::testing::Test
    {
    public:
        /**
         * @brief 生成两把可用的密钥
         */
        void SetUp() override
        {
            auto rsa = AcmeKeyPair::generate(AcmeKeyAlgorithm::Rs256);
            ASSERT_TRUE(rsa.has_value()) << rsa.error().message;
            m_rsaKey = std::make_unique<AcmeKeyPair>(std::move(*rsa));

            auto ec = AcmeKeyPair::generate(AcmeKeyAlgorithm::Es256);
            ASSERT_TRUE(ec.has_value()) << ec.error().message;
            m_ecKey = std::make_unique<AcmeKeyPair>(std::move(*ec));
        }

    protected:
        std::unique_ptr<AcmeKeyPair> m_rsaKey; ///< RS256 那一把
        std::unique_ptr<AcmeKeyPair> m_ecKey;  ///< ES256 那一把
    };

    /**
     * @brief 钉住：指纹算法与 RFC 7638 公布的向量逐位一致，且输入差一个字节就换指纹
     * @details 用规范里的示例向量而不是自造数据：指纹是 HTTP-01 自证串的后半段，
     *          这里错了机构侧永远验不过，而它报的是「挑战失败」，不会指出是编码问题。
     */
    TEST_F(AcmeKeyPairTest, JsonWebKeyThumbprintMatchesThePublishedRfcVector)
    {
        EXPECT_EQ(AcmeKeyPair::computeJsonWebKeyThumbprint(kRfc7638CanonicalJsonWebKey), kRfc7638PublishedThumbprint);

        // 成员顺序变了就是另一份输入：字典序是凭据的一部分，不是排版偏好
        const std::string reordered =
                R"({"kty":"RSA","e":"AQAB","n":"0vx7agoebGcQSuuPiLJXZptN9nndrQmbXEps2aiAFbWhM78LhWx4cbbfAAtVT86zwu1RK7aPFFxuhDR1L6tSoc_BJECPebWKRXjBZCiFV4n3oknjhMstn64tZ_2W-5JsGY4Hc5n9yBXArwl93lqt7_RN5w6Cf0h4QyQ5v-65YGjQR0_FDW2QvzqY368QQMicAtaSqzs8KJZgnYb9c7d0zgdAZHzu6qMQvRL5hajrn1n91CbOpbISD08qNLyrdkt-bFTWhAI4vMQFh6WeZu0fM4lFd2NcRwr3XPksINHaQ-G_xBniIqbw0Ls1jF44-csFCur-kEgU8awapJzKnqDKgw"})";
        EXPECT_NE(AcmeKeyPair::computeJsonWebKeyThumbprint(reordered), kRfc7638PublishedThumbprint);
    }

    /**
     * @brief 钉住：RSA 密钥交回的 JWK 只含必需公开成员，且与算指纹时用的那份是同一串
     */
    TEST_F(AcmeKeyPairTest, RsaJsonWebKeyCarriesOnlyTheRequiredPublicMembers)
    {
        const std::string jwkText = m_rsaKey->publicJsonWebKeyText();

        EXPECT_EQ(m_rsaKey->jsonWebAlgorithmName(), "RS256");
        EXPECT_EQ(jwkText.find("\"d\":"), std::string_view::npos) << "私钥指数泄露进了公开表示：" << jwkText;

        const auto parsed = Base::parseConfigValue(jwkText);
        ASSERT_TRUE(parsed.has_value()) << "JWK 不是合法 JSON：" << jwkText;
        EXPECT_EQ((*parsed)["kty"], "RSA");
        // 65537 的 base64url 就是 AQAB：写错成 "AgAB" 之类是把字节序倒了
        EXPECT_EQ((*parsed)["e"], "AQAB");
        EXPECT_EQ((*parsed)["n"].get<std::string>().size(), kEncodedRsaModulusLength);

        // 逐字比一遍交出去的文本：JSON 解析看不出成员顺序，而 RFC 7638 的指纹恰恰由顺序决定——
        // 机构按它自己规范化的那份算出的指纹与我们给它的必须一致，否则 HTTP-01 自证永远配不上
        const std::string modulusValue = (*parsed)["n"].get<std::string>();
        EXPECT_EQ(jwkText, std::format(R"({{"e":"{}","kty":"RSA","n":"{}"}})", "AQAB", modulusValue)) << "成员顺序或成员集合与规范写法不同";

        // 指纹必须等于「对这份交出去的文本」求摘要：两处一旦分叉，机构按 JWK 重算的指纹与我们给的就不一致
        EXPECT_EQ(m_rsaKey->jsonWebKeyThumbprint(), AcmeKeyPair::computeJsonWebKeyThumbprint(jwkText));
        EXPECT_EQ(m_rsaKey->jsonWebKeyThumbprint().size(), kEncodedTwoHundredFiftySixBitLength);
    }

    /**
     * @brief 钉住：EC 密钥的坐标按定长编码（前导零不许被削掉）
     * @details JWA 的 x/y 是定长 32 字节，最短表示会让机构侧按定长切分时读歪；
     *          这条断言只在坐标恰好带前导零时才有牙，因此两把密钥都各测一遍长度。
     */
    TEST_F(AcmeKeyPairTest, EcJsonWebKeyUsesFixedWidthCoordinates)
    {
        const std::string jwkText = m_ecKey->publicJsonWebKeyText();

        EXPECT_EQ(m_ecKey->jsonWebAlgorithmName(), "ES256");

        const auto parsed = Base::parseConfigValue(jwkText);
        ASSERT_TRUE(parsed.has_value()) << "JWK 不是合法 JSON：" << jwkText;
        EXPECT_EQ((*parsed)["kty"], "EC");
        EXPECT_EQ((*parsed)["crv"], "P-256");
        EXPECT_EQ((*parsed)["x"].get<std::string>().size(), kEncodedTwoHundredFiftySixBitLength);
        EXPECT_EQ((*parsed)["y"].get<std::string>().size(), kEncodedTwoHundredFiftySixBitLength);

        // 同上：crv < kty < x < y 的字典序写法逐字钉住，成员一个都不许多
        const std::string abscissaValue = (*parsed)["x"].get<std::string>();
        const std::string ordinateValue = (*parsed)["y"].get<std::string>();
        EXPECT_EQ(jwkText, std::format(R"({{"crv":"{}","kty":"EC","x":"{}","y":"{}"}})", "P-256", abscissaValue, ordinateValue)) << "成员顺序或成员集合与规范写法不同";

        EXPECT_EQ(m_ecKey->jsonWebKeyThumbprint(), AcmeKeyPair::computeJsonWebKeyThumbprint(jwkText));
    }

    /**
     * @brief 钉住：RS256 的签名段能被本次请求自带的公钥验过
     */
    TEST_F(AcmeKeyPairTest, RsaSignatureVerifiesAgainstTheRequestPublicKey)
    {
        const auto request = m_rsaKey->createCertificateSigningRequest({"sign-check.example.com"});
        ASSERT_TRUE(request.has_value()) << request.error().message;
        const auto parsedRequest = TestSupport::parseCertificateSigningRequest(*request);
        ASSERT_TRUE(parsedRequest.has_value());
        ASSERT_NE(parsedRequest->publicKey, nullptr);

        const std::string signingInput = R"({"alg":"RS256","jwk":)" + m_rsaKey->publicJsonWebKeyText() + R"(},"payload")";
        const auto        signature    = m_rsaKey->signJsonWebSigningInput(signingInput);
        ASSERT_TRUE(signature.has_value()) << signature.error().message;

        EXPECT_TRUE(TestSupport::verifyJsonWebSignature(*parsedRequest->publicKey, AcmeKeyAlgorithm::Rs256, signingInput, *signature));
        // 换一段输入就必须验不过：签名与输入的绑定关系一旦丢了，重放就无人拦得住
        EXPECT_FALSE(TestSupport::verifyJsonWebSignature(*parsedRequest->publicKey, AcmeKeyAlgorithm::Rs256, signingInput + "x", *signature));
    }

    /**
     * @brief 钉住：ES256 的签名段是定长裸 R‖S（不是 DER），且能被公钥验过
     * @details RFC 7518 §3.4 规定 ES256 的签名值是 R 与 S 两个定长整数串起来。交回 DER 序列
     *          不会让本地任何断言变红，只有按定长切分的对端会失败——长度这一条就是这一档唯一看得见的证据。
     */
    TEST_F(AcmeKeyPairTest, EcSignatureIsRawFixedWidthPairsAndNotDer)
    {
        const auto request = m_ecKey->createCertificateSigningRequest({"sign-check.example.com"});
        ASSERT_TRUE(request.has_value()) << request.error().message;
        const auto parsedRequest = TestSupport::parseCertificateSigningRequest(*request);
        ASSERT_TRUE(parsedRequest.has_value());
        ASSERT_NE(parsedRequest->publicKey, nullptr);

        const std::string signingInput = "asyn-gyanis.es256.probe";
        const auto        signature    = m_ecKey->signJsonWebSigningInput(signingInput);
        ASSERT_TRUE(signature.has_value()) << signature.error().message;

        const auto decodedSignature = Base::base64UrlDecode(*signature);
        ASSERT_TRUE(decodedSignature.has_value());
        EXPECT_EQ(decodedSignature->size(), kEs256SignatureLength) << "ES256 的签名段必须是 32+32 字节的裸串，DER 序列长度会随整数位长变化";

        EXPECT_TRUE(TestSupport::verifyJsonWebSignature(*parsedRequest->publicKey, AcmeKeyAlgorithm::Es256, signingInput, *signature));
    }

    /**
     * @brief 钉住：CSR 把每一个域名都写进 subjectAltName，并且自带可用的自签
     * @details 校验侧按 RFC 9525 只看 SAN，CN 不参与匹配：漏掉任何一个域名就等于那张证书覆盖不到它，
     *          浏览器按 SAN 判「名称不匹配」，而 ACME 机构会照常把证书发出来。
     */
    TEST_F(AcmeKeyPairTest, CertificateSigningRequestCoversEveryDomainInSubjectAlternativeName)
    {
        const std::vector<std::string> domainNames{"api.example.com", "www.example.com", "example.com"};

        const auto request = m_rsaKey->createCertificateSigningRequest(domainNames);
        ASSERT_TRUE(request.has_value()) << request.error().message;
        const auto parsedRequest = TestSupport::parseCertificateSigningRequest(*request);
        ASSERT_TRUE(parsedRequest.has_value());

        EXPECT_EQ(parsedRequest->dnsNames, domainNames);
        EXPECT_TRUE(parsedRequest->isSelfSignedValid) << "请求自签验不过：机构侧会按「csr 无效」直接判死这一单";
    }

    /**
     * @brief 钉住：空域名列表被拒，而不是交出一张没有 SAN 的请求
     */
    TEST_F(AcmeKeyPairTest, EmptyDomainListIsRefusedInsteadOfAnUnsolicitedRequest)
    {
        const auto request = m_ecKey->createCertificateSigningRequest({});
        ASSERT_FALSE(request.has_value());
        EXPECT_EQ(request.error().kind, AcmeErrorKind::InvalidConfiguration);
        EXPECT_NE(request.error().message.find("域名"), std::string::npos) << request.error().message;
    }

    /**
     * @brief 钉住：私钥落盘再读回，公钥表示与指纹一字不变，且文件权限收成属主可读写
     */
    TEST_F(AcmeKeyPairTest, PrivateKeyRoundTripsThroughAnOwnerOnlyFile)
    {
        const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory("AcmeKeyPairRoundTrip");
        const std::filesystem::path                       privateKeyPath = temporaryDirectory.path() / "account-key.pem";

        ASSERT_TRUE(m_ecKey->saveToFile(privateKeyPath).has_value());

        const auto loaded = AcmeKeyPair::loadFromFile(privateKeyPath);
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
        EXPECT_EQ(loaded->publicJsonWebKeyText(), m_ecKey->publicJsonWebKeyText());
        EXPECT_EQ(loaded->jsonWebKeyThumbprint(), m_ecKey->jsonWebKeyThumbprint());
        EXPECT_EQ(loaded->jsonWebAlgorithmName(), m_ecKey->jsonWebAlgorithmName());

#ifndef _WIN32
        // 属主之外一个字节都不给：同机其它账号读到这把密钥就能冒名签发账户请求
        const std::filesystem::perms permissions = std::filesystem::status(privateKeyPath).permissions();
        EXPECT_EQ(static_cast<int>(permissions) & 0777, 0600);
#endif
    }

    /**
     * @brief 钉住：位数低于下限的 RSA 私钥被拒，而不是收下后在对端才失败
     */
    TEST_F(AcmeKeyPairTest, LoadingRejectsRsaKeysBelowTheBitFloor)
    {
        const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory("AcmeKeyPairShortRsa");
        const std::filesystem::path                       privateKeyPath = temporaryDirectory.path() / "short-rsa.pem";
        ASSERT_TRUE(TestSupport::writeGeneratedPrivateKeyPem(privateKeyPath, "RSA:1024"));

        const auto loaded = AcmeKeyPair::loadFromFile(privateKeyPath);
        ASSERT_FALSE(loaded.has_value());
        EXPECT_EQ(loaded.error().kind, AcmeErrorKind::KeyMaterial);
        EXPECT_NE(loaded.error().message.find("位"), std::string::npos) << loaded.error().message;
    }

    /**
     * @brief 钉住：非 P-256 的曲线被拒，且文案点名是哪条曲线
     * @details "crv" 一旦写成 P-256 配的却是别的曲线，机构读到的是错的公钥——失败会表现为
     *          「签名无效」这种谁都不会往曲线上想的形状，所以必须在加载这一层就拦下。
     */
    TEST_F(AcmeKeyPairTest, LoadingRejectsOtherEllipticCurves)
    {
        const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory("AcmeKeyPairOtherCurve");
        const std::filesystem::path                       privateKeyPath = temporaryDirectory.path() / "p384.pem";
        ASSERT_TRUE(TestSupport::writeGeneratedPrivateKeyPem(privateKeyPath, "EC:secp384r1"));

        const auto loaded = AcmeKeyPair::loadFromFile(privateKeyPath);
        ASSERT_FALSE(loaded.has_value());
        EXPECT_EQ(loaded.error().kind, AcmeErrorKind::KeyMaterial);
        EXPECT_NE(loaded.error().message.find("secp384r1"), std::string::npos) << "文案要点名实际读到的曲线：" << loaded.error().message;
    }

    /**
     * @brief 钉住：读不出的文件路径与「不是未加密私钥」是两档不同的告状
     * @details 两者在部署里对应完全不同的处置：前者多半是还没配私钥（要走 generate），
     *          后者是配了错的或带口令的文件（要人去换）。并成一档就会两边都只会重试。
     */
    TEST_F(AcmeKeyPairTest, MissingFileAndGarbageFileFailInDifferentWays)
    {
        const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory("AcmeKeyPairBadInputs");

        const std::filesystem::path missingPath = temporaryDirectory.path() / "never-written.pem";
        const auto                  missing     = AcmeKeyPair::loadFromFile(missingPath);
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(missing.error().kind, AcmeErrorKind::FileSystem);
        EXPECT_NE(missing.error().message.find(missingPath.string()), std::string::npos) << "文案要带上是哪份文件：" << missing.error().message;
        EXPECT_NE(missing.error().message.find("generate"), std::string::npos) << "没私钥时的下一步要写清：" << missing.error().message;

        const std::filesystem::path garbagePath = temporaryDirectory.path() / "not-a-key.pem";
        TestSupport::writeTextFile(garbagePath, "这不是私钥，只是一段随便写的文本\n");
        const auto garbage = AcmeKeyPair::loadFromFile(garbagePath);
        ASSERT_FALSE(garbage.has_value());
        EXPECT_EQ(garbage.error().kind, AcmeErrorKind::KeyMaterial);
        EXPECT_NE(garbage.error().message.find("未加密"), std::string::npos) << garbage.error().message;
    }
} // namespace AsynGyanis::Net
