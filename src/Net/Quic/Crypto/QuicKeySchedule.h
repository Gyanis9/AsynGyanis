/**
 * @file QuicKeySchedule.h
 * @brief QUIC 密钥表（RFC 9001 §5.1/§5.2、§6.1）：Initial 密钥推导、按流量秘密导出包保护密钥与密钥更新
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AsynGyanis::Net
{
    /// v1 的 Initial 盐，逐字节取自 RFC 9001 §5.2 的 pseudocode（换版本必须换新盐，以防中间盒跨版本解密）
    inline constexpr std::array<std::uint8_t, 20> kQuicInitialSalt{
            0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
            0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};

    /// Initial 一律用 SHA-256 做 HKDF（RFC 9001 §5.2），与协商出的套件无关
    inline constexpr std::size_t kQuicInitialSecretByteLength = 32;

    /**
     * @brief 密钥的作用方向，决定用哪个 HKDF 标签
     *
     * @details 命名按「谁发出」而不是「谁使用」：服务端解客户端报文要的是 `ClientToServer` 那组。
     *          取错方向的后果是握手一条报文都对不上，且症状与密钥本身错一致，难以从表象区分。
     */
    enum class QuicPacketDirection
    {
        ClientToServer, ///< 客户端发出、服务端接收：标签 "client in"
        ServerToClient, ///< 服务端发出、客户端接收：标签 "server in"
    };

    /**
     * @brief 由客户端首个 Initial 的目的连接标识推出 Initial 包保护密钥
     * @details Initial 密钥的输入是**客户端报文里的目的连接标识**（不是本端签发的源标识），
     *          且套件固定为 AEAD_AES_128_GCM；服务端要解客户端的 Initial、以及自己回 Initial
     *          都用同一个目的标识推导，只有方向标签不同（RFC 9001 §5.2）。
     * @param destinationConnectionId 客户端 Initial 的目的连接标识，可为零长（Retry 之后可能为 0）
     * @param direction 密钥作用方向
     * @return QuicPacketKeys Initial 的密钥、IV 与头部保护密钥
     * @throws Base::Exception 运行期故障：OpenSSL 取不到 HKDF 实现或推导失败
     */
    [[nodiscard]] QuicPacketKeys deriveQuicInitialPacketKeys(std::span<const std::uint8_t> destinationConnectionId,
                                                             QuicPacketDirection direction);

    /**
     * @brief 由 TLS 交出的流量秘密导出一组包保护密钥
     * @details 用 "quic key"/"quic iv"/"quic hp" 三个标签、零长 Context、套件的哈希函数
     *          （RFC 9001 §5.1）。Handshake 与 Application 级别都走这里；导出的同时把这一代的
     *          流量秘密留在返回值里，§6.1 的更新要靠它递推。
     * @param cipherSuite 协商出的密码套件
     * @param trafficSecret 该方向的当前流量秘密，长度必须等于套件哈希长度（32 或 48 字节）
     * @return QuicPacketKeys 三件套密钥
     * @throws Base::InvalidArgumentException 用法错误：trafficSecret 长度与套件不符
     * @throws Base::Exception 运行期故障：OpenSSL 取不到 HKDF 实现或推导失败
     */
    [[nodiscard]] QuicPacketKeys deriveQuicPacketKeys(QuicCipherSuite cipherSuite, std::span<const std::uint8_t> trafficSecret);

    /**
     * @brief 从一组 1-RTT 密钥推出下一代（RFC 9001 §6.1）
     * @details 只换 AEAD 密钥与 IV：新一代流量秘密由 "quic ku" 递推而来，头部保护密钥原样带走——
     *          §6.1 明确要求不换，否则对端连携带相位位的包头都解不开。
     * @param current 该方向当前的包保护密钥组，其 generationSecret 必须有效
     * @return QuicPacketKeys 下一代密钥，套件与头部保护密钥与入参相同
     * @throws Base::Exception 运行期故障：OpenSSL 拒绝了对应的 HKDF 参数
     */
    [[nodiscard]] QuicPacketKeys deriveQuicUpdatedPacketKeys(const QuicPacketKeys &current);
} // namespace AsynGyanis::Net
