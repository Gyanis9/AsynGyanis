#include "Net/Quic/Crypto/QuicPacketKeys.h"

namespace AsynGyanis::Net
{
    std::size_t quicCipherSuiteKeyByteLength(const QuicCipherSuite cipherSuite) noexcept
    {
        // AES-128-GCM 是 16 字节档，AES-256-GCM 与 ChaCha20 都是 32 字节档
        switch (cipherSuite)
        {
            case QuicCipherSuite::Aes128Gcm:
                return 16;
            case QuicCipherSuite::Aes256Gcm:
            case QuicCipherSuite::ChaCha20Poly1305:
                return 32;
        }
        // 枚举可能被强转成未定义取值：这里不能静默给个长度，让调用方拿到错误的密钥位数
        return 0;
    }

    std::size_t quicCipherSuiteHeaderProtectionKeyByteLength(const QuicCipherSuite cipherSuite) noexcept
    {
        // AES 用与 AEAD 同位数的 ECB（§5.4.3），ChaCha20 恒用 256 位密钥（§5.4.4）
        switch (cipherSuite)
        {
            case QuicCipherSuite::Aes128Gcm:
                return 16;
            case QuicCipherSuite::Aes256Gcm:
            case QuicCipherSuite::ChaCha20Poly1305:
                return 32;
        }
        return 0;
    }

    std::size_t quicCipherSuiteSecretByteLength(const QuicCipherSuite cipherSuite) noexcept
    {
        // 流量秘密长度就是套件哈希的输出：SHA-384 那一档是 48 字节（RFC 9001 §5.1）
        return cipherSuite == QuicCipherSuite::Aes256Gcm ? 48 : 32;
    }

    std::string_view quicCipherSuiteName(const QuicCipherSuite cipherSuite) noexcept
    {
        switch (cipherSuite)
        {
            case QuicCipherSuite::Aes128Gcm:
                return "TLS_AES_128_GCM_SHA256";
            case QuicCipherSuite::Aes256Gcm:
                return "TLS_AES_256_GCM_SHA384";
            case QuicCipherSuite::ChaCha20Poly1305:
                return "TLS_CHACHA20_POLY1305_SHA256";
        }
        return "未定义套件";
    }

    std::span<const std::uint8_t> QuicPacketKeys::encryptionKeyBytes() const noexcept
    {
        return {encryptionKey.data(), quicCipherSuiteKeyByteLength(cipherSuite)};
    }

    std::span<const std::uint8_t> QuicPacketKeys::initializationVectorBytes() const noexcept
    {
        return initializationVector;
    }

    std::span<const std::uint8_t> QuicPacketKeys::headerProtectionKeyBytes() const noexcept
    {
        return {headerProtectionKey.data(), quicCipherSuiteHeaderProtectionKeyByteLength(cipherSuite)};
    }

    std::span<const std::uint8_t> QuicPacketKeys::generationSecretBytes() const noexcept
    {
        return {generationSecret.data(), quicCipherSuiteSecretByteLength(cipherSuite)};
    }
} // namespace AsynGyanis::Net
