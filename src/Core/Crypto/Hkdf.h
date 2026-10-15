/**
 * @file Hkdf.h
 * @brief HKDF（RFC 5869）的薄封装：抽取、扩展与一次做完的派生，走 OpenSSL 的 HKDF 提供方
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/Crypto/Digest.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AsynGyanis::Core::Hkdf
{
    /**
     * @brief RFC 5869 §2.2 的 HKDF-Extract：把输入密钥材料压成一份伪随机密钥
     * @details 盐（salt）是可选的：规范允许不给（此时按全零的哈希长度处理），外部服务规定的派生
     *          常常正好是「无盐」那一种，这里就把空视图当「不给」处理，调用方不必自己拼零缓冲。
     * @param algorithm 哈希算法，决定 PRK 与中间轮的输出长度
     * @param salt 盐；空视图表示不带盐
     * @param inputKeyMaterial 输入密钥材料（按字节取，允许含 NUL，长度不设下限）
     * @return std::vector<std::uint8_t> PRK，长度等于所选哈希的摘要长度
     * @throws Base::Exception OpenSSL 没有可用的 HKDF 提供方、上下文建不起来，或算不出完整输出
     */
    [[nodiscard]] ASYN_CORE_API std::vector<std::uint8_t> extract(Digest::Algorithm algorithm, std::span<const std::uint8_t> salt, std::span<const std::uint8_t> inputKeyMaterial);

    /**
     * @brief RFC 5869 §2.3 的 HKDF-Expand：把伪随机密钥扩展成指定长度的输出
     * @details 长度上限按规范来：`outputByteLength` 不得超过 `(哈希长度 × 255)`，超出是调用方算错了
     *          而不是「多给一点」，因此这里当场拒而不是截断。
     * @param algorithm 哈希算法
     * @param pseudoRandomKey 伪随机密钥（通常是 extract 的产物；也可以直接是共享秘密）
     * @param info 上下文绑定串；空视图表示不带
     * @param outputByteLength 期望的输出长度
     * @return std::vector<std::uint8_t> 长度等于 outputByteLength
     * @throws Base::InvalidArgumentException outputByteLength 超过规范上限，或密钥为空
     * @throws Base::Exception 提供方缺失、上下文建不起来或算不出完整输出
     */
    [[nodiscard]] ASYN_CORE_API std::vector<std::uint8_t> expand(Digest::Algorithm algorithm, std::span<const std::uint8_t> pseudoRandomKey, std::span<const std::uint8_t> info,
                                                                 std::size_t outputByteLength);

    /**
     * @brief 一次做完的 HKDF（RFC 5869 §2：Extract 后接 Expand，盐与上下文一次给齐）
     * @details 多数调用方要的就是这一条：从一份共享秘密直接取定长密钥。需要分开两步时用
     *          extract()/expand()（TLS 1.3 与 QUIC 的密钥套表正是分开的：中间还要插标签）。
     * @param algorithm 哈希算法
     * @param salt 盐；空视图表示不带盐
     * @param inputKeyMaterial 输入密钥材料
     * @param info 上下文绑定串；空视图表示不带
     * @param outputByteLength 期望的输出长度
     * @return std::vector<std::uint8_t> 长度等于 outputByteLength
     * @throws Base::InvalidArgumentException outputByteLength 超过规范上限
     * @throws Base::Exception 同 extract() 与 expand()
     */
    [[nodiscard]] ASYN_CORE_API std::vector<std::uint8_t> derive(Digest::Algorithm algorithm, std::span<const std::uint8_t> salt, std::span<const std::uint8_t> inputKeyMaterial,
                                                                 std::span<const std::uint8_t> info, std::size_t outputByteLength);
} // namespace AsynGyanis::Core::Hkdf
