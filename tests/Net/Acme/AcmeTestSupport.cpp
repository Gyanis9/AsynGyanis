#include "AcmeTestSupport.h"

#include "Base/Coding/Base64.h"
#include "Base/Config/ConfigValue.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    namespace
    {
        /// OpenSSL 通用分配的释放器（i2d 一类函数交回的缓冲归 OpenSSL 管）
        struct OpenSslMemoryDeleter
        {
            void operator()(void *pointer) const noexcept
            {
                OPENSSL_free(pointer);
            }
        };

        /// X509_REQ 的释放器
        struct RequestDeleter
        {
            void operator()(X509_REQ *request) const noexcept
            {
                X509_REQ_free(request);
            }
        };

        /// X509 的释放器
        struct CertificateDeleter
        {
            void operator()(X509 *certificate) const noexcept
            {
                X509_free(certificate);
            }
        };

        /// 扩展列表的释放器：X509_REQ_get_extensions 交回的是**复制出来**的一叠，必须逐个放掉
        struct ExtensionStackDeleter
        {
            void operator()(STACK_OF(X509_EXTENSION) * extensions) const noexcept
            {
                sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
            }
        };

        /// SAN 条目列表的释放器
        struct GeneralNameStackDeleter
        {
            void operator()(GENERAL_NAMES *names) const noexcept
            {
                sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
            }
        };

        /// BIGNUM 的释放器
        struct BignumDeleter
        {
            void operator()(BIGNUM *value) const noexcept
            {
                BN_free(value);
            }
        };

        /// ECDSA 签名值的释放器
        struct SignatureValueDeleter
        {
            void operator()(ECDSA_SIG *value) const noexcept
            {
                ECDSA_SIG_free(value);
            }
        };

        /// EVP_MD_CTX 的释放器
        struct MessageContextDeleter
        {
            void operator()(EVP_MD_CTX *context) const noexcept
            {
                EVP_MD_CTX_free(context);
            }
        };

        /// 内存 BIO 的释放器
        struct BioDeleter
        {
            void operator()(BIO *bio) const noexcept
            {
                BIO_free(bio);
            }
        };

        /// ES256 的 R 与 S 各自的定长字节数（与实现侧同一个数，这里独立写一遍：切法是 JWA 的规定）
        constexpr std::size_t kSignatureIntegerLength = 32U;

        /**
         * @brief 把 JWA 的裸 R‖S 签名段组回 OpenSSL 认的 DER 序列
         * @return std::string DER 字节；切分不合规时返回空串
         */
        [[nodiscard]] std::string rebuildDerSignature(std::string_view rawSignature)
        {
            if (rawSignature.size() != 2U * kSignatureIntegerLength)
            {
                return {};
            }
            const auto *bytes = reinterpret_cast<const unsigned char *>(rawSignature.data());
            // 非 const：ECDSA_SIG_set0 成功即接管所有权，那时要 release 掉 unique_ptr 手里的引用
            std::unique_ptr<BIGNUM, BignumDeleter> absissa(BN_bin2bn(bytes, static_cast<int>(kSignatureIntegerLength), nullptr));
            std::unique_ptr<BIGNUM, BignumDeleter> ordinate(BN_bin2bn(bytes + kSignatureIntegerLength, static_cast<int>(kSignatureIntegerLength), nullptr));
            if (!absissa || !ordinate)
            {
                return {};
            }

            const std::unique_ptr<ECDSA_SIG, SignatureValueDeleter> value(ECDSA_SIG_new());
            if (!value)
            {
                return {};
            }
            // set0 成功即接管两个 BIGNUM：失败路径才由上面的 unique_ptr 负责放掉
            if (ECDSA_SIG_set0(value.get(), absissa.release(), ordinate.release()) != 1)
            {
                return {};
            }

            unsigned char *derStorage = nullptr;
            const int      derLength  = i2d_ECDSA_SIG(value.get(), &derStorage);
            if (derLength <= 0 || derStorage == nullptr)
            {
                return {};
            }
            const std::unique_ptr<void, OpenSslMemoryDeleter> guard(derStorage);
            return std::string(reinterpret_cast<const char *>(derStorage), static_cast<std::size_t>(derLength));
        }
    } // namespace

    std::optional<ParsedCertificateRequest> parseCertificateSigningRequest(const std::string_view encodedRequest)
    {
        const auto der = Base::base64UrlDecode(encodedRequest);
        if (!der.has_value())
        {
            return std::nullopt;
        }

        const unsigned char                            *cursor = reinterpret_cast<const unsigned char *>(der->data());
        const std::unique_ptr<X509_REQ, RequestDeleter> request(d2i_X509_REQ(nullptr, &cursor, static_cast<long>(der->size())));
        if (!request)
        {
            return std::nullopt;
        }

        // X509_REQ_get0_pubkey 交回的是**借用**指针，随请求生死：多取一份引用才带得走
        EVP_PKEY *requestKey = X509_REQ_get0_pubkey(request.get());
        if (requestKey == nullptr || EVP_PKEY_up_ref(requestKey) != 1)
        {
            return std::nullopt;
        }

        ParsedCertificateRequest parsed;
        parsed.publicKey.reset(requestKey);
        parsed.isSelfSignedValid = X509_REQ_verify(request.get(), requestKey) == 1;

        const std::unique_ptr<STACK_OF(X509_EXTENSION), ExtensionStackDeleter> extensions(X509_REQ_get_extensions(request.get()));
        if (extensions == nullptr)
        {
            return parsed;
        }
        for (int index = 0; index < sk_X509_EXTENSION_num(extensions.get()); ++index)
        {
            X509_EXTENSION *extension = sk_X509_EXTENSION_value(extensions.get(), index);
            if (OBJ_obj2nid(X509_EXTENSION_get_object(extension)) != NID_subject_alt_name)
            {
                continue;
            }
            const ASN1_OCTET_STRING *payload = X509_EXTENSION_get_data(extension);
            if (payload == nullptr)
            {
                continue;
            }
            const unsigned char                                          *payloadCursor = ASN1_STRING_get0_data(payload);
            const std::unique_ptr<GENERAL_NAMES, GeneralNameStackDeleter> names(d2i_GENERAL_NAMES(nullptr, &payloadCursor, ASN1_STRING_length(payload)));
            if (names == nullptr)
            {
                continue;
            }
            for (int entryIndex = 0; entryIndex < sk_GENERAL_NAME_num(names.get()); ++entryIndex)
            {
                const GENERAL_NAME *entry = sk_GENERAL_NAME_value(names.get(), entryIndex);
                // 只收 DNS 类型：其他类型（URI、IP）进了 ACME 的 identifier 就是另一档校验
                if (entry != nullptr && entry->type == GEN_DNS && entry->d.dNSName != nullptr)
                {
                    parsed.dnsNames.emplace_back(reinterpret_cast<const char *>(ASN1_STRING_get0_data(entry->d.dNSName)),
                                                 static_cast<std::size_t>(ASN1_STRING_length(entry->d.dNSName)));
                }
            }
        }
        return parsed;
    }

    bool verifyJsonWebSignature(const EVP_PKEY &publicKey, const AcmeKeyAlgorithm algorithm, const std::string_view signingInput, const std::string_view encodedSignature)
    {
        const auto signatureBytes = Base::base64UrlDecode(encodedSignature);
        if (!signatureBytes.has_value())
        {
            return false;
        }

        // ES256 要把定长裸串组回 DER 才交给 OpenSSL 验；RS256 的 PKCS#1 v1.5 字节串 OpenSSL 原样认
        std::string derSignature = algorithm == AcmeKeyAlgorithm::Es256 ? rebuildDerSignature(*signatureBytes) : std::move(*signatureBytes);
        if (derSignature.empty())
        {
            return false;
        }

        const std::unique_ptr<EVP_MD_CTX, MessageContextDeleter> context(EVP_MD_CTX_new());
        if (!context)
        {
            return false;
        }
        if (EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, const_cast<EVP_PKEY *>(&publicKey)) != 1)
        {
            return false;
        }
        return EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(derSignature.data()), derSignature.size(),
                                reinterpret_cast<const unsigned char *>(signingInput.data()), signingInput.size()) == 1;
    }

    bool writeGeneratedPrivateKeyPem(const std::filesystem::path &path, const std::string_view keyTypeSpecification)
    {
        // 形如 "RSA:1024" 或 "EC:secp384r1"：本函数只为摆出「实现侧应当拒绝」的原料，
        // 因此刻意不经过 AcmeKeyPair（它只肯造自己收得下的那两档）
        const std::size_t separatorIndex = keyTypeSpecification.find(':');
        if (separatorIndex == std::string_view::npos)
        {
            return false;
        }
        const std::string kind     = std::string(keyTypeSpecification.substr(0, separatorIndex));
        const std::string argument = std::string(keyTypeSpecification.substr(separatorIndex + 1));

        std::unique_ptr<EVP_PKEY, Net::Detail::EvpKeyDeleter> key(kind == "RSA" ? EVP_RSA_gen(static_cast<unsigned int>(std::stoul(argument))) : EVP_EC_gen(argument.c_str()));
        if (!key)
        {
            return false;
        }

        const std::unique_ptr<BIO, BioDeleter> memory(BIO_new(BIO_s_mem()));
        if (!memory || PEM_write_bio_PKCS8PrivateKey(memory.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        {
            return false;
        }

        char      *buffer = nullptr;
        const long length = BIO_get_mem_data(memory.get(), &buffer);
        if (buffer == nullptr || length <= 0)
        {
            return false;
        }
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(buffer, length);
        return stream.good();
    }

    void writeTextFile(const std::filesystem::path &path, const std::string_view text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    void X509Deleter::operator()(X509 *certificate) const noexcept
    {
        X509_free(certificate);
    }

    std::unique_ptr<EVP_PKEY, Net::Detail::EvpKeyDeleter> publicKeyFromJsonWebKeyText(const std::string_view jwkText)
    {
        std::unique_ptr<EVP_PKEY, Net::Detail::EvpKeyDeleter> rebuilt;
        const auto                                            parsed = Base::parseConfigValue(jwkText);
        if (!parsed.has_value() || !parsed->is_object())
        {
            return nullptr;
        }
        if (!parsed->contains("kty"))
        {
            return nullptr;
        }
        const auto kind = Base::configValueAs<std::string>(parsed->at("kty"));
        // 只认这两类，且在建上下文之前就判：把别的 kty（OKP 之类）喂给 EC 的 fromdata 只会得到一条
        // 看不出根由的失败
        if (!kind.has_value() || (*kind != "RSA" && *kind != "EC"))
        {
            return nullptr;
        }

        // 按 JWK 交出的成员重建：RSA 走 n/e，EC 走 crv/x/y。两条都只用 OpenSSL 的参数接口，
        // 与被测实现「把密钥写成 JWK」那条路是两份代码，因此成员写歪在这里必然露出来
        const std::unique_ptr<EVP_PKEY_CTX, void (*)(EVP_PKEY_CTX *)> context(EVP_PKEY_CTX_new_from_name(nullptr, kind->c_str(), nullptr),
                                                                              [](EVP_PKEY_CTX *pointer) noexcept { EVP_PKEY_CTX_free(pointer); });
        if (!context || EVP_PKEY_fromdata_init(context.get()) <= 0)
        {
            return nullptr;
        }

        if (*kind == "RSA")
        {
            const auto modulusText  = parsed->contains("n") ? Base::configValueAs<std::string>(parsed->at("n")) : std::nullopt;
            const auto exponentText = parsed->contains("e") ? Base::configValueAs<std::string>(parsed->at("e")) : std::nullopt;
            if (!modulusText.has_value() || !exponentText.has_value())
            {
                return nullptr;
            }
            const auto modulusBytes  = Base::base64UrlDecode(*modulusText);
            const auto exponentBytes = Base::base64UrlDecode(*exponentText);
            if (!modulusBytes.has_value() || !exponentBytes.has_value())
            {
                return nullptr;
            }
            const std::unique_ptr<BIGNUM, BignumDeleter> modulus(
                    BN_bin2bn(reinterpret_cast<const unsigned char *>(modulusBytes->data()), static_cast<int>(modulusBytes->size()), nullptr));
            const std::unique_ptr<BIGNUM, BignumDeleter> exponent(
                    BN_bin2bn(reinterpret_cast<const unsigned char *>(exponentBytes->data()), static_cast<int>(exponentBytes->size()), nullptr));
            if (!modulus || !exponent)
            {
                return nullptr;
            }

            OSSL_PARAM parameters[3] = {
                    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N, modulus.get(), static_cast<size_t>(BN_num_bytes(modulus.get()))),
                    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, exponent.get(), static_cast<size_t>(BN_num_bytes(exponent.get()))),
                    OSSL_PARAM_END,
            };
            EVP_PKEY *rawKey = nullptr;
            if (EVP_PKEY_fromdata(context.get(), &rawKey, EVP_PKEY_PUBLIC_KEY, parameters) <= 0)
            {
                return nullptr;
            }
            rebuilt.reset(rawKey);
            return rebuilt;
        }

        const auto abscissaText = parsed->contains("x") ? Base::configValueAs<std::string>(parsed->at("x")) : std::nullopt;
        const auto ordinateText = parsed->contains("y") ? Base::configValueAs<std::string>(parsed->at("y")) : std::nullopt;
        if (!abscissaText.has_value() || !ordinateText.has_value())
        {
            return nullptr;
        }
        const auto abscissaBytes = Base::base64UrlDecode(*abscissaText);
        const auto ordinateBytes = Base::base64UrlDecode(*ordinateText);
        if (!abscissaBytes.has_value() || !ordinateBytes.has_value() || abscissaBytes->size() != kSignatureIntegerLength || ordinateBytes->size() != kSignatureIntegerLength)
        {
            return nullptr;
        }

        // 未压缩点的写法是 0x04 前缀加定长 X 与 Y：这里的定长要求正是「坐标必须按定长编码」那条判据的镜像
        std::vector<unsigned char> point(1U + 2U * kSignatureIntegerLength);
        point[0] = 0x04U;
        std::ranges::copy(*abscissaBytes, point.begin() + 1);
        std::ranges::copy(*ordinateBytes, point.begin() + 1 + static_cast<std::ptrdiff_t>(kSignatureIntegerLength));

        char       curveName[]   = "prime256v1";
        OSSL_PARAM parameters[3] = {
                OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, curveName, sizeof(curveName)),
                OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()),
                OSSL_PARAM_END,
        };
        EVP_PKEY *rawKey = nullptr;
        if (EVP_PKEY_fromdata(context.get(), &rawKey, EVP_PKEY_PUBLIC_KEY, parameters) <= 0)
        {
            return nullptr;
        }
        rebuilt.reset(rawKey);
        return rebuilt;
    }

    std::optional<TestCertificateAuthority> mintTestCertificateAuthority(const std::string_view commonName)
    {
        TestCertificateAuthority authority;
        authority.privateKey.reset(EVP_RSA_gen(2048U));
        if (!authority.privateKey)
        {
            return std::nullopt;
        }

        authority.certificate.reset(X509_new());
        if (!authority.certificate)
        {
            return std::nullopt;
        }
        X509_set_version(authority.certificate.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(authority.certificate.get()), 1);
        X509_gmtime_adj(X509_getm_notBefore(authority.certificate.get()), 0);
        // 根证书只在一轮用例里当签发者用，一小时足够；写太长会让人误以为它可以留着复用
        X509_gmtime_adj(X509_getm_notAfter(authority.certificate.get()), 60L * 60L);
        X509_set_pubkey(authority.certificate.get(), authority.privateKey.get());

        X509_NAME        *subjectName = X509_get_subject_name(authority.certificate.get());
        const std::string nameText(commonName);
        if (X509_NAME_add_entry_by_txt(subjectName, SN_commonName, MBSTRING_ASC, reinterpret_cast<const unsigned char *>(nameText.c_str()), -1, -1, 0) != 1)
        {
            return std::nullopt;
        }
        // 自签：issuer 就是自己，与仓库夹具那张根同一形状
        if (X509_set_issuer_name(authority.certificate.get(), subjectName) != 1)
        {
            return std::nullopt;
        }

        const std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION *)> basicConstraints(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_basic_constraints, "critical,CA:TRUE"),
                                                                                           [](X509_EXTENSION *extension) noexcept { X509_EXTENSION_free(extension); });
        if (!basicConstraints || X509_add_ext(authority.certificate.get(), basicConstraints.get(), -1) != 1)
        {
            return std::nullopt;
        }
        if (X509_sign(authority.certificate.get(), authority.privateKey.get(), EVP_sha256()) == 0)
        {
            return std::nullopt;
        }
        return authority;
    }

    std::string certificateToPem(const X509 &certificate)
    {
        const std::unique_ptr<BIO, BioDeleter> memory(BIO_new(BIO_s_mem()));
        if (!memory || PEM_write_bio_X509(memory.get(), const_cast<X509 *>(&certificate)) != 1)
        {
            return {};
        }
        char      *buffer = nullptr;
        const long length = BIO_get_mem_data(memory.get(), &buffer);
        if (buffer == nullptr || length <= 0)
        {
            return {};
        }
        return std::string(buffer, static_cast<std::size_t>(length));
    }

    std::vector<std::string> subjectAlternativeNamesOfCertificatePem(const std::string_view pemText)
    {
        std::vector<std::string>               dnsNames;
        const std::unique_ptr<BIO, BioDeleter> memory(BIO_new_mem_buf(pemText.data(), static_cast<int>(pemText.size())));
        if (!memory)
        {
            return dnsNames;
        }
        // 链里第一张是叶证书（与 TlsContext 的读法同一口径），SAN 只从它身上取
        CertificateHandle certificate(PEM_read_bio_X509(memory.get(), nullptr, nullptr, nullptr));
        if (!certificate)
        {
            return dnsNames;
        }

        const std::unique_ptr<GENERAL_NAMES, GeneralNameStackDeleter> names(
                static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, nullptr, nullptr)));
        if (names == nullptr)
        {
            return dnsNames;
        }
        for (int index = 0; index < sk_GENERAL_NAME_num(names.get()); ++index)
        {
            const GENERAL_NAME *entry = sk_GENERAL_NAME_value(names.get(), index);
            if (entry != nullptr && entry->type == GEN_DNS && entry->d.dNSName != nullptr)
            {
                dnsNames.emplace_back(reinterpret_cast<const char *>(ASN1_STRING_get0_data(entry->d.dNSName)), static_cast<std::size_t>(ASN1_STRING_length(entry->d.dNSName)));
            }
        }
        return dnsNames;
    }

    std::optional<std::string> issueCertificatePemFromRequest(const TestCertificateAuthority &authority, const std::string_view encodedRequest, const long serialNumber,
                                                              const long validDays)
    {
        const auto parsedRequest = parseCertificateSigningRequest(encodedRequest);
        if (!parsedRequest.has_value() || !parsedRequest->publicKey || parsedRequest->dnsNames.empty())
        {
            return std::nullopt;
        }

        CertificateHandle leaf(X509_new());
        if (!leaf)
        {
            return std::nullopt;
        }
        X509_set_version(leaf.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(leaf.get()), serialNumber);
        X509_gmtime_adj(X509_getm_notBefore(leaf.get()), 0);
        X509_gmtime_adj(X509_getm_notAfter(leaf.get()), validDays * 24L * 60L * 60L);
        if (X509_set_pubkey(leaf.get(), parsedRequest->publicKey.get()) != 1)
        {
            return std::nullopt;
        }

        X509_NAME *subjectName = X509_get_subject_name(leaf.get());
        if (X509_NAME_add_entry_by_txt(subjectName, SN_commonName, MBSTRING_ASC, reinterpret_cast<const unsigned char *>(parsedRequest->dnsNames.front().c_str()), -1, -1, 0) != 1)
        {
            return std::nullopt;
        }
        if (X509_set_issuer_name(leaf.get(), X509_get_subject_name(authority.certificate.get())) != 1)
        {
            return std::nullopt;
        }

        // SAN 照请求里交出的 DNS 名原样签进去：证书与被测实现生成的 CSR 是否合形，
        // 由用例再从这张 PEM 里读回 SAN 比对，而不是由桩替它补名字
        std::string subjectAlternativeNameText;
        for (const std::string &dnsName: parsedRequest->dnsNames)
        {
            subjectAlternativeNameText += subjectAlternativeNameText.empty() ? "DNS:" : ",DNS:";
            subjectAlternativeNameText += dnsName;
        }
        const std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION *)> subjectAlternativeName(
                X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, subjectAlternativeNameText.c_str()),
                [](X509_EXTENSION *extension) noexcept { X509_EXTENSION_free(extension); });
        if (!subjectAlternativeName || X509_add_ext(leaf.get(), subjectAlternativeName.get(), -1) != 1)
        {
            return std::nullopt;
        }
        const std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION *)> leafConstraints(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_basic_constraints, "CA:FALSE"),
                                                                                          [](X509_EXTENSION *extension) noexcept { X509_EXTENSION_free(extension); });
        if (!leafConstraints || X509_add_ext(leaf.get(), leafConstraints.get(), -1) != 1)
        {
            return std::nullopt;
        }

        if (X509_sign(leaf.get(), authority.privateKey.get(), EVP_sha256()) == 0)
        {
            return std::nullopt;
        }
        // 只交叶证书：根是自签的、按 RFC 5280 不必出现在链里，服务端与对端都靠信任库补最后一跳
        return certificateToPem(*leaf);
    }
} // namespace AsynGyanis::Net::TestSupport
