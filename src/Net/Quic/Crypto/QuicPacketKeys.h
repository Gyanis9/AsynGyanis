/**
 * @file QuicPacketKeys.h
 * @brief QUIC 包保护密钥组（RFC 9001 §5.1/§5.3）：套件、AEAD 密钥、IV 与头部保护密钥
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 四种可用 AEAD 的标签长度都是 16 字节（RFC 9001 §5.3：输出比输入大 16 字节）
    inline constexpr std::size_t kQuicAuthenticationTagByteLength = 16;

    /// 四种可用 AEAD 的 nonce/IV 长度都是 12 字节（RFC 9001 §5.1）
    inline constexpr std::size_t kQuicInitializationVectorByteLength = 12;

    /// AES-256 与 ChaCha20 的密钥和头部保护密钥都是 32 字节，是四套件里的最大取值
    inline constexpr std::size_t kQuicMaximumKeyByteLength = 32;

    /// 流量秘密的长度等于套件的哈希长度，SHA-384 是四套件里最长的
    inline constexpr std::size_t kQuicMaximumSecretByteLength = 48;

    /**
     * @brief TLS 协商出的密码套件，决定 AEAD、密钥长度与 HKDF 用的哈希
     *
     * @details RFC 9001 §5.3 允许 [TLS13] 的全部套件（除 TLS_AES_128_CCM_8_SHA256，它没有定义
     *          头部保护方案）。这里刻意**不收** TLS_AES_128_CCM_SHA256：本仓库的 OpenSSL 默认不
     *          协商它，而 CCM 的 EVP 通路要求先报明文与总长度才能取标签，实现路径与另外三个不同。
     *          留着枚举值就等于留一个「声明支持、实际会失败」的状态。
     */
    enum class QuicCipherSuite
    {
        Aes128Gcm,         ///< TLS_AES_128_GCM_SHA256：主线 OpenSSL 的默认档，Initial 一律用它
        Aes256Gcm,         ///< TLS_AES_256_GCM_SHA384
        ChaCha20Poly1305,  ///< TLS_CHACHA20_POLY1305_SHA256
    };

    /**
     * @brief AEAD 密钥长度
     * @param cipherSuite 密码套件
     * @return std::size_t 16 或 32 字节
     */
    [[nodiscard]] std::size_t quicCipherSuiteKeyByteLength(QuicCipherSuite cipherSuite) noexcept;

    /**
     * @brief 头部保护密钥长度（RFC 9001 §5.4.3/§5.4.4）
     * @param cipherSuite 密码套件
     * @return std::size_t AES 系列 16（AES-256-GCM 为 32），ChaCha20 为 32 字节
     */
    [[nodiscard]] std::size_t quicCipherSuiteHeaderProtectionKeyByteLength(QuicCipherSuite cipherSuite) noexcept;

    /**
     * @brief 该套件的流量秘密长度，即 HKDF 所用哈希的输出长度
     * @param cipherSuite 密码套件
     * @return std::size_t SHA-256 系列 32、SHA-384 为 48 字节
     */
    [[nodiscard]] std::size_t quicCipherSuiteSecretByteLength(QuicCipherSuite cipherSuite) noexcept;

    /**
     * @brief 套件的中文名，用于错误文案与日志
     * @param cipherSuite 密码套件
     * @return std::string_view TLS 里的套件名
     */
    [[nodiscard]] std::string_view quicCipherSuiteName(QuicCipherSuite cipherSuite) noexcept;

    /**
     * @brief 一个方向、一个加密级别上的包保护密钥组
     *
     * @details 三段密钥按套件的实际长度使用（数组按最大长度定死，免得每包一次堆分配）。
     *          头部保护密钥在密钥更新后**不变**（RFC 9001 §5.4），所以它和会换的 AEAD 密钥分开存。
     */
    struct QuicPacketKeys
    {
        QuicCipherSuite cipherSuite{QuicCipherSuite::Aes128Gcm};                                    ///< 决定下面三段的实际长度
        std::array<std::uint8_t, kQuicMaximumKeyByteLength> encryptionKey{};                        ///< AEAD 密钥（"quic key"）
        std::array<std::uint8_t, kQuicInitializationVectorByteLength> initializationVector{};        ///< AEAD 的 IV（"quic iv"）
        std::array<std::uint8_t, kQuicMaximumKeyByteLength> headerProtectionKey{};                  ///< 头部保护密钥（"quic hp"）

        /**
         * @brief 按套件取 AEAD 密钥的有效字节
         * @return std::span<const std::uint8_t> 长度 16 或 32
         */
        [[nodiscard]] std::span<const std::uint8_t> encryptionKeyBytes() const noexcept;

        /**
         * @brief 按套件取 IV 的有效字节
         * @return std::span<const std::uint8_t> 恒为 12 字节
         */
        [[nodiscard]] std::span<const std::uint8_t> initializationVectorBytes() const noexcept;

        /**
         * @brief 按套件取头部保护密钥的有效字节
         * @return std::span<const std::uint8_t> 长度 16 或 32
         */
        [[nodiscard]] std::span<const std::uint8_t> headerProtectionKeyBytes() const noexcept;
    };
} // namespace AsynGyanis::Net
