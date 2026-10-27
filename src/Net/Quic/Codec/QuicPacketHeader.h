/**
 * @file QuicPacketHeader.h
 * @brief QUIC 报文头（RFC 9000 §17.2/§17.3）的解码：版本、两条连接标识、Token、包号与整包长度
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Codec/QuicDecodeError.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

    /// 还没探出更大尺寸时的数据报净载荷上限，也是 DPLPMTUD 的 BASE_PLPMTU（RFC 9000 §14.1、§14.3）
    inline constexpr std::size_t kQuicMaximumDatagramPayloadByteLength = 1200;

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
     * @details 分两趟解：首字节的保留位与包号长度受头部保护（RFC 9001 §5.4），拿到密钥去掉保护之前
     *          读到的只是被掩过的值。因此第一趟只填「明文就能确定的字段」（版本、两条连接标识、Token、
     *          Length 域、包号字段起点），`packetNumberByteCount` 与 `packetNumber` 留空；
     *          去掉头部保护后调 `refreshQuicPacketHeader()` 补上这几项。
     *
     * @note 所有连接标识与 Token 都是指向原数据报的视图，不拷贝：调用方必须让数据报活过对这些字段的读取。
     */
    struct ASYN_NET_API QuicPacketHeader
    {
        std::uint8_t                  firstByte{0};                                ///< 首字节原文；去保护后由 refresh 换成掩回来的值
        bool                          isLongHeader{false};                         ///< 长头为 true，短头（1-RTT）为 false
        QuicLongPacketType            longPacketType{QuicLongPacketType::Initial}; ///< 仅长头有意义
        std::uint32_t                 version{0};                                  ///< 版本；短头线上不携带，解码后保持 0，本端要用的是建连接时谈定的那个
        std::span<const std::uint8_t> destinationConnectionId{};                   ///< 目的连接标识（本端签发的，路由键）
        std::span<const std::uint8_t> sourceConnectionId{};                        ///< 源连接标识（对端签发的）
        std::span<const std::uint8_t> token{};                                     ///< Initial 的 Token；其余形态为空
        std::size_t                   packetNumberOffset{0};                       ///< 包号字段的起始偏移
        std::size_t                   packetByteCount{0};                          ///< 本包在数据报里占的总字节数
        std::size_t                   packetNumberAndPayloadByteCount{0};          ///< Length 域的值：包号 + 受保护载荷
        std::size_t                   packetNumberByteCount{0};                    ///< 去头部保护前为 0（那两位不可信）
        std::uint64_t                 packetNumber{0};                             ///< 截断包号，去保护并 refresh 后有效
        bool                          isSpinBitSet{false};                         ///< 短头自旋位，refresh 后才有意义
        bool                          isKeyPhaseBitSet{false};                     ///< 短头密钥相位位，refresh 后才有意义

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

    /// 包号字段的最长编码：1 到 4 字节（RFC 9000 §17.1）
    inline constexpr std::size_t kQuicMaximumPacketNumberByteCount = 4;

    /// Retry 完整性标签的字节数：128 位（RFC 9001 §5.8），落在整条报文的末尾
    inline constexpr std::size_t kQuicRetryIntegrityTagByteLength = 16;

    /**
     * @brief 一条 Retry 报文的明文字段（RFC 9000 §17.2.5）
     *
     * @details 线上只有目的标识、源标识、令牌与末尾的完整性标签；原始目的标识不在线上，它由调用方
     *          按自己发出的第一个 Initial 补进这一格，只用于完整性标签的伪包（RFC 9001 §5.8）。
     *          各 span 都是指向数据报的视图，调用方必须让数据报活过对这些字段的读取。
     */
    struct ASYN_NET_API QuicRetryPacketHeader
    {
        std::uint32_t                 version{0};                        ///< 报文里的版本；本实现只支持 v1
        std::span<const std::uint8_t> destinationConnectionId{};         ///< 线上目的标识：应为客户端 Initial 的源标识
        std::span<const std::uint8_t> sourceConnectionId{};              ///< 线上源标识：客户端此后所有包的目的标识
        std::span<const std::uint8_t> token{};                           ///< 地址验证令牌；零长令牌按 §17.2.5.2 必须丢弃
        std::span<const std::uint8_t> originalDestinationConnectionId{}; ///< 第一个 Initial 的目的标识，不在线上，只进伪包
    };

    /**
     * @brief 解一条 Retry 报文的明文字段（RFC 9000 §17.2.5）
     * @details 与 `decodeQuicPacketHeader` 刻意分开：那个函数是服务端入口，对 Retry 仍按 Malformed
     *          处置（服务端只发不收），这里只给客户端接 Retry 用。只读不验标签——完整性校验要用
     *          客户端自己的原始目的标识拼伪包，见 `verifyQuicRetryPacketIntegrity`。
     * @param datagram 一个完整的数据报，Retry 没有 Length 域，因此它必须独占一条数据报
     * @param originalDestinationConnectionId 客户端第一个 Initial 的目的标识，原样带进结果供校验使用
     * @return 成功返回线上四格与调用方给的原始目的标识
     * @return 失败返回 `QuicDecodeError`：字节不足为 `Truncated`，形态或 v1 字段违规为 `Malformed`
     */
    [[nodiscard]] ASYN_NET_API QuicDecodeExpected<QuicRetryPacketHeader> decodeQuicRetryPacket(std::span<const std::uint8_t> datagram,
                                                                                               std::span<const std::uint8_t> originalDestinationConnectionId);

    /**
     * @brief 按本包包号选出能安全还原的最小包号字段字节数（RFC 9000 §17.1）
     * @details 规范要求包号字段能覆盖「最大已确认包号与本包号之差的两倍」，接收方按半窗口还原
     *          才落不到别的候选值上。这里把本包包号本身当作该差值的保守上界：等价于假设最大已确认
     *          包号为 0，算出来的档位只会偏宽。四档封顶 4 字节，也就是 2^32 的窗口。
     * @param packetNumber 本包的完整包号
     * @return std::size_t 1 到 4：小于 128 取 1、小于 32768 取 2、小于 8388608 取 3，其余取 4
     */
    [[nodiscard]] inline constexpr std::size_t quicPacketNumberByteCountFor(const std::uint64_t packetNumber) noexcept
    {
        for (std::size_t byteCount = 1; byteCount < kQuicMaximumPacketNumberByteCount; ++byteCount)
        {
            if (packetNumber < (1ULL << (byteCount * 8U - 1U)))
            {
                return byteCount;
            }
        }
        return kQuicMaximumPacketNumberByteCount;
    }

    /**
     * @brief 版本协商包里「本端支持的版本」这一列的取值数上限
     * @details 一条数据报净载荷上限 1200 字节，头部固定 14 字节上限（含两条 20 字节标识），
     *          32 位一档远够本实现用；设这一格是为了让编码器的入参校验有可对照的界，
     *          而不是任一条越界的请求把整个数据报撑破
     */
    inline constexpr std::size_t kQuicMaximumVersionNegotiationListLength = 64;

    /**
     * @brief 一条「本端不支持的版本」的长头报文里，回版本协商包要照抄的两条连接标识
     *
     * @details 只取版本协商决策与回包字段所需的部分，不解释类型位之后的 Token/Length/包号：
     *          那些字段的布局按版本各定一套，而本端既然不支持这个版本就没有解它的资格。
     *          RFC 9000 §17.2.1 明写「连接标识的版本特定规则 MUST NOT 影响是否发版本协商包的决定」，
     *          因此这里按各版本共有的前缀读：首字节 + 32 位版本 + 8 位长度 + 标识 + 8 位长度 + 标识，
     *          长度上限取该 8 位字段本身能表达的 255 字节，而不是 v1 的 20 字节。
     * @see readQuicVersionNegotiationRequest
     */
    struct ASYN_NET_API QuicVersionNegotiationRequest
    {
        std::uint32_t                 version{};                 ///< 报文里带的版本号，一定既不是 0 也不是本端支持的版本
        std::span<const std::uint8_t> destinationConnectionId{}; ///< 报文里的目的标识：抄进回包的 Source 字段
        std::span<const std::uint8_t> sourceConnectionId{};      ///< 报文里的源标识：抄进回包的 Destination 字段
    };

    /**
     * @brief 读出一个长头报文，判它是否该被回一个版本协商包
     * @details 三类一律交回空：短头（线上没有版本字段，谈不上「版本不合」）、版本字段为 0 的版本协商报文
     *          （§6.1 的 MUST NOT：不许拿版本协商报文回敬版本协商报文）、以及版本就是本端支持的那一版
     *          （该走正常路由，不是版本协商的对象）。字节不够或标识长度字段越过数据报末尾同样交回空——
     *          那种报文连回敬所需的字段都取不出来。
     * @param datagram 一个完整的 UDP 数据报净字节
     * @return 有值＝该回版本协商包，并给出要照抄的两条连接标识（视图指向入参，入参必须活到用完）
     * @return 空＝不回，按既有口径丢弃或继续处理
     */
    [[nodiscard]] ASYN_NET_API std::optional<QuicVersionNegotiationRequest> readQuicVersionNegotiationRequest(std::span<const std::uint8_t> datagram) noexcept;

    /**
     * @brief 编一个版本协商包（RFC 9000 §17.2.1）
     * @details 布局：首字节（最高位 1 表长头，其余 7 位本实现取 `0xC0`——把 §17.2.1 建议的那一位置 1，
     *          让它看起来带固定位，余下按「任意值」取 0）、32 位版本恒 0、两条长度 + 标识、再跟一列
     *          32 位支持的版本。包里没有包号与 Length 字段，因此一条数据报只装得下它自己，
     *          且对单个数据报最多回一条（§17.2.1 的 MUST NOT）。它不被确认，也不进任何连接状态。
     * @param destinationConnectionId 回包的目的标识，取值为收到报文的源连接标识（§17.2.1 的映射）
     * @param sourceConnectionId 回包的源标识，取值为收到报文的目的连接标识；两条一起回显，
     *        客户端据此判这条回包不是旁观者凭空造的
     * @param supportedVersions 本端支持的版本列表，至少一条
     * @return std::vector<std::uint8_t> 可直接交给 UDP 的整条数据报
     * @throws Base::InvalidArgumentException 用法错误：连接标识超过 255 字节（8 位长度域装不下），
     *         或版本列表为空/超过 kQuicMaximumVersionNegotiationListLength——空的版本列表等于
     *         告诉对端「什么版本都不支持」，那不如不回
     */
    [[nodiscard]] ASYN_NET_API std::vector<std::uint8_t> buildQuicVersionNegotiationPacket(std::span<const std::uint8_t>  destinationConnectionId,
                                                                                           std::span<const std::uint8_t>  sourceConnectionId,
                                                                                           std::span<const std::uint32_t> supportedVersions);


    /**
     * @brief 解出一个报文头的明文部分
     * @details 只做「不依赖密钥就能判」的校验：固定位、v1 的连接标识长度上限、Token 与 Length 是否
     *          越出数据报末尾。版本策略（不支持的版本要回什么、Retry/版本协商怎么处理）不在本层，
     *          本层对 Retry 与版本协商一律报 `Malformed`，由上层按丢弃处理。
     * @param datagram 一个完整的 UDP 数据报净字节，按「指针 + 长度」取
     * @param shortHeaderDestinationConnectionIdLength 短头报文的目的连接标识长度，必须是本端签发
     *        标识的字节数。短头线上不带长度字段（§17.3.1），传大了解进来的包号字节、传小了解出的标识
     *        不完整，两种都让路由表命不中，只能整包丢
     * @return 成功返回只填了明文部分的 `QuicPacketHeader`，包号相关字段要等 refresh
     * @return 失败返回 `QuicDecodeError`：字节不足为 `Truncated`，字段违反 v1 规则为 `Malformed`
     */
    [[nodiscard]] ASYN_NET_API QuicDecodeExpected<QuicPacketHeader> decodeQuicPacketHeader(std::span<const std::uint8_t> datagram,
                                                                                           std::size_t                   shortHeaderDestinationConnectionIdLength);

    /**
     * @brief 去掉头部保护后把首字节换回真值，并补齐包号长度、包号与短头标志位
     * @details 掩出来的首字节会改动保留位与包号长度两位，因此这两趟之间不能使用包号；
     *          本函数同时复核 Length 域与真实包号长度是否自洽。
     * @param header 第一趟解出的报文头，就地更新
     * @param unmaskedFirstByte 去掉头部保护之后的首字节
     * @param datagram 原始数据报，包号字节从中按 `packetNumberOffset` 取
     * @return 成功返回 void
     * @return 失败返回 `QuicDecodeError`：包号字段越出数据报末尾，或 Length 域容不下真实包号长度
     */
    [[nodiscard]] ASYN_NET_API QuicDecodeStatus refreshQuicPacketHeader(QuicPacketHeader &header, std::uint8_t unmaskedFirstByte, std::span<const std::uint8_t> datagram);

    /**
     * @brief 按线格式写出截断包号（RFC 9000 §17.1）
     * @details 包号是定长大端、只保留最低 `packetNumberByteCount` 个字节，不带变长整数的
     *          前缀位——§16 末段把版本、连接标识长度与包号都排除在变长整数编码之外。对端靠
     *          「已收过的最大包号 + 半窗口」把高位补回来（见 `restoreQuicPacketNumber`）。
     * @param bytes 目标缓冲，二进制安全
     * @param packetNumber 完整包号
     * @param packetNumberByteCount 写出的字节数，1 到 4
     * @throws Base::InvalidArgumentException 用法错误：字节数不在 1..4 内
     */
    ASYN_NET_API void appendQuicTruncatedPacketNumber(std::string &bytes, std::uint64_t packetNumber, std::size_t packetNumberByteCount);

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
    [[nodiscard]] ASYN_NET_API std::uint64_t restoreQuicPacketNumber(std::uint64_t largestReceivedPacketNumber, std::uint64_t truncatedPacketNumber,
                                                                     std::size_t packetNumberByteCount);
} // namespace AsynGyanis::Net
