#include "Net/Quic/Crypto/QuicKeySchedule.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Crypto/Digest.h"
#include "Core/Crypto/Hkdf.h"

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

        /// 套件到哈希的对应：RFC 9001 §5.1 只有 AES-256-GCM 配 SHA-384，其余一律 SHA-256
        constexpr Core::Digest::Algorithm digestForSuite(const QuicCipherSuite cipherSuite) noexcept
        {
            return cipherSuite == QuicCipherSuite::Aes256Gcm ? Core::Digest::Algorithm::Sha384 : Core::Digest::Algorithm::Sha256;
        }

        /**
         * @brief 把 Core::Hkdf 的失败换成带「哪一步」的文案
         * @details KDF 本身只知道自己在做抽取或扩展，而排查密钥套表时要看出是 Initial 秘密、
         *          还是某个方向的密钥更新——那一步的名字只有调用方知道，所以在这一处补上再抛出
         * @param operation 面向文案的操作名
         * @param body 真正的派生动作
         * @return std::vector<std::uint8_t> 派生结果
         */
        template<typename Body>
        std::vector<std::uint8_t> withOperation(const std::string_view operation, Body &&body)
        {
            try
            {
                return body();
            } catch (const Base::Exception &failure)
            {
                throw Base::Exception(std::format("QUIC {} 失败：{}", operation, failure.what()));
            }
        }

        /**
         * @brief 跑一次 HKDF-Extract（RFC 5869 §2.2），走 Core::Hkdf
         * @param salt 抽取段的盐
         * @param inputKeyMaterial 输入密钥材料
         * @param algorithm 哈希算法
         * @param operation 面向文案的操作名
         * @return std::vector<std::uint8_t> PRK，长度等于所选哈希的摘要长度
         */
        std::vector<std::uint8_t> extract(const std::span<const std::uint8_t> salt, const std::span<const std::uint8_t> inputKeyMaterial, const Core::Digest::Algorithm algorithm,
                                          const std::string_view operation)
        {
            return withOperation(operation, [&] { return Core::Hkdf::extract(algorithm, salt, inputKeyMaterial); });
        }

        /**
         * @brief 按 RFC 8446 §7.1 拼出 HkdfLabel 并做 HKDF-Expand
         * @details 字段依次是 uint16 输出长度、uint8(「tls13 」+标签的总长)、该拼接标签、uint8 上下文长、上下文。
         *          QUIC 的所有用法上下文都是零长（RFC 9001 §5.1），但长度字节仍要写。
         * @param secret 输入秘密
         * @param label 标签原文，不含前缀
         * @param outputLength 需要的输出字节数
         * @param algorithm 哈希算法
         * @param operation 面向文案的操作名
         * @return std::vector<std::uint8_t> 输出
         * @throws Base::InvalidArgumentException 用法错误：标签或上下文长到 uint8 表达不下
         */
        std::vector<std::uint8_t> expandLabel(const std::span<const std::uint8_t> secret, const std::string_view label, const std::size_t outputLength,
                                              const Core::Digest::Algorithm algorithm, const std::string_view operation)
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

            return withOperation(operation, [&] { return Core::Hkdf::expand(algorithm, secret, encodedLabel, outputLength); });
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
                throw Base::Exception(std::format("QUIC 密钥导出异常：{} 需要 {} 字节却拿到 {} 字节：请核对哈希与套件的搭配", what, destination.size(), source.size()));
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
        QuicPacketKeys deriveKeyTriple(const QuicCipherSuite cipherSuite, const std::span<const std::uint8_t> trafficSecret, const std::string_view operation)
        {
            const Core::Digest::Algorithm algorithm = digestForSuite(cipherSuite);

            QuicPacketKeys keys;
            keys.cipherSuite = cipherSuite;
            // 留住这一代的流量秘密：§6.1 的密钥更新是在它上面再递推一步，拿不到秘密就没法往前推
            std::copy(trafficSecret.begin(), trafficSecret.end(), keys.generationSecret.begin());
            // 三个标签逐字取自 RFC 9001 §5.1；IV 长度取 AEAD nonce 的最小长度 12（§5.1 末段）。
            // 目标区间要先按套件的实际长度截好：数组是 32 字节的公共容器，拿整个数组比长度会把
            // AES-128 这类短密钥一律判成长度不符
            const auto encryptionKey = expandLabel(trafficSecret, "quic key", quicCipherSuiteKeyByteLength(cipherSuite), algorithm, std::format("{}：AEAD 密钥", operation));
            copyInto(std::span(keys.encryptionKey).first(quicCipherSuiteKeyByteLength(cipherSuite)), encryptionKey, "AEAD 密钥");

            const auto initializationVector = expandLabel(trafficSecret, "quic iv", kQuicInitializationVectorByteLength, algorithm, std::format("{}：初始化向量", operation));
            copyInto(keys.initializationVector, initializationVector, "初始化向量");

            const auto headerProtectionKey =
                    expandLabel(trafficSecret, "quic hp", quicCipherSuiteHeaderProtectionKeyByteLength(cipherSuite), algorithm, std::format("{}：头部保护密钥", operation));
            copyInto(std::span(keys.headerProtectionKey).first(quicCipherSuiteHeaderProtectionKeyByteLength(cipherSuite)), headerProtectionKey, "头部保护密钥");
            return keys;
        }
    } // namespace

    QuicPacketKeys deriveQuicInitialPacketKeys(const std::span<const std::uint8_t> destinationConnectionId, const QuicPacketDirection direction)
    {
        // 抽取段的 IKM 是**客户端报文里的目的连接标识**，方向只决定扩展段用哪个标签（RFC 9001 §5.2）
        const std::span<const std::uint8_t> salt{kQuicInitialSalt};
        // Initial 一族的哈希恒为 SHA-256（RFC 9001 §5.2），与后面协商出的套件无关
        const auto initialSecret = extract(salt, destinationConnectionId, Core::Digest::Algorithm::Sha256, "Initial 秘密抽取");

        const std::string_view directionLabel = direction == QuicPacketDirection::ClientToServer ? "client in" : "server in";
        const auto             trafficSecret =
                expandLabel(initialSecret, directionLabel, kQuicInitialSecretByteLength, Core::Digest::Algorithm::Sha256, std::format("Initial 方向密钥（{}）", directionLabel));
        // Initial 的 AEAD 固定是 AES_128_GCM，与后面协商出的套件无关
        return deriveKeyTriple(QuicCipherSuite::Aes128Gcm, trafficSecret, "Initial");
    }

    QuicPacketKeys deriveQuicUpdatedPacketKeys(const QuicPacketKeys &current)
    {
        const std::size_t             secretLength = quicCipherSuiteSecretByteLength(current.cipherSuite);
        const Core::Digest::Algorithm algorithm    = digestForSuite(current.cipherSuite);
        // secret_<n+1> = HKDF-Expand-Label(secret_<n>, "quic ku", "", Hash.length)（RFC 9001 §6.1）
        const auto     nextSecret = expandLabel(current.generationSecretBytes(), "quic ku", secretLength, algorithm, "密钥更新");
        QuicPacketKeys updated    = deriveKeyTriple(current.cipherSuite, nextSecret, "密钥更新");
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
                                                             quicCipherSuiteName(cipherSuite), expectedSecretLength, trafficSecret.size()));
        }
        return deriveKeyTriple(cipherSuite, trafficSecret, quicCipherSuiteName(cipherSuite));
    }
} // namespace AsynGyanis::Net
