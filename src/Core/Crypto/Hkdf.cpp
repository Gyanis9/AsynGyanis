#include "Core/Crypto/Hkdf.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"

#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core::Hkdf
{
    namespace
    {
        /// 零长输入也要给参数一个有效地址（octet string 不接受空指针）；长度仍按原始视图上报，不参与计算
        std::uint8_t g_emptyOctet = 0;

        /// 取进程级共享的 HKDF 提供方算法：成功才缓存，取不到留给下一次重试；EVP_KDF 只读，上下文由 EVP_KDF_CTX_new 自持引用
        [[nodiscard]] EVP_KDF *sharedHkdfAlgorithm() noexcept
        {
            // 失败不缓存：提供方可能下一次才载入，内存压力也可能恢复；两条线程同时首取时
            // 只有一个装上，另一份引用还回去
            static std::atomic<EVP_KDF *> cached{nullptr};
            if (EVP_KDF *const existing = cached.load(std::memory_order_acquire); existing != nullptr)
            {
                return existing;
            }
            EVP_KDF *const fetched = EVP_KDF_fetch(nullptr, OSSL_KDF_NAME_HKDF, nullptr);
            if (fetched == nullptr)
            {
                return nullptr;
            }
            EVP_KDF *expected = nullptr;
            if (!cached.compare_exchange_strong(expected, fetched, std::memory_order_release, std::memory_order_relaxed))
            {
                EVP_KDF_free(fetched);
                return expected;
            }
            return fetched;
        }

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
            EVP_KDF *const kdf = sharedHkdfAlgorithm();
            if (kdf == nullptr)
            {
                throw Base::Exception("Core::Hkdf: OpenSSL 里没有可用的 HKDF 实现（库未正确初始化或默认提供者被裁掉）。"
                                      "原因：EVP_KDF_fetch(\"HKDF\") 返回空。替代做法：确认依赖里的 OpenSSL 3.x 完整可用，"
                                      "不要退回自己拼 HMAC 轮——那等于绕开提供者的算法合规校验");
            }
            EVP_KDF_CTX *const context = EVP_KDF_CTX_new(kdf);
            if (context == nullptr)
            {
                throw Base::Exception("Core::Hkdf: 无法创建 HKDF 上下文（内存不足）");
            }

            // OSSL_PARAM 的构造接口收非 const 指针（它并不写数据），所以输入要拷进可写缓冲；
            // 只拷本模式真正用到的那份：extract 不看 info，expand 不看 salt
            std::string                         digestNameOwner(Digest::openSslName(algorithm));
            std::vector<std::uint8_t>           secretBuffer(secret.begin(), secret.end());
            const bool                          isExtract = mode == EVP_KDF_HKDF_MODE_EXTRACT_ONLY;
            const std::span<const std::uint8_t> modeInput = isExtract ? salt : info;
            std::vector<std::uint8_t>           modeBuffer(modeInput.begin(), modeInput.end());
            std::uint8_t *const                 secretData = secretBuffer.empty() ? &g_emptyOctet : secretBuffer.data();
            std::uint8_t *const                 modeData   = modeBuffer.empty() ? &g_emptyOctet : modeBuffer.data();
            int                                 modeValue  = mode;

            std::array<OSSL_PARAM, 5> parameters{
                    OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &modeValue),
                    OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digestNameOwner.data(), 0),
                    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, secretData, secret.size()),
                    isExtract ? OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, modeData, salt.size())
                              : OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, modeData, info.size()),
                    OSSL_PARAM_construct_end(),
            };

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
