/**
 * @file QuicPacketBuilder.h
 * @brief QUIC 发包组包（RFC 9001 §5.3/§5.4）：把明文帧封成「密文 + 已加头部保护」的完整报文
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 一个待发包的明文侧描述
     *
     * @details 只放「发包时要落到字节上的东西」。目的连接标识由上层给：服务端回包用的是**对端**
     *          自报的源标识（RFC 9000 §7.2 要求回包打到客户端自报的 SCID 上），填成报文里那个
     *          目的标识会让对端认不出这条连接。
     * @warning 各 span 字段都是视图，本结构活多久它们就要活多久：**不要**写成
     *          `packet.sourceConnectionId = makeBytesFromHex(...)` 这种从临时量取视图的形式，
     *          语句结束即悬空。先把字节落到具名容器里再取视图。
     */
    struct QuicOutboundPacket
    {
        bool isLongHeader{true};                                                  ///< 握手期用长头，1-RTT 用短头
        QuicLongPacketType longPacketType{QuicLongPacketType::Initial};          ///< 仅长头有意义
        std::uint32_t version{kQuicVersion1};                                     ///< 版本；短头线上不携带，本字段被忽略
        std::span<const std::uint8_t> destinationConnectionId{};                  ///< 目的连接标识（对端的标识）
        std::span<const std::uint8_t> sourceConnectionId{};                       ///< 源连接标识（本端签发的）
        std::span<const std::uint8_t> token{};                                    ///< 仅 Initial 写出；服务端回 Initial 时为空
        std::uint64_t packetNumber{0};                                            ///< 完整包号，线格式按 packetNumberByteCount 截断
        std::size_t packetNumberByteCount{1};                                     ///< 包号字段字节数，1 到 4
        bool isKeyPhaseBitSet{false};                                               ///< 短头的 Key Phase 位，与 QuicPacketHeader 同名同义
        std::span<const std::uint8_t> frames{};                                   ///< 明文帧序列，至少一帧（§12.4）
    };

    /**
     * @brief 组一个报文并追写到数据报缓冲末尾
     * @details 依次做：写未保护头部 → 按需要补 PADDING 帧凑够头部保护的取样长度 → AEAD 加密 →
     *          用密文样本算掩码并加头部保护。Length 域在拿到帧之后就能算准，因此不需要回填。
     *
     *          自旋位固定写 0：RFC 9000 §17.4 允许不实现它的端点恒发 0，被动 RTT 测量本端不做。
     * @param datagram 目标数据报缓冲；多个报文可以依次追写（合包，§12.2）
     * @param packet 明文侧描述
     * @param keys 该方向的包保护密钥（AEAD 与头部保护密钥都要有）
     * @throws Base::InvalidArgumentException 用法错误：帧序列为空、包号超 62 位上限、包号字节数不在
     *         1..4、连接标识超过 v1 的 20 字节、长头里给非 Initial 带了 Token、或套件取值未定义
     * @throws Base::Exception 运行期故障：OpenSSL 侧的加密或掩码计算失败
     */
    void appendQuicPacket(std::string &datagram, const QuicOutboundPacket &packet, const QuicPacketKeys &keys);
} // namespace AsynGyanis::Net
