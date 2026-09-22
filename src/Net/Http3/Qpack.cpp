#include "Net/Http3/Qpack.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 增量解析一条指令时的三态结果
         * @details Incomplete 与 Invalid 必须分开：指令边界不保证落在包边界上，把「字节还没到」当成非法
         *          会让一条连接因为普通的乱序而作废（RFC 9204 §4.2 的两条流本就独立推进）。
         */
        enum class QpackParseStatus
        {
            Complete,   ///< 该表示完整解出
            Incomplete, ///< 字节数不足，需要等后续字节
            Invalid,    ///< 表示本身非法（溢出、越界、字面量超长），原因已写入出参
        };

        // 编码器流指令的首字节模式（RFC 9204 §4.3.1~§4.3.4）
        constexpr std::uint8_t kThreeBitPatternBitMask = 0xE0;        ///< '001'/'000' 这类 3 位模式共用的掩码
        constexpr std::uint8_t kSetCapacityPatternBits = 0x20;        ///< Set Dynamic Table Capacity：'001'
        constexpr std::uint8_t kDuplicatePatternBits = 0x00;          ///< Duplicate：'000'
        constexpr std::uint8_t kTwoBitPatternBitMask = 0xC0;          ///< 2 位模式共用的掩码
        constexpr std::uint8_t kInsertLiteralNamePatternBits = 0x40;  ///< Insert With Literal Name：'01'
        constexpr std::uint8_t kInsertNameReferencePatternBits = 0x80;///< Insert With Name Reference：'1'

        // 解码器流指令的首字节模式（RFC 9204 §4.4.1~§4.4.3）
        constexpr std::uint8_t kSectionAckPatternBits = 0x80;         ///< Section Acknowledgment：'1'
        constexpr std::uint8_t kStreamCancellationPatternBits = 0x40; ///< Stream Cancellation：'01'
        constexpr std::uint8_t kInsertCountIncrementPatternBits = 0x00;///< Insert Count Increment：'00'

        // 字段行表示的首字节模式（RFC 9204 §4.5.2~§4.5.6）
        constexpr std::uint8_t kOneBitPatternBitMask = 0x80;             ///< '1' 与 '0' 的分界
        constexpr std::uint8_t kIndexedPatternBits = 0x80;               ///< Indexed Field Line：'1'，T=0
        constexpr std::uint8_t kIndexedStaticPatternBits = 0xC0;         ///< 同上且 T=1（引用静态表）
        constexpr std::uint8_t kLiteralNameReferencePatternBits = 0x40;  ///< Literal With Name Reference：'01'，N=0、T=0
        constexpr std::uint8_t kLiteralStaticNamePatternBits = 0x50;     ///< 同上且 T=1
        constexpr std::uint8_t kLiteralNamePatternBits = 0x20;           ///< Literal With Literal Name：'001'，N=0
        constexpr std::uint8_t kFourBitPatternBitMask = 0xF0;            ///< '0000'/'0001' 共用的掩码
        constexpr std::uint8_t kPostBaseIndexedPatternBits = 0x10;       ///< Indexed With Post-Base Index：'0001'
        constexpr std::uint8_t kPostBaseNameReferencePatternBits = 0x00; ///< Literal With Post-Base Name Reference：'0000'

        // 各表示里 T 位与 N 位的位置：它们跟着模式位一起挪动，不能共用一个掩码
        constexpr std::uint8_t kTableBitInOneBitPatternMask = 0x40;      ///< '1'/'01'-类：'1' + T（§4.3.2、§4.5.2）
        constexpr std::uint8_t kTableBitInLiteralNameReferenceMask = 0x10;///< '01' + N + T（§4.5.4）
        constexpr std::uint8_t kContinuationBitMask = 0x80;              ///< 前缀整数续字节的最高位标记
        constexpr std::uint8_t kSignBitMask = 0x80;                      ///< 头块前缀里 Delta Base 的符号位

        /**
         * @brief 造一个失败对象
         * @param errorKind 失败类别（决定上线错误码）
         * @param message 中文原因，含可定位坐标与 RFC 章节
         * @return QpackError 移动语义的失败对象
         */
        [[nodiscard]] QpackError makeQpackError(QpackErrorKind errorKind, std::string message)
        {
            return QpackError{errorKind, std::move(message)};
        }

        /**
         * @brief 把字节写成可直接抄进测试的十六进制串（如 "0x3F 0xBD 0x01"）
         * @param bytes 待展示的字节
         * @return std::string 十六进制文本，便于从失败日志里逐字节比对线上取值
         */
        [[nodiscard]] std::string toHexText(std::string_view bytes)
        {
            static constexpr char hexDigits[] = "0123456789ABCDEF";
            std::string text;
            for (const char item : bytes)
            {
                const auto byteValue = static_cast<unsigned char>(item);
                if (!text.empty())
                {
                    text.push_back(' ');
                }
                text.push_back('0');
                text.push_back('x');
                text.push_back(hexDigits[byteValue >> 4]);
                text.push_back(hexDigits[byteValue & 0x0F]);
            }
            return text;
        }

        /**
         * @brief 解一个前缀整数，并区分「字节不够」与「数值非法」
         * @details 表示本身沿用 RFC 7541 §5.1（RFC 9204 §4.1.1 明确原样复用），故首字节到位后交给
         *          Hpack.h 的 decodeHpackInteger；先自己数一遍续字节，才能把「还没到齐」和「非法」分开。
         * @param bytes 从该表示首字节开始的剩余数据
         * @param prefixBitCount 前缀位数，取值 1..8
         * @param errorKind 该表示所在通道对应的失败类别
         * @param what 中文称呼，写进文案便于定位
         * @param value 输出参数：解出的数值
         * @param consumedByteCount 输出参数：该表示占用的字节数
         * @param error 输出参数：失败详情
         * @return QpackParseStatus 三态结果
         */
        [[nodiscard]] QpackParseStatus decodePrefixedInteger(std::string_view bytes, std::uint8_t prefixBitCount,
                                                             QpackErrorKind errorKind, std::string_view what,
                                                             std::uint64_t &value, std::size_t &consumedByteCount,
                                                             QpackError *error)
        {
            if (bytes.empty())
            {
                return QpackParseStatus::Incomplete;
            }

            // 只有前缀位全为 1 才需要续字节；先确认续字节链条在现有数据里闭合，否则算「还没到齐」
            const std::uint8_t prefixValueMask = static_cast<std::uint8_t>((std::uint8_t{1} << prefixBitCount) - 1);
            std::size_t expectedByteCount = 1;
            if ((static_cast<std::uint8_t>(bytes[0]) & prefixValueMask) == prefixValueMask)
            {
                while (true)
                {
                    if (expectedByteCount >= bytes.size())
                    {
                        return QpackParseStatus::Incomplete;
                    }
                    const bool hasMoreContinuationBytes =
                        (static_cast<std::uint8_t>(bytes[expectedByteCount]) & kContinuationBitMask) != 0;
                    ++expectedByteCount;
                    if (!hasMoreContinuationBytes)
                    {
                        break;
                    }
                }
            }

            std::string errorText;
            if (!decodeHpackInteger(bytes, prefixBitCount, value, consumedByteCount, &errorText))
            {
                *error = makeQpackError(errorKind, std::string(what) + "非法：" + errorText + "（RFC 9204 §4.1.1、§7.4）");
                return QpackParseStatus::Invalid;
            }

            // 62 位上限：更大取值不取整也不回绕，否则索引换算会得到另一个「看起来合法」的表项
            if (value > kQpackMaximumIntegerValue)
            {
                *error = makeQpackError(errorKind, std::string(what) + "取值 " + std::to_string(value) +
                                                       " 超过 62 位上限 " + std::to_string(kQpackMaximumIntegerValue) +
                                                       "（RFC 9204 §4.1.1、§7.4）");
                return QpackParseStatus::Invalid;
            }
            return QpackParseStatus::Complete;
        }

        /**
         * @brief 解一个「N 位前缀字符串字面量」（RFC 9204 §4.1.2）
         * @details N 位前缀意味着首字节高 (8-N) 位属于前一个字段，随后 1 位 H 位与 (N-1) 位长度前缀；
         *          N=8 即退化为 RFC 7541 §5.2 的字节对齐写法。H 位为 1 时按 RFC 7541 附录 B 的码表解压
         *          （RFC 9204 §4.1.2 明确复用该表），故解码侧必须支持 Huffman。长度已到手、本体还没到齐算
         *          Incomplete：编码器流是无框架的字节流（§4.2），只有越界的长度才判错（§7.4）。
         * @param bytes 从该字面量首字节开始的剩余数据
         * @param prefixBitCount 该字面量的前缀位数 N，取值 2..8
         * @param errorKind 该字面量所在通道对应的失败类别
         * @param what 中文称呼（如「字段值」）
         * @param lineIndex 第几个字段行，写进文案便于定位
         * @param value 输出参数：解出的字节（Huffman 时已解压）
         * @param consumedByteCount 输出参数：该字面量占用的字节数
         * @param error 输出参数：失败详情
         * @return QpackParseStatus 三态结果
         */
        [[nodiscard]] QpackParseStatus decodePrefixedStringLiteral(std::string_view bytes, std::uint8_t prefixBitCount,
                                                                    QpackErrorKind errorKind, std::string_view what,
                                                                    std::size_t lineIndex, std::string &value,
                                                                    std::size_t &consumedByteCount, QpackError *error)
        {
            if (bytes.empty())
            {
                return QpackParseStatus::Incomplete;
            }

            // H 位紧贴长度前缀之上：N 位前缀的字面量，H 位在 bit(N-1)
            const bool isHuffmanEncoded = (static_cast<std::uint8_t>(bytes[0]) & static_cast<std::uint8_t>(std::uint8_t{1} << (prefixBitCount - 1))) != 0;
            std::uint64_t declaredLength = 0;
            std::size_t headerByteCount = 0;
            const QpackParseStatus headerStatus = decodePrefixedInteger(bytes, static_cast<std::uint8_t>(prefixBitCount - 1),
                                                                        errorKind, what, declaredLength, headerByteCount, error);
            if (headerStatus != QpackParseStatus::Complete)
            {
                return headerStatus;
            }
            if (declaredLength > kQpackMaximumStringLengthByteCount)
            {
                *error = makeQpackError(errorKind, "第 " + std::to_string(lineIndex) + " 个字段行的" + std::string(what) +
                                                        "声明长度 " + std::to_string(declaredLength) + " 超过本端上限 " +
                                                        std::to_string(kQpackMaximumStringLengthByteCount) +
                                                        " 字节（RFC 9204 §4.1.2、§7.4）");
                return QpackParseStatus::Invalid;
            }

            const std::size_t literalByteCount = static_cast<std::size_t>(declaredLength);
            if (bytes.size() - headerByteCount < literalByteCount)
            {
                // 长度已到手但本体还没到齐：等后续字节，别把普通的分片送达当成非法指令（§4.2、§7.4）
                return QpackParseStatus::Incomplete;
            }

            const std::string_view literalBytes = bytes.substr(headerByteCount, literalByteCount);
            if (isHuffmanEncoded)
            {
                std::string huffmanErrorText;
                if (!decodeHpackHuffmanString(literalBytes, value, &huffmanErrorText))
                {
                    *error = makeQpackError(errorKind, "第 " + std::to_string(lineIndex) + " 个字段行的" + std::string(what) +
                                                            " Huffman 解码失败：" + huffmanErrorText + "（RFC 9204 §4.1.2）");
                    return QpackParseStatus::Invalid;
                }
            }
            else
            {
                // 长度即原始字节数，原样按字节收下（可含 NUL 与任意二进制）
                value.assign(literalBytes.data(), literalBytes.size());
            }

            consumedByteCount = headerByteCount + literalByteCount;
            return QpackParseStatus::Complete;
        }

        /**
         * @brief 把「还没到齐」与「表示非法」两种失败统一成头块通道的错误对象
         * @param status 解析三态结果
         * @param error 非法时的详情（Incomplete 时忽略）
         * @param streamId 头块所在流，写进文案便于定位
         * @param lineIndex 第几个字段行
         * @param what 中文称呼
         * @return QpackError DecompressionFailed 类别的失败对象（RFC 9204 §7.4：请求流上的超限按此码）
         */
        [[nodiscard]] QpackError makeFieldSectionError(QpackParseStatus status, const QpackError &error, std::uint64_t streamId,
                                                       std::size_t lineIndex, std::string_view what)
        {
            if (status == QpackParseStatus::Incomplete)
            {
                return makeQpackError(QpackErrorKind::DecompressionFailed,
                                      "流 " + std::to_string(streamId) + " 的第 " + std::to_string(lineIndex) + " 个字段行（" +
                                          std::string(what) + "）字节不够，头块被截断（RFC 9204 §4.5）");
            }
            return error;
        }

        /**
         * @brief 按前缀整数编码追加一段字节
         * @param out 目标串
         * @param value 数值
         * @param prefixBitCount 前缀位数
         * @param firstBytePatternBits 首字节高位模式
         */
        void appendPrefixedInteger(std::string &out, std::uint64_t value, std::uint8_t prefixBitCount, std::uint8_t firstBytePatternBits)
        {
            out.append(encodeHpackInteger(value, prefixBitCount, firstBytePatternBits));
        }

        /**
         * @brief 把字节 span 转成 string_view，供 Hpack.h 的助手使用
         * @param bytes 按「指针 + 长度」给出的数据
         * @return std::string_view 同一块字节的视图（不拷贝，生命周期由调用方保证）
         */
        [[nodiscard]] std::string_view toByteView(std::span<const std::uint8_t> bytes)
        {
            return std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }

        /**
         * @brief 取首字节的某一位作为布尔标记
         * @param firstByte 首字节
         * @param bitMask 该位掩码
         * @return true 该位为 1
         */
        [[nodiscard]] bool testBit(std::uint8_t firstByte, std::uint8_t bitMask)
        {
            return (firstByte & bitMask) != 0;
        }
    } // namespace

    // ============================================================================
    // 静态表查表（RFC 9204 附录 A）
    // ============================================================================

    std::size_t findQpackStaticTableIndex(std::string_view name, std::string_view value) noexcept
    {
        for (std::size_t entryIndex = 0; entryIndex < kQpackStaticTable.size(); ++entryIndex)
        {
            // 表序按常见度排列，先命中者索引更小、编码更短，因此不做「同名里再挑一次」
            if (kQpackStaticTable[entryIndex].name == name && kQpackStaticTable[entryIndex].value == value)
            {
                return entryIndex;
            }
        }
        return kQpackStaticTableNoIndex;
    }

    std::size_t findQpackStaticTableNameIndex(std::string_view name) noexcept
    {
        for (std::size_t entryIndex = 0; entryIndex < kQpackStaticTable.size(); ++entryIndex)
        {
            if (kQpackStaticTable[entryIndex].name == name)
            {
                return entryIndex;
            }
        }
        return kQpackStaticTableNoIndex;
    }

    Http3ErrorCode toHttp3ErrorCode(QpackErrorKind errorKind) noexcept
    {
        switch (errorKind)
        {
        case QpackErrorKind::DecompressionFailed:
            return Http3ErrorCode::DecompressionFailed;
        case QpackErrorKind::EncoderStreamError:
            return Http3ErrorCode::EncoderStreamError;
        case QpackErrorKind::DecoderStreamError:
            return Http3ErrorCode::DecoderStreamError;
        case QpackErrorKind::FieldSectionTooLarge:
            return Http3ErrorCode::ExcessiveLoad;
        case QpackErrorKind::InvalidLocalState:
            return Http3ErrorCode::InternalError;
        }
        // 类别取值越界时按最保守的「本端内部故障」收场，绝不回落成 NoError
        return Http3ErrorCode::InternalError;
    }

    // ============================================================================
    // 动态表（RFC 9204 §3.2）
    // ============================================================================

    QpackDynamicTable::QpackDynamicTable(std::size_t capacityByteCount) noexcept
        : m_capacityByteCount(capacityByteCount)
    {
    }

    bool QpackDynamicTable::setCapacityByteCount(std::size_t capacityByteCount, const EvictionPredicate &evictionPermitted)
    {
        const std::size_t entryCountToKeep = [&]() -> std::size_t
        {
            std::size_t projectedSizeByteCount = m_sizeByteCount;
            std::size_t remainingEntryCount = m_entries.size();
            while (projectedSizeByteCount > capacityByteCount && remainingEntryCount > 0)
            {
                const std::uint64_t oldestAbsoluteIndex = m_insertCount - remainingEntryCount;
                if (evictionPermitted && !evictionPermitted(oldestAbsoluteIndex))
                {
                    return static_cast<std::size_t>(-1);
                }
                projectedSizeByteCount -= entrySizeByteCountOf(m_entries[remainingEntryCount - 1]);
                --remainingEntryCount;
            }
            return remainingEntryCount;
        }();

        if (entryCountToKeep == static_cast<std::size_t>(-1))
        {
            // §4.3.1：缩减容量不得淘汰不可淘汰的项，判失败而不是偷偷少缩一点
            return false;
        }

        while (m_entries.size() > entryCountToKeep)
        {
            m_sizeByteCount -= entrySizeByteCountOf(m_entries.back());
            m_entries.pop_back();
        }
        m_capacityByteCount = capacityByteCount;
        return true;
    }

    std::optional<std::uint64_t> QpackDynamicTable::insert(QpackHeaderField field, const EvictionPredicate &evictionPermitted)
    {
        const std::size_t entrySizeByteCount = entrySizeByteCountOf(field);
        if (entrySizeByteCount > m_capacityByteCount)
        {
            // §3.2.2：单项大于容量是错误。解码侧的淘汰判据恒真，故返回空即唯一地表示「这项放不下」
            return std::nullopt;
        }

        // §3.2.2：先淘汰到「容量 − 本项大小」以内再插入；淘汰不动就整个放弃（§2.1.1 禁止强插）
        while (m_sizeByteCount + entrySizeByteCount > m_capacityByteCount)
        {
            if (m_entries.empty())
            {
                return std::nullopt;
            }
            const std::uint64_t oldestAbsoluteIndex = m_insertCount - m_entries.size();
            if (evictionPermitted && !evictionPermitted(oldestAbsoluteIndex))
            {
                return std::nullopt;
            }
            m_sizeByteCount -= entrySizeByteCountOf(m_entries.back());
            m_entries.pop_back();
        }

        const std::uint64_t absoluteIndex = m_insertCount;
        m_entries.push_front(std::move(field));
        m_sizeByteCount += entrySizeByteCount;
        ++m_insertCount;
        return absoluteIndex;
    }

    bool QpackDynamicTable::tryGetEntryByAbsoluteIndex(std::uint64_t absoluteIndex, QpackHeaderField &field) const
    {
        const std::uint64_t oldestAbsoluteIndex = m_insertCount - m_entries.size();
        if (absoluteIndex < oldestAbsoluteIndex || absoluteIndex >= m_insertCount)
        {
            return false;
        }
        // 绝对索引越大越新：表内下标 = 项数 - 1 - (该索引 − 最旧项的索引)
        const std::size_t entryIndex = m_entries.size() - 1 - static_cast<std::size_t>(absoluteIndex - oldestAbsoluteIndex);
        field = m_entries[entryIndex];
        return true;
    }

    bool QpackDynamicTable::tryGetEntryByRelativeIndexFromInsertionPoint(std::uint64_t relativeIndex, QpackHeaderField &field) const
    {
        if (m_insertCount == 0 || relativeIndex >= m_insertCount)
        {
            return false;
        }
        return tryGetEntryByAbsoluteIndex(m_insertCount - 1 - relativeIndex, field);
    }

    std::uint64_t QpackDynamicTable::findMatchingEntry(std::string_view name, std::string_view value) const
    {
        // 下标 0 是最新插入项：从新到旧扫，让命中的项尽量新，少拖累淘汰（§2.1.1 的可淘汰性）
        for (std::size_t entryIndex = 0; entryIndex < m_entries.size(); ++entryIndex)
        {
            if (m_entries[entryIndex].name == name && m_entries[entryIndex].value == value)
            {
                return m_insertCount - 1 - entryIndex;
            }
        }
        return kQpackNoAbsoluteIndex;
    }

    std::uint64_t QpackDynamicTable::findNameEntry(std::string_view name) const
    {
        for (std::size_t entryIndex = 0; entryIndex < m_entries.size(); ++entryIndex)
        {
            if (m_entries[entryIndex].name == name)
            {
                return m_insertCount - 1 - entryIndex;
            }
        }
        return kQpackNoAbsoluteIndex;
    }

    void QpackDynamicTable::clearEntries() noexcept
    {
        m_entries.clear();
        m_sizeByteCount = 0;
    }

    std::size_t QpackDynamicTable::entrySizeByteCountOf(const QpackHeaderField &field) noexcept
    {
        // §3.2.1：按名与值未做 Huffman 编码的长度算，再加 32 字节固定开销
        return field.name.size() + field.value.size() + kQpackEntryOverheadByteCount;
    }

    std::size_t QpackDynamicTable::capacityByteCount() const noexcept
    {
        return m_capacityByteCount;
    }

    std::size_t QpackDynamicTable::sizeByteCount() const noexcept
    {
        return m_sizeByteCount;
    }

    std::size_t QpackDynamicTable::entryCount() const noexcept
    {
        return m_entries.size();
    }

    std::uint64_t QpackDynamicTable::insertCount() const noexcept
    {
        return m_insertCount;
    }

    std::uint64_t QpackDynamicTable::droppedEntryCount() const noexcept
    {
        return m_insertCount - m_entries.size();
    }

    std::uint64_t QpackDynamicTable::maximumEntryCount(std::size_t maximumTableCapacityByteCount) noexcept
    {
        // §4.5.1.1：最小的项是两个空串 + 32 字节开销，故项数上界为容量除以 32 的向下取整
        return maximumTableCapacityByteCount / kQpackEntryOverheadByteCount;
    }

    const std::deque<QpackHeaderField> &QpackDynamicTable::entries() const noexcept
    {
        return m_entries;
    }

    // ============================================================================
    // 编码器（RFC 9204 §2.1、附录 C）
    // ============================================================================

    QpackEncoder::QpackEncoder(std::size_t peerMaximumTableCapacityByteCount, std::size_t peerMaximumBlockedStreamCount,
                               std::size_t localTableCapacityByteCount)
        : m_peerMaximumTableCapacityByteCount(peerMaximumTableCapacityByteCount)
        , m_peerMaximumBlockedStreamCount(peerMaximumBlockedStreamCount)
        , m_tableCapacityByteCount(localTableCapacityByteCount)
        , m_hasPendingCapacityInstruction(localTableCapacityByteCount != 0)
        , m_dynamicTable(localTableCapacityByteCount)
    {
    }

    bool QpackEncoder::isEntryEvictable(std::uint64_t absoluteIndex) const
    {
        // §2.2.2.2：绝对索引小于已知接收计数且引用数为 0 才算可淘汰；刚插入的项即使没被引用也不能淘汰
        // （§2.1.1），这一点由「小于已知接收计数」天然保证
        if (absoluteIndex >= m_knownReceivedInsertCount)
        {
            return false;
        }
        const auto referenceIterator = m_entryReferenceCount.find(absoluteIndex);
        return referenceIterator == m_entryReferenceCount.end() || referenceIterator->second == 0;
    }

    void QpackEncoder::refreshDrainingAbsoluteIndex() noexcept
    {
        // §2.1.1.1 的固定余量启发式：只保留最近 capacity/2 字节的项可被直接引用，更旧的留给淘汰
        const std::size_t referenceableByteCount = m_tableCapacityByteCount / kQpackDrainingFreeSpaceDivisor;
        std::size_t accumulatedByteCount = 0;
        std::uint64_t projectedDrainingIndex = m_dynamicTable.insertCount();
        for (std::size_t entryIndex = 0; entryIndex < m_dynamicTable.entries().size(); ++entryIndex)
        {
            const QpackHeaderField &candidate = m_dynamicTable.entries()[entryIndex];
            if (accumulatedByteCount + QpackDynamicTable::entrySizeByteCountOf(candidate) > referenceableByteCount)
            {
                // 这一项起（含它）不再直接引用，正好把可淘汰区留到目标余量
                projectedDrainingIndex = m_dynamicTable.insertCount() - entryIndex;
                break;
            }
            accumulatedByteCount += QpackDynamicTable::entrySizeByteCountOf(candidate);
            projectedDrainingIndex = m_dynamicTable.insertCount() - entryIndex - 1;
        }
        // draining 索引只单调前进：倒退会让已承诺不再引用的旧项重新被引用，抵消掉留出的余量
        m_drainingAbsoluteIndex = std::max(m_drainingAbsoluteIndex, projectedDrainingIndex);
    }

    std::map<std::uint64_t, std::deque<QpackEncoder::PendingFieldSection>>::iterator
    QpackEncoder::findEarliestAwaitingAcknowledgement(std::uint64_t streamId) noexcept
    {
        // §2.2.2.1：一次 Ack 只对应「该流上最早一段含动态表引用的头块」，队列里存的就只有这种段
        const auto streamIterator = m_pendingSectionsByStreamId.find(streamId);
        if (streamIterator != m_pendingSectionsByStreamId.end() && !streamIterator->second.empty())
        {
            return streamIterator;
        }
        return m_pendingSectionsByStreamId.end();
    }

    void QpackEncoder::releasePendingSectionReferences(const PendingFieldSection &pendingSection) noexcept
    {
        for (const std::uint64_t absoluteIndex : pendingSection.referencedAbsoluteIndices)
        {
            const auto referenceIterator = m_entryReferenceCount.find(absoluteIndex);
            if (referenceIterator != m_entryReferenceCount.end())
            {
                if (referenceIterator->second <= 1)
                {
                    m_entryReferenceCount.erase(referenceIterator);
                }
                else
                {
                    --referenceIterator->second;
                }
            }
        }
    }

    void QpackEncoder::subtractBlockingSectionCount(std::uint64_t streamId) noexcept
    {
        const auto streamIterator = m_blockingSectionCountByStreamId.find(streamId);
        if (streamIterator == m_blockingSectionCountByStreamId.end())
        {
            return;
        }
        if (streamIterator->second <= 1)
        {
            m_blockingSectionCountByStreamId.erase(streamIterator);
        }
        else
        {
            --streamIterator->second;
        }
    }

    void QpackEncoder::cancelStreamReferences(std::uint64_t streamId) noexcept
    {
        const auto streamIterator = m_pendingSectionsByStreamId.find(streamId);
        if (streamIterator == m_pendingSectionsByStreamId.end())
        {
            return;
        }
        for (const PendingFieldSection &pendingSection : streamIterator->second)
        {
            releasePendingSectionReferences(pendingSection);
            if (pendingSection.risksBlocking)
            {
                subtractBlockingSectionCount(streamId);
            }
        }
        m_pendingSectionsByStreamId.erase(streamId);
        m_blockingSectionCountByStreamId.erase(streamId);
    }

    void QpackEncoder::refreshBlockingState() noexcept
    {
        // §2.1.2：已知接收计数追上某段的 Required Insert Count 后，该段不再可能让对端阻塞，名额归还
        for (auto &[streamId, sections] : m_pendingSectionsByStreamId)
        {
            for (PendingFieldSection &section : sections)
            {
                if (section.risksBlocking && section.requiredInsertCount <= m_knownReceivedInsertCount)
                {
                    section.risksBlocking = false;
                    subtractBlockingSectionCount(streamId);
                }
            }
        }
    }

    std::expected<void, QpackError> QpackEncoder::setMaximumTableCapacityByteCount(std::size_t capacityByteCount,
                                                                                    std::string &encoderStreamBytes)
    {
        encoderStreamBytes.clear();
        if (capacityByteCount > m_peerMaximumTableCapacityByteCount)
        {
            // §3.2.3：不得超过对端 SETTINGS_QPACK_MAX_TABLE_CAPACITY；本端既不静默收窄，也不发出去让对端判错
            return std::unexpected(makeQpackError(
                QpackErrorKind::InvalidLocalState, "要求的动态表容量 " + std::to_string(capacityByteCount) +
                                                       " 超过对端上限 " + std::to_string(m_peerMaximumTableCapacityByteCount) +
                                                       " 字节（RFC 9204 §3.2.3）"));
        }

        const QpackDynamicTable::EvictionPredicate evictionPermitted =
            [this](std::uint64_t absoluteIndex) { return isEntryEvictable(absoluteIndex); };
        if (!m_dynamicTable.setCapacityByteCount(capacityByteCount, evictionPermitted))
        {
            // §4.3.1：缩容不得淘汰仍被未确认头块引用的项，宁可拒绝这次变更也不让对端解不开后续头块
            return std::unexpected(makeQpackError(
                QpackErrorKind::InvalidLocalState, "把动态表容量降到 " + std::to_string(capacityByteCount) +
                                                       " 字节会淘汰尚未确认的表项（RFC 9204 §4.3.1、§2.1.1）"));
        }

        m_tableCapacityByteCount = capacityByteCount;
        m_hasPendingCapacityInstruction = false;
        if (capacityByteCount == 0 && m_peerMaximumTableCapacityByteCount == 0)
        {
            // §3.2.3：对端上限为 0 时不得发任何编码器流指令，而初始容量本就是 0，无需通告这次「不变」
            return {};
        }
        appendPrefixedInteger(encoderStreamBytes, capacityByteCount, 5, kSetCapacityPatternBits);
        return {};
    }

    std::expected<void, QpackError> QpackEncoder::encodeFieldSection(std::uint64_t streamId,
                                                                     std::span<const QpackHeaderField> fieldLines,
                                                                     std::string &headerBlock,
                                                                     std::string &encoderStreamBytes)
    {
        headerBlock.clear();
        encoderStreamBytes.clear();

        if (m_tableCapacityByteCount > m_peerMaximumTableCapacityByteCount)
        {
            return std::unexpected(makeQpackError(
                QpackErrorKind::InvalidLocalState, "动态表容量 " + std::to_string(m_tableCapacityByteCount) +
                                                       " 超过对端上限 " + std::to_string(m_peerMaximumTableCapacityByteCount) +
                                                       " 字节，无法编码（RFC 9204 §3.2.3）"));
        }

        std::string fieldLineBytes;
        std::string localEncoderStream;
        if (m_hasPendingCapacityInstruction)
        {
            // 容量指令要早于任何插入到达对端，否则对端会因容量仍为 0 而判 EncoderStreamError（§3.2.2）
            appendPrefixedInteger(localEncoderStream, m_tableCapacityByteCount, 5, kSetCapacityPatternBits);
            m_hasPendingCapacityInstruction = false;
        }

        // §4.5.1.2 与附录 C：Base 取本段开始时的插入计数快照，段内新插入的项靠表后索引引用
        const std::uint64_t baseValue = m_dynamicTable.insertCount();
        std::uint64_t requiredInsertCount = 0;
        std::vector<std::uint64_t> referencedAbsoluteIndices;

        const bool thisStreamAlreadyRisksBlocking = m_blockingSectionCountByStreamId.contains(streamId);
        // §2.1.2：可能阻塞的流数恒不得超过对端 SETTINGS_QPACK_BLOCKED_STREAMS；同一条流再阻塞不占新名额
        const bool mayReferenceUnacknowledgedEntries =
            m_blockingSectionCountByStreamId.size() < m_peerMaximumBlockedStreamCount || thisStreamAlreadyRisksBlocking;
        const QpackDynamicTable::EvictionPredicate evictionPermitted =
            [this](std::uint64_t absoluteIndex) { return isEntryEvictable(absoluteIndex); };

        for (std::size_t lineIndex = 0; lineIndex < fieldLines.size(); ++lineIndex)
        {
            const QpackHeaderField &fieldLine = fieldLines[lineIndex];
            const std::size_t staticFullIndex = findQpackStaticTableIndex(fieldLine.name, fieldLine.value);
            if (staticFullIndex != kQpackStaticTableNoIndex)
            {
                // 静态表整项命中最省字节且不引入任何动态状态，故优先级最高（附录 C 的第一步）
                appendPrefixedInteger(fieldLineBytes, staticFullIndex, 6, kIndexedStaticPatternBits);
                continue;
            }

            std::uint64_t matchedAbsoluteIndex = m_dynamicTable.findMatchingEntry(fieldLine.name, fieldLine.value);
            if (matchedAbsoluteIndex != kQpackNoAbsoluteIndex && matchedAbsoluteIndex < m_drainingAbsoluteIndex &&
                matchedAbsoluteIndex + 1 < m_dynamicTable.insertCount() && mayReferenceUnacknowledgedEntries)
            {
                // §2.1.1.1：命中项已进入 draining 区间时改发 Duplicate 并引用表首的新副本，免得这条旧引用
                // 继续拖住淘汰；表首项本身不必复制（复制出来的索引引用起来等价）
                QpackHeaderField sourceEntry;
                const bool sourceFound = m_dynamicTable.tryGetEntryByAbsoluteIndex(matchedAbsoluteIndex, sourceEntry);
                const std::uint64_t relativeIndexToSource = m_dynamicTable.insertCount() - 1 - matchedAbsoluteIndex;
                const std::optional<std::uint64_t> duplicatedAbsoluteIndex =
                    sourceFound ? m_dynamicTable.insert(std::move(sourceEntry), evictionPermitted) : std::nullopt;
                if (duplicatedAbsoluteIndex.has_value())
                {
                    appendPrefixedInteger(localEncoderStream, relativeIndexToSource, 5, kDuplicatePatternBits);
                    matchedAbsoluteIndex = *duplicatedAbsoluteIndex;
                    refreshDrainingAbsoluteIndex();
                }
            }

            if (matchedAbsoluteIndex != kQpackNoAbsoluteIndex)
            {
                const bool acknowledged = matchedAbsoluteIndex < m_knownReceivedInsertCount;
                if (acknowledged || mayReferenceUnacknowledgedEntries)
                {
                    // 绝对索引小于 Base 的用相对索引，等于或大于 Base 的用表后索引（§3.2.5、§3.2.6）
                    if (matchedAbsoluteIndex < baseValue)
                    {
                        appendPrefixedInteger(fieldLineBytes, baseValue - 1 - matchedAbsoluteIndex, 6, kIndexedPatternBits);
                    }
                    else
                    {
                        appendPrefixedInteger(fieldLineBytes, matchedAbsoluteIndex - baseValue, 4, kPostBaseIndexedPatternBits);
                    }
                    referencedAbsoluteIndices.push_back(matchedAbsoluteIndex);
                    requiredInsertCount = std::max(requiredInsertCount, matchedAbsoluteIndex + 1);
                    continue;
                }
            }

            if (matchedAbsoluteIndex == kQpackNoAbsoluteIndex && m_tableCapacityByteCount != 0 && mayReferenceUnacknowledgedEntries)
            {
                // 附录 C 的「插入并引用」：先定名的来源（静态同名 → 插入时引用静态名；否则动态同名；再否则字面量名）
                const std::size_t staticNameIndex = findQpackStaticTableNameIndex(fieldLine.name);
                const std::uint64_t dynamicNameIndex =
                    staticNameIndex == kQpackStaticTableNoIndex ? m_dynamicTable.findNameEntry(fieldLine.name) : kQpackNoAbsoluteIndex;
                const std::uint64_t dynamicNameRelativeIndex =
                    dynamicNameIndex == kQpackNoAbsoluteIndex ? 0 : m_dynamicTable.insertCount() - 1 - dynamicNameIndex;
                const std::optional<std::uint64_t> insertedAbsoluteIndex =
                    m_dynamicTable.insert(QpackHeaderField{fieldLine.name, fieldLine.value}, evictionPermitted);
                if (insertedAbsoluteIndex.has_value())
                {
                    if (staticNameIndex != kQpackStaticTableNoIndex)
                    {
                        appendPrefixedInteger(localEncoderStream, staticNameIndex, 6,
                                              kInsertNameReferencePatternBits | kTableBitInOneBitPatternMask);
                        appendHpackString(localEncoderStream, fieldLine.value);
                    }
                    else if (dynamicNameIndex != kQpackNoAbsoluteIndex)
                    {
                        appendPrefixedInteger(localEncoderStream, dynamicNameRelativeIndex, 6, kInsertNameReferencePatternBits);
                        appendHpackString(localEncoderStream, fieldLine.value);
                    }
                    else
                    {
                        // 6 位前缀字符串字面量：H 位在 bit5、长度前缀 5 位，本端不启用 Huffman
                        localEncoderStream.append(encodeHpackInteger(fieldLine.name.size(), 5, kInsertLiteralNamePatternBits));
                        localEncoderStream.append(fieldLine.name);
                        appendHpackString(localEncoderStream, fieldLine.value);
                    }
                    refreshDrainingAbsoluteIndex();

                    const std::uint64_t newAbsoluteIndex = *insertedAbsoluteIndex;
                    if (newAbsoluteIndex < baseValue)
                    {
                        appendPrefixedInteger(fieldLineBytes, baseValue - 1 - newAbsoluteIndex, 6, kIndexedPatternBits);
                    }
                    else
                    {
                        appendPrefixedInteger(fieldLineBytes, newAbsoluteIndex - baseValue, 4, kPostBaseIndexedPatternBits);
                    }
                    referencedAbsoluteIndices.push_back(newAbsoluteIndex);
                    requiredInsertCount = std::max(requiredInsertCount, newAbsoluteIndex + 1);
                    continue;
                }
            }

            // 既不引用也不插入：退化成字面量表示（名仍可引用，值一律字面量）
            const std::size_t staticNameIndex = findQpackStaticTableNameIndex(fieldLine.name);
            const std::uint64_t dynamicNameIndex =
                staticNameIndex == kQpackStaticTableNoIndex ? m_dynamicTable.findNameEntry(fieldLine.name) : kQpackNoAbsoluteIndex;
            if (staticNameIndex != kQpackStaticTableNoIndex)
            {
                appendPrefixedInteger(fieldLineBytes, staticNameIndex, 4, kLiteralStaticNamePatternBits);
                appendHpackString(fieldLineBytes, fieldLine.value);
                continue;
            }
            if (dynamicNameIndex != kQpackNoAbsoluteIndex &&
                (dynamicNameIndex < m_knownReceivedInsertCount || mayReferenceUnacknowledgedEntries))
            {
                if (dynamicNameIndex < baseValue)
                {
                    appendPrefixedInteger(fieldLineBytes, baseValue - 1 - dynamicNameIndex, 4, kLiteralNameReferencePatternBits);
                }
                else
                {
                    appendPrefixedInteger(fieldLineBytes, dynamicNameIndex - baseValue, 3, kPostBaseNameReferencePatternBits);
                }
                appendHpackString(fieldLineBytes, fieldLine.value);
                referencedAbsoluteIndices.push_back(dynamicNameIndex);
                requiredInsertCount = std::max(requiredInsertCount, dynamicNameIndex + 1);
                continue;
            }

            // 4 位前缀字符串字面量：首字节高 3 位是 '001'、N 位 0、H 位在 bit3、长度前缀 3 位
            fieldLineBytes.append(encodeHpackInteger(fieldLine.name.size(), 3, kLiteralNamePatternBits));
            fieldLineBytes.append(fieldLine.name);
            appendHpackString(fieldLineBytes, fieldLine.value);
        }

        // §4.5.1 的前缀占头块的最前两字节，必须排在所有字段行表示之前；它的取值要等整段编完才定得下来，
        // 故先单独攒在 prefixBytes 里，最后与字段行拼成一整段
        std::string prefixBytes;
        if (requiredInsertCount == 0)
        {
            appendPrefixedInteger(prefixBytes, 0, 8, 0x00);
            appendPrefixedInteger(prefixBytes, 0, 7, 0x00);
        }
        else
        {
            const std::uint64_t maximumEntryCount = QpackDynamicTable::maximumEntryCount(m_peerMaximumTableCapacityByteCount);
            if (maximumEntryCount == 0)
            {
                // 容不下任何表项的容量不可能产生引用：走到这里说明状态自相矛盾，宁可报错也不做除零回绕
                return std::unexpected(makeQpackError(QpackErrorKind::InvalidLocalState,
                                                      "对端容量上限 " + std::to_string(m_peerMaximumTableCapacityByteCount) +
                                                          " 字节容不下一项，却产生了 Required Insert Count " +
                                                          std::to_string(requiredInsertCount) + "（RFC 9204 §4.5.1.1）"));
            }
            // §4.5.1.1：Required Insert Count 按 2×MaxEntries 取模再加一编码，MaxEntries 取自对端公布的容量上限
            const std::uint64_t fullRange = 2 * maximumEntryCount;
            appendPrefixedInteger(prefixBytes, (requiredInsertCount % fullRange) + 1, 8, 0x00);
            if (baseValue >= requiredInsertCount)
            {
                appendPrefixedInteger(prefixBytes, baseValue - requiredInsertCount, 7, 0x00);
            }
            else
            {
                // 符号位为 1 表示段内插过项：Base = Required Insert Count - Delta Base - 1（§4.5.1.2）
                appendPrefixedInteger(prefixBytes, requiredInsertCount - baseValue - 1, 7, kSignBitMask);
            }
        }

        for (const std::uint64_t absoluteIndex : referencedAbsoluteIndices)
        {
            // 每条引用都记到该段被确认或取消为止，否则淘汰判据会把还被引用的项当成可淘汰（§2.1.1）
            ++m_entryReferenceCount[absoluteIndex];
        }

        const bool risksBlocking = requiredInsertCount > m_knownReceivedInsertCount;
        if (requiredInsertCount != 0)
        {
            // 只登记「会被 Ack」的段：Required Insert Count 为 0 的段既不含动态表引用，也永远不会收到 Ack
            m_pendingSectionsByStreamId[streamId].push_back(PendingFieldSection{requiredInsertCount, referencedAbsoluteIndices, risksBlocking});
        }
        if (risksBlocking)
        {
            ++m_blockingSectionCountByStreamId[streamId];
        }

        headerBlock.reserve(prefixBytes.size() + fieldLineBytes.size());
        headerBlock.append(prefixBytes);
        headerBlock.append(fieldLineBytes);
        encoderStreamBytes = std::move(localEncoderStream);
        return {};
    }

    std::expected<std::size_t, QpackError> QpackEncoder::feedDecoderStream(std::span<const std::uint8_t> bytes)
    {
        const std::size_t bufferedByteCount = m_decoderStreamBuffer.size();
        m_decoderStreamBuffer.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        enum class InstructionKind
        {
            SectionAcknowledgment,
            StreamCancellation,
            InsertCountIncrement,
        };

        std::size_t parsedByteCount = 0;
        while (parsedByteCount < m_decoderStreamBuffer.size())
        {
            const std::string_view remaining = std::string_view(m_decoderStreamBuffer).substr(parsedByteCount);
            const std::uint8_t firstByte = static_cast<std::uint8_t>(remaining[0]);

            // §4.4 的三种指令按首字节模式区分：'1' 是 Section Ack，'01' 是 Stream Cancellation，'00' 是 ICI
            InstructionKind kind = InstructionKind::InsertCountIncrement;
            std::uint8_t prefixBitCount = 6;
            if (testBit(firstByte, kSectionAckPatternBits))
            {
                kind = InstructionKind::SectionAcknowledgment;
                prefixBitCount = 7;
            }
            else if ((firstByte & kTwoBitPatternBitMask) == kStreamCancellationPatternBits)
            {
                kind = InstructionKind::StreamCancellation;
            }

            std::uint64_t parameterValue = 0;
            std::size_t consumedByteCount = 0;
            QpackError error;
            const QpackParseStatus status =
                decodePrefixedInteger(remaining, prefixBitCount, QpackErrorKind::DecoderStreamError, "解码器流指令", parameterValue,
                                      consumedByteCount, &error);
            if (status == QpackParseStatus::Incomplete)
            {
                // 指令边界不保证落在包边界上：半截留在缓冲里，下趟接着解（§4.2 的流是无框架的字节流）
                break;
            }
            if (status != QpackParseStatus::Complete)
            {
                m_decoderStreamBuffer.erase(0, parsedByteCount);
                return std::unexpected(error);
            }

            if (kind == InstructionKind::SectionAcknowledgment)
            {
                const auto sectionIterator = findEarliestAwaitingAcknowledgement(parameterValue);
                if (sectionIterator == m_pendingSectionsByStreamId.end())
                {
                    // 这条流本端已不再等确认：要么流被放弃了，要么压根没发过头块。放弃那条流时本端发出
                    // 的收口指令与对端早已上路的那条 Ack 分属两条独立流、彼此没有先后保证（RFC 9204 §2.1），
                    // 因此「Ack 骑在收口之上」是合法竞态，与对端记账错乱无从区分。既然区分不了，就只能忽略：
                    // 忽略的代价是两三个字节，判错的代价是把一条健康连接杀掉——任何客户端每条连接
                    // 取消一次请求就能做到。指令本身照常消费掉，不然解析游标不前进就成了死循环
                    parsedByteCount += consumedByteCount;
                    continue;
                }
                PendingFieldSection &acknowledgedSection = sectionIterator->second.front();
                releasePendingSectionReferences(acknowledgedSection);
                if (acknowledgedSection.risksBlocking)
                {
                    subtractBlockingSectionCount(parameterValue);
                }
                if (acknowledgedSection.requiredInsertCount > m_knownReceivedInsertCount)
                {
                    // §2.1.4：Ack 隐含对端已收到该段所需的全部插入
                    m_knownReceivedInsertCount = acknowledgedSection.requiredInsertCount;
                }
                sectionIterator->second.pop_front();
                if (sectionIterator->second.empty())
                {
                    m_pendingSectionsByStreamId.erase(sectionIterator);
                }
                refreshBlockingState();
            }
            else if (kind == InstructionKind::StreamCancellation)
            {
                cancelStreamReferences(parameterValue);
            }
            else
            {
                if (parameterValue == 0)
                {
                    m_decoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(QpackErrorKind::DecoderStreamError,
                                                          "Insert Count Increment 的增量为 0（RFC 9204 §4.4.3）"));
                }
                if (m_knownReceivedInsertCount > m_dynamicTable.insertCount() ||
                    parameterValue > m_dynamicTable.insertCount() - m_knownReceivedInsertCount)
                {
                    m_decoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::DecoderStreamError, "Insert Count Increment 增量 " + std::to_string(parameterValue) +
                                                                " 会把已知接收计数推到 " +
                                                                std::to_string(m_knownReceivedInsertCount + parameterValue) +
                                                                "，超过本端已发出的插入数 " + std::to_string(m_dynamicTable.insertCount()) +
                                                                "（RFC 9204 §4.4.3）"));
                }
                m_knownReceivedInsertCount += parameterValue;
                refreshBlockingState();
            }

            parsedByteCount += consumedByteCount;
        }

        m_decoderStreamBuffer.erase(0, parsedByteCount);
        // 本趟消费的输入字节数：指令边界可能骑在上趟残留的半截上，故按「总解析量 − 上趟残留量」计
        return parsedByteCount > bufferedByteCount ? parsedByteCount - bufferedByteCount : 0;
    }

    void QpackEncoder::noteStreamAbandoned(std::uint64_t streamId, std::string &decoderStreamBytes)
    {
        decoderStreamBytes.clear();
        // §4.4.2：本端不再等这条流的头块，告诉对端该流上的动态表引用全部作废
        appendPrefixedInteger(decoderStreamBytes, streamId, 6, kStreamCancellationPatternBits);
        cancelStreamReferences(streamId);
    }

    bool QpackEncoder::hasBlockedStreams() const noexcept
    {
        return !m_blockingSectionCountByStreamId.empty();
    }

    std::size_t QpackEncoder::blockedStreamCount() const noexcept
    {
        return m_blockingSectionCountByStreamId.size();
    }

    std::size_t QpackEncoder::tableCapacityByteCount() const noexcept
    {
        return m_tableCapacityByteCount;
    }

    std::size_t QpackEncoder::dynamicTableSizeByteCount() const noexcept
    {
        return m_dynamicTable.sizeByteCount();
    }

    std::uint64_t QpackEncoder::insertCount() const noexcept
    {
        return m_dynamicTable.insertCount();
    }

    std::uint64_t QpackEncoder::knownReceivedInsertCount() const noexcept
    {
        return m_knownReceivedInsertCount;
    }

    const std::deque<QpackHeaderField> &QpackEncoder::dynamicTableEntries() const noexcept
    {
        return m_dynamicTable.entries();
    }

    // ============================================================================
    // 解码器（RFC 9204 §2.2、§4.3、§4.4、§4.5）
    // ============================================================================

    QpackDecoder::QpackDecoder(QpackDecoderSettings settings)
        : m_settings(settings)
        , m_dynamicTable(0)
    {
    }

    std::expected<std::uint64_t, QpackError> QpackDecoder::decodeRequiredInsertCount(std::uint64_t encodedInsertCount) const
    {
        if (encodedInsertCount == 0)
        {
            return 0;
        }

        const std::uint64_t maximumEntryCount = QpackDynamicTable::maximumEntryCount(m_settings.maximumTableCapacityByteCount);
        if (maximumEntryCount == 0)
        {
            return std::unexpected(makeQpackError(
                QpackErrorKind::DecompressionFailed, "对端容量上限 " + std::to_string(m_settings.maximumTableCapacityByteCount) +
                                                         " 字节容不下任何表项，Encoded Insert Count 只能是 0，实际为 " +
                                                         std::to_string(encodedInsertCount) + "（RFC 9204 §4.5.1.1）"));
        }

        const std::uint64_t fullRange = 2 * maximumEntryCount;
        if (encodedInsertCount > fullRange)
        {
            return std::unexpected(makeQpackError(QpackErrorKind::DecompressionFailed,
                                                  "Encoded Insert Count " + std::to_string(encodedInsertCount) +
                                                      " 超过可表示范围 " + std::to_string(fullRange) + "（RFC 9204 §4.5.1.1）"));
        }

        // §4.5.1.1 的还原算法：把模 2×MaxEntries 的取值放回离本端当前插入数最近的那一圈
        const std::uint64_t totalInsertCount = m_dynamicTable.insertCount();
        const std::uint64_t maximumValue = totalInsertCount + maximumEntryCount;
        const std::uint64_t wrappedBaseValue = (maximumValue / fullRange) * fullRange;
        std::uint64_t requiredInsertCount = wrappedBaseValue + encodedInsertCount - 1;
        if (requiredInsertCount > maximumValue)
        {
            if (requiredInsertCount <= fullRange)
            {
                return std::unexpected(makeQpackError(QpackErrorKind::DecompressionFailed,
                                                      "Required Insert Count 还原为 " + std::to_string(requiredInsertCount) +
                                                          "，无法回退到合法区间（RFC 9204 §4.5.1.1）"));
            }
            requiredInsertCount -= fullRange;
        }
        if (requiredInsertCount == 0)
        {
            return std::unexpected(makeQpackError(QpackErrorKind::DecompressionFailed,
                                                  "还原出的 Required Insert Count 为 0，但取值 0 必须编码成 0（RFC 9204 §4.5.1.1）"));
        }
        return requiredInsertCount;
    }

    void QpackDecoder::appendInsertCountIncrementIfPending(std::string &decoderStreamBytes)
    {
        const std::uint64_t insertCount = m_dynamicTable.insertCount();
        if (insertCount <= m_knownReceivedInsertCount)
        {
            // 本端已处理过的插入全都告诉过对端了：这里必须什么都不发，否则就是对同一计数重复告知
            return;
        }
        appendPrefixedInteger(decoderStreamBytes, insertCount - m_knownReceivedInsertCount, 6, kInsertCountIncrementPatternBits);
        m_knownReceivedInsertCount = insertCount;
    }

    void QpackDecoder::eraseBlockedSection(std::uint64_t streamId) noexcept
    {
        const auto streamIterator = m_blockedSectionsByStreamId.find(streamId);
        if (streamIterator == m_blockedSectionsByStreamId.end())
        {
            return;
        }
        streamIterator->second.pop_front();
        if (streamIterator->second.empty())
        {
            m_blockedSectionsByStreamId.erase(streamIterator);
        }
    }

    std::expected<void, QpackError> QpackDecoder::blockStream(std::uint64_t streamId, std::span<const std::uint8_t> section,
                                                             std::uint64_t requiredInsertCount)
    {
        const bool streamAlreadyBlocked = m_blockedSectionsByStreamId.contains(streamId);
        if (!streamAlreadyBlocked && m_blockedSectionsByStreamId.size() >= m_settings.maximumBlockedStreamCount)
        {
            // §2.1.2：超出本端承诺的阻塞流数即对端违规，判 DECOMPRESSION_FAILED
            return std::unexpected(makeQpackError(
                QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 需要阻塞，但本端最多只支持 " +
                                                         std::to_string(m_settings.maximumBlockedStreamCount) +
                                                         " 条阻塞流（RFC 9204 §2.1.2）"));
        }

        m_blockedSectionsByStreamId[streamId].push_back(
            BlockedFieldSection{requiredInsertCount, std::string(toByteView(section))});
        return {};
    }

    std::expected<QpackFieldSectionDecodeStatus, QpackError>
    QpackDecoder::decodeFieldSection(std::uint64_t streamId, std::span<const std::uint8_t> encodedFieldSection,
                                     std::vector<QpackHeaderField> &fields, std::string &decoderStreamBytes)
    {
        fields.clear();
        decoderStreamBytes.clear();

        const std::string_view section = toByteView(encodedFieldSection);
        std::size_t cursor = 0;
        QpackError error;
        std::uint64_t encodedInsertCount = 0;
        std::size_t consumedByteCount = 0;

        // §4.5.1 的前缀：Required Insert Count 是 8 位前缀整数，随后是符号位加 7 位前缀的 Delta Base
        const QpackParseStatus insertCountStatus = decodePrefixedInteger(section, 8, QpackErrorKind::DecompressionFailed,
                                                                        "Required Insert Count", encodedInsertCount,
                                                                        consumedByteCount, &error);
        if (insertCountStatus != QpackParseStatus::Complete)
        {
            return std::unexpected(makeFieldSectionError(insertCountStatus, error, streamId, 0, "Required Insert Count"));
        }
        cursor += consumedByteCount;

        std::expected<std::uint64_t, QpackError> requiredInsertCountResult = decodeRequiredInsertCount(encodedInsertCount);
        if (!requiredInsertCountResult.has_value())
        {
            return std::unexpected(requiredInsertCountResult.error());
        }
        const std::uint64_t requiredInsertCount = *requiredInsertCountResult;

        if (cursor >= section.size())
        {
            return std::unexpected(makeQpackError(QpackErrorKind::DecompressionFailed,
                                                  "流 " + std::to_string(streamId) + " 的头块缺少 Base 那一字节（RFC 9204 §4.5.1）"));
        }
        const bool baseIsBelowRequiredInsertCount = testBit(static_cast<std::uint8_t>(section[cursor]), kSignBitMask);
        std::uint64_t deltaBaseValue = 0;
        const QpackParseStatus baseStatus = decodePrefixedInteger(section.substr(cursor), 7, QpackErrorKind::DecompressionFailed,
                                                                 "Delta Base", deltaBaseValue, consumedByteCount, &error);
        if (baseStatus != QpackParseStatus::Complete)
        {
            return std::unexpected(makeFieldSectionError(baseStatus, error, streamId, 0, "Delta Base"));
        }
        cursor += consumedByteCount;

        if (baseIsBelowRequiredInsertCount && requiredInsertCount <= deltaBaseValue)
        {
            // §4.5.1.2：Base 不得为负，符号位为 1 时要求 Required Insert Count 大于 Delta Base
            return std::unexpected(makeQpackError(
                QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 的符号位为 1，但 Required Insert Count " +
                                                         std::to_string(requiredInsertCount) + " 不大于 Delta Base " +
                                                         std::to_string(deltaBaseValue) + "，Base 会为负（RFC 9204 §4.5.1.2）"));
        }
        const std::uint64_t baseValue = baseIsBelowRequiredInsertCount ? requiredInsertCount - deltaBaseValue - 1
                                                                      : requiredInsertCount + deltaBaseValue;

        if (requiredInsertCount > m_dynamicTable.insertCount())
        {
            // §2.2.1：表还没收到该段需要的插入，挂起这条流并保留原始字节，不把数据放流控窗口
            if (auto blockResult = blockStream(streamId, encodedFieldSection, requiredInsertCount); !blockResult.has_value())
            {
                return std::unexpected(blockResult.error());
            }
            return QpackFieldSectionDecodeStatus::Blocked;
        }

        std::vector<QpackHeaderField> decodedFields;
        std::size_t decodedSizeByteCount = 0;
        std::uint64_t maximumReferencedAbsoluteIndex = 0;
        std::size_t lineCount = 0;
        while (cursor < section.size())
        {
            if (auto lineResult = decodeFieldLineRepresentation(streamId, encodedFieldSection, cursor, baseValue, requiredInsertCount,
                                                               decodedFields, maximumReferencedAbsoluteIndex);
                !lineResult.has_value())
            {
                return std::unexpected(lineResult.error());
            }
            // 每解一个字段行就核对一次：字段行数与大小都是对端可以拉爆本端的维度
            const QpackHeaderField &appended = decodedFields.back();
            decodedSizeByteCount += QpackDynamicTable::entrySizeByteCountOf(appended);
            if (m_settings.maximumFieldSectionSizeByteCount != 0 && decodedSizeByteCount > m_settings.maximumFieldSectionSizeByteCount)
            {
                return std::unexpected(makeQpackError(
                    QpackErrorKind::FieldSectionTooLarge, "流 " + std::to_string(streamId) + " 解到第 " +
                                                              std::to_string(lineCount + 1) + " 个字段行时头段大小已达 " +
                                                              std::to_string(decodedSizeByteCount) +
                                                              " 字节，超过本端 SETTINGS_MAX_FIELD_SECTION_SIZE " +
                                                              std::to_string(m_settings.maximumFieldSectionSizeByteCount) +
                                                              " 字节（RFC 9114 §4.2.2、§10.5.1）"));
            }
            ++lineCount;
        }

        fields = std::move(decodedFields);
        if (requiredInsertCount != 0)
        {
            // §4.4.1：Required Insert Count 非 0 的段必须回 Section Ack，但要等整段交付上层之后才发
            m_unacknowledgedRequiredInsertCountsByStreamId[streamId].push_back(requiredInsertCount);
        }
        appendInsertCountIncrementIfPending(decoderStreamBytes);
        return QpackFieldSectionDecodeStatus::Decoded;
    }

    std::expected<QpackFieldSectionDecodeStatus, QpackError>
    QpackDecoder::resumeBlockedFieldSection(std::uint64_t streamId, std::vector<QpackHeaderField> &fields,
                                           std::string &decoderStreamBytes)
    {
        const auto streamIterator = m_blockedSectionsByStreamId.find(streamId);
        if (streamIterator == m_blockedSectionsByStreamId.end() || streamIterator->second.empty())
        {
            return std::unexpected(makeQpackError(QpackErrorKind::DecompressionFailed,
                                                  "流 " + std::to_string(streamId) + " 没有挂起的头块可续解（RFC 9204 §2.2.1）"));
        }

        // 队首即该流上最早的一段：前一段没解开就不许解后面的（§2.2.1 要求「已开始读的每段」都够）
        if (streamIterator->second.front().requiredInsertCount > m_dynamicTable.insertCount())
        {
            fields.clear();
            decoderStreamBytes.clear();
            return QpackFieldSectionDecodeStatus::Blocked;
        }

        const std::string encodedSection = streamIterator->second.front().encodedFieldSection;
        std::span<const std::uint8_t> retained(reinterpret_cast<const std::uint8_t *>(encodedSection.data()), encodedSection.size());
        auto decodeResult = decodeFieldSection(streamId, retained, fields, decoderStreamBytes);
        if (decodeResult.has_value() && *decodeResult == QpackFieldSectionDecodeStatus::Decoded)
        {
            eraseBlockedSection(streamId);
        }
        return decodeResult;
    }

    std::expected<void, QpackError> QpackDecoder::noteFieldSectionDelivered(std::uint64_t streamId, std::string &decoderStreamBytes)
    {
        decoderStreamBytes.clear();
        const auto streamIterator = m_unacknowledgedRequiredInsertCountsByStreamId.find(streamId);
        if (streamIterator == m_unacknowledgedRequiredInsertCountsByStreamId.end() || streamIterator->second.empty())
        {
            return std::unexpected(makeQpackError(
                QpackErrorKind::DecoderStreamError, "流 " + std::to_string(streamId) +
                                                        " 上没有待确认的头块，不该发 Section Ack（RFC 9204 §4.4.1）"));
        }

        const std::uint64_t requiredInsertCount = streamIterator->second.front();
        streamIterator->second.pop_front();
        if (streamIterator->second.empty())
        {
            m_unacknowledgedRequiredInsertCountsByStreamId.erase(streamIterator);
        }

        appendPrefixedInteger(decoderStreamBytes, streamId, 7, kSectionAckPatternBits);
        if (requiredInsertCount > m_knownReceivedInsertCount)
        {
            // §2.1.4：Ack 隐含确认了该段所需的插入，已知接收计数只前进不回退
            m_knownReceivedInsertCount = requiredInsertCount;
        }
        appendInsertCountIncrementIfPending(decoderStreamBytes);
        return {};
    }

    std::size_t QpackDecoder::emitInsertCountIncrement(std::string &decoderStreamBytes)
    {
        const std::size_t beforeByteCount = decoderStreamBytes.size();
        appendInsertCountIncrementIfPending(decoderStreamBytes);
        return decoderStreamBytes.size() - beforeByteCount;
    }

    void QpackDecoder::noteStreamAbandoned(std::uint64_t streamId, std::string &decoderStreamBytes)
    {
        decoderStreamBytes.clear();
        // §4.4.2：这条流的头块不再处理，告诉对端其上的动态表引用全部作废
        appendPrefixedInteger(decoderStreamBytes, streamId, 6, kStreamCancellationPatternBits);
        m_blockedSectionsByStreamId.erase(streamId);
        m_unacknowledgedRequiredInsertCountsByStreamId.erase(streamId);
    }

    std::expected<std::size_t, QpackError> QpackDecoder::feedEncoderStream(std::span<const std::uint8_t> bytes,
                                                                           std::vector<std::uint64_t> &unblockedStreamIds,
                                                                           std::string &decoderStreamBytes)
    {
        unblockedStreamIds.clear();
        decoderStreamBytes.clear();

        const std::size_t bufferedByteCount = m_encoderStreamBuffer.size();
        m_encoderStreamBuffer.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        std::size_t parsedByteCount = 0;
        while (parsedByteCount < m_encoderStreamBuffer.size())
        {
            const std::string_view remaining = std::string_view(m_encoderStreamBuffer).substr(parsedByteCount);
            const std::uint8_t firstByte = static_cast<std::uint8_t>(remaining[0]);
            QpackError error;
            std::size_t consumedByteCount = 0;

            if ((firstByte & kThreeBitPatternBitMask) == kSetCapacityPatternBits)
            {
                std::uint64_t capacityValue = 0;
                const QpackParseStatus status = decodePrefixedInteger(remaining, 5, QpackErrorKind::EncoderStreamError,
                                                                     "Set Dynamic Table Capacity", capacityValue, consumedByteCount,
                                                                     &error);
                if (status == QpackParseStatus::Incomplete)
                {
                    break;
                }
                if (status != QpackParseStatus::Complete)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(error);
                }
                if (capacityValue > m_settings.maximumTableCapacityByteCount)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    // §3.2.3：容量不得超过本端公布的 SETTINGS_QPACK_MAX_TABLE_CAPACITY
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::EncoderStreamError, "对端把动态表容量设为 " + std::to_string(capacityValue) +
                                                                " 字节，超过本端公布的 " +
                                                                std::to_string(m_settings.maximumTableCapacityByteCount) +
                                                                " 字节（RFC 9204 §3.2.3、§4.3.1）"));
                }
                // RFC 9204 §4.3.1 允许容量在编码器流的任意位置出现，只核对取值不超过上限，不要求它打头。
                // 解码侧的淘汰判据恒真（对端要淘汰什么就淘汰什么），失败即本层表状态自相矛盾
                if (!m_dynamicTable.setCapacityByteCount(static_cast<std::size_t>(capacityValue), nullptr))
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::EncoderStreamError, "把动态表容量设为 " + std::to_string(capacityValue) +
                                                                " 字节后本端表状态无法自洽（RFC 9204 §3.2.2）"));
                }
            }
            else if ((firstByte & kInsertNameReferencePatternBits) == kInsertNameReferencePatternBits)
            {
                // §4.3.2：'1' + T + 名字索引（6 位前缀），随后是 8 位前缀的字段值字面量
                const bool isStaticNameReference = testBit(firstByte, kTableBitInOneBitPatternMask);
                std::uint64_t nameIndexValue = 0;
                QpackParseStatus status = decodePrefixedInteger(remaining, 6, QpackErrorKind::EncoderStreamError,
                                                               "Insert With Name Reference 的名字索引", nameIndexValue,
                                                               consumedByteCount, &error);
                if (status == QpackParseStatus::Incomplete)
                {
                    break;
                }
                if (status != QpackParseStatus::Complete)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(error);
                }

                QpackHeaderField entry;
                if (isStaticNameReference)
                {
                    if (nameIndexValue >= kQpackStaticTable.size())
                    {
                        m_encoderStreamBuffer.erase(0, parsedByteCount);
                        return std::unexpected(makeQpackError(
                            QpackErrorKind::EncoderStreamError, "插入指令引用静态表索引 " + std::to_string(nameIndexValue) +
                                                                    "，超出 " + std::to_string(kQpackStaticTable.size()) +
                                                                    " 项（RFC 9204 §3.1）"));
                    }
                    entry.name = kQpackStaticTable[nameIndexValue].name;
                }
                else
                {
                    QpackHeaderField referenced;
                    if (!m_dynamicTable.tryGetEntryByRelativeIndexFromInsertionPoint(nameIndexValue, referenced))
                    {
                        m_encoderStreamBuffer.erase(0, parsedByteCount);
                        return std::unexpected(makeQpackError(
                            QpackErrorKind::EncoderStreamError, "插入指令引用的动态表相对索引 " + std::to_string(nameIndexValue) +
                                                                    " 已被淘汰或从未插入（RFC 9204 §2.2.3、§3.2.5）"));
                    }
                    entry.name = std::move(referenced.name);
                }

                std::string valueText;
                std::size_t valueConsumedByteCount = 0;
                status = decodePrefixedStringLiteral(remaining.substr(consumedByteCount), 8, QpackErrorKind::EncoderStreamError,
                                                    "字段值", 0, valueText, valueConsumedByteCount, &error);
                if (status == QpackParseStatus::Incomplete)
                {
                    break;
                }
                if (status != QpackParseStatus::Complete)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(error);
                }
                entry.value = std::move(valueText);

                if (!m_dynamicTable.insert(std::move(entry), nullptr).has_value())
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::EncoderStreamError, "插入的表项大于当前动态表容量 " +
                                                                std::to_string(m_dynamicTable.capacityByteCount()) +
                                                                " 字节（RFC 9204 §3.2.2）"));
                }
                consumedByteCount += valueConsumedByteCount;
            }
            else if ((firstByte & kTwoBitPatternBitMask) == kInsertLiteralNamePatternBits)
            {
                // §4.3.3：'01' + 6 位前缀的字段名字面量 + 8 位前缀的字段值字面量
                std::string nameText;
                std::size_t nameConsumedByteCount = 0;
                QpackParseStatus status =
                    decodePrefixedStringLiteral(remaining, 6, QpackErrorKind::EncoderStreamError, "字段名", 0, nameText,
                                                nameConsumedByteCount, &error);
                if (status == QpackParseStatus::Incomplete)
                {
                    break;
                }
                if (status != QpackParseStatus::Complete)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(error);
                }

                std::string valueText;
                std::size_t valueConsumedByteCount = 0;
                status = decodePrefixedStringLiteral(remaining.substr(nameConsumedByteCount), 8, QpackErrorKind::EncoderStreamError,
                                                    "字段值", 0, valueText, valueConsumedByteCount, &error);
                if (status == QpackParseStatus::Incomplete)
                {
                    break;
                }
                if (status != QpackParseStatus::Complete)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(error);
                }

                if (!m_dynamicTable.insert(QpackHeaderField{std::move(nameText), std::move(valueText)}, nullptr).has_value())
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::EncoderStreamError, "插入的表项大于当前动态表容量 " +
                                                                std::to_string(m_dynamicTable.capacityByteCount()) +
                                                                " 字节（RFC 9204 §3.2.2）"));
                }
                consumedByteCount = nameConsumedByteCount + valueConsumedByteCount;
            }
            else if ((firstByte & kThreeBitPatternBitMask) == kDuplicatePatternBits)
            {
                // §4.3.4：'000' + 相对索引（5 位前缀）；名与值都不重发，直接复制表项
                std::uint64_t relativeIndexValue = 0;
                const QpackParseStatus status = decodePrefixedInteger(remaining, 5, QpackErrorKind::EncoderStreamError,
                                                                     "Duplicate 的相对索引", relativeIndexValue, consumedByteCount,
                                                                     &error);
                if (status == QpackParseStatus::Incomplete)
                {
                    break;
                }
                if (status != QpackParseStatus::Complete)
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(error);
                }

                QpackHeaderField sourceEntry;
                if (!m_dynamicTable.tryGetEntryByRelativeIndexFromInsertionPoint(relativeIndexValue, sourceEntry))
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::EncoderStreamError, "Duplicate 引用的动态表相对索引 " + std::to_string(relativeIndexValue) +
                                                                " 已被淘汰或从未插入（RFC 9204 §2.2.3、§4.3.4）"));
                }
                if (!m_dynamicTable.insert(std::move(sourceEntry), nullptr).has_value())
                {
                    m_encoderStreamBuffer.erase(0, parsedByteCount);
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::EncoderStreamError, "Duplicate 复制出的表项大于当前动态表容量 " +
                                                                std::to_string(m_dynamicTable.capacityByteCount()) +
                                                                " 字节（RFC 9204 §3.2.2）"));
                }
            }
            else
            {
                m_encoderStreamBuffer.erase(0, parsedByteCount);
                return std::unexpected(makeQpackError(
                    QpackErrorKind::EncoderStreamError, "编码器流出现无法识别的指令，首字节 " +
                                                            toHexText(std::string_view(reinterpret_cast<const char *>(&firstByte), 1)) +
                                                            "（RFC 9204 §4.3）"));
            }

            parsedByteCount += consumedByteCount;
        }

        m_encoderStreamBuffer.erase(0, parsedByteCount);

        // §2.2.1：插入数追上某段挂起头块的 Required Insert Count，该流即可续解
        for (const auto &[streamId, sections] : m_blockedSectionsByStreamId)
        {
            if (!sections.empty() && sections.front().requiredInsertCount <= m_dynamicTable.insertCount())
            {
                unblockedStreamIds.push_back(streamId);
            }
        }
        appendInsertCountIncrementIfPending(decoderStreamBytes);

        // 指令边界可能骑在上趟残留的半截上，故本趟消费的输入字节按「总解析量 − 上趟残留量」计
        return parsedByteCount > bufferedByteCount ? parsedByteCount - bufferedByteCount : 0;
    }

    std::expected<void, QpackError> QpackDecoder::resolveDynamicReference(std::uint64_t absoluteIndex, std::uint64_t requiredInsertCount,
                                                                          QpackHeaderField &field) const
    {
        if (absoluteIndex >= requiredInsertCount)
        {
            // §2.2.3：引用的绝对索引不小于本段声明的 Required Insert Count，即声明的表状态不足以解这段
            return std::unexpected(makeQpackError(
                QpackErrorKind::DecompressionFailed, "字段行引用了绝对索引 " + std::to_string(absoluteIndex) +
                                                          "，不小于本段的 Required Insert Count " +
                                                          std::to_string(requiredInsertCount) + "（RFC 9204 §2.2.3）"));
        }
        if (!m_dynamicTable.tryGetEntryByAbsoluteIndex(absoluteIndex, field))
        {
            return std::unexpected(makeQpackError(QpackErrorKind::DecompressionFailed,
                                                  "字段行引用的绝对索引 " + std::to_string(absoluteIndex) +
                                                      " 对应的动态表项已被淘汰（RFC 9204 §2.2.3）"));
        }
        return {};
    }

    std::expected<void, QpackError>
    QpackDecoder::decodeFieldLineRepresentation(std::uint64_t streamId, std::span<const std::uint8_t> encodedFieldSection,
                                               std::size_t &cursor, std::uint64_t baseValue, std::uint64_t requiredInsertCount,
                                               std::vector<QpackHeaderField> &fields, std::uint64_t &maximumReferencedAbsoluteIndex)
    {
        const std::string_view section = toByteView(encodedFieldSection);
        const std::size_t lineIndex = fields.size() + 1;
        const std::uint8_t firstByte = static_cast<std::uint8_t>(section[cursor]);
        std::string_view remaining = section.substr(cursor);
        QpackError error;
        std::uint64_t indexValue = 0;
        std::size_t consumedByteCount = 0;
        QpackHeaderField field;

        // 动态表引用统一走这里：先核对是否落在本段声明的表状态内，再取项（§2.2.3）
        const auto referenceDynamicEntry = [this, requiredInsertCount, &field, &maximumReferencedAbsoluteIndex](
                                               std::uint64_t absoluteIndex) -> std::expected<void, QpackError>
        {
            if (auto resolveResult = resolveDynamicReference(absoluteIndex, requiredInsertCount, field); !resolveResult.has_value())
            {
                return resolveResult;
            }
            maximumReferencedAbsoluteIndex = std::max(maximumReferencedAbsoluteIndex, absoluteIndex);
            return {};
        };

        // 只取名字不取值的场景（带名引用的字面量）：名来自表项，值另解字面量
        const auto referenceDynamicName = [this, requiredInsertCount, &field, &maximumReferencedAbsoluteIndex](
                                              std::uint64_t absoluteIndex) -> std::expected<void, QpackError>
        {
            QpackHeaderField referenced;
            if (auto resolveResult = resolveDynamicReference(absoluteIndex, requiredInsertCount, referenced); !resolveResult.has_value())
            {
                return resolveResult;
            }
            field.name = std::move(referenced.name);
            maximumReferencedAbsoluteIndex = std::max(maximumReferencedAbsoluteIndex, absoluteIndex);
            return {};
        };

        if ((firstByte & kOneBitPatternBitMask) == kIndexedPatternBits)
        {
            // §4.5.2：'1' + T + 索引（6 位前缀）
            const QpackParseStatus status = decodePrefixedInteger(remaining, 6, QpackErrorKind::DecompressionFailed, "索引字段行的索引",
                                                                 indexValue, consumedByteCount, &error);
            if (status != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(status, error, streamId, lineIndex, "索引字段行"));
            }
            if (testBit(firstByte, kTableBitInOneBitPatternMask))
            {
                if (indexValue >= kQpackStaticTable.size())
                {
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 的第 " + std::to_string(lineIndex) +
                                                                  " 个字段行引用静态表索引 " + std::to_string(indexValue) + "，超出 " +
                                                                  std::to_string(kQpackStaticTable.size()) + " 项（RFC 9204 §3.1）"));
                }
                field.name = kQpackStaticTable[indexValue].name;
                field.value = kQpackStaticTable[indexValue].value;
            }
            else
            {
                if (indexValue >= baseValue)
                {
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 的第 " + std::to_string(lineIndex) +
                                                                  " 个字段行的相对索引 " + std::to_string(indexValue) + " 不小于 Base " +
                                                                  std::to_string(baseValue) + "，换算出的绝对索引会为负（RFC 9204 §3.2.5）"));
                }
                if (auto referenceResult = referenceDynamicEntry(baseValue - 1 - indexValue); !referenceResult.has_value())
                {
                    return referenceResult;
                }
            }
            cursor += consumedByteCount;
            fields.push_back(std::move(field));
            return {};
        }

        if ((firstByte & kTwoBitPatternBitMask) == kLiteralNameReferencePatternBits)
        {
            // §4.5.4：'01' + N + T + 名字索引（4 位前缀），值是另一个 8 位前缀字面量
            const QpackParseStatus status =
                decodePrefixedInteger(remaining, 4, QpackErrorKind::DecompressionFailed, "带名引用字段行的名字索引", indexValue,
                                      consumedByteCount, &error);
            if (status != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(status, error, streamId, lineIndex, "带名引用的字段行"));
            }
            if (testBit(firstByte, kTableBitInLiteralNameReferenceMask))
            {
                if (indexValue >= kQpackStaticTable.size())
                {
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 的第 " + std::to_string(lineIndex) +
                                                                  " 个字段行引用静态表索引 " + std::to_string(indexValue) + "，超出 " +
                                                                  std::to_string(kQpackStaticTable.size()) + " 项（RFC 9204 §3.1）"));
                }
                field.name = kQpackStaticTable[indexValue].name;
            }
            else
            {
                if (indexValue >= baseValue)
                {
                    return std::unexpected(makeQpackError(
                        QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 的第 " + std::to_string(lineIndex) +
                                                                  " 个字段行的名字相对索引 " + std::to_string(indexValue) +
                                                                  " 不小于 Base " + std::to_string(baseValue) + "（RFC 9204 §4.5.4）"));
                }
                if (auto referenceResult = referenceDynamicName(baseValue - 1 - indexValue); !referenceResult.has_value())
                {
                    return referenceResult;
                }
            }

            std::string valueText;
            std::size_t valueConsumedByteCount = 0;
            const QpackParseStatus valueStatus =
                decodePrefixedStringLiteral(remaining.substr(consumedByteCount), 8, QpackErrorKind::DecompressionFailed, "字段值",
                                            lineIndex, valueText, valueConsumedByteCount, &error);
            if (valueStatus != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(valueStatus, error, streamId, lineIndex, "带名引用的字段行"));
            }
            field.value = std::move(valueText);
            cursor += consumedByteCount + valueConsumedByteCount;
            fields.push_back(std::move(field));
            return {};
        }

        if ((firstByte & kThreeBitPatternBitMask) == kLiteralNamePatternBits)
        {
            // §4.5.6：'001' + N + 4 位前缀的字段名字面量 + 8 位前缀的字段值字面量
            std::string nameText;
            std::size_t nameConsumedByteCount = 0;
            QpackParseStatus status = decodePrefixedStringLiteral(remaining, 4, QpackErrorKind::DecompressionFailed, "字段名",
                                                                 lineIndex, nameText, nameConsumedByteCount, &error);
            if (status != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(status, error, streamId, lineIndex, "双字面量字段行"));
            }

            std::string valueText;
            std::size_t valueConsumedByteCount = 0;
            status = decodePrefixedStringLiteral(remaining.substr(nameConsumedByteCount), 8, QpackErrorKind::DecompressionFailed,
                                                "字段值", lineIndex, valueText, valueConsumedByteCount, &error);
            if (status != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(status, error, streamId, lineIndex, "双字面量字段行"));
            }
            field.name = std::move(nameText);
            field.value = std::move(valueText);
            cursor += nameConsumedByteCount + valueConsumedByteCount;
            fields.push_back(std::move(field));
            return {};
        }

        if ((firstByte & kFourBitPatternBitMask) == kPostBaseIndexedPatternBits)
        {
            // §4.5.3：'0001' + 表后索引（4 位前缀），绝对索引 = Base + 索引
            const QpackParseStatus status = decodePrefixedInteger(remaining, 4, QpackErrorKind::DecompressionFailed, "表后索引",
                                                                 indexValue, consumedByteCount, &error);
            if (status != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(status, error, streamId, lineIndex, "表后索引的字段行"));
            }
            if (auto referenceResult = referenceDynamicEntry(baseValue + indexValue); !referenceResult.has_value())
            {
                return referenceResult;
            }
            cursor += consumedByteCount;
            fields.push_back(std::move(field));
            return {};
        }

        if ((firstByte & kFourBitPatternBitMask) == kPostBaseNameReferencePatternBits)
        {
            // §4.5.5：'0000' + N + 表后名字索引（3 位前缀）。第五位 N（0x08）是给重编码中间方的提示
            // （§7.1.3），本层只按字面量解回，不参与判定，也已被 3 位前缀的掩码挡在索引之外
            const QpackParseStatus status = decodePrefixedInteger(remaining, 3, QpackErrorKind::DecompressionFailed, "表后名字索引",
                                                                 indexValue, consumedByteCount, &error);
            if (status != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(status, error, streamId, lineIndex, "表后名引用的字段行"));
            }
            if (auto referenceResult = referenceDynamicName(baseValue + indexValue); !referenceResult.has_value())
            {
                return referenceResult;
            }

            std::string valueText;
            std::size_t valueConsumedByteCount = 0;
            const QpackParseStatus valueStatus =
                decodePrefixedStringLiteral(remaining.substr(consumedByteCount), 8, QpackErrorKind::DecompressionFailed, "字段值",
                                           lineIndex, valueText, valueConsumedByteCount, &error);
            if (valueStatus != QpackParseStatus::Complete)
            {
                return std::unexpected(makeFieldSectionError(valueStatus, error, streamId, lineIndex, "表后名引用的字段行"));
            }
            field.value = std::move(valueText);
            cursor += consumedByteCount + valueConsumedByteCount;
            fields.push_back(std::move(field));
            return {};
        }

        return std::unexpected(makeQpackError(
            QpackErrorKind::DecompressionFailed, "流 " + std::to_string(streamId) + " 的第 " + std::to_string(lineIndex) +
                                                      " 个字段行首字节 " +
                                                      toHexText(std::string_view(reinterpret_cast<const char *>(&firstByte), 1)) +
                                                      " 不属于任何已定义表示（RFC 9204 §4.5）"));
    }

    std::size_t QpackDecoder::blockedStreamCount() const noexcept
    {
        return m_blockedSectionsByStreamId.size();
    }

    bool QpackDecoder::hasBlockedStreams() const noexcept
    {
        return !m_blockedSectionsByStreamId.empty();
    }

    std::size_t QpackDecoder::tableCapacityByteCount() const noexcept
    {
        return m_dynamicTable.capacityByteCount();
    }

    std::size_t QpackDecoder::dynamicTableSizeByteCount() const noexcept
    {
        return m_dynamicTable.sizeByteCount();
    }

    std::uint64_t QpackDecoder::insertCount() const noexcept
    {
        return m_dynamicTable.insertCount();
    }

    std::uint64_t QpackDecoder::knownReceivedInsertCount() const noexcept
    {
        return m_knownReceivedInsertCount;
    }

    const std::deque<QpackHeaderField> &QpackDecoder::dynamicTableEntries() const noexcept
    {
        return m_dynamicTable.entries();
    }
} // namespace AsynGyanis::Net
