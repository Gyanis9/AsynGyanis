/**
 * @file AcmeTestSupport.h
 * @brief ACME 用例共用的 OpenSSL 侧取证助手：解 CSR、验 JWS 签名、造畸形私钥文件
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Acme/AcmeKeyPair.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    /// X509 的归还动作
    struct X509Deleter
    {
        void operator()(X509 *certificate) const noexcept;
    };

    /// 一张证书的所有权句柄
    using CertificateHandle = std::unique_ptr<X509, X509Deleter>;
    /// 一份已解开的 PKCS#10 请求：公钥、SAN 里的 DNS 名，以及它自己签得对不对
    struct ParsedCertificateRequest
    {
        std::unique_ptr<EVP_PKEY, Net::detail::EvpKeyDeleter> publicKey;                ///< 请求里带着的公钥（SPKI 那条解码路）
        std::vector<std::string>                              dnsNames;                 ///< subjectAltName 里 DNS 类型的条目，按出现顺序
        bool                                                  isSelfSignedValid{false}; ///< 请求自带的签名能否用上面的公钥验过
    };

    /**
     * @brief 解开 base64url(DER) 形态的证书签名请求
     * @param encodedRequest AcmeKeyPair::createCertificateSigningRequest() 交回的那段文本
     * @return std::optional<ParsedCertificateRequest> 解开的请求；不是合法 base64url、DER 或 PKCS#10 时为空
     * @note 公钥走请求里的 SPKI 解码而不是本仓库的 JWK 序列化：验签一侧因此与被测实现无关
     */
    [[nodiscard]] std::optional<ParsedCertificateRequest> parseCertificateSigningRequest(std::string_view encodedRequest);

    /**
     * @brief 用给定的公钥验一段 JWS 签名
     * @details ES256 的签名段按 JWA 的定长 R‖S 切回整数再组 DER 才能交给 OpenSSL 验：
     *          被测实现若交回的是 DER，切出来的就是两个歪掉的整数，这里必然验不过——
     *          这一步正是本函数存在的理由（RS256 则是原样把字节交给 OpenSSL）。
     * @param publicKey 与签名对应的公钥（一般取自本次那份 CSR）
     * @param algorithm 签名算法，决定签名段的切法
     * @param signingInput 待签原文（"protected.payload"）
     * @param encodedSignature base64url 无填充的签名段
     * @return true 验签通过
     */
    [[nodiscard]] bool verifyJsonWebSignature(const EVP_PKEY &publicKey, AcmeKeyAlgorithm algorithm, std::string_view signingInput, std::string_view encodedSignature);

    /**
     * @brief 从一份 JWK 文本重建公钥
     * @details 桩颁发机构要拿账户公钥验账户自己签的 JWS，因此必须走「按 JWK 里交出的成员重建」这条路：
     *          它与被测实现把密钥写成 JWK 的那条路是两份代码，n 与 e（或 x 与 y）谁写错都会在这里露出来。
     * @param jwkText JWK 的紧凑 JSON 文本
     * @return 重建出的公钥；kty 不认识、成员缺失或 OpenSSL 拒绝时为空
     */
    [[nodiscard]] std::unique_ptr<EVP_PKEY, Net::detail::EvpKeyDeleter> publicKeyFromJsonWebKeyText(std::string_view jwkText);

    /**
     * @brief 一份测试用的证书颁发机构：一把私钥加一张自签根
     */
    struct TestCertificateAuthority
    {
        std::unique_ptr<EVP_PKEY, Net::detail::EvpKeyDeleter> privateKey;  ///< 签发用的密钥
        CertificateHandle                                     certificate; ///< 自签根，签出来的证书以它为 issuer
    };

    /**
     * @brief 造一张测试用的自签 CA（RSA 2048 + basicConstraints critical CA:TRUE）
     * @param commonName 主题名，也作为下一级证书的 issuer 名
     * @return std::optional<TestCertificateAuthority> 造好的 CA；OpenSSL 失败时为空
     */
    [[nodiscard]] std::optional<TestCertificateAuthority> mintTestCertificateAuthority(std::string_view commonName);

    /**
     * @brief 按一份 PKCS#10 请求签发一张叶证书，交回 PEM 文本
     * @details 请求里带什么 DNS 名就签什么，与真机构一致：证书与被测实现生成的 CSR 是否合形，
     *          因此由这张证书反证（用例再从 PEM 里读回 SAN 比对）。
     * @param authority 上一步造出的 CA
     * @param encodedRequest base64url(DER) 的 CSR
     * @param serialNumber 序列号（换证用例靠它区分新旧两张）
     * @param validDays 有效期天数
     * @return std::optional<std::string> PEM 叶证书；请求不合法时为空
     */
    [[nodiscard]] std::optional<std::string> issueCertificatePemFromRequest(const TestCertificateAuthority &authority, std::string_view encodedRequest, long serialNumber,
                                                                            long validDays);

    /**
     * @brief 把一张 X509 交回 PEM 文本
     */
    [[nodiscard]] std::string certificateToPem(const X509 &certificate);

    /**
     * @brief 读一份 PEM 证书（链里第一张）的 subjectAltName DNS 名
     * @param pemText PEM 文本
     * @return std::vector<std::string> DNS 名，按出现顺序；读不出扩展时为空
     */
    [[nodiscard]] std::vector<std::string> subjectAlternativeNamesOfCertificatePem(std::string_view pemText);

    /**
     * @brief 用 OpenSSL 现场生成一把私钥并写成 PEM 文件，供「畸形私钥要拒绝」那类用例摆原料
     * @param path 输出路径
     * @param keyTypeSpecification OpenSSL 的算法名与参数写法：如 "RSA:2048"、"EC:secp384r1"
     * @return true 文件已写出
     * @note 本函数刻意能造出本框架**不收**的密钥（1024 位 RSA、P-384 曲线）：拒绝面要有真原料才测得到
     */
    [[nodiscard]] bool writeGeneratedPrivateKeyPem(const std::filesystem::path &path, std::string_view keyTypeSpecification);

    /**
     * @brief 往路径里写一段任意文本（不是 PEM 也行），供加载失败那类用例摆原料
     * @param path 输出路径
     * @param text 原文
     */
    void writeTextFile(const std::filesystem::path &path, std::string_view text);

} // namespace AsynGyanis::Net::TestSupport
