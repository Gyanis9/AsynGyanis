#include "Net/Quic/Crypto/QuicKeySchedule.h"

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
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// TLS 1.3 的 HKDF-Expand-Label 前缀（RFC 8446 §7.1）；QUIC 复用同一构造（RFC 9001 §5.1）
        constexpr std::string_view kHkdfLabelPrefix = "tls13 ";

        /// OpenSSL 提供者里的哈希算法名：只有 AES-256-GCM 用 SHA-384，其余都是 SHA-256
        constexpr const char *kShaTwo256Name = "SHA2-256";

        constexpr const char *kShaTwo384Name = "SHA2-384";

        /**
         * @brief 跑一次 OpenSSL 的 HKDF（只取抽取或只取扩展两段之一）
         * @details 用 EVP_KDF 而不是已废弃的 EVP_PKEY_CTX 通路：3.x 起这才是受支持的接口，
         *          且不需要为一次推导造一把假密钥。
         * @param mode EVP_KDF_HKDF_MODE_EXTRACT_ONLY 或 EVP_KDF_HKDF_MODE_EXPAND_ONLY
         * @param secret 输入密钥材料（抽取段的 IKM，扩展段的 PRK）
         * @param salt 抽取段的盐，扩展段忽略
         * @param info 扩展段的info，抽取段忽略
         * @param outputLength 需要的输出字节数
         * @param hashName 哈希算法名
         * @param operation 面向文案的操作名，失败时报出来好定位是哪一步
         * @return std::vector<std::uint8_t> 输出
         * @throws Base::Exception 运行期故障：取不到实现、建不了上下文或推导本身失败
         */
        std::vector<std::uint8_t> runHkdf(const int mode, const std::span<const std::uint8_t> secret,
                                         const std::span<const std::uint8_t> salt, const std::span<const std::uint8_t> info,
                                         const std::size_t outputLength, const char *hashName, const std::string_view operation)
        {
            EVP_KDF *const kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
            if (kdf == nullptr)
            {
                throw Base::Exception(std::format("QUIC {} 失败：OpenSSL 没有可用的 HKDF 实现（库未正确初始化或默认提供者被裁掉）："
                                                  "请确认依赖里的 OpenSSL 3.x 完整可用",
                                                  operation));
            }
            EVP_KDF_CTX *const context = EVP_KDF_CTX_new(kdf);
            // 上下文创建时已持有算法引用，取到之后本处的引用就可以还掉
            EVP_KDF_free(kdf);
            if (context == nullptr)
            {
                throw Base::Exception(std::format("QUIC {} 失败：无法创建 HKDF 上下文（内存不足）", operation));
            }

            // OSSL_PARAM 的构造接口收非 const 指针（它并不写数据）。把输入拷进局部缓冲，
            // 而不是对调用方的 const 数据挂 const_cast
            char digestName[16]{};
            std::memcpy(digestName, hashName, std::strlen(hashName));
            std::vector<std::uint8_t> secretBuffer(secret.begin(), secret.end());
            std::vector<std::uint8_t> saltBuffer(salt.begin(), salt.end());
            std::vector<std::uint8_t> infoBuffer(info.begin(), info.end());
            // 空输入的 vector 允许返回空指针，而 octet string 参数不接受空地址——HKDF-Extract 的 IKM
            // 确实可能是零长（RFC 9001 §5.2 的目的连接标识可以为 0 字节）。各占一字节保证地址有效，
            // 长度仍按原始 span 报上去，不参与计算
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
            parameters.push_back(OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digestName, 0));
            parameters.push_back(OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, secretBuffer.data(), secret.size()));
            parameters.push_back(mode == EVP_KDF_HKDF_MODE_EXTRACT_ONLY
                                         ? OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, saltBuffer.data(), salt.size())
                                         : OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, infoBuffer.data(), info.size()));
            parameters.push_back(OSSL_PARAM_construct_end());

            std::vector<std::uint8_t> output(outputLength);
            const int deriveResult = EVP_KDF_derive(context, output.data(), output.size(), parameters.data());
            EVP_KDF_CTX_free(context);
            if (deriveResult != 1)
            {
                throw Base::Exception(std::format("QUIC {} 失败：HKDF 未能算出 {} 字节输出（OpenSSL 拒绝了参数或该哈希不可用）",
                                                  operation, outputLength));
            }
            return output;
        }

        /**
         * @brief 按 RFC 8446 §7.1 拼出 HkdfLabel 并做 HKDF-Expand
         * @details 字段依次是 uint16 输出长度、uint8(「tls13 」+标签的总长)、该拼接标签、uint8 上下文长、上下文。
         *          QUIC 的所有用法上下文都是零长（RFC 9001 §5.1），但长度字节仍要写。
         * @param secret 输入秘密
         * @param label 标签原文，不含前缀
         * @param outputLength 需要的输出字节数
         * @param hashName 哈希算法名
         * @param operation 面向文案的操作名
         * @return std::vector<std::uint8_t> 输出
         * @throws Base::InvalidArgumentException 用法错误：标签或上下文长到 uint8 表达不下
         */
        std::vector<std::uint8_t> expandLabel(const std::span<const std::uint8_t> secret, const std::string_view label,
                                             const std::size_t outputLength, const char *hashName, const std::string_view operation)
        {
            const std::size_t labelledLength = kHkdfLabelPrefix.size() + label.size();
            if (labelledLength > 0xFF)
            {
                throw Base::InvalidArgumentException(std::format("HKDF-Expand-Label 的标签总长 {} 字节超过 uint8 上限 255"
                                                                 "（RFC 8446 §7.1 用单字节表达「tls13 」+标签的长度）：请缩短标签",
                                                                 labelledLength));
            }

            std::vector<std::uint8_t> encodedLabel;
            encodedLabel.reserve(2 + 1 + labelledLength + 1);
            encodedLabel.push_back(static_cast<std::uint8_t>((outputLength >> 8) & 0xFFU));
            encodedLabel.push_back(static_cast<std::uint8_t>(outputLength & 0xFFU));
            encodedLabel.push_back(static_cast<std::uint8_t>(labelledLength));
            for (const char prefixCharacter: kHkdfLabelPrefix)
            {
                encodedLabel.push_back(static_cast<std::uint8_t>(prefixCharacter));
            }
            for (const char labelCharacter: label)
            {
                encodedLabel.push_back(static_cast<std::uint8_t>(labelCharacter));
            }
            // 零长上下文：长度字节写 0，后面不带字节
            encodedLabel.push_back(0);

            return runHkdf(EVP_KDF_HKDF_MODE_EXPAND_ONLY, secret, {}, encodedLabel, outputLength, hashName, operation);
        }

        /**
         * @brief 把导出结果拷进定长数组
         * @param destination 目标数组
         * @param source 导出的字节
         * @param what 目标用途的中文名，长度不符时报出来
         * @throws Base::Exception 运行期故障：导出结果长度与预期不符
         */
        void copyInto(std::span<std::uint8_t> destination, const std::span<const std::uint8_t> source, std::string_view what)
        {
            if (source.size() != destination.size())
            {
                throw Base::Exception(std::format("QUIC 密钥导出异常：{} 需要 {} 字节却拿到 {} 字节：请核对哈希与套件的搭配",
                                                  what, destination.size(), source.size()));
            }
            std::copy(source.begin(), source.end(), destination.begin());
        }

        /**
         * @brief 用一个流量秘密导出「key / iv / hp」三段
         * @param cipherSuite 套件，决定哈希与三段长度
         * @param trafficSecret 流量秘密
         * @param operation 面向文案的操作名
         * @return QuicPacketKeys 密钥组
         */
        QuicPacketKeys deriveKeyTriple(const QuicCipherSuite cipherSuite, const std::span<const std::uint8_t> trafficSecret,
                                       const std::string_view operation)
        {
            const char *const hashName = cipherSuite == QuicCipherSuite::Aes256Gcm ? kShaTwo384Name : kShaTwo256Name;

            QuicPacketKeys keys;
            keys.cipherSuite = cipherSuite;
            // 留住这一代的流量秘密：§6.1 的密钥更新是在它上面再递推一步，拿不到秘密就没法往前推
            std::copy(trafficSecret.begin(), trafficSecret.end(), keys.generationSecret.begin());
            // 三个标签逐字取自 RFC 9001 §5.1；IV 长度取 AEAD nonce 的最小长度 12（§5.1 末段）。
            // 目标区间要先按套件的实际长度截好：数组是 32 字节的公共容器，拿整个数组比长度会把
            // AES-128 这类短密钥一律判成长度不符
            const auto encryptionKey = expandLabel(trafficSecret, "quic key", quicCipherSuiteKeyByteLength(cipherSuite), hashName,
                                                   std::format("{}：AEAD 密钥", operation));
            copyInto(std::span(keys.encryptionKey).first(quicCipherSuiteKeyByteLength(cipherSuite)), encryptionKey, "AEAD 密钥");

            const auto initializationVector = expandLabel(trafficSecret, "quic iv", kQuicInitializationVectorByteLength, hashName,
                                                          std::format("{}：初始化向量", operation));
            copyInto(keys.initializationVector, initializationVector, "初始化向量");

            const auto headerProtectionKey = expandLabel(trafficSecret, "quic hp",
                                                         quicCipherSuiteHeaderProtectionKeyByteLength(cipherSuite), hashName,
                                                         std::format("{}：头部保护密钥", operation));
            copyInto(std::span(keys.headerProtectionKey).first(quicCipherSuiteHeaderProtectionKeyByteLength(cipherSuite)),
                     headerProtectionKey, "头部保护密钥");
            return keys;
        }
    } // namespace

    QuicPacketKeys deriveQuicInitialPacketKeys(const std::span<const std::uint8_t> destinationConnectionId,
                                               const QuicPacketDirection direction)
    {
        // 抽取段的 IKM 是**客户端报文里的目的连接标识**，方向只决定扩展段用哪个标签（RFC 9001 §5.2）
        const std::span<const std::uint8_t> salt{kQuicInitialSalt};
        const auto initialSecret = runHkdf(EVP_KDF_HKDF_MODE_EXTRACT_ONLY, destinationConnectionId, salt, {},
                                           kQuicInitialSecretByteLength, kShaTwo256Name, "Initial 秘密抽取");

        const std::string_view directionLabel =
                direction == QuicPacketDirection::ClientToServer ? "client in" : "server in";
        const auto trafficSecret = expandLabel(initialSecret, directionLabel, kQuicInitialSecretByteLength, kShaTwo256Name,
                                              std::format("Initial 方向密钥（{}）", directionLabel));
        // Initial 的 AEAD 固定是 AES_128_GCM，与后面协商出的套件无关
        return deriveKeyTriple(QuicCipherSuite::Aes128Gcm, trafficSecret, "Initial");
    }

    QuicPacketKeys deriveQuicUpdatedPacketKeys(const QuicPacketKeys &current)
    {
        const std::size_t secretLength = quicCipherSuiteSecretByteLength(current.cipherSuite);
        const char *const hashName =
                current.cipherSuite == QuicCipherSuite::Aes256Gcm ? kShaTwo384Name : kShaTwo256Name;
        // secret_<n+1> = HKDF-Expand-Label(secret_<n>, "quic ku", "", Hash.length)（RFC 9001 §6.1）
        const auto nextSecret = expandLabel(current.generationSecretBytes(), "quic ku", secretLength, hashName, "密钥更新");
        QuicPacketKeys updated = deriveKeyTriple(current.cipherSuite, nextSecret, "密钥更新");
        // §6.1 明写头部保护密钥不跟着换：换了会对端解不开包头，而相位位正是要写在包头里递过去的
        updated.headerProtectionKey = current.headerProtectionKey;
        return updated;
    }

    QuicPacketKeys deriveQuicPacketKeys(const QuicCipherSuite cipherSuite, const std::span<const std::uint8_t> trafficSecret)
    {
        const std::size_t expectedSecretLength = quicCipherSuiteSecretByteLength(cipherSuite);
        if (trafficSecret.size() != expectedSecretLength)
        {
            throw Base::InvalidArgumentException(std::format("{} 的流量秘密必须是 {} 字节（RFC 9001 §5.1：长度等于套件哈希的输出），"
                                                             "本次是 {} 字节：请确认交过来的是 TLS 该方向的当前写入秘密",
                                                             quicCipherSuiteName(cipherSuite), expectedSecretLength,
                                                             trafficSecret.size()));
        }
        return deriveKeyTriple(cipherSuite, trafficSecret, quicCipherSuiteName(cipherSuite));
    }
} // namespace AsynGyanis::Net
