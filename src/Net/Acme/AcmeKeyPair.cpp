#include "Net/Acme/AcmeKeyPair.h"

#include "Base/Coding/Base64.h"
#include "Net/Quic/QuicOpenSslError.h"
#include "Platform/FileSystem/AtomicFileWriter.h"
#include "Platform/IO/FileContents.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace Detail
    {
        void EvpKeyDeleter::operator()(EVP_PKEY *key) const noexcept
        {
            EVP_PKEY_free(key);
        }
    } // namespace Detail

    namespace
    {
        /// ES256 里 R 与 S 各自的定长字节数：P-256 的域长是 256 位
        constexpr std::size_t kEs256CoordinateLength = 32U;

        /// RSA 位数下限：CA/Browser 基线不接受 2048 以下，机构侧同样会拒；低于此值的私钥直接拒绝加载
        constexpr int kMinimumRsaKeyBits = 2048;

        /// OpenSSL 的曲线名与 JWA 的曲线名是固定的一对
        constexpr char             kOpenSslPrime256v1GroupName[] = "prime256v1";
        constexpr std::string_view kJwaP256CurveName             = "P-256";

        /// 未压缩 EC 点的字节数：1 字节的 0x04 前缀 + X 与 Y 各 32 字节
        constexpr std::size_t kUncompressedP256PointSize = 1U + 2U * kEs256CoordinateLength;

        /// PKCS#10 的版本号：v1 就是 0，ACME 的 csr 字段只认这一档
        constexpr long kPkcs10Version = 0;

        /// BIGNUM 的释放器
        struct BignumDeleter
        {
            void operator()(BIGNUM *value) const noexcept
            {
                BN_free(value);
            }
        };
        using BignumHandle = std::unique_ptr<BIGNUM, BignumDeleter>;

        /// 内存 BIO 的释放器（BIO_free 对内存 BIO 会一并放掉它持有的缓冲）
        struct BioDeleter
        {
            void operator()(BIO *bio) const noexcept
            {
                BIO_free(bio);
            }
        };
        using BioHandle = std::unique_ptr<BIO, BioDeleter>;

        /// EVP_MD_CTX 的释放器
        struct MessageContextDeleter
        {
            void operator()(EVP_MD_CTX *context) const noexcept
            {
                EVP_MD_CTX_free(context);
            }
        };
        using MessageContextHandle = std::unique_ptr<EVP_MD_CTX, MessageContextDeleter>;

        /// X509_REQ 的释放器
        struct RequestDeleter
        {
            void operator()(X509_REQ *request) const noexcept
            {
                X509_REQ_free(request);
            }
        };
        using RequestHandle = std::unique_ptr<X509_REQ, RequestDeleter>;

        /// 扩展列表的释放器：栈归本函数，压进栈的每一条扩展也随即归栈管
        struct ExtensionStackDeleter
        {
            void operator()(STACK_OF(X509_EXTENSION) * extensions) const noexcept
            {
                sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
            }
        };
        using ExtensionStackHandle = std::unique_ptr<STACK_OF(X509_EXTENSION), ExtensionStackDeleter>;

        /// ECDSA 签名值的释放器
        struct SignatureValueDeleter
        {
            void operator()(ECDSA_SIG *value) const noexcept
            {
                ECDSA_SIG_free(value);
            }
        };
        using SignatureValueHandle = std::unique_ptr<ECDSA_SIG, SignatureValueDeleter>;

        using KeyHandle = std::unique_ptr<EVP_PKEY, Detail::EvpKeyDeleter>;

        /**
         * @brief 拼一条「哪一步失败 + OpenSSL 的原话」的 KeyMaterial 档错误
         * @details 排空整条错误队列这一步复用 QUIC 角落那份工具，而不是在这里再写一份：同一句
         *          「OpenSSL 为什么拒」有两份实现迟早会漂，那份还专门处理了残留条目误导定位
         */
        [[nodiscard]] AcmeError keyMaterialFailure(const std::string_view step)
        {
            return AcmeError{AcmeErrorKind::KeyMaterial, std::format("ACME 密钥操作在「{}」这一步失败：{}", step, quicOpenSslErrorText())};
        }

        /**
         * @brief 把一个大数按定长（左侧补零）编成 base64url
         * @details JWA 里 EC 的 x/y 与 ES256 的 R/S 都必须定长：少写一个前导零字节，机构侧按定长
         *          切分就会把签名读歪，而它报回来的是「签名无效」，看不出原因在编码上
         */
        [[nodiscard]] std::expected<std::string, AcmeError> encodeFixedWidth(const BIGNUM *value, const std::size_t width, const std::string_view what)
        {
            if (value == nullptr)
            {
                return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, std::format("ACME 密钥的 {} 取不到：OpenSSL 交回了空值", what)});
            }

            std::string padded(width, '\0');
            if (BN_bn2binpad(value, reinterpret_cast<unsigned char *>(padded.data()), static_cast<int>(width)) != static_cast<int>(width))
            {
                return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, std::format("ACME 密钥的 {} 超出了 {} 字节的定长范围，这把密钥与所选算法不匹配", what, width)});
            }
            return Base::base64UrlEncode(padded);
        }

        /**
         * @brief 把一个大数按最短表示（不补前导零）编成 base64url
         * @details RSA 的 n 与 e 走这条：RFC 7518 §6.3.1 要的是大端整数的值本身，前导零属于表示法而非数值
         */
        [[nodiscard]] std::expected<std::string, AcmeError> encodeMinimal(const BIGNUM *value, const std::string_view what)
        {
            if (value == nullptr)
            {
                return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, std::format("ACME 密钥的 {} 取不到：OpenSSL 交回了空值", what)});
            }

            const int   byteCount = BN_num_bytes(value);
            std::string bytes(static_cast<std::size_t>(byteCount), '\0');
            if (byteCount > 0 && BN_bn2bin(value, reinterpret_cast<unsigned char *>(bytes.data())) != byteCount)
            {
                return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, std::format("ACME 密钥的 {} 编码失败：OpenSSL 短写了 {} 字节", what, byteCount)});
            }
            return Base::base64UrlEncode(bytes);
        }

        /**
         * @brief 读一个大数参数并接管它
         */
        [[nodiscard]] std::expected<BignumHandle, AcmeError> readBignumParameter(const EVP_PKEY &key, const char *parameterName, const std::string_view what)
        {
            BIGNUM *rawValue = nullptr;
            if (EVP_PKEY_get_bn_param(&key, parameterName, &rawValue) != 1 || rawValue == nullptr)
            {
                // 分配失败时 OpenSSL 也可能交回成功码加一个空指针，两条都算这一步失败
                return std::unexpected(keyMaterialFailure(what));
            }
            return BignumHandle(rawValue);
        }

        /**
         * @brief 取 RSA 密钥的 n 与 e 并拼成 JWK
         */
        [[nodiscard]] std::expected<Detail::PublicIdentity, AcmeError> buildRsaIdentity(const EVP_PKEY &key)
        {
            auto modulus = readBignumParameter(key, OSSL_PKEY_PARAM_RSA_N, "读取 RSA 模数");
            if (!modulus.has_value())
            {
                return std::unexpected(modulus.error());
            }
            auto publicExponent = readBignumParameter(key, OSSL_PKEY_PARAM_RSA_E, "读取 RSA 公钥指数");
            if (!publicExponent.has_value())
            {
                return std::unexpected(publicExponent.error());
            }

            auto encodedModulus = encodeMinimal(modulus->get(), "RSA 模数");
            if (!encodedModulus.has_value())
            {
                return std::unexpected(encodedModulus.error());
            }
            auto encodedExponent = encodeMinimal(publicExponent->get(), "RSA 公钥指数");
            if (!encodedExponent.has_value())
            {
                return std::unexpected(encodedExponent.error());
            }

            // 成员按 ASCII 键的字典序写死（e < kty < n）：这就是 RFC 7638 要的规范化输入。
            // 交给通用 JSON 序列化去排序，等于把「顺序即凭据」寄在库的默认行为上
            Detail::PublicIdentity identity;
            identity.jsonWebKeyText       = std::format(R"({{"e":"{}","kty":"RSA","n":"{}"}})", *encodedExponent, *encodedModulus);
            identity.jsonWebKeyThumbprint = AcmeKeyPair::computeJsonWebKeyThumbprint(identity.jsonWebKeyText);
            return identity;
        }

        /**
         * @brief 取 P-256 密钥的 crv、x 与 y 并拼成 JWK
         */
        [[nodiscard]] std::expected<Detail::PublicIdentity, AcmeError> buildEllipticCurveIdentity(const EVP_PKEY &key)
        {
            char        groupName[64]{};
            std::size_t groupNameLength = 0;
            if (EVP_PKEY_get_utf8_string_param(&key, OSSL_PKEY_PARAM_GROUP_NAME, groupName, sizeof(groupName), &groupNameLength) != 1)
            {
                return std::unexpected(keyMaterialFailure("读取 EC 曲线名"));
            }
            // 曲线名在这里再核一遍而不是只在加载时核：算法与曲线的对应关系必须只有一处判据，
            // 而 "crv" 写成 P-256、手里却是别的曲线时，机构读到的是错的公钥
            if (std::string_view(groupName, groupNameLength) != std::string_view(kOpenSslPrime256v1GroupName))
            {
                return std::unexpected(
                        AcmeError{AcmeErrorKind::KeyMaterial, std::format("ACME 这一路只用得起 prime256v1（JWA 的 P-256），这把密钥的曲线是 {}。请换一把 P-256 私钥，"
                                                                          "或改用 RS256 一档",
                                                                          std::string(groupName, groupNameLength))});
            }

            unsigned char point[kUncompressedP256PointSize]{};
            std::size_t   pointLength = 0;
            if (EVP_PKEY_get_octet_string_param(&key, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &pointLength) != 1 || pointLength != sizeof(point))
            {
                return std::unexpected(keyMaterialFailure("读取 EC 公钥点：长度不是未压缩 P-256 的 65 字节"));
            }

            const BignumHandle abscissa(BN_bin2bn(point + 1, static_cast<int>(kEs256CoordinateLength), nullptr));
            const BignumHandle ordinate(BN_bin2bn(point + 1 + kEs256CoordinateLength, static_cast<int>(kEs256CoordinateLength), nullptr));
            auto               encodedAbscissa = encodeFixedWidth(abscissa.get(), kEs256CoordinateLength, "EC 横坐标");
            if (!encodedAbscissa.has_value())
            {
                return std::unexpected(encodedAbscissa.error());
            }
            auto encodedOrdinate = encodeFixedWidth(ordinate.get(), kEs256CoordinateLength, "EC 纵坐标");
            if (!encodedOrdinate.has_value())
            {
                return std::unexpected(encodedOrdinate.error());
            }

            Detail::PublicIdentity identity;
            identity.jsonWebKeyText       = std::format(R"({{"crv":"{}","kty":"EC","x":"{}","y":"{}"}})", kJwaP256CurveName, *encodedAbscissa, *encodedOrdinate);
            identity.jsonWebKeyThumbprint = AcmeKeyPair::computeJsonWebKeyThumbprint(identity.jsonWebKeyText);
            return identity;
        }

        /// 一把密钥的算法与其公开表示
        struct DescribedKey
        {
            Detail::PublicIdentity identity;  ///< 公钥的 JWK 文本与指纹
            AcmeKeyAlgorithm       algorithm; ///< 由密钥内容推出来的算法
        };

        /**
         * @brief 由密钥内容推出算法并造出公开表示
         * @details 两者一起出：分开算就会出现「按 A 算法登记、密钥其实是 B」这种线上凭据对不上的状态
         */
        [[nodiscard]] std::expected<DescribedKey, AcmeError> describeKey(EVP_PKEY &key)
        {
            switch (EVP_PKEY_get_id(&key))
            {
                case EVP_PKEY_RSA:
                {
                    // 位数下限按「拒绝并说明」处理而不是照收：一把 1024 位的 RSA 会让 JWS 签名短于
                    // 机构侧的下限而整条通路失败，而失败点远在别处，日志里看不出是密钥太短
                    const int keyBits = EVP_PKEY_get_bits(&key);
                    if (keyBits < kMinimumRsaKeyBits)
                    {
                        return std::unexpected(
                                AcmeError{AcmeErrorKind::KeyMaterial,
                                          std::format("RSA 私钥只有 {} 位，低于可用的下限 {} 位。请重新生成一把（generate）而不是加载这份文件", keyBits, kMinimumRsaKeyBits)});
                    }
                    auto identity = buildRsaIdentity(key);
                    if (!identity.has_value())
                    {
                        return std::unexpected(identity.error());
                    }
                    return DescribedKey{std::move(*identity), AcmeKeyAlgorithm::Rs256};
                }
                case EVP_PKEY_EC:
                {
                    auto identity = buildEllipticCurveIdentity(key);
                    if (!identity.has_value())
                    {
                        return std::unexpected(identity.error());
                    }
                    return DescribedKey{std::move(*identity), AcmeKeyAlgorithm::Es256};
                }
                default:
                {
                    // 报错里带 OpenSSL 的类型编号而不是名字：取名字要 OBJ_nid2obj 那一条链，
                    // 而这里的用途是「让运维认出这把密钥不该用」，编号配上原文足够定位
                    return std::unexpected(
                            AcmeError{AcmeErrorKind::KeyMaterial, std::format("这把私钥的密钥类型（OpenSSL id {}）不能用于 ACME：本通路只支持 RSA（RS256）与 prime256v1（ES256）。"
                                                                              "请换一把这两类之一的私钥",
                                                                              EVP_PKEY_get_id(&key))});
                }
            }
        }

        /**
         * @brief 把 DER 序列的 ECDSA 签名摊平成 JWA 要的裸 R‖S
         * @details OpenSSL 交回的 ECDSA 签名是 ASN.1 SEQUENCE，而 RFC 7518 §3.4 规定 ES256 的签名值是
         *          R 与 S 两个定长整数串起来。把 DER 原样 base64url 交出去，机构回的是「签名验证失败」
         */
        [[nodiscard]] std::expected<std::string, AcmeError> flattenEllipticCurveSignature(std::string_view derSignature)
        {
            const unsigned char       *cursor = reinterpret_cast<const unsigned char *>(derSignature.data());
            const SignatureValueHandle value(d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(derSignature.size())));
            if (!value)
            {
                return std::unexpected(keyMaterialFailure("拆解 ES256 的 DER 签名"));
            }

            const BIGNUM *rawAbscissa = nullptr;
            const BIGNUM *rawOrdinate = nullptr;
            ECDSA_SIG_get0(value.get(), &rawAbscissa, &rawOrdinate);
            if (rawAbscissa == nullptr || rawOrdinate == nullptr)
            {
                return std::unexpected(keyMaterialFailure("取 ES256 签名的 R 与 S"));
            }

            // 先把两个整数各补成定长 32 字节的**裸字节**接成一串，再整串编码一次：
            // 「各编成 base64 再拼字符串」会得到一段看着像签名、按定长切分却全歪的双重编码产物
            std::string rawSignature(2U * kEs256CoordinateLength, '\0');
            auto       *writeCursor = reinterpret_cast<unsigned char *>(rawSignature.data());
            if (BN_bn2binpad(rawAbscissa, writeCursor, static_cast<int>(kEs256CoordinateLength)) != static_cast<int>(kEs256CoordinateLength) ||
                BN_bn2binpad(rawOrdinate, writeCursor + kEs256CoordinateLength, static_cast<int>(kEs256CoordinateLength)) != static_cast<int>(kEs256CoordinateLength))
            {
                return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, "ES256 签名的 R 或 S 超出了 32 字节的定长范围，这把密钥与所选算法不匹配"});
            }
            return Base::base64UrlEncode(rawSignature);
        }

        /**
         * @brief 把 PEM 私钥文本读成 EVP_PKEY
         */
        [[nodiscard]] KeyHandle readPrivateKeyFromPemText(std::string_view pemText)
        {
            const BioHandle bio(BIO_new_mem_buf(pemText.data(), static_cast<int>(pemText.size())));
            if (!bio)
            {
                return nullptr;
            }
            // 不带口令回调：加密私钥因此直接失败，而不是在无人值守的续期循环里等一次交互输入
            return KeyHandle(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
        }
    } // namespace

    AcmeKeyPair::~AcmeKeyPair() = default;

    AcmeKeyPair::AcmeKeyPair(AcmeKeyPair &&) noexcept = default;

    AcmeKeyPair &AcmeKeyPair::operator=(AcmeKeyPair &&) noexcept = default;

    AcmeKeyPair::AcmeKeyPair(std::unique_ptr<EVP_PKEY, Detail::EvpKeyDeleter> key, const AcmeKeyAlgorithm algorithm, Detail::PublicIdentity identity) :
        m_key(std::move(key)), m_algorithm(algorithm), m_jsonWebKeyThumbprint(std::move(identity.jsonWebKeyThumbprint)), m_publicJsonWebKeyText(std::move(identity.jsonWebKeyText))
    {
    }

    std::expected<AcmeKeyPair, AcmeError> AcmeKeyPair::generate(const AcmeKeyAlgorithm algorithm)
    {
        // EVP_RSA_gen / EVP_EC_gen 是 OpenSSL 3.0 起的便捷入口：省去 EVP_PKEY_CTX 那套参数装配，
        // 交回的句柄与手工 keygen 的完全同形（后续 get_*_param 都认）
        KeyHandle key(algorithm == AcmeKeyAlgorithm::Rs256 ? EVP_RSA_gen(static_cast<unsigned int>(kMinimumRsaKeyBits)) : EVP_EC_gen(kOpenSslPrime256v1GroupName));
        if (!key)
        {
            return std::unexpected(keyMaterialFailure(algorithm == AcmeKeyAlgorithm::Rs256 ? "生成 RSA 2048 密钥" : "生成 prime256v1 密钥"));
        }

        auto described = describeKey(*key);
        if (!described.has_value())
        {
            return std::unexpected(described.error());
        }
        return AcmeKeyPair(std::move(key), described->algorithm, std::move(described->identity));
    }

    std::expected<AcmeKeyPair, AcmeError> AcmeKeyPair::loadFromFile(const std::filesystem::path &privateKeyFile)
    {
        std::error_code      sizeFailure;
        const std::uintmax_t fileSize = std::filesystem::file_size(privateKeyFile, sizeFailure);
        if (sizeFailure)
        {
            return std::unexpected(AcmeError{AcmeErrorKind::FileSystem, std::format("读不出私钥文件 {}：{}。若这台服务还没拿到私钥，请先 generate() 造一把并 saveToFile() 落盘",
                                                                                    privateKeyFile.string(), sizeFailure.message())});
        }

        auto contents = Platform::readFileContents(privateKeyFile, 0U, static_cast<std::size_t>(fileSize));
        if (!contents.has_value())
        {
            return std::unexpected(AcmeError{AcmeErrorKind::FileSystem, std::format("读不出私钥文件 {}：{}", privateKeyFile.string(), contents.error().message())});
        }

        KeyHandle key = readPrivateKeyFromPemText(*contents);
        if (!key)
        {
            return std::unexpected(
                    AcmeError{AcmeErrorKind::KeyMaterial, std::format("私钥文件 {} 里没有可读的未加密 PEM 私钥：{}。本通路不收带口令的私钥（无人值守的续期循环没法输入口令），"
                                                                      "也不收只含公钥的文件",
                                                                      privateKeyFile.string(), quicOpenSslErrorText())});
        }

        auto described = describeKey(*key);
        if (!described.has_value())
        {
            return std::unexpected(described.error());
        }
        return AcmeKeyPair(std::move(key), described->algorithm, std::move(described->identity));
    }

    std::expected<void, AcmeError> AcmeKeyPair::saveToFile(const std::filesystem::path &privateKeyFile) const
    {
        const BioHandle bio(BIO_new(BIO_s_mem()));
        if (!bio)
        {
            return std::unexpected(keyMaterialFailure("开一块内存缓冲用于写私钥"));
        }
        // 走 PKCS#8（"BEGIN PRIVATE KEY"）而不是各密钥类型各自的裸格式：OpenSSL 与服务端读取侧都认这一份，
        // 且日后换算法时文件格式不变
        if (PEM_write_bio_PKCS8PrivateKey(bio.get(), m_key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        {
            return std::unexpected(keyMaterialFailure("把私钥写成 PKCS#8 PEM"));
        }

        char      *buffer     = nullptr;
        const long usedLength = BIO_get_mem_data(bio.get(), &buffer);
        if (buffer == nullptr || usedLength <= 0)
        {
            return std::unexpected(keyMaterialFailure("从内存缓冲取回写好的私钥"));
        }
        const std::string pemText(buffer, static_cast<std::size_t>(usedLength));

        std::string writeFailure;
        // 0600：私钥被同机其它用户读到就等于本机身份可被冒用。Windows 侧这条只落到只读位，
        // 那侧要靠 ACL 或存放目录来守（见头文件的 @note）
        if (!Platform::AtomicFileWriter::writeText(privateKeyFile, pemText, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, &writeFailure))
        {
            return std::unexpected(AcmeError{AcmeErrorKind::FileSystem, std::format("私钥落盘到 {} 失败：{}", privateKeyFile.string(), writeFailure)});
        }
        return {};
    }

    std::string AcmeKeyPair::publicJsonWebKeyText() const
    {
        return m_publicJsonWebKeyText;
    }

    const std::string &AcmeKeyPair::jsonWebKeyThumbprint() const noexcept
    {
        return m_jsonWebKeyThumbprint;
    }

    std::string_view AcmeKeyPair::jsonWebAlgorithmName() const noexcept
    {
        return m_algorithm == AcmeKeyAlgorithm::Rs256 ? "RS256" : "ES256";
    }

    std::expected<std::string, AcmeError> AcmeKeyPair::signJsonWebSigningInput(const std::string_view signingInput) const
    {
        const MessageContextHandle context(EVP_MD_CTX_new());
        if (!context)
        {
            return std::unexpected(keyMaterialFailure("创建签名上下文"));
        }
        // 摘要算法固定 SHA-256：它由所选 JWA 决定（RS256 与 ES256 都是 256），不是可配项
        if (EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, m_key.get()) != 1)
        {
            return std::unexpected(keyMaterialFailure("初始化签名上下文"));
        }

        const auto  inputBytes      = reinterpret_cast<const unsigned char *>(signingInput.data());
        std::size_t signatureLength = 0;
        // 两段式：先问长度（OpenSSL 允许传空缓冲探长度），再取正文，省掉按猜测预留
        if (EVP_DigestSign(context.get(), nullptr, &signatureLength, inputBytes, signingInput.size()) != 1)
        {
            return std::unexpected(keyMaterialFailure("探 ES256/RS256 的签名长度"));
        }

        std::string signature(signatureLength, '\0');
        if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &signatureLength, inputBytes, signingInput.size()) != 1)
        {
            return std::unexpected(keyMaterialFailure("签名"));
        }
        signature.resize(signatureLength);

        if (m_algorithm == AcmeKeyAlgorithm::Es256)
        {
            auto flattened = flattenEllipticCurveSignature(signature);
            if (!flattened.has_value())
            {
                return std::unexpected(flattened.error());
            }
            return flattened;
        }
        return Base::base64UrlEncode(signature);
    }

    std::expected<std::string, AcmeError> AcmeKeyPair::createCertificateSigningRequest(const std::vector<std::string> &domainNames) const
    {
        if (domainNames.empty())
        {
            return std::unexpected(AcmeError{AcmeErrorKind::InvalidConfiguration, "生成证书签名请求需要至少一个域名，这里交回的是空列表。请把要覆盖的域名填进配置"});
        }

        const RequestHandle request(X509_REQ_new());
        if (!request)
        {
            return std::unexpected(keyMaterialFailure("创建 PKCS#10 请求"));
        }
        X509_REQ_set_version(request.get(), kPkcs10Version);
        if (X509_REQ_set_pubkey(request.get(), m_key.get()) != 1)
        {
            return std::unexpected(keyMaterialFailure("把公钥写进 PKCS#10 请求"));
        }

        // 主题名只为人在 `openssl req -text` 里看着方便，凭据全在 SAN：校验侧按 RFC 9525 只看
        // subjectAltName，CN 不再参与匹配
        X509_NAME *subjectName = X509_REQ_get_subject_name(request.get());
        if (X509_NAME_add_entry_by_txt(subjectName, SN_commonName, MBSTRING_ASC, reinterpret_cast<const unsigned char *>(domainNames.front().c_str()), -1, -1, 0) != 1)
        {
            return std::unexpected(keyMaterialFailure("写 PKCS#10 请求的主题名"));
        }

        std::string subjectAlternativeNames;
        for (const std::string &domainName: domainNames)
        {
            // 域名列表拼成扩展文本喂给 OpenSSL 的解析器，',' 是它的分隔符。域名本身不含逗号
            // （RFC 1035 的标签规则），且 identifier 的合法性在 AcmeClient 一侧先核一遍
            subjectAlternativeNames += subjectAlternativeNames.empty() ? "DNS:" : ",DNS:";
            subjectAlternativeNames += domainName;
        }

        // PKCS#10 上没有 X509_add_ext 那种单条入口：扩展要成叠交给 X509_REQ_add_extensions，
        // 而它把每一条**复制**进请求，栈与扩展本体都由本函数收口
        const ExtensionStackHandle extensionStack(sk_X509_EXTENSION_new_null());
        if (!extensionStack)
        {
            return std::unexpected(keyMaterialFailure("建 PKCS#10 的扩展列表"));
        }
        X509_EXTENSION *sanExtension = X509V3_EXT_nconf_nid(nullptr, nullptr, NID_subject_alt_name, subjectAlternativeNames.c_str());
        if (sanExtension == nullptr)
        {
            return std::unexpected(AcmeError{AcmeErrorKind::KeyMaterial, std::format("证书签名请求里的 subjectAltName 扩展没能生成：{}。待覆盖的域名是 {}，逐个核对是不是合法主机名"
                                                                                     "（只允许字母、数字、连字符与点）",
                                                                                     quicOpenSslErrorText(), subjectAlternativeNames)});
        }
        // push 失败时所有权仍在调用方手上，因此这条出口要自己放掉刚造的扩展
        if (sk_X509_EXTENSION_push(extensionStack.get(), sanExtension) <= 0)
        {
            X509_EXTENSION_free(sanExtension);
            return std::unexpected(keyMaterialFailure("把 subjectAltName 扩展压进列表"));
        }
        if (X509_REQ_add_extensions(request.get(), extensionStack.get()) != 1)
        {
            return std::unexpected(keyMaterialFailure("把 subjectAltName 扩展挂上 PKCS#10 请求"));
        }

        if (X509_REQ_sign(request.get(), m_key.get(), EVP_sha256()) == 0)
        {
            return std::unexpected(keyMaterialFailure("签名 PKCS#10 请求"));
        }

        const int derLength = i2d_X509_REQ(request.get(), nullptr);
        if (derLength <= 0)
        {
            return std::unexpected(keyMaterialFailure("把 PKCS#10 请求编成 DER"));
        }
        std::string    der(static_cast<std::size_t>(derLength), '\0');
        unsigned char *cursor = reinterpret_cast<unsigned char *>(der.data());
        if (i2d_X509_REQ(request.get(), &cursor) != derLength)
        {
            return std::unexpected(keyMaterialFailure("写出 PKCS#10 请求的 DER"));
        }
        // ACME 的 "csr" 字段要的是 base64url 无填充的 DER（RFC 8555 §6.5）
        return Base::base64UrlEncode(der);
    }

    std::string AcmeKeyPair::computeJsonWebKeyThumbprint(const std::string_view canonicalJwkMembers)
    {
        // SHA-256 覆盖的是这段 ASCII 原文本身，不是它的 pretty 形式：多一个空格就换一个指纹，
        // 而 HTTP-01 的自证串（token.指纹）差一位就是机构侧校验失败
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int  digestLength = 0;
        if (EVP_Digest(canonicalJwkMembers.data(), canonicalJwkMembers.size(), digest, &digestLength, EVP_sha256(), nullptr) != 1)
        {
            // 到这里只剩内存不足一种可能；交回空串而不是「看着像指纹」的占位文本，
            // 让调用方在自证失败时看得见是这一步出了问题
            return {};
        }
        return Base::base64UrlEncode(std::string_view(reinterpret_cast<const char *>(digest), digestLength));
    }
} // namespace AsynGyanis::Net
