/**
 * @file QuicFrame.h
 * @brief QUIC 帧（RFC 9000 §19）的表示与编解码：一次解出报文载荷里的完整帧序列
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 帧永远不跨报文（§12.4），载荷去掉保护后就是一段完整内存，因此解码是**一次性批量**
 *          而不是增量状态机——与 Http2FrameDecoder 那种字节流口子刻意不同。
 *          DATAGRAM（RFC 9221）不在本层：本端不通告 max_datagram_frame_size，收到即按未知帧类型
 *          交上层判 PROTOCOL_VIOLATION，正好是 RFC 9221 §2 要求的行为。
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Codec/QuicDecodeError.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace AsynGyanis::Net
{
    /// 无状态重置令牌的固定长度 16 字节（RFC 9000 §19.15 图 39 的 Stateless Reset Token (128)）
    inline constexpr std::size_t kQuicStatelessResetTokenByteLength = 16;

    /// PATH_CHALLENGE / PATH_RESPONSE 的固定数据长度（RFC 9000 §19.17/§19.18）
    inline constexpr std::size_t kQuicPathValidationDataByteLength = 8;

    /// NEW_CONNECTION_ID 里连接标识的最小长度：0 长度标识不允许出现在这一帧里（RFC 9000 §19.15）
    inline constexpr std::size_t kQuicMinimumIssuedConnectionIdLength = 1;

    /// 一条 ACK 最多带几段区间：砍掉尾部老区间不改变「哪些包到了」的结论，却能保证 ACK 帧本身
    /// 不会大到把整包预算吃光。这是本实现自设的保护值，不是规范值
    inline constexpr std::size_t kQuicMaximumAcknowledgementRanges = 32;

    /**
     * @brief 帧类型取值（RFC 9000 §12.4 表 3）
     *
     * @details STREAM 家族的 0x08..0x0f 与 ACK 的 0x02/0x03 由低位决定，不在此逐条列举；
     *          这里只列**独立**的类型值，编码与解码按位算出变体。
     */
    enum class QuicFrameType : std::uint64_t
    {
        Padding              = 0x00, ///< 只填充，不占拥塞窗口也不触发 ACK（§19.1）
        Ping                 = 0x01, ///< 对端必须回 ACK，用于探活（§19.2）
        Acknowledgement      = 0x02, ///< 确认，不带 ECN 计数（§19.3）
        AcknowledgementEcn   = 0x03, ///< 确认，带三个 ECN 计数（§19.3.2）
        ResetStream          = 0x04, ///< 放弃读某条流的剩余数据（§19.4）
        StopSending          = 0x05, ///< 要求对端停止发送某条流（§19.5）
        Crypto               = 0x06, ///< TLS 握手字节，按偏移递交（§19.6）
        NewToken             = 0x07, ///< 地址验证令牌，供下次连接用（§19.7）
        Stream               = 0x08, ///< 流数据的基值，低位是 OFF/LEN/FIN（§19.8）
        MaxData              = 0x10, ///< 连接级接收额度（§19.9）
        MaxStreamData        = 0x11, ///< 流级接收额度（§19.10）
        MaxStreamsBidi       = 0x12, ///< 双向流数量上限（§19.11）
        MaxStreamsUni        = 0x13, ///< 单向流数量上限（§19.11）
        DataBlocked          = 0x14, ///< 卡在连接级额度上（§19.12）
        StreamDataBlocked    = 0x15, ///< 卡在流级额度上（§19.13）
        StreamsBlockedBidi   = 0x16, ///< 卡在双向流数量上（§19.14）
        StreamsBlockedUni    = 0x17, ///< 卡在单向流数量上（§19.14）
        NewConnectionId      = 0x18, ///< 签发新连接标识（§19.15）
        RetireConnectionId   = 0x19, ///< 本端不再用某个连接标识收包（§19.16）
        PathChallenge        = 0x1a, ///< 路径验证探测（§19.17）
        PathResponse         = 0x1b, ///< 路径验证应答（§19.18）
        ConnectionClose      = 0x1c, ///< 传输层收口（含应用错误码 APPLICATION_ERROR）（§19.19）
        ApplicationClose     = 0x1d, ///< 应用层收口（§19.19）
        HandshakeDone        = 0x1e, ///< 握手完成，服务端在确认后首次飞行里发（§19.20）
    };

    /// STREAM 帧类型位的 OFF 位：置位则带 Offset 字段（RFC 9000 §19.8）
    inline constexpr std::uint64_t kQuicStreamFrameOffsetBit = 0x04;

    /// STREAM 帧类型位的 LEN 位：置位则带 Length 字段，否则流数据延伸到载荷末尾（RFC 9000 §19.8）
    inline constexpr std::uint64_t kQuicStreamFrameLengthBit = 0x02;

    /// STREAM 帧类型位的 FIN 位（RFC 9000 §19.8）
    inline constexpr std::uint64_t kQuicStreamFrameFinalBit = 0x01;

    // ============================================================================
    // 各帧的字段结构：只放该帧自己的字段，不留「本类型用不上」的空位
    // ============================================================================

    /// PADDING 帧（§19.1）：无字段，出现即纯填充
    struct QuicPaddingFrame
    {
    };

    /// PING 帧（§19.2）：无字段
    struct QuicPingFrame
    {
    };

    /// HANDSHAKE_DONE 帧（§19.20）：无字段，只允许出现在 1-RTT 包里
    struct QuicHandshakeDoneFrame
    {
    };

    /**
     * @brief 一段连续被确认的包号区间（RFC 9000 §19.3.1 的 Gap + ACK Range Length 对）
     */
    struct QuicAcknowledgementRange
    {
        std::uint64_t smallestAcknowledged{0}; ///< 本区间最小包号（由上一区间的最小值减去 gap 再减一再跳过 gap 个未确认包算出）
        std::uint64_t largestAcknowledged{0};  ///< 本区间最大包号

        [[nodiscard]] bool operator==(const QuicAcknowledgementRange &) const = default;
    };

    /**
     * @brief ACK 帧（§19.3）
     *
     * @details 区间以**绝对包号**存（解码时按 `largest = previous_smallest - gap - 2` 递推，
     *          编码时反推 gap），这样上层丢包检测不必自己重算，也不必区分「首区间用 First ACK Range、
     *          其余用 Gap」这一线上差异。区间必须严格递减且不重叠。
     * @note 不变式：`ranges` 永不为空，且 `ranges.front().largestAcknowledged` 恒等于
     *       `largestAcknowledgedPacketNumber`——线格式的 First ACK Range 描述的就是含最大包号那一段。
     */
    struct QuicAcknowledgementFrame
    {
        std::uint64_t largestAcknowledgedPacketNumber{0};        ///< 被确认的最大包号
        std::uint64_t acknowledgementDelay{0};                   ///< 收到最大包号到发出本帧的延迟，**线上值**：单位是 2^本端 ack_delay_exponent 微秒，本层只搬运不换算（§19.3）
        std::vector<QuicAcknowledgementRange> ranges{};          ///< 被确认的包号区间，按包号递减且不重叠
        bool hasEcnCounts{false};                                ///< 是否带三个 ECN 计数（帧类型为 0x03 时为 true）
        std::array<std::uint64_t, 3> ecnCounts{};                ///< 依次是 ECT(0)、ECT(1)、ECN-CE 的包数
    };

    /// RESET_STREAM 帧（§19.4）：告知对端本端不再发送这条流的剩余数据，收尾长度由本帧给出
    struct QuicResetStreamFrame
    {
        std::uint64_t streamId{0};           ///< 流号
        std::uint64_t applicationErrorCode{0}; ///< 应用错误码
        std::uint64_t finalSize{0};          ///< 本端发送侧的收尾长度，重发时不得改变（§13.3）
    };

    /// STOP_SENDING 帧（§19.5）：要求对端别再往这条流上发
    struct QuicStopSendingFrame
    {
        std::uint64_t streamId{0};             ///< 流号
        std::uint64_t applicationErrorCode{0}; ///< 要求对端在 RESET_STREAM 里带上这个码
    };

    /**
     * @brief CRYPTO 帧（§19.6）
     * @note data 是指向报文载荷的视图，载荷必须活得比本帧久；本层不拷贝握手字节。
     */
    struct QuicCryptoFrame
    {
        std::uint64_t offset{0};              ///< 这段握手字节在加密握手流上的偏移
        std::span<const std::uint8_t> data{}; ///< 握手字节
    };

    /**
     * @brief NEW_TOKEN 帧（§19.7）
     */
    struct QuicNewTokenFrame
    {
        std::span<const std::uint8_t> token{}; ///< 地址验证令牌，语义对本层不透明
    };

    /**
     * @brief 一个 STREAM 帧在某条流上带过的一段字节区间
     *
     * @details 发送侧的记账单位：确认要按它销账，判丢要按它把同一批字节重新排队，所以字段就是
     *          §19.8 里描述「这段字节落在哪」的那几项。定义放在帧这一层，因为恢复层与流层都要用
     *          同一份类型，而恢复层不该反过来依赖流层。
     */
    struct QuicStreamRange
    {
        std::uint64_t streamId{0};    ///< 流号
        std::uint64_t beginOffset{0}; ///< 本段起始偏移
        std::uint64_t endOffset{0};   ///< 结束偏移（不含）
        bool isFinal{false};          ///< 本段是否带着 FIN

        [[nodiscard]] bool operator==(const QuicStreamRange &) const = default;
    };

    /**
     * @brief 一个包里带出的一条流收口宣告（RESET_STREAM 或 STOP_SENDING）
     *
     * @details 记账单位与 `QuicStreamRange` 同类：这两类帧都「发到被确认为止」，所以流层要交出
     *          「本包带了哪几条」，核心据此登记、确认时落定、判丢时补发同一份内容（RFC 9000 §13.3）。
     *          定义同样放在帧这一层，因为恢复层与流层都要用，而恢复层不该反过来依赖流层。
     */
    struct QuicStreamAnnouncement
    {
        std::uint64_t streamId{0}; ///< 流号
        bool isResetStream{false}; ///< true 是 RESET_STREAM（本端不再发），false 是 STOP_SENDING（请对端别再发）

        [[nodiscard]] bool operator==(const QuicStreamAnnouncement &) const = default;
    };

    /**
     * @brief STREAM 帧（§19.8）
     *
     * @details 编码时按「偏移非 0 才带 Offset 字段、Length 字段恒带」产出规范形态，因此
     *          decode→encode 不保证逐字节相同，只保证语义与字段值相同（LEN 不带的写法是同一帧的另一种编码）。
     * @note data 是指向报文载荷的视图，载荷必须活得比本帧久。
     */
    struct QuicStreamFrame
    {
        std::uint64_t streamId{0};            ///< 流号
        std::uint64_t offset{0};              ///< 这段数据在流上的偏移
        std::span<const std::uint8_t> data{}; ///< 流数据；LEN 位为 0 时覆盖到载荷末尾
        bool isFinal{false};                  ///< FIN 位：本段之后流结束
    };

    /// MAX_DATA 帧（§19.9）：连接级的累计接收上限
    struct QuicMaxDataFrame
    {
        std::uint64_t maximumData{0}; ///< 本端在整个连接上愿意接收的最大累计偏移
    };

    /// MAX_STREAM_DATA 帧（§19.10）：单条流的累计接收上限
    struct QuicMaxStreamDataFrame
    {
        std::uint64_t streamId{0};       ///< 流号
        std::uint64_t maximumStreamData{0}; ///< 该流上愿意接收的最大累计偏移
    };

    /**
     * @brief MAX_STREAMS 帧（§19.11）
     */
    struct QuicMaxStreamsFrame
    {
        std::uint64_t maximumStreams{0}; ///< 允许对端发起的流数上限（按单向/双向各自计数）
        bool isUnidirectional{false};    ///< true 对应 0x13（单向），false 对应 0x12（双向）
    };

    /// DATA_BLOCKED 帧（§19.12）：本端被连接级额度卡住
    struct QuicDataBlockedFrame
    {
        std::uint64_t maximumData{0}; ///< 本端当前已达到的上限
    };

    /// STREAM_DATA_BLOCKED 帧（§19.13）：本端被某条流的额度卡住
    struct QuicStreamDataBlockedFrame
    {
        std::uint64_t streamId{0};         ///< 流号
        std::uint64_t streamDataLimit{0};  ///< 该流上已达到的上限
    };

    /// STREAMS_BLOCKED 帧（§19.14）：本端想开新流但数量到顶
    struct QuicStreamsBlockedFrame
    {
        std::uint64_t streamLimit{0};     ///< 已达到的流数上限
        bool isUnidirectional{false};     ///< true 对应 0x17（单向），false 对应 0x16（双向）
    };

    /**
     * @brief NEW_CONNECTION_ID 帧（§19.15）
     * @note connectionId 与 statelessResetToken 是指向载荷的视图，载荷必须活得比本帧久。
     */
    struct QuicNewConnectionIdFrame
    {
        std::uint64_t sequenceNumber{0};    ///< 序号：对端按它退休标识
        std::uint64_t retirePriorTo{0};     ///< 序号小于此值的标识都要退休
        std::span<const std::uint8_t> connectionId{};                  ///< 新标识，长度 1..20
        std::span<const std::uint8_t> statelessResetToken{};           ///< 16 字节无状态重置令牌
    };

    /// RETIRE_CONNECTION_ID 帧（§19.16）：告知对端本端不再用某个标识收包
    struct QuicRetireConnectionIdFrame
    {
        std::uint64_t sequenceNumber{0}; ///< 要退休的标识序号
    };

    /**
     * @brief PATH_CHALLENGE 帧（§19.17）
     */
    struct QuicPathChallengeFrame
    {
        std::array<std::uint8_t, kQuicPathValidationDataByteLength> data{}; ///< 8 字节探测值，原样进 PATH_RESPONSE
    };

    /**
     * @brief PATH_RESPONSE 帧（§19.18）
     */
    struct QuicPathResponseFrame
    {
        std::array<std::uint8_t, kQuicPathValidationDataByteLength> data{}; ///< 必须逐字节回显对应的 PATH_CHALLENGE
    };

    /**
     * @brief CONNECTION_CLOSE 帧（§19.19）
     *
     * @details 两种形态合一个结构：`triggeredFrameType` 有值即 0x1c（传输层收口，字段恒在线上出现，
     *          不知道触发帧时填 0），无值即 0x1d（应用错误码，线上不带该字段）。
     */
    struct QuicConnectionCloseFrame
    {
        std::uint64_t errorCode{0};                        ///< 0x1c 用 §20.1 的传输错误码，0x1d 用应用自定义码
        std::optional<std::uint64_t> triggeredFrameType{};  ///< 触发错误的帧类型；nullopt 表示走 0x1d 形态
        std::span<const std::uint8_t> reasonPhrase{};       ///< 诊断文本，约定 UTF-8 但不校验；可为空
    };

    /**
     * @brief 一帧的联合类型
     *
     * @details 用 variant 而不是「一个大结构 + 类型判别」：每种帧的字段互不相干，摊平会造出一堆
     *          对本类型无意义的空位，上层 switch 完还要各自记住哪几个字段才有效。
     */
    using QuicFrame = std::variant<QuicPaddingFrame, QuicPingFrame, QuicAcknowledgementFrame, QuicResetStreamFrame,
                                   QuicStopSendingFrame, QuicCryptoFrame, QuicNewTokenFrame, QuicStreamFrame, QuicMaxDataFrame,
                                   QuicMaxStreamDataFrame, QuicMaxStreamsFrame, QuicDataBlockedFrame, QuicStreamDataBlockedFrame,
                                   QuicStreamsBlockedFrame, QuicNewConnectionIdFrame, QuicRetireConnectionIdFrame,
                                   QuicPathChallengeFrame, QuicPathResponseFrame, QuicConnectionCloseFrame, QuicHandshakeDoneFrame>;

    /**
     * @brief 取帧的类型值（含 STREAM 家族与 ACK 的低位变体）
     * @param frame 帧
     * @return std::uint64_t RFC 9000 §12.4 表 3 的取值，可直接填进 CONNECTION_CLOSE 的 Frame Type 字段
     */
    [[nodiscard]] std::uint64_t quicFrameTypeValue(const QuicFrame &frame) noexcept;

    /**
     * @brief 解出报文载荷里的全部帧
     * @details 载荷是**去掉包保护之后**的净字节；帧不跨报文，因此一次交完整段。
     *          类型字段要求最短编码（§16 里唯一的例外就是帧类型），其余变长整数不要求。
     * @param payload 一个报文的净载荷，按「指针 + 长度」取
     * @return 成功返回按出现顺序排列的帧序列；空载荷返回空序列，是否判 PROTOCOL_VIOLATION 由上层定
     * @return 失败返回 `QuicDecodeError`：字段越出载荷末尾为 `Truncated`，类型未定义、区间不自洽、
     *         长度违规等为 `Malformed`
     * @note 返回的帧里 CRYPTO/STREAM/NEW_TOKEN/NEW_CONNECTION_ID 等持有指向 payload 的视图，
     *       payload 必须活到这些帧用完为止
     */
    [[nodiscard]] std::expected<std::vector<QuicFrame>, QuicDecodeError>
    decodeQuicFrames(std::span<const std::uint8_t> payload);

    /**
     * @brief 把一帧追写到缓冲末尾
     * @details 变长整数按最少字节数编码；STREAM 恒带 Length 字段，偏移为 0 时省掉 Offset 字段。
     * @param bytes 目标缓冲，二进制安全
     * @param frame 帧
     * @throws Base::InvalidArgumentException 用法错误：字段值超出 62 位上限、ACK 区间不自洽、
     *         连接标识或无状态重置令牌长度不符、ACK 的 ECN 计数标记与取值不一致
     */
    void appendQuicFrame(std::string &bytes, const QuicFrame &frame);

    /**
     * @brief 把已收到的包号集合折成 ACK 区间
     * @details §19.3.1 要求区间按包号递减、互不重叠，相邻的会被合并成一段；`QuicAcknowledgementRange`
     *          存绝对包号，递推出来的 gap 由帧编码器负责。超过 `kQuicMaximumAcknowledgementRanges`
     *          段的老区间直接砍掉。
     * @param receivedPacketNumbers 升序的已收包号
     * @param acknowledgedUpTo 本次确认到的包号（含），即 ACK 帧的最大确认值
     * @return std::vector<QuicAcknowledgementRange> 递减的区间，至少一段且首段含 `acknowledgedUpTo`
     */
    [[nodiscard]] std::vector<QuicAcknowledgementRange>
    buildQuicAcknowledgementRanges(const std::set<std::uint64_t> &receivedPacketNumbers, std::uint64_t acknowledgedUpTo);
} // namespace AsynGyanis::Net
