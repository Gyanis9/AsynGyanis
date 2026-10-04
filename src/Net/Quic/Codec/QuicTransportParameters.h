/**
 * @file QuicTransportParameters.h
 * @brief QUIC 传输参数的编解码（RFC 9000 §18）：identifier/length/value 三元组序列
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这段字节由 TLS 携带（RFC 9001 §8.2 的 quic_transport_parameters 扩展），本类只管字节，
 *          不碰 TLS 也不碰连接：`QuicTlsContext` 交回来的 `peerTransportParameters()` 直接喂这里。
 *
 * @note 刻意分两层：本文件的解码只做**自洽**的校验（框架、重复、取值长度与范围、角色专属项），
 *       而依赖握手上下文的绑定校验（参数里的连接标识要等于实际收发的那几个，§7.3）留给连接核心。
 * @note `preferred_address`（0x0d）不建模：本实现不做迁移，收到就按 §7.4.2 当作不支持的参数忽略。
 *       但它仍在「服务端专属」名单里，客户端交来照样判错，不会被漏成未知参数。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Codec/QuicDecodeError.h"
#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /// 无状态重置令牌的固定长度（RFC 9000 §10.3：16 字节随机数）。同一件事此前在这里与 QuicFrame.h
    /// 各写一份字面量，两处一起改才会保持一致、单边改则编译期没有任何东西会响——这里取帧编解码那份为真源
    inline constexpr std::size_t kQuicStatelessResetTokenLength = kQuicStatelessResetTokenByteLength;

    /// `max_udp_payload_size` 的默认值：本端愿意收下的最大 UDP 载荷。RFC 9000 §18.2 的下限是 1200，
    /// 上限则由**本端收包缓冲**决定——`Platform::DatagramSocket::kMaximumDatagramBytes`（65535 − 20 − 8
    /// ＝ 65507）。报得比它大是自伤：对端信了这个数并照发，超出部分被 UDP 静默截断，AEAD 解不开就是
    /// 整条连接死掉，现场只留下「对端突然断线」。两处的相等关系由 `TestQuicTransportParameters.cpp` 里一条
    /// static_assert 钉住（测试树同时看得到这两个常量，编解码这层不必依赖 Platform）
    inline constexpr std::uint64_t kQuicDefaultMaximumUdpPayloadSize = 65507;

    /// `ack_delay_exponent` 的默认值；取值上限 20 也是同一节定的
    inline constexpr std::uint64_t kQuicDefaultAcknowledgmentDelayExponent = 3;

    /// `max_ack_delay` 的默认毫秒数；大于等于 2^14 即非法
    inline constexpr std::uint64_t kQuicDefaultMaximumAcknowledgmentDelayMilliseconds = 25;

    /// `active_connection_id_limit` 的默认值，同时是允许的最小值
    inline constexpr std::uint64_t kQuicDefaultActiveConnectionIdLimit = 2;

    /**
     * @brief 配置里给的空闲超时能否编进 `max_idle_timeout`
     * @details 负数不是「小一点」而是**根本没有合法编码**：`std::chrono::milliseconds` 的负值直接
     *          `static_cast<std::uint64_t>` 得到约 1.8×10^19，超过 QUIC 变长整数的上限 2^62-1，
     *          而编码器在为服务端 Initial 写这份参数的那一刻抛异常——那发生在事件循环线程上、
     *          在连接建立的中途，症状是「第一个 QUIC 连接把循环打崩」而不是「配置写错了」。
     *          0 是合法的：RFC 9000 §18.2 明确「值为 0 等价于没设这一项」，即本端不启用空闲超时。
     * @param timeout 配置里的空闲超时
     * @return true 落在可编码域 [0, 2^62-1] 内
     */
    [[nodiscard]] inline constexpr bool isEncodableIdleTimeout(const std::chrono::milliseconds timeout) noexcept
    {
        return timeout.count() >= 0 && static_cast<std::uint64_t>(timeout.count()) <= kQuicMaximumIntegerValue;
    }

    /**
     * @brief 把可编码的空闲超时换成 `max_idle_timeout` 的线上取值（毫秒）
     * @pre isEncodableIdleTimeout(timeout) 为真；不满足时结果无意义（调用方应先判并出声）
     * @param timeout 配置里的空闲超时
     * @return std::uint64_t 传输参数的取值
     */
    [[nodiscard]] inline constexpr std::uint64_t idleTimeoutParameterMilliseconds(const std::chrono::milliseconds timeout) noexcept
    {
        return static_cast<std::uint64_t>(timeout.count());
    }

    /**
     * @brief 这批参数是谁交来的，用来判定「服务端专属参数」是否违规
     *
     * @details 判据是**对端**的角色而不是本端：同一份字节，服务端交来合法的东西客户端交来就是错误
     *          （RFC 9000 §18.2 末段列出四个服务端专属项）。
     */
    enum class QuicTransportParameterSenderRole
    {
        Client, ///< 对端是客户端：不得出现 original_destination_connection_id 等四项
        Server, ///< 对端是服务端：四个专属项都合法
    };

    /**
     * @brief 一对连接的全部传输参数
     *
     * @details 整型成员的初值就是 RFC 9000 §18.2 规定的默认值，缺席即取默认；其余四项（连接标识与
     *          无状态重置令牌）用 `std::optional`：**「没发」与「发了但长度为 0」是两回事**，
     *          §7.3 明确要求选了零长连接标识时仍要带上零长的取值，缺 ISCID/ODCID 才是错误。
     *          成员顺序与参数标识递增一致，编码按此顺序写出，因此同一份参数编出来的字节是确定的。
     */
    struct ASYN_NET_API QuicTransportParameters
    {
        /// 客户端第一个 Initial 的目的连接标识（0x00，仅服务端发）；与实际收到的值的比对归连接核心
        std::optional<std::vector<std::uint8_t>> originalDestinationConnectionId{};

        std::uint64_t                                                           maximumIdleTimeoutMilliseconds{0}; ///< max_idle_timeout（0x01），0 表示不启用
        std::optional<std::array<std::uint8_t, kQuicStatelessResetTokenLength>> statelessResetToken{};             ///< 0x02，仅服务端发
        std::uint64_t                                                           maximumUdpPayloadSize{kQuicDefaultMaximumUdpPayloadSize};             ///< 0x03，小于 1200 非法
        std::uint64_t                                                           initialMaximumData{0};                                                ///< initial_max_data（0x04）
        std::uint64_t                                                           initialMaximumStreamDataBidirectionalLocal{0};                        ///< 0x05，本端发起的双向流
        std::uint64_t                                                           initialMaximumStreamDataBidirectionalRemote{0};                       ///< 0x06，对端发起的双向流
        std::uint64_t                                                           initialMaximumStreamDataUnidirectional{0};                            ///< 0x07，对端发起的单向流
        std::uint64_t                                                           initialMaximumBidirectionalStreams{0};                                ///< 0x08
        std::uint64_t                                                           initialMaximumUnidirectionalStreams{0};                               ///< 0x09
        std::uint64_t                                                           acknowledgmentDelayExponent{kQuicDefaultAcknowledgmentDelayExponent}; ///< 0x0a，大于 20 非法
        std::uint64_t maximumAcknowledgmentDelayMilliseconds{kQuicDefaultMaximumAcknowledgmentDelayMilliseconds};                                     ///< 0x0b
        bool          disableActiveMigration{false};                                ///< 0x0c，零长取值，出现即为真
        std::uint64_t activeConnectionIdLimit{kQuicDefaultActiveConnectionIdLimit}; ///< 0x0e，小于 2 非法

        /// 本端第一个 Initial 的源连接标识（0x0f，两端都必须发）；缺失即 TRANSPORT_PARAMETER_ERROR
        std::optional<std::vector<std::uint8_t>> initialSourceConnectionId{};

        /// Retry 包的源连接标识（0x10，仅服务端发且只在发过 Retry 时出现）
        std::optional<std::vector<std::uint8_t>> retrySourceConnectionId{};

        /// 逐成员比较：往返用例要整份断言，不必把十几个字段各写一行
        [[nodiscard]] friend bool operator==(const QuicTransportParameters &, const QuicTransportParameters &) noexcept = default;
    };

    /**
     * @brief 把传输参数追写成一个字节串（RFC 9000 §18 图 21）
     * @details 整型项一律写出，不按「等于默认值就省略」处理：省不了几个字节（Initial 包本来就要
     *          补到 1200），却会让「编出来的字节」依赖默认值，一改默认值就等于改线上格式。
     * @param bytes 目标缓冲，二进制安全
     * @param parameters 待编码的参数
     * @throws Base::InvalidArgumentException 用法错误：整型项超过变长整数上限，或连接标识超过 20 字节
     */
    ASYN_NET_API void appendQuicTransportParameters(std::string &bytes, const QuicTransportParameters &parameters);

    /**
     * @brief 解出一段传输参数并做完备的自洽校验
     * @details 未识别的标识直接忽略（§7.4.2 的硬要求），形如 `31 * N + 27` 的保留项也一并忽略，
     *          它们存在的意义就是逼实现别把未知参数当错误（§18.1）。
     * @param bytes 参数原文，即 `QuicTlsContext::peerTransportParameters()`
     * @param senderRole 对端的角色，决定哪些专属项算违规
     * @return 成功返回参数；`initial_source_connection_id` 缺失、重复、取值长度或范围不合、
     *         末尾余字节等一律返回 `QuicDecodeError`
     */
    [[nodiscard]] ASYN_NET_API std::expected<QuicTransportParameters, QuicDecodeError> decodeQuicTransportParameters(std::span<const std::uint8_t>    bytes,
                                                                                                                     QuicTransportParameterSenderRole senderRole);
} // namespace AsynGyanis::Net
