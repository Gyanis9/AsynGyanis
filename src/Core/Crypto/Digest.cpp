#include "Core/Crypto/Digest.h"

#include "Base/Exception/Exception.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/macros.h>
#include <openssl/params.h>

#include <string>

namespace AsynGyanis::Core::Digest
{
    namespace
    {
        /**
         * @brief 用 EVP 的摘要接口一次算完
         * @details 上下文无论成败都要释放：这条路径会抛出，漏掉就是每次调用泄一个句柄
         * @param data 待摘要数据
         * @param algorithm 摘要算法（EVP_sha1() / EVP_sha256() 之类）
         * @param digestSize 期望的摘要长度，用于核对返回值
         * @param algorithmName 报错文案里用的算法名
         * @return std::span<std::uint8_t> 指向 out 的视图；调用方保证 out 足够长
         */
        void runDigest(std::string_view data, const EVP_MD *algorithm, std::span<std::uint8_t> out, std::string_view algorithmName)
        {
            EVP_MD_CTX *const context = EVP_MD_CTX_new();
            if (context == nullptr)
            {
                throw Base::Exception("Core::Digest: 无法创建 " + std::string(algorithmName) + " 摘要上下文（OpenSSL 未正确初始化或内存不足）");
            }

            unsigned int producedLength = 0;
            const bool   isSucceeded    = EVP_DigestInit_ex(context, algorithm, nullptr) == 1 && EVP_DigestUpdate(context, data.data(), data.size()) == 1 &&
                                          EVP_DigestFinal_ex(context, out.data(), &producedLength) == 1;
            EVP_MD_CTX_free(context);

            if (!isSucceeded || producedLength != out.size())
            {
                throw Base::Exception("Core::Digest: " + std::string(algorithmName) + " 摘要未能算出完整结果（OpenSSL 摘要接口返回失败）");
            }
        }

        /**
         * @brief 取进程级共享的 HMAC 算法句柄
         * @details 每次调用都 fetch 一次等于把提供方查找放进摘要热路径；
         *          上下文由 EVP_MAC_CTX_new 自持引用，这个静态句柄只需活到进程退出
         * @return EVP_MAC * 共享的算法句柄；取不到时为 nullptr
         */
        [[nodiscard]] EVP_MAC *sharedHmacAlgorithm() noexcept
        {
            static EVP_MAC *const algorithm = EVP_MAC_fetch(nullptr, OSSL_MAC_NAME_HMAC, nullptr);
            return algorithm;
        }

        /**
         * @brief 用 EVP_MAC 的 HMAC 提供方一次算完带密钥的认证码
         * @details 走 EVP_MAC 的显式流程而不是 HMAC()/EVP_Q_mac 一次性接口：前者在 OpenSSL 3.x 里被标弃用，
         *          后者的参数形状在 3.x 各小版本之间漂过（本机这份的第 5 参已是 OSSL_PARAM*），
         *          而摘要名一类的东西按参数交进去才是稳定契约。
         *          上下文无论成败都要释放：这条路径会抛出，漏掉就是每次调用泄一个句柄
         * @param key 密钥（按「指针 + 长度」取，允许含 NUL）
         * @param data 待认证数据
         * @param digestName OpenSSL 的摘要名（OSSL_DIGEST_NAME_* 那一族）
         * @param out 输出缓冲，其长度就是期望的认证码长度，用于核对返回值
         * @param algorithmName 报错文案里用的算法名
         */
        void runHmac(std::string_view key, std::string_view data, std::string_view digestName, std::span<std::uint8_t> out, std::string_view algorithmName)
        {
            EVP_MAC *const algorithm = sharedHmacAlgorithm();
            if (algorithm == nullptr)
            {
                throw Base::Exception("Core::Digest: 无法加载 HMAC MAC 提供方（OpenSSL 未正确初始化或默认提供方未载入）");
            }
            EVP_MAC_CTX *const context = EVP_MAC_CTX_new(algorithm);
            if (context == nullptr)
            {
                throw Base::Exception("Core::Digest: 无法创建 HMAC 上下文（OpenSSL 内存不足）");
            }

            // 摘要名必须落在自己持有的可写缓冲里：OSSL_PARAM_construct_utf8_string 收的是 char*，
            // 而 OpenSSL 不承诺绝不回写这段参数
            std::string digestNameOwner(digestName);
            OSSL_PARAM  parameters[2];
            parameters[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestNameOwner.data(), 0);
            parameters[1] = OSSL_PARAM_construct_end();

            size_t     producedLength = 0;
            const bool isSucceeded    = EVP_MAC_init(context, reinterpret_cast<const unsigned char *>(key.data()), key.size(), parameters) == 1 &&
                                        EVP_MAC_update(context, reinterpret_cast<const unsigned char *>(data.data()), data.size()) == 1 &&
                                        EVP_MAC_final(context, out.data(), &producedLength, out.size()) == 1;
            EVP_MAC_CTX_free(context);

            if (!isSucceeded || producedLength != out.size())
            {
                throw Base::Exception("Core::Digest: " + std::string(algorithmName) + " 未能算出完整结果（OpenSSL MAC 接口返回失败，可能是密钥长度非法或提供方拒绝该摘要）");
            }
        }
    } // namespace

    Sha1Value sha1(const std::string_view data)
    {
        Sha1Value digest{};
        runDigest(data, EVP_sha1(), std::span<std::uint8_t>{digest}, "SHA-1");
        return digest;
    }

    Sha256Value sha256(const std::string_view data)
    {
        Sha256Value digest{};
        runDigest(data, EVP_sha256(), std::span<std::uint8_t>{digest}, "SHA-256");
        return digest;
    }

    std::string_view openSslName(const Algorithm algorithm)
    {
        switch (algorithm)
        {
            case Algorithm::Sha256:
                return OSSL_DIGEST_NAME_SHA2_256;
            case Algorithm::Sha384:
                return OSSL_DIGEST_NAME_SHA2_384;
            case Algorithm::Sha512:
                return OSSL_DIGEST_NAME_SHA2_512;
        }
        // 走到这里说明上面漏了一条 case（新增枚举值时最容易漏的就是这里）：与其返回一个
        // 「看着像 SHA-256」的名字让 KDF 悄悄换掉哈希，不如当场报出来
        throw Base::Exception("Core::Digest: 请求了一个不认识的摘要算法的选择子，无法给出它在 OpenSSL 里的名字");
    }

    std::size_t byteLength(const Algorithm algorithm)
    {
        switch (algorithm)
        {
            case Algorithm::Sha256:
                return kSha256Length;
            case Algorithm::Sha384:
                return kSha384Length;
            case Algorithm::Sha512:
                return kSha512Length;
        }
        // 同 openSslName：漏了 case 就报，不交回一个「看着像 32 字节」的长度——KDF 拿它定
        // PRK 长度与输出上限，错一位就是派生出另一份密钥
        throw Base::Exception("Core::Digest: 请求了一个不认识的摘要算法的选择子，无法给出它的摘要长度");
    }

    Sha384Value sha384(const std::string_view data)
    {
        Sha384Value digest{};
        runDigest(data, EVP_sha384(), std::span<std::uint8_t>{digest}, "SHA-384");
        return digest;
    }

    Sha512Value sha512(const std::string_view data)
    {
        Sha512Value digest{};
        runDigest(data, EVP_sha512(), std::span<std::uint8_t>{digest}, "SHA-512");
        return digest;
    }

    Sha1Value hmacSha1(const std::string_view key, const std::string_view data)
    {
        Sha1Value mac{};
        runHmac(key, data, OSSL_DIGEST_NAME_SHA1, std::span<std::uint8_t>{mac}, "HMAC-SHA-1");
        return mac;
    }

    Sha256Value hmacSha256(const std::string_view key, const std::string_view data)
    {
        Sha256Value mac{};
        runHmac(key, data, OSSL_DIGEST_NAME_SHA2_256, std::span<std::uint8_t>{mac}, "HMAC-SHA-256");
        return mac;
    }

    std::string toHex(const std::span<const std::uint8_t> bytes)
    {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string           text(bytes.size() * 2, '\0');
        char                 *cursor = text.data();
        for (const std::uint8_t byte: bytes)
        {
            *cursor++ = kHexDigits[byte >> 4U];
            *cursor++ = kHexDigits[byte & 0x0FU];
        }
        return text;
    }

    std::string sha256Hex(const std::string_view data)
    {
        const Sha256Value digest = sha256(data);
        return toHex(digest);
    }

    std::string hmacSha256Hex(const std::string_view key, const std::string_view data)
    {
        const Sha256Value mac = hmacSha256(key, data);
        return toHex(mac);
    }
} // namespace AsynGyanis::Core::Digest
