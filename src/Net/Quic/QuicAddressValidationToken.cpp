/**
 * @file QuicAddressValidationToken.cpp
 * @brief 无状态地址验证令牌的签发与核验实现
 * @author Gyanis
 * @date 2026-10-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Quic/QuicAddressValidationToken.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Exception/Exception.h"
#include "Core/Crypto/Digest.h"
#include "Core/Crypto/Random.h"
#include "Core/Socket/InetAddress.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端签的令牌类别。RFC 9000 §8.1.1 要求服务端能分辨令牌来自 Retry 还是 NEW_TOKEN——本实现只签
        /// 前者，其余类别（含对端自造的）一律按不合形状处理，不进核验
        constexpr std::uint8_t kQuicAddressValidationTokenKind = 0x01;

        /// 地址栏与认证码各取 HMAC-SHA-256 输出的前 16 字节：128 位是本层别处（连接标识、无状态重置令牌）
        /// 认定的「难猜」下限，多留只是让令牌在线上更占地方
        constexpr std::size_t kQuicAddressValidationTagByteLength = 16;

        /// 令牌里固定那三段：类别 1 字节、签发秒 4 字节、原始目的标识长度 1 字节
        constexpr std::size_t kQuicAddressValidationHeaderByteLength = 6;

        /// 签发秒的字段下标与长度
        constexpr std::size_t kQuicAddressValidationIssuedSecondOffset = 1;
        constexpr std::size_t kQuicAddressValidationIssuedSecondLength = 4;

        /// 原始目的标识长度那一字节的位置
        constexpr std::size_t kQuicAddressValidationConnectionIdLengthOffset = 5;

        /// v1 的连接标识上限（RFC 9000 §17.1）：令牌封的就是这一条，超出就不是本端签的形状
        constexpr std::size_t kQuicMaximumConnectionIdByteLength = 20;

        /// 密钥长度下限：低于 128 位谈不上「难猜」，与连接标识用的熵下限同一条线
        constexpr std::size_t kQuicMinimumSigningKeyByteLength = 16;

        /// 没配密钥时自造的随机密钥长度
        constexpr std::size_t kQuicRandomSigningKeyByteLength = 32;

        /// 轮换窗口里保留的密钥把数：当前这把 + 上一把（上一把签出的令牌最长活不过有效期）
        constexpr std::size_t kQuicMaximumKeptKeySetCount = 2;

        /// 签发秒取 32 位无符号：按秒够用一万多年，写 64 位则每条令牌白多 4 字节
        constexpr std::uint64_t kQuicIssuedSecondsMask = 0xFFFFFFFFULL;

        /// 派生子密钥用的两段标签（域分离：拿到一类的输出推不出另一类）
        constexpr std::string_view kQuicAddressTagDomain = "asyn-quic-address-validation-address";
        constexpr std::string_view kQuicAuthTagDomain    = "asyn-quic-address-validation-auth";

        void appendBigEndianSeconds(std::string &bytes, const std::uint64_t seconds)
        {
            for (std::size_t shiftByte = kQuicAddressValidationIssuedSecondLength; shiftByte > 0; --shiftByte)
            {
                bytes.push_back(static_cast<char>((seconds >> ((shiftByte - 1U) * 8U)) & 0xFFU));
            }
        }

        [[nodiscard]] std::uint64_t readBigEndianSeconds(std::string_view bytes)
        {
            std::uint64_t value = 0;
            for (const char byte: bytes)
            {
                value = (value << 8U) | static_cast<std::uint8_t>(byte);
            }
            return value;
        }

        /**
         * @brief 把对端地址折成「同一地址在任何收包路径下都逐字节相同」的身份文本
         * @details 不能直接拿 `sockaddr` 那一段字节去算地址栏：IPv4 的结构里有 8 字节填充（sin_zero），
         *          内核填不填它按收包入口不同而不同（批量收包与单条收包实测就不一致），于是同一条连接
         *          的第一包与第二包会算出两个地址栏——令牌就变成偶尔验不过的通行证。走 `InetAddress`
         *          的文本形（含家族、地址与端口）就把这一格定死了。
         */
        [[nodiscard]] std::string peerIdentityOf(const Platform::SocketAddress &peerAddress)
        {
            return Core::InetAddress(peerAddress.storage, peerAddress.length).toString();
        }

        /// 取 HMAC-SHA-256 输出的前 16 字节，落成 owning 字符串（摘要数组是栈上的，视图交出去就悬空）
        [[nodiscard]] std::string hmacTagOf(std::string_view key, std::string_view data)
        {
            const Core::Digest::Sha256Value digest = Core::Digest::hmacSha256(key, data);
            return std::string(reinterpret_cast<const char *>(digest.data()), kQuicAddressValidationTagByteLength);
        }

        [[nodiscard]] std::string_view bytesOf(const std::array<std::uint8_t, Core::Digest::kSha256Length> &value)
        {
            return {reinterpret_cast<const char *>(value.data()), value.size()};
        }

        /// 常数时间比对两段等长字节：这道判定说的是「是不是本端给这个地址签的」，早退造成的耗时差异会
        /// 把密钥位泄露出去（RFC 9000 §10.3.1 对无状态重置令牌提的是同一条要求）
        [[nodiscard]] bool tagsMatch(std::string_view left, std::string_view right)
        {
            return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
        }

        [[nodiscard]] std::uint64_t epochSecondsOf(const std::chrono::system_clock::time_point moment)
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(moment.time_since_epoch()).count()) & kQuicIssuedSecondsMask;
        }
    } // namespace

    QuicAddressValidationTokenCodec::QuicAddressValidationTokenCodec(const std::span<const std::uint8_t> signingKey, const std::chrono::seconds lifetime) : m_lifetime(lifetime)
    {
        if (signingKey.size() < kQuicMinimumSigningKeyByteLength)
        {
            throw Base::InvalidArgumentException(std::format("地址验证令牌的密钥只有 {} 字节，低于下限 {} 字节：短到这个程度，令牌里的认证码就成了"
                                                             "可穷举的东西，地址验证这道防线等于没设（RFC 9000 §8.1.4）",
                                                             signingKey.size(), kQuicMinimumSigningKeyByteLength));
        }
        if (lifetime <= std::chrono::seconds{0})
        {
            throw Base::InvalidArgumentException(std::format("地址验证令牌的有效期配成了 {} 秒：这一项要么不开（配置侧给 0），要么必须是正数——"
                                                             "0 或负数的令牌一签出来就过期，等于每笔握手都白跑一趟",
                                                             lifetime.count()));
        }
        m_keySets.push_back(deriveKeySet(signingKey));
    }

    std::unique_ptr<QuicAddressValidationTokenCodec> QuicAddressValidationTokenCodec::createWithRandomKey(const std::chrono::seconds lifetime)
    {
        // 这把密钥是签名密钥材料的级别，走私有 DRBG 那一侧；取不到没有安全的降级路径：
        // 拿全零密钥签出的令牌谁都验得过，那比不发更糟
        std::vector<std::uint8_t> signingKey;
        try
        {
            signingKey = Core::Random::secretBytes(kQuicRandomSigningKeyByteLength);
        } catch (const Base::Exception &failure)
        {
            LOG_ERROR_FMT("QuicAddressValidationToken: 自造签名密钥失败，Retry 令牌功能未启用。原因：{}", failure.what());
            return nullptr;
        }
        return std::make_unique<QuicAddressValidationTokenCodec>(std::span<const std::uint8_t>(signingKey), lifetime);
    }

    void QuicAddressValidationTokenCodec::rotateSigningKey(const std::span<const std::uint8_t> newSigningKey)
    {
        if (newSigningKey.size() < kQuicMinimumSigningKeyByteLength)
        {
            throw Base::InvalidArgumentException(std::format("轮换用的新密钥只有 {} 字节，低于下限 {} 字节：RFC 9000 §8.1.4 的「难猜」要求不随轮换打折", newSigningKey.size(),
                                                             kQuicMinimumSigningKeyByteLength));
        }
        m_keySets.insert(m_keySets.begin(), deriveKeySet(newSigningKey));
        m_keySets.resize(std::min(m_keySets.size(), kQuicMaximumKeptKeySetCount));
    }

    QuicAddressValidationTokenCodec::KeySet QuicAddressValidationTokenCodec::deriveKeySet(const std::span<const std::uint8_t> signingKey)
    {
        const std::string_view key(reinterpret_cast<const char *>(signingKey.data()), signingKey.size());
        return KeySet{
                .addressKey = Core::Digest::hmacSha256(key, kQuicAddressTagDomain),
                .authKey    = Core::Digest::hmacSha256(key, kQuicAuthTagDomain),
        };
    }

    std::chrono::seconds QuicAddressValidationTokenCodec::lifetime() const noexcept
    {
        return m_lifetime;
    }

    std::vector<std::uint8_t> QuicAddressValidationTokenCodec::issue(const Platform::SocketAddress              &peerAddress,
                                                                     const std::span<const std::uint8_t>         originalDestinationConnectionId,
                                                                     const std::chrono::system_clock::time_point issuedAt) const
    {
        if (originalDestinationConnectionId.size() > kQuicMaximumConnectionIdByteLength)
        {
            throw Base::InvalidArgumentException(std::format("要塞进地址验证令牌的原始目的标识有 {} 字节，超过 v1 连接标识的 {} 字节上限（RFC 9000 §17.1）："
                                                             "这一格只能取客户端 Initial 报文里的目的标识，不要自己造更长的",
                                                             originalDestinationConnectionId.size(), kQuicMaximumConnectionIdByteLength));
        }

        std::string header;
        header.push_back(static_cast<char>(kQuicAddressValidationTokenKind));
        appendBigEndianSeconds(header, epochSecondsOf(issuedAt));
        header.push_back(static_cast<char>(originalDestinationConnectionId.size()));
        header.append(reinterpret_cast<const char *>(originalDestinationConnectionId.data()), originalDestinationConnectionId.size());

        try
        {
            const std::string addressTag = hmacTagOf(bytesOf(m_keySets.front().addressKey), peerIdentityOf(peerAddress));
            const std::string authTag    = hmacTagOf(bytesOf(m_keySets.front().authKey), header + addressTag);
            const std::string token      = header + addressTag + authTag;
            return std::vector<std::uint8_t>(token.begin(), token.end());
        } catch (const Base::Exception &)
        {
            // HMAC 算不出来是运行期故障：交回空令牌，调用方据此**不**回 Retry。核验侧同一处故障按「不合」
            // 处理——最坏的后果是多花一次握手，而不是放行一张没有认证的令牌
            return {};
        }
    }

    std::expected<QuicAddressValidationTokenContent, QuicAddressValidationTokenStatus>
    QuicAddressValidationTokenCodec::verify(const std::span<const std::uint8_t> token, const Platform::SocketAddress &peerAddress,
                                            const std::chrono::system_clock::time_point now) const
    {
        if (token.empty())
        {
            return std::unexpected(QuicAddressValidationTokenStatus::Absent);
        }
        const std::string_view tokenText(reinterpret_cast<const char *>(token.data()), token.size());
        if (tokenText.size() < kQuicAddressValidationHeaderByteLength || tokenText[0] != static_cast<char>(kQuicAddressValidationTokenKind))
        {
            return std::unexpected(QuicAddressValidationTokenStatus::Malformed);
        }

        const std::size_t originalConnectionIdByteCount = static_cast<std::size_t>(static_cast<std::uint8_t>(tokenText[kQuicAddressValidationConnectionIdLengthOffset]));
        const std::size_t expectedTokenByteLength       = kQuicAddressValidationHeaderByteLength + originalConnectionIdByteCount + 2U * kQuicAddressValidationTagByteLength;
        if (originalConnectionIdByteCount > kQuicMaximumConnectionIdByteLength || tokenText.size() != expectedTokenByteLength)
        {
            // 长度对不上就是被截过或拼过：这里不猜对端想表达什么
            return std::unexpected(QuicAddressValidationTokenStatus::Malformed);
        }

        const std::string_view headerPart   = tokenText.substr(0, kQuicAddressValidationHeaderByteLength + originalConnectionIdByteCount);
        const std::string_view addressTag   = tokenText.substr(headerPart.size(), kQuicAddressValidationTagByteLength);
        const std::string_view authTag      = tokenText.substr(headerPart.size() + kQuicAddressValidationTagByteLength, kQuicAddressValidationTagByteLength);
        const std::uint64_t    issuedSecond = readBigEndianSeconds(tokenText.substr(kQuicAddressValidationIssuedSecondOffset, kQuicAddressValidationIssuedSecondLength));

        for (const KeySet &keySet: m_keySets)
        {
            std::string expectedAuth;
            try
            {
                const std::string signedPart = std::string(headerPart) + std::string(addressTag);
                expectedAuth                 = hmacTagOf(bytesOf(keySet.authKey), signedPart);
            } catch (const Base::Exception &)
            {
                return std::unexpected(QuicAddressValidationTokenStatus::IntegrityFailed);
            }
            if (!tagsMatch(expectedAuth, authTag))
            {
                continue;
            }
            // 认证码合上说明这张令牌确实出自本端某把密钥之手，这时才轮到看地址：改得动前缀就过不了上面
            // 这一关，搬运一张别人地址的令牌会在这一格露出来
            const std::string expectedAddressTag = hmacTagOf(bytesOf(keySet.addressKey), peerIdentityOf(peerAddress));
            if (!tagsMatch(expectedAddressTag, addressTag))
            {
                return std::unexpected(QuicAddressValidationTokenStatus::AddressMismatch);
            }
            const std::uint64_t nowSecond = epochSecondsOf(now);
            // 未来时刻一并拒掉：32 位回绕之后的签发秒会读成一个小数，「现在减它」就成了一个巨大的正数，
            // 与其让它落到过期那一格，不如在这里按不合时刻处理
            if (issuedSecond > nowSecond || nowSecond - issuedSecond > static_cast<std::uint64_t>(m_lifetime.count()))
            {
                return std::unexpected(QuicAddressValidationTokenStatus::Expired);
            }
            return QuicAddressValidationTokenContent{
                    .originalDestinationConnectionId =
                            std::vector<std::uint8_t>(token.begin() + static_cast<std::ptrdiff_t>(kQuicAddressValidationHeaderByteLength),
                                                      token.begin() + static_cast<std::ptrdiff_t>(expectedTokenByteLength - 2U * kQuicAddressValidationTagByteLength)),
                    .issuedAge = std::chrono::seconds(nowSecond - issuedSecond),
            };
        }
        return std::unexpected(QuicAddressValidationTokenStatus::IntegrityFailed);
    }
} // namespace AsynGyanis::Net
