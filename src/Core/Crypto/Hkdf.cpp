#include "Core/Crypto/Hkdf.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"

#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Core::Hkdf
{
    namespace
    {
        /**
         * @brief 跑一次 OpenSSL 的 HKDF（抽取段与扩展段之一）
         * @details 走 EVP_KDF 而不是已废弃的 `EVP_PKEY_CTX` 通路：3.x 起这才是受支持的接口，
         *          而且不需要为一次推导先造一把假密钥。
         * @param mode `EVP_KDF_HKDF_MODE_EXTRACT_ONLY` 或 `EVP_KDF_HKDF_MODE_EXPAND_ONLY`
         * @param algorithm 哈希算法
         * @param secret 输入密钥材料（抽取段的 IKM，扩展段的 PRK）
         * @param salt 抽取段的盐；扩展段忽略
         * @param info 扩展段的上下文绑定串；抽取段忽略
         * @param outputByteLength 需要的输出长度
         * @param operation 报错文案里的操作名，失败时好定位是哪一步
         * @return std::vector<std::uint8_t> 长度等于 outputByteLength 的输出
         */
        [[nodiscard]] std::vector<std::uint8_t> run(const int mode, const Digest::Algorithm algorithm, const std::span<const std::uint8_t> secret,
                                                    const std::span<const std::uint8_t> salt, const std::span<const std::uint8_t> info, const std::size_t outputByteLength,
                                                    const std::string_view operation)
        {
            EVP_KDF *const kdf = EVP_KDF_fetch(nullptr, OSSL_KDF_NAME_HKDF, nullptr);
            if (kdf == nullptr)
            {
                throw Base::Exception("Core::Hkdf: OpenSSL 里没有可用的 HKDF 实现（库未正确初始化或默认提供者被裁掉）。"
                                      "原因：EVP_KDF_fetch(\"HKDF\") 返回空。替代做法：确认依赖里的 OpenSSL 3.x 完整可用，"
                                      "不要退回自己拼 HMAC 轮——那等于绕开提供者的算法合规校验");
            }
            EVP_KDF_CTX *const context = EVP_KDF_CTX_new(kdf);
            // 上下文创建时已自带一份算法引用，本处的引用先还掉：抛出时才不会漏句柄
            EVP_KDF_free(kdf);
            if (context == nullptr)
            {
                throw Base::Exception("Core::Hkdf: 无法创建 HKDF 上下文（内存不足）");
            }

            // OSSL_PARAM 的构造接口收非 const 指针（它并不写数据），因此三类输入各拷一份，
            // 不对调用方的 const 数据挂 const_cast
            std::string               digestNameOwner(Digest::openSslName(algorithm));
            std::vector<std::uint8_t> secretBuffer(secret.begin(), secret.end());
            std::vector<std::uint8_t> saltBuffer(salt.begin(), salt.end());
            std::vector<std::uint8_t> infoBuffer(info.begin(), info.end());
            // 空输入的 vector 允许返回空指针，而 octet string 参数不接受空地址——HKDF-Extract 的 IKM
            // 确实可能是零长（RFC 9001 §5.2 的目的连接标识可以为 0 字节）。各占一字节保证地址有效，
            // 长度仍按原始视图报上去，不参与计算
            if (secretBuffer.empty())
            {
                secretBuffer.resize(1);
            }
            if (saltBuffer.empty())
            {
                saltBuffer.resize(1);
            }
            if (infoBuffer.empty())
            {
                infoBuffer.resize(1);
            }
            int modeValue = mode;

            std::vector<OSSL_PARAM> parameters;
            parameters.push_back(OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &modeValue));
            parameters.push_back(OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digestNameOwner.data(), 0));
            parameters.push_back(OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, secretBuffer.data(), secret.size()));
            parameters.push_back(mode == EVP_KDF_HKDF_MODE_EXTRACT_ONLY ? OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, saltBuffer.data(), salt.size())
                                                                        : OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, infoBuffer.data(), info.size()));
            parameters.push_back(OSSL_PARAM_construct_end());

            std::vector<std::uint8_t> output(outputByteLength);
            const int                 deriveResult = EVP_KDF_derive(context, output.data(), output.size(), parameters.data());
            EVP_KDF_CTX_free(context);
            if (deriveResult != 1)
            {
                throw Base::Exception(std::format("Core::Hkdf: {} 未能算出 {} 字节输出（OpenSSL 拒绝了参数或该哈希不可用）", operation, outputByteLength));
            }
            return output;
        }
    } // namespace

    std::vector<std::uint8_t> extract(const Digest::Algorithm algorithm, const std::span<const std::uint8_t> salt, const std::span<const std::uint8_t> inputKeyMaterial)
    {
        return run(EVP_KDF_HKDF_MODE_EXTRACT_ONLY, algorithm, inputKeyMaterial, salt, {}, Digest::byteLength(algorithm), "HKDF-Extract");
    }

    std::vector<std::uint8_t> expand(const Digest::Algorithm algorithm, const std::span<const std::uint8_t> pseudoRandomKey, const std::span<const std::uint8_t> info,
                                     const std::size_t outputByteLength)
    {
        // RFC 5869 §2.3：输出长度不得超过 255 × 哈希长度（T(i) 的下标是单字节）。
        // 越界一律拒而不是截断：截断出来的那份密钥与调用方算的那份不是同一个东西
        const std::size_t maximumOutputByteLength = 255U * Digest::byteLength(algorithm);
        if (outputByteLength > maximumOutputByteLength)
        {
            throw Base::InvalidArgumentException(std::format("Core::Hkdf: HKDF-Expand 要求 {} 字节输出，超过所选哈希的上限 {} 字节"
                                                             "（RFC 5869 §2.3 的 255 × Hash.length）。原因：这个长度不是「多给一点」，"
                                                             "而是派生本身用错了哈希或算错了额度。替代做法：换更长的哈希，或按用途分成多次派生",
                                                             outputByteLength, maximumOutputByteLength));
        }
        if (pseudoRandomKey.empty())
        {
            throw Base::InvalidArgumentException("Core::Hkdf: HKDF-Expand 的伪随机密钥为空。原因：扩展段的密钥必须是 extract 的产物"
                                                 "（或一份真实共享秘密），空密钥派生出来的只是「对一个零密钥的认证」。"
                                                 "替代做法：先 extract，或直接给出真实的共享秘密");
        }
        return run(EVP_KDF_HKDF_MODE_EXPAND_ONLY, algorithm, pseudoRandomKey, {}, info, outputByteLength, "HKDF-Expand");
    }

    std::vector<std::uint8_t> derive(const Digest::Algorithm algorithm, const std::span<const std::uint8_t> salt, const std::span<const std::uint8_t> inputKeyMaterial,
                                     const std::span<const std::uint8_t> info, const std::size_t outputByteLength)
    {
        const std::vector<std::uint8_t> pseudoRandomKey = extract(algorithm, salt, inputKeyMaterial);
        return expand(algorithm, pseudoRandomKey, info, outputByteLength);
    }
} // namespace AsynGyanis::Core::Hkdf
