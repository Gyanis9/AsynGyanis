/**
 * @file Random.h
 * @brief 密码学随机源：给连接标识、nonce 与密钥材料取字节的唯一入口（OpenSSL RAND 的薄封装）
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AsynGyanis::Core::Random
{
    /**
     * @brief 用公开随机源填满目的缓冲
     * @details 走 `RAND_bytes`：给对端看的、可以公开的随机值用它——QUIC 的连接标识、路径验证数据、
     *          HTTP 掩码帧的密钥、外部服务的 nonce 都算这一类。
     * @param destination 待填满的缓冲；长度为 0 时是空操作（不碰随机源，也不抛）
     * @throws Base::Exception 随机源取不出字节：熵池尚未初始化或底层 DRBG 失败。
     *         抛出时缓冲**保持原样**，不会留下半填的状态——半填的字节看起来像取到了，比取不到更危险
     */
    ASYN_CORE_API void fill(std::span<std::uint8_t> destination);

    /**
     * @brief 用**私有实例**取密钥材料：签名密钥、令牌密钥、无状态重置密钥这类长期秘密
     * @details 走 `RAND_priv_bytes`：OpenSSL 为密钥材料另备一份 DRBG 实例，与公开随机数分流——
     *          一份实例的状态被推断出来时，另一份不受牵连（OpenSSL 手册里 RAND_priv_bytes 的存在理由）。
     *          判据按用途挑：这个字节会不会被拿去当密钥、口令或 HMAC 密钥？会就用本函数。
     * @param destination 待填满的缓冲；长度为 0 时是空操作
     * @throws Base::Exception 同 fill()，且熵池不足时的报错文案会点名「不能用作密钥材料」
     */
    ASYN_CORE_API void fillSecret(std::span<std::uint8_t> destination);

    /**
     * @brief 按长度取一段公开随机字节
     * @details 长度在调用时才知道（连接标识、随机正文）用这条口，省掉调用方自己摆缓冲。
     *          长度为 0 时返回空 vector：本仓几处「随机源取不到就是没连接可建」的写法靠这个返回值判空
     * @param byteLength 需要的字节数
     * @return std::vector<std::uint8_t> 长度等于 byteLength
     * @throws Base::Exception 同 fill()
     */
    [[nodiscard]] ASYN_CORE_API std::vector<std::uint8_t> bytes(std::size_t byteLength);

    /**
     * @brief 按长度取一段密钥材料（私有实例）
     * @param byteLength 需要的字节数；0 返回空 vector
     * @return std::vector<std::uint8_t> 长度等于 byteLength
     * @throws Base::Exception 同 fillSecret()
     */
    [[nodiscard]] ASYN_CORE_API std::vector<std::uint8_t> secretBytes(std::size_t byteLength);
} // namespace AsynGyanis::Core::Random
