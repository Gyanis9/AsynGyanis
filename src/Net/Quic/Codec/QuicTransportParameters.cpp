#include "Net/Quic/Codec/QuicTransportParameters.h"

#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicRawBytes.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        // 参数标识（RFC 9000 §18.2 与 RFC 9001 §7.3）。0x0d 的 preferred_address 只出现在
        // 「服务端专属」名单里：本实现不做迁移，它的字节按 §7.4.2 当不支持的参数忽略。
        constexpr std::uint64_t kIdOriginalDestinationConnectionId             = 0x00;
        constexpr std::uint64_t kIdMaximumIdleTimeout                          = 0x01;
        constexpr std::uint64_t kIdStatelessResetToken                         = 0x02;
        constexpr std::uint64_t kIdMaximumUdpPayloadSize                       = 0x03;
        constexpr std::uint64_t kIdInitialMaximumData                          = 0x04;
        constexpr std::uint64_t kIdInitialMaximumStreamDataBidirectionalLocal  = 0x05;
        constexpr std::uint64_t kIdInitialMaximumStreamDataBidirectionalRemote = 0x06;
        constexpr std::uint64_t kIdInitialMaximumStreamDataUnidirectional      = 0x07;
        constexpr std::uint64_t kIdInitialMaximumBidirectionalStreams          = 0x08;
        constexpr std::uint64_t kIdInitialMaximumUnidirectionalStreams         = 0x09;
        constexpr std::uint64_t kIdAcknowledgmentDelayExponent                 = 0x0a;
        constexpr std::uint64_t kIdMaximumAcknowledgmentDelay                  = 0x0b;
        constexpr std::uint64_t kIdDisableActiveMigration                      = 0x0c;
        constexpr std::uint64_t kIdPreferredAddress                            = 0x0d;
        constexpr std::uint64_t kIdActiveConnectionIdLimit                     = 0x0e;
        constexpr std::uint64_t kIdInitialSourceConnectionId                   = 0x0f;
        constexpr std::uint64_t kIdRetrySourceConnectionId                     = 0x10;

        /// `max_ack_delay` 的上限：2^14 及以上非法（RFC 9000 §18.2）
        constexpr std::uint64_t kQuicMaximumAcknowledgmentDelayBound = 1ULL << 14;

        /**
         * @brief 只允许服务端发的四项（RFC 9000 §18.2 末段：客户端带了任一项，服务端按 TRANSPORT_PARAMETER_ERROR 处理）
         * @param identifier 参数标识
         * @return true 该标识属于服务端专属
         */
        bool isServerOnlyIdentifier(const std::uint64_t identifier) noexcept
        {
            return identifier == kIdOriginalDestinationConnectionId || identifier == kIdStatelessResetToken || identifier == kIdPreferredAddress ||
                   identifier == kIdRetrySourceConnectionId;
        }

        /// 整型参数的「标识 + 名字 + 成员 + 合法区间」一张表：11 项各写一遍 switch 分支是 100 行重复
        struct IntegerParameterBinding
        {
            std::uint64_t    identifier;
            std::string_view name;
            std::uint64_t QuicTransportParameters::*member;
            std::uint64_t                           minimumValue;
            std::uint64_t                           maximumValue;
        };

        constexpr IntegerParameterBinding kIntegerParameters[] = {
                {kIdMaximumIdleTimeout, "max_idle_timeout", &QuicTransportParameters::maximumIdleTimeoutMilliseconds, 0, kQuicMaximumIntegerValue},
                {kIdMaximumUdpPayloadSize, "max_udp_payload_size", &QuicTransportParameters::maximumUdpPayloadSize, 1200, kQuicMaximumIntegerValue},
                {kIdInitialMaximumData, "initial_max_data", &QuicTransportParameters::initialMaximumData, 0, kQuicMaximumIntegerValue},
                {kIdInitialMaximumStreamDataBidirectionalLocal, "initial_max_stream_data_bidi_local", &QuicTransportParameters::initialMaximumStreamDataBidirectionalLocal, 0,
                 kQuicMaximumIntegerValue},
                {kIdInitialMaximumStreamDataBidirectionalRemote, "initial_max_stream_data_bidi_remote", &QuicTransportParameters::initialMaximumStreamDataBidirectionalRemote, 0,
                 kQuicMaximumIntegerValue},
                {kIdInitialMaximumStreamDataUnidirectional, "initial_max_stream_data_uni", &QuicTransportParameters::initialMaximumStreamDataUnidirectional, 0,
                 kQuicMaximumIntegerValue},
                {kIdInitialMaximumBidirectionalStreams, "initial_max_streams_bidi", &QuicTransportParameters::initialMaximumBidirectionalStreams, 0, kQuicMaximumStreamLimitValue},
                {kIdInitialMaximumUnidirectionalStreams, "initial_max_streams_uni", &QuicTransportParameters::initialMaximumUnidirectionalStreams, 0, kQuicMaximumStreamLimitValue},
                {kIdAcknowledgmentDelayExponent, "ack_delay_exponent", &QuicTransportParameters::acknowledgmentDelayExponent, 0, 20},
                {kIdMaximumAcknowledgmentDelay, "max_ack_delay", &QuicTransportParameters::maximumAcknowledgmentDelayMilliseconds, 0, kQuicMaximumAcknowledgmentDelayBound - 1},
                {kIdActiveConnectionIdLimit, "active_connection_id_limit", &QuicTransportParameters::activeConnectionIdLimit, 2, kQuicMaximumIntegerValue},
        };

        /// 三个连接标识参数同型（标识 + 名字 + 成员），一并列表
        struct ConnectionIdParameterBinding
        {
            std::uint64_t                            identifier;
            std::string_view                         name;
            std::optional<std::vector<std::uint8_t>> QuicTransportParameters::*member;
        };

        constexpr ConnectionIdParameterBinding kConnectionIdParameters[] = {
                {kIdOriginalDestinationConnectionId, "original_destination_connection_id", &QuicTransportParameters::originalDestinationConnectionId},
                {kIdInitialSourceConnectionId, "initial_source_connection_id", &QuicTransportParameters::initialSourceConnectionId},
                {kIdRetrySourceConnectionId, "retry_source_connection_id", &QuicTransportParameters::retrySourceConnectionId},
        };

        /**
         * @brief 按 §18 图 21 写一项：标识 + 取值长度 + 取值
         * @param bytes 目标缓冲
         * @param identifier 参数标识
         * @param value 取值字节，长度域由它算出
         */
        void appendParameter(std::string &bytes, const std::uint64_t identifier, const std::span<const std::uint8_t> value)
        {
            appendQuicVariableLengthInteger(bytes, identifier);
            appendQuicVariableLengthInteger(bytes, value.size());
            appendQuicRawBytes(bytes, value);
        }

        /**
         * @brief 按 §18 图 21 写一项，取值已经是字节文本的形式
         * @details 与 span 版并列存在只为这一处：整型项先经 `appendQuicVariableLengthInteger` 编成
         *          `std::string`，再交过来包长度域，省掉一次 reinterpret 的窄门。
         * @param bytes 目标缓冲
         * @param identifier 参数标识
         * @param value 取值，长度域由它的字节数算出
         */
        void appendParameter(std::string &bytes, const std::uint64_t identifier, const std::string_view value)
        {
            appendQuicVariableLengthInteger(bytes, identifier);
            appendQuicVariableLengthInteger(bytes, value.size());
            bytes.append(value);
        }

        /**
         * @brief 写一个整型参数：先把值编成变长整数，再按它的实际字节数包一层长度域
         * @param bytes 目标缓冲
         * @param identifier 参数标识
         * @param value 整型取值
         */
        void appendIntegerParameter(std::string &bytes, const std::uint64_t identifier, const std::uint64_t value)
        {
            std::string encodedValue;
            appendQuicVariableLengthInteger(encodedValue, value);
            appendParameter(bytes, identifier, encodedValue);
        }

        /**
         * @brief 参数序列的读位置游标
         * @details 每个读法都自带「还剩不够就不往前走」的判断，失败文案带上读到的是哪一项：
         *          一段几十参数的字节解到一半失败，只有字段名能定位是谁的长度域写错。
         */
        class ParameterReader
        {
        public:
            /// @param bytes 参数原文
            explicit ParameterReader(const std::span<const std::uint8_t> bytes) : m_bytes(bytes)
            {
            }

            /// 是否已经读到末尾
            [[nodiscard]] bool atEnd() const noexcept
            {
                return m_offset >= m_bytes.size();
            }

            /**
             * @brief 读一个变长整数
             * @param fieldName 进入失败文案的字段名
             * @return 成功返回数值，失败返回截断错误
             */
            [[nodiscard]] std::expected<std::uint64_t, QuicDecodeError> readInteger(const std::string_view fieldName)
            {
                const auto decoded = decodeQuicVariableLengthInteger(m_bytes.subspan(m_offset));
                if (!decoded.has_value())
                {
                    return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Truncated, std::format("传输参数的{}读不完一个变长整数：{}", fieldName, decoded.error().message)});
                }
                m_offset += decoded->byteCount;
                return decoded->value;
            }

            /**
             * @brief 读定长的一段字节
             * @param length 声明要读的字节数
             * @param fieldName 进入失败文案的字段名
             * @return 成功返回指向原文的视图，失败返回截断错误
             */
            [[nodiscard]] std::expected<std::span<const std::uint8_t>, QuicDecodeError> readBytes(const std::size_t length, const std::string_view fieldName)
            {
                if (length > m_bytes.size() - m_offset)
                {
                    return std::unexpected(
                            QuicDecodeError{QuicDecodeErrorKind::Truncated, std::format("传输参数{}声明 {} 字节取值，只剩 {} 字节", fieldName, length, m_bytes.size() - m_offset)});
                }
                const std::span<const std::uint8_t> bytes = m_bytes.subspan(m_offset, length);
                m_offset += length;
                return bytes;
            }

        private:
            std::span<const std::uint8_t> m_bytes;     ///< 参数原文
            std::size_t                   m_offset{0}; ///< 读位置
        };

        /**
         * @brief 整型参数的取值必须是「恰好一个变长整数」
         * @details 允许非最短编码（§16 只对帧类型要求最短，长度域与取值都允许宽写），但不容许取值
         *          里再藏字段：长度域 6、整数本身占 4，剩下 2 字节没有任何定义能解释它。
         * @param value 该项的取值字节
         * @param name 参数名，进入失败文案
         * @return 成功返回数值，失败返回截断或格式错误
         */
        std::expected<std::uint64_t, QuicDecodeError> readIntegerValue(const std::span<const std::uint8_t> value, const std::string_view name)
        {
            const auto decoded = decodeQuicVariableLengthInteger(value);
            if (!decoded.has_value())
            {
                return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Truncated, std::format("传输参数 {} 的取值读不出一个完整整数：{}", name, decoded.error().message)});
            }
            if (decoded->byteCount != value.size())
            {
                return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed,
                                                       std::format("传输参数 {} 的长度域是 {} 字节，整数值只占 {} 字节（RFC 9000 §18.2 里整数项的取值就是一个变长整数）", name,
                                                                   value.size(), decoded->byteCount)});
            }
            return decoded->value;
        }

        /**
         * @brief 连接标识类参数的长度把关
         * @param value 该项的取值字节
         * @param name 参数名
         * @return 通过返回它自己（零长合法，见 §7.3 末段），超上限返回格式错误
         */
        std::expected<std::vector<std::uint8_t>, QuicDecodeError> readConnectionIdValue(const std::span<const std::uint8_t> value, const std::string_view name)
        {
            if (value.size() > kQuicMaximumConnectionIdLength)
            {
                return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed, std::format("传输参数 {} 的连接标识有 {} 字节，超过 v1 上限 {} 字节（RFC 9000 §5.1）", name,
                                                                                                   value.size(), kQuicMaximumConnectionIdLength)});
            }
            return std::vector<std::uint8_t>(value.begin(), value.end());
        }

        /**
         * @brief 已知参数的重复检查
         * @details 用位掩码而不是集合：已知的标识都在 0x00..0x10，且这里只在已知分支里调用。
         * @param seenMask 已见过的标识位图
         * @param identifier 本项标识
         * @param name 参数名
         * @return 通过返回空，第二次出现返回格式错误
         */
        std::expected<void, QuicDecodeError> markSeen(std::uint64_t &seenMask, const std::uint64_t identifier, const std::string_view name)
        {
            if ((seenMask & (1ULL << identifier)) != 0)
            {
                return std::unexpected(
                        QuicDecodeError{QuicDecodeErrorKind::Malformed, std::format("传输参数 {} 出现第二次：RFC 9000 §7.4 要求一个参数在一次握手里只出现一次", name)});
            }
            seenMask |= 1ULL << identifier;
            return {};
        }
    } // namespace

    void appendQuicTransportParameters(std::string &bytes, const QuicTransportParameters &parameters)
    {
        // 写出顺序规范没作要求（§18 只是「一串三元组」），这里取固定顺序以便同一份参数编出的字节确定：
        // 带存在语义的三个连接标识一次、整型项一次、最后是令牌与零长的迁移禁止项
        for (const ConnectionIdParameterBinding &binding: kConnectionIdParameters)
        {
            if (const std::optional<std::vector<std::uint8_t>> &value = parameters.*(binding.member); value.has_value())
            {
                appendParameter(bytes, binding.identifier, *value);
            }
        }
        for (const IntegerParameterBinding &binding: kIntegerParameters)
        {
            appendIntegerParameter(bytes, binding.identifier, parameters.*(binding.member));
        }
        if (parameters.statelessResetToken.has_value())
        {
            appendParameter(bytes, kIdStatelessResetToken, *parameters.statelessResetToken);
        }
        if (parameters.disableActiveMigration)
        {
            // 这一项只有「在」与「不在」两种状态，取值长度恒为 0（RFC 9000 §18.2）
            appendParameter(bytes, kIdDisableActiveMigration, std::span<const std::uint8_t>{});
        }
    }

    std::expected<QuicTransportParameters, QuicDecodeError> decodeQuicTransportParameters(const std::span<const std::uint8_t>    bytes,
                                                                                          const QuicTransportParameterSenderRole senderRole)
    {
        QuicTransportParameters parameters;
        ParameterReader         reader(bytes);
        std::uint64_t           seenMask = 0;

        while (!reader.atEnd())
        {
            const auto identifier = reader.readInteger("标识域");
            if (!identifier.has_value())
            {
                return std::unexpected(identifier.error());
            }
            const auto length = reader.readInteger("长度域");
            if (!length.has_value())
            {
                return std::unexpected(length.error());
            }
            const auto value = reader.readBytes(*length, "取值");
            if (!value.has_value())
            {
                return std::unexpected(value.error());
            }

            if (isServerOnlyIdentifier(*identifier) && senderRole == QuicTransportParameterSenderRole::Client)
            {
                return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed,
                                                       std::format("客户端交来了标识 {:#04x} 的服务端专属传输参数（RFC 9000 §18.2 末段列为禁止项）", *identifier)});
            }

            if (const auto *const binding =
                        std::ranges::find_if(kIntegerParameters, [id = *identifier](const IntegerParameterBinding &candidate) { return candidate.identifier == id; });
                binding != std::ranges::end(kIntegerParameters))
            {
                if (const auto marked = markSeen(seenMask, *identifier, binding->name); !marked.has_value())
                {
                    return std::unexpected(marked.error());
                }
                const auto parsed = readIntegerValue(*value, binding->name);
                if (!parsed.has_value())
                {
                    return std::unexpected(parsed.error());
                }
                if (*parsed < binding->minimumValue || *parsed > binding->maximumValue)
                {
                    return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed, std::format("传输参数 {} 的取值 {} 不在允许区间 [{}, {}]（RFC 9000 §18.2）",
                                                                                                       binding->name, *parsed, binding->minimumValue, binding->maximumValue)});
                }
                parameters.*(binding->member) = *parsed;
                continue;
            }

            if (const auto *const binding =
                        std::ranges::find_if(kConnectionIdParameters, [id = *identifier](const ConnectionIdParameterBinding &candidate) { return candidate.identifier == id; });
                binding != std::ranges::end(kConnectionIdParameters))
            {
                if (const auto marked = markSeen(seenMask, *identifier, binding->name); !marked.has_value())
                {
                    return std::unexpected(marked.error());
                }
                auto parsed = readConnectionIdValue(*value, binding->name);
                if (!parsed.has_value())
                {
                    return std::unexpected(parsed.error());
                }
                parameters.*(binding->member) = std::move(*parsed);
                continue;
            }

            switch (*identifier)
            {
                case kIdStatelessResetToken:
                {
                    if (const auto marked = markSeen(seenMask, *identifier, "stateless_reset_token"); !marked.has_value())
                    {
                        return std::unexpected(marked.error());
                    }
                    if (value->size() != kQuicStatelessResetTokenLength)
                    {
                        return std::unexpected(
                                QuicDecodeError{QuicDecodeErrorKind::Malformed, std::format("传输参数 stateless_reset_token 有 {} 字节，规范要求恒为 {} 字节（RFC 9000 §18.2）",
                                                                                            value->size(), kQuicStatelessResetTokenLength)});
                    }
                    std::array<std::uint8_t, kQuicStatelessResetTokenLength> token{};
                    std::ranges::copy(*value, token.begin());
                    parameters.statelessResetToken = token;
                    break;
                }
                case kIdDisableActiveMigration:
                {
                    if (const auto marked = markSeen(seenMask, *identifier, "disable_active_migration"); !marked.has_value())
                    {
                        return std::unexpected(marked.error());
                    }
                    if (!value->empty())
                    {
                        return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed,
                                                               std::format("传输参数 disable_active_migration 带了 {} 字节取值，规范要求是零长（RFC 9000 §18.2）", value->size())});
                    }
                    parameters.disableActiveMigration = true;
                    break;
                }
                default:
                    // §7.4.2 的硬要求：不支持的参数一律忽略，包括 §18.1 里那些 31*N+27 的保留项
                    // （它们存在的唯一目的就是不让人把未知标识当错误）。0x0d 也走这条路：本实现不做迁移。
                    break;
            }
        }

        if (!parameters.initialSourceConnectionId.has_value())
        {
            return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed, "缺少 initial_source_connection_id：RFC 9000 §7.3 要求两端都必须带"});
        }
        if (senderRole == QuicTransportParameterSenderRole::Server && !parameters.originalDestinationConnectionId.has_value())
        {
            return std::unexpected(
                    QuicDecodeError{QuicDecodeErrorKind::Malformed, "服务端参数缺少 original_destination_connection_id：RFC 9000 §7.3 规定只有服务端发它，且必须发"});
        }
        return parameters;
    }
} // namespace AsynGyanis::Net
