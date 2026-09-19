/**
 * @file QuicPacketHeader.h
 * @brief QUIC 报文头（RFC 9000 §17.2/§17.3）的解码：版本、两条连接标识、Token、包号与整包长度
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Codec/QuicDecodeError.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace AsynGyanis::Net
{
    /// QUIC 版本 1（RFC 9000 §15）：本实现只支持这一版
    inline constexpr std::uint32_t kQuicVersion1 = 0x00000001U;

    /// 版本协商报文的版本字段恒为 0（RFC 9000 §17.2.1）
    inline constexpr std::uint32_t kQuicVersionNegotiationVersion = 0x00000000U;

    /// 首字节最高位：1 为长头，0 为短头（RFC 9000 §17.2/§17.3）
    inline constexpr std::uint8_t kQuicLongHeaderFlagBit = 0x80;

    /// 首字节次高位固定位：v1 下为 0 的报文不是合法报文，必须丢弃（RFC 9000 §17.2/§17.3.1）
    inline constexpr std::uint8_t kQuicFixedBit = 0x40;

    /// 长头的类型位掩码（RFC 9000 §17.2 表 5）
    inline constexpr std::uint8_t kQuicLongPacketTypeBitMask = 0x30;

    /// 短头的自旋位掩码（RFC 9000 §17.4）
    inline constexpr std::uint8_t kQuicSpinBitMask = 0x20;

    /// 短头的密钥相位位掩码（RFC 9000 §17.3.1）
    inline constexpr std::uint8_t kQuicKeyPhaseBitMask = 0x04;

    /// 包号长度位掩码：取值 +1 才是包号字节数（RFC 9000 §17.2/§17.3.1）
    inline constexpr std::uint8_t kQuicPacketNumberLengthBitMask = 0x03;

    /// v1 里连接标识的最大字节数；长头收进长度域、短头靠本端约定（RFC 9000 §5.1.1、§17.2）
    inline constexpr std::size_t kQuicMaximumConnectionIdLength = 20;

    /**
     * @brief 长头的子类型，枚举取值即线上那 2 位的原始值
     *
     * @details 与 RFC 9000 §17.2 表 5 逐位对齐，因此可直接由 `(firstByte & 0x30) >> 4` 得到。
     */
    enum class QuicLongPacketType : std::uint8_t
    {
        Initial   = 0x00, ///< 承载首批握手数据，多出 Token 字段（§17.2.2）
        ZeroRtt   = 0x01, ///< 早数据（本实现不支持，收到即丢弃）
        Handshake = 0x02, ///< 握手后续报文（§17.2.4）
        Retry     = 0x03, ///< 服务端地址验证用，本端作为服务端不会收到（§17.2.5）
    };

    /**
     * @brief 一个报文头的解码结果
     *
     * @details **分两趟解**：首字节的保留位与包号长度受头部保护（RFC 9001 §5.4），拿到密钥去掉保护之前
     *          读到的只是被掩过的值。因此第一趟只填「明文就能确定的字段」（版本、两条连接标识、Token、
     *          Length 域、包号字段起点），`packetNumberByteCount` 与 `packetNumber` 留空；
     *          去掉头部保护后调 `refreshQuicPacketHeader()` 补上这几项。
     *
     * @note 所有连接标识与 Token 都是指向原数据报的视图，不拷贝：调用方必须让数据报活过对这些字段的读取。
     */
    struct QuicPacketHeader
    {
        std::uint8_t firstByte{0};                                          ///< 首字节原文；去保护后由 refresh 换成掩回来的值
        bool isLongHeader{false};                                           ///< 长头为 true，短头（1-RTT）为 false
        QuicLongPacketType longPacketType{QuicLongPacketType::Initial};      ///< 仅长头有意义
        std::uint32_t version{0};                                           ///< 版本；短头线上不携带，解码后保持 0，本端要用的是建连接时谈定的那个
        std::span<const std::uint8_t> destinationConnectionId{};            ///< 目的连接标识（本端签发的，路由键）
        std::span<const std::uint8_t> sourceConnectionId{};                 ///< 源连接标识（对端签发的）
        std::span<const std::uint8_t> token{};                              ///< Initial 的 Token；其余形态为空
        std::size_t packetNumberOffset{0};                                  ///< 包号字段的起始偏移
        std::size_t packetByteCount{0};                                     ///< 本包在数据报里占的总字节数
        std::size_t packetNumberAndPayloadByteCount{0};                     ///< Length 域的值：包号 + 受保护载荷
        std::size_t packetNumberByteCount{0};                               ///< 去头部保护前为 0（那两位不可信）
        std::uint64_t packetNumber{0};                                      ///< 截断包号，去保护并 refresh 后有效
        bool isSpinBitSet{false};                                           ///< 短头自旋位，refresh 后才有意义
        bool isKeyPhaseBitSet{false};                                       ///< 短头密钥相位位，refresh 后才有意义

        /// 长头保留位掩码：Initial/0-RTT/Handshake 的 0x0C（RFC 9000 §17.2）
        static constexpr std::uint8_t kLongHeaderReservedBitMask = 0x0C;

        /// 短头保留位掩码（RFC 9000 §17.3.1）
        static constexpr std::uint8_t kShortHeaderReservedBitMask = 0x18;

        /// 保留位是否为 0：v1 要求保护前置 0，去保护后非 0 即 PROTOCOL_VIOLATION（RFC 9000 §17.2/§17.3.1）
        [[nodiscard]] bool areReservedBitsClear() const noexcept
        {
            const std::uint8_t mask = isLongHeader ? kLongHeaderReservedBitMask : kShortHeaderReservedBitMask;
            return (firstByte & mask) == 0;
        }
    };

    /**
     * @brief 解出一个报文头的明文部分
     * @details 只做「不依赖密钥就能判」的校验：固定位、v1 的连接标识长度上限、Token 与 Length 是否
     *          越出数据报末尾。版本策略（不支持的版本要回什么、Retry/版本协商怎么处理）不在本层，
     *          本层对 Retry 与版本协商一律报 `Malformed`，由上层按丢弃处理。
     * @param datagram 一个完整的 UDP 数据报净字节，按「指针 + 长度」取
     * @param shortHeaderDestinationConnectionIdLength **短头报文的**目的连接标识长度，必须是本端签发
     *        标识的字节数。短头线上不带长度字段（§17.3.1），传大了解进来的包号字节、传小了解出的标识
     *        不完整，两种都让路由表命不中，只能整包丢
     * @return 成功返回只填了明文部分的 `QuicPacketHeader`，包号相关字段要等 refresh
     * @return 失败返回 `QuicDecodeError`：字节不足为 `Truncated`，字段违反 v1 规则为 `Malformed`
     */
    [[nodiscard]] std::expected<QuicPacketHeader, QuicDecodeError>
    decodeQuicPacketHeader(std::span<const std::uint8_t> datagram,
                           std::size_t shortHeaderDestinationConnectionIdLength);

    /**
     * @brief 去掉头部保护后把首字节换回真值，并补齐包号长度、包号与短头标志位
     * @details 掩出来的首字节会改动保留位与包号长度两位，因此这两趟之间**不能**使用包号；
     *          本函数同时复核 Length 域与真实包号长度是否自洽。
     * @param header 第一趟解出的报文头，就地更新
     * @param unmaskedFirstByte 去掉头部保护之后的首字节
     * @param datagram 原始数据报，包号字节从中按 `packetNumberOffset` 取
     * @return 成功返回 void
     * @return 失败返回 `QuicDecodeError`：包号字段越出数据报末尾，或 Length 域容不下真实包号长度
     */
    [[nodiscard]] std::expected<void, QuicDecodeError>
    refreshQuicPacketHeader(QuicPacketHeader &header, std::uint8_t unmaskedFirstByte, std::span<const std::uint8_t> datagram);

    /**
     * @brief 还原截断包号（RFC 9000 §A.3 的 DecodePacketNumber）
     * @details 包号在头上只留最低若干位，要靠「本空间已收过的最大包号」把高位补回来；
     *          判定窗口取 ±半个回绕区间，因此乱序与丢包都能落到唯一的候选值上。
     * @param largestReceivedPacketNumber 本包号空间已认证通过的最大包号
     * @param truncatedPacketNumber 头上解出的截断值
     * @param packetNumberByteCount 头上包号字段的字节数，只能是 1、2、3、4
     * @return std::uint64_t 还原后的完整包号
     * @throws Base::InvalidArgumentException 用法错误：packetNumberByteCount 不在 1..4 内
     */
    [[nodiscard]] std::uint64_t restoreQuicPacketNumber(std::uint64_t largestReceivedPacketNumber, std::uint64_t truncatedPacketNumber,
                                                        std::size_t packetNumberByteCount);
} // namespace AsynGyanis::Net
