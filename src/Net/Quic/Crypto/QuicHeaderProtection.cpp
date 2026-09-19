#include "Net/Quic/Crypto/QuicHeaderProtection.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 用 ECB 块密码把一个 16 字节样本加密成掩码（RFC 9001 §5.4.3）
         * @param key 头部保护密钥，长度决定 AES-128 还是 AES-256
         * @param sample 16 字节样本
         * @return std::array<std::uint8_t, 16> 一个块的密文，调用方取前 5 字节
         * @throws Base::Exception 运行期故障：上下文建不起来或加密失败
         */
        std::array<std::uint8_t, kQuicHeaderProtectionSampleByteLength> encryptWithElectronicCodebook(
                const std::span<const std::uint8_t> key, const std::span<const std::uint8_t> sample)
        {
            // 密钥长度就是档位：16 字节 AES-128、32 字节 AES-256，其余取值不可能是本层的密钥
            const EVP_CIPHER *const cipher = key.size() == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb();

            EVP_CIPHER_CTX *const context = EVP_CIPHER_CTX_new();
            if (context == nullptr)
            {
                throw Base::Exception("QUIC 头部保护失败：无法创建 AES-ECB 上下文（OpenSSL 未正确初始化或内存不足）");
            }
            std::array<std::uint8_t, kQuicHeaderProtectionSampleByteLength> output{};
            int outputLength = 0;
            // ECB 且恰好一个整块，关掉填充才不会在 16 字节之外多吐一块
            const bool isSucceeded = EVP_CipherInit_ex(context, cipher, nullptr, key.data(), nullptr, 1) == 1 &&
                                     EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
                                     EVP_CipherUpdate(context, output.data(), &outputLength, sample.data(),
                                                      static_cast<int>(sample.size())) == 1 &&
                                     static_cast<std::size_t>(outputLength) == output.size();
            // 上下文无论成败都要释放：这条路径每包都走，漏一次就是一句一个的泄漏
            EVP_CIPHER_CTX_free(context);
            if (!isSucceeded)
            {
                throw Base::Exception("QUIC 头部保护失败：AES-ECB 未能算出完整的一个密文块（OpenSSL 拒绝了密钥长度或样本长度）");
            }
            return output;
        }

        /**
         * @brief 用 ChaCha20 原始函数把样本编成掩码（RFC 9001 §5.4.4）
         * @details 这里有个可以用上的巧合：§5.4.4 要求「样本前 4 字节当块计数器、后 12 字节当 Nonce」，
         *          而 OpenSSL 的 CHACHA20 密码的 16 字节 IV 正好就是这个布局，所以样本可以直接当 IV
         *          交进去，不必拆开重拼。
         * @param key 32 字节头部保护密钥
         * @param sample 16 字节样本
         * @return std::array<std::uint8_t, kQuicHeaderProtectionMaskByteLength> 5 字节掩码
         * @throws Base::Exception 运行期故障：上下文建不起来或加密失败
         */
        std::array<std::uint8_t, kQuicHeaderProtectionMaskByteLength> encryptZerosWithChaCha20(const std::span<const std::uint8_t> key,
                                                                                              const std::span<const std::uint8_t> sample)
        {
            EVP_CIPHER_CTX *const context = EVP_CIPHER_CTX_new();
            if (context == nullptr)
            {
                throw Base::Exception("QUIC 头部保护失败：无法创建 ChaCha20 上下文（OpenSSL 未正确初始化或内存不足）");
            }
            std::array<std::uint8_t, kQuicHeaderProtectionMaskByteLength> mask{};
            const std::array<std::uint8_t, kQuicHeaderProtectionMaskByteLength> zeros{};
            int outputLength = 0;
            // 对全零明文加密得到的就是密钥流本身，即规范要的那 5 字节掩码
            const bool isSucceeded = EVP_CipherInit_ex(context, EVP_chacha20(), nullptr, key.data(), sample.data(), 1) == 1 &&
                                     EVP_CipherUpdate(context, mask.data(), &outputLength, zeros.data(),
                                                      static_cast<int>(zeros.size())) == 1 &&
                                     static_cast<std::size_t>(outputLength) == mask.size();
            EVP_CIPHER_CTX_free(context);
            if (!isSucceeded)
            {
                throw Base::Exception("QUIC 头部保护失败：ChaCha20 未能算出 5 字节掩码（OpenSSL 拒绝了 32 字节密钥或 16 字节 IV）");
            }
            return mask;
        }

        /**
         * @brief 按首字节取该侧被保护的位数掩码
         * @param isLongHeader 是否长头
         * @return std::uint8_t 长头 0x0F、短头 0x1F（RFC 9001 §5.4.1）
         */
        std::uint8_t maskedBitOf(const bool isLongHeader) noexcept
        {
            return isLongHeader ? kQuicLongHeaderMaskBit : kQuicShortHeaderMaskBit;
        }

        /**
         * @brief 用首字节的包号长度位算出包号字节数，并核对报文字节够不够
         * @param firstByte 已经可信的首字节（去保护后或明文）
         * @param packet 报文字节
         * @param packetNumberOffset 包号字段起点
         * @param operation 面向文案的操作名
         * @return std::expected<std::size_t, QuicDecodeError> 包号字节数，不够则返回截断错误
         */
        std::expected<std::size_t, QuicDecodeError> packetNumberByteCountOf(const std::uint8_t firstByte,
                                                                           const std::span<const std::uint8_t> packet,
                                                                           const std::size_t packetNumberOffset,
                                                                           const std::string_view operation)
        {
            const std::size_t packetNumberByteCount = std::size_t{1} + static_cast<std::size_t>(firstByte & kQuicPacketNumberLengthBitMask);
            if (packetNumberOffset > packet.size() || packet.size() - packetNumberOffset < packetNumberByteCount)
            {
                return std::unexpected(QuicDecodeError{
                        QuicDecodeErrorKind::Truncated,
                        std::format("QUIC {}：包号字段需要 {} 字节，偏移 {} 之后只剩 {} 字节，本包只能整包丢弃",
                                    operation, packetNumberByteCount, packetNumberOffset,
                                    packet.size() > packetNumberOffset ? packet.size() - packetNumberOffset : 0)});
            }
            return packetNumberByteCount;
        }
    } // namespace

    std::span<const std::uint8_t> QuicHeaderProtectionMask::first(const std::size_t byteCount) const noexcept
    {
        return {bytes.data(), std::min(byteCount, bytes.size())};
    }

    std::expected<std::span<const std::uint8_t>, QuicDecodeError>
    extractQuicHeaderProtectionSample(const std::span<const std::uint8_t> packet, const QuicPacketHeader &header)
    {
        // 样本起点按「包号字段最长 4 字节」定，而不是按真实长度：去保护这一侧此时还不知道长度
        const std::size_t sampleOffset = header.packetNumberOffset + 4;
        if (sampleOffset > packet.size() || packet.size() - sampleOffset < kQuicHeaderProtectionSampleByteLength)
        {
            return std::unexpected(QuicDecodeError{
                    QuicDecodeErrorKind::Truncated,
                    std::format("QUIC 取头部保护样本：偏移 {} 处需要 {} 字节样本，报文总共只有 {} 字节（RFC 9001 §5.4.2 要求短包直接丢弃）",
                                sampleOffset, kQuicHeaderProtectionSampleByteLength, packet.size())});
        }
        return packet.subspan(sampleOffset, kQuicHeaderProtectionSampleByteLength);
    }

    QuicHeaderProtectionMask generateQuicHeaderProtectionMask(const QuicPacketKeys &keys, const std::span<const std::uint8_t> sample)
    {
        if (sample.size() != kQuicHeaderProtectionSampleByteLength)
        {
            throw Base::InvalidArgumentException(std::format("头部保护样本必须是 {} 字节，本次是 {} 字节（RFC 9001 §5.4.2 固定取样长度）："
                                                             "请从报文里按「包号起点 + 4」取满再交进来",
                                                             kQuicHeaderProtectionSampleByteLength, sample.size()));
        }

        const std::span<const std::uint8_t> headerProtectionKey = keys.headerProtectionKeyBytes();
        if (headerProtectionKey.empty())
        {
            throw Base::InvalidArgumentException(std::format("套件 {} 的头部保护密钥长度为 0：请先给出有效的密码套件",
                                                             quicCipherSuiteName(keys.cipherSuite)));
        }

        QuicHeaderProtectionMask mask;
        if (keys.cipherSuite == QuicCipherSuite::ChaCha20Poly1305)
        {
            mask.bytes = encryptZerosWithChaCha20(headerProtectionKey, sample);
            return mask;
        }
        const auto block = encryptWithElectronicCodebook(headerProtectionKey, sample);
        std::copy(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(mask.bytes.size()), mask.bytes.begin());
        return mask;
    }

    std::expected<std::uint8_t, QuicDecodeError>
    removeQuicHeaderProtection(std::span<std::uint8_t> packet, const QuicPacketHeader &header, const QuicHeaderProtectionMask &mask)
    {
        if (packet.empty())
        {
            return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Truncated, "QUIC 去头部保护：报文一个字节都没有"});
        }

        // 先用掩码还原首字节，再由还原后的首字节取包号长度：那两位本身也被掩着，
        // 按保护状态下的值取长度会读到错的包号（RFC 9001 §5.4.1 末尾的顺序说明）
        const std::uint8_t unmaskedFirstByte = static_cast<std::uint8_t>(packet[0] ^ (mask.bytes[0] & maskedBitOf(header.isLongHeader)));
        const auto packetNumberByteCount =
                packetNumberByteCountOf(unmaskedFirstByte, packet, header.packetNumberOffset, "去头部保护");
        if (!packetNumberByteCount.has_value())
        {
            return std::unexpected(packetNumberByteCount.error());
        }

        for (std::size_t byteIndex = 0; byteIndex < *packetNumberByteCount; ++byteIndex)
        {
            packet[header.packetNumberOffset + byteIndex] =
                    static_cast<std::uint8_t>(packet[header.packetNumberOffset + byteIndex] ^ mask.bytes[1 + byteIndex]);
        }
        packet[0] = unmaskedFirstByte;
        return unmaskedFirstByte;
    }

    std::expected<std::uint8_t, QuicDecodeError>
    applyQuicHeaderProtection(std::span<std::uint8_t> packet, const QuicPacketHeader &header, const QuicHeaderProtectionMask &mask)
    {
        if (packet.empty())
        {
            return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Truncated, "QUIC 加头部保护：报文一个字节都没有"});
        }

        // 加保护一侧的首字节是调用方写好的明文值，包号长度可以直接信它
        const auto packetNumberByteCount =
                packetNumberByteCountOf(header.firstByte, packet, header.packetNumberOffset, "加头部保护");
        if (!packetNumberByteCount.has_value())
        {
            return std::unexpected(packetNumberByteCount.error());
        }

        for (std::size_t byteIndex = 0; byteIndex < *packetNumberByteCount; ++byteIndex)
        {
            packet[header.packetNumberOffset + byteIndex] =
                    static_cast<std::uint8_t>(packet[header.packetNumberOffset + byteIndex] ^ mask.bytes[1 + byteIndex]);
        }
        const std::uint8_t protectedFirstByte =
                static_cast<std::uint8_t>(header.firstByte ^ (mask.bytes[0] & maskedBitOf(header.isLongHeader)));
        packet[0] = protectedFirstByte;
        return protectedFirstByte;
    }
} // namespace AsynGyanis::Net
