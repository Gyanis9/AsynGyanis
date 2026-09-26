/**
 * @file QuicPacketProtection.h
 * @brief QUIC 包保护（RFC 9001 §5.3）：AEAD 加密载荷与解密验标签
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details nonce 由 IV 与**完整包号**异或得到（不是线上那个截断值），AAD 是「从头字节到包号末尾」
 *          的未加密头部。两者都只在本层出现一次，写错的代价是「对端全都解不开」。
 */

#pragma once

#include "Net/Quic/Codec/QuicDecodeError.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 加密载荷，把密文与 16 字节标签追写到缓冲末尾
     * @details 明文为空也合法（会产出一个只有标签的密文）；输入按「指针 + 长度」取，可含任意二进制。
     * @param output 目标缓冲，二进制安全
     * @param keys 该方向、该级别的包保护密钥
     * @param packetNumber **完整**包号（未截断的那个），只取低 62 位参与 nonce
     * @param additionalData 报文的未加密头部：从首字节起、含包号字段
     * @param plaintext 帧序列字节
     * @throws Base::Exception 运行期故障：OpenSSL 建不了上下文、拒绝了参数或加密本身失败
     */
    void appendQuicProtectedPayload(std::string &output, const QuicPacketKeys &keys, std::uint64_t packetNumber, std::span<const std::uint8_t> additionalData,
                                    std::span<const std::uint8_t> plaintext);

    /**
     * @brief 解密载荷并校验标签
     * @param plaintextOutput 输出缓冲，长度必须等于 protectedPayload 去掉 16 字节标签后的长度
     * @param keys 该方向、该级别的包保护密钥
     * @param packetNumber 从报文里还原出的**完整**包号
     * @param additionalData 报文的未加密头部：从首字节起、含包号字段
     * @param protectedPayload 密文加标签
     * @return 成功返回写入的明文字节数
     * @return 失败返回 `QuicDecodeError`：长度容不下标签为 `Truncated`，标签校验不过为
     *         `AuthenticationFailed`（调用方整包丢弃即可，别当成对端违规去回错误码）
     * @throws Base::Exception 运行期故障：输出缓冲长度不符、OpenSSL 拒绝了参数
     */
    [[nodiscard]] std::expected<std::size_t, QuicDecodeError> openQuicProtectedPayload(std::span<std::uint8_t> plaintextOutput, const QuicPacketKeys &keys,
                                                                                       std::uint64_t packetNumber, std::span<const std::uint8_t> additionalData,
                                                                                       std::span<const std::uint8_t> protectedPayload);
} // namespace AsynGyanis::Net
