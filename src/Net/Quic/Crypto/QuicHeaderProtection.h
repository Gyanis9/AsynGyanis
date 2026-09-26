/**
 * @file QuicHeaderProtection.h
 * @brief QUIC 头部保护（RFC 9001 §5.4）：取样、算掩码、加保护与去保护
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 被保护的是首字节的低位（长头 4 位、短头 5 位）与整个包号字段。去保护时**必须**先从
 *          被掩的首字节算出掩码、还原首字节，再由还原后的首字节取包号长度——包号长度那两位本身
 *          也被掩着，按掩着的值取长度会读到错的包号（§5.4.1 明说加/去保护只在这一处的顺序上不同）。
 */

#pragma once

#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace AsynGyanis::Net
{
    /// 头部保护一律取 16 字节密文做样本（RFC 9001 §5.4.2）
    inline constexpr std::size_t kQuicHeaderProtectionSampleByteLength = 16;

    /// 掩码固定 5 字节：首字节 1 字节 + 最长 4 字节的包号（RFC 9001 §5.4.1）
    inline constexpr std::size_t kQuicHeaderProtectionMaskByteLength = 5;

    /// 长头首字节被保护的是低 4 位（保留位 + 包号长度）
    inline constexpr std::uint8_t kQuicLongHeaderMaskBit = 0x0F;

    /// 短头首字节被保护的是低 5 位（保留位 + 密钥相位 + 包号长度）
    inline constexpr std::uint8_t kQuicShortHeaderMaskBit = 0x1F;

    /**
     * @brief 头部保护掩码
     */
    struct QuicHeaderProtectionMask
    {
        std::array<std::uint8_t, kQuicHeaderProtectionMaskByteLength> bytes{}; ///< 掩码字节，只用前 1+包号长度 个

        /**
         * @brief 取掩码的前若干字节
         * @param byteCount 需要的字节数，不得超过掩码长度
         * @return std::span<const std::uint8_t> 掩码前缀
         */
        [[nodiscard]] std::span<const std::uint8_t> first(std::size_t byteCount) const noexcept;

        [[nodiscard]] bool operator==(const QuicHeaderProtectionMask &) const = default;
    };

    /**
     * @brief 从报文里取头部保护的样本
     * @details 样本起点固定为「包号字段起点 + 4」，与真实包号长度无关——去保护一侧还不知道长度，
     *          所以按包号的最长可能编码（4 字节）留位置（RFC 9001 §5.4.2）。
     * @param packet 报文起始处的字节，至少要有样本末尾
     * @param header 已解出明文部分的报文头，本函数只读它的 packetNumberOffset
     * @return 成功返回 16 字节样本（指向 packet 的视图）
     * @return 失败返回 `QuicDecodeError`：报文短到装不下完整样本，类别为 `Truncated`，
     *         按 §5.4.2 的要求整包丢弃
     */
    [[nodiscard]] std::expected<std::span<const std::uint8_t>, QuicDecodeError> extractQuicHeaderProtectionSample(std::span<const std::uint8_t> packet,
                                                                                                                  const QuicPacketHeader       &header);

    /**
     * @brief 由头部保护密钥与样本算出掩码
     * @details AES 系列用 ECB 加密样本取前 5 字节（§5.4.3）；ChaCha20 直接把样本当「计数器 + Nonce」
     *          加密 5 个零字节（§5.4.4）。
     * @param keys 该方向的包保护密钥，用到其中的头部保护密钥与套件
     * @param sample 16 字节样本
     * @return QuicHeaderProtectionMask 5 字节掩码
     * @throws Base::Exception 运行期故障：OpenSSL 建不了密码上下文或加密失败
     */
    [[nodiscard]] QuicHeaderProtectionMask generateQuicHeaderProtectionMask(const QuicPacketKeys &keys, std::span<const std::uint8_t> sample);

    /**
     * @brief 去掉报文头部保护，并把包号就地还原
     * @details 就地改写 packet 的首字节与包号字段；调用方拿到的首字节要交给
     *          `refreshQuicPacketHeader` 才能补出包号长度与包号——本函数不碰报文头结构，避免
     *          「结构体字段与字节不一致」的中间态。
     * @param packet 可写的报文字节
     * @param header 第一趟解出的报文头，只读 packetNumberOffset 与 isLongHeader
     * @param mask 掩码
     * @return 成功返回去掉保护后的首字节
     * @return 失败返回 `QuicDecodeError`：报文短于包号字段末尾，类别为 `Truncated`
     */
    [[nodiscard]] std::expected<std::uint8_t, QuicDecodeError> removeQuicHeaderProtection(std::span<std::uint8_t> packet, const QuicPacketHeader &header,
                                                                                          const QuicHeaderProtectionMask &mask);

    /**
     * @brief 给明文报文加上头部保护
     * @details 与去保护互为逆运算：先按明文首字节算出包号长度，再掩首字节与包号。
     *          调用方必须已经按 §5.4 的要求把保留位与密钥相位之外该置 0 的位清干净。
     * @param packet 可写的报文字节，内容已是「载荷已加密、头部未保护」的状态
     * @param header 明文报文头，用到 firstByte 与 packetNumberOffset
     * @param mask 掩码
     * @return 成功返回加上保护后的首字节
     * @return 失败返回 `QuicDecodeError`：报文短于包号字段末尾，或首字节的包号长度位越界
     */
    [[nodiscard]] std::expected<std::uint8_t, QuicDecodeError> applyQuicHeaderProtection(std::span<std::uint8_t> packet, const QuicPacketHeader &header,
                                                                                         const QuicHeaderProtectionMask &mask);
} // namespace AsynGyanis::Net
