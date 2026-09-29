#include "Core/Crypto/Digest.h"

#include "Base/Exception/Exception.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/macros.h>
#include <openssl/params.h>

#include <format>
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
            EVP_MAC *const algorithm = EVP_MAC_fetch(nullptr, OSSL_MAC_NAME_HMAC, nullptr);
            if (algorithm == nullptr)
            {
                throw Base::Exception("Core::Digest: 无法加载 HMAC MAC 提供方（OpenSSL 未正确初始化或默认提供方未载入）");
            }
            EVP_MAC_CTX *const context = EVP_MAC_CTX_new(algorithm);
            EVP_MAC_free(algorithm); // 上下文自带一份引用：先建上下文再放算法，抛出时也不会漏句柄
            if (context == nullptr)
            {
                throw Base::Exception("Core::Digest: 无法创建 HMAC 上下文（OpenSSL 内存不足）");
            }

            // 摘要名必须落在自己持有的**可写**缓冲里：OSSL_PARAM_construct_utf8_string 收的是 char*，
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
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const std::uint8_t byte: bytes)
        {
            text += std::format("{:02x}", byte);
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
