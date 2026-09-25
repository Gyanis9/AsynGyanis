#include "Net/Http2/Hpack.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 索引表示的模式位：1xxxxxxx（RFC 7541 §6.1）
        constexpr std::uint8_t kIndexedRepresentationPattern = 0x80;

        /// 带增量索引的字面量的模式：01xxxxxx（RFC 7541 §6.2.1）
        constexpr std::uint8_t kLiteralIncrementalIndexingMask = 0xC0;
        constexpr std::uint8_t kLiteralIncrementalIndexingPattern = 0x40;

        /// 动态表大小更新的模式：001xxxxx（RFC 7541 §6.3）
        constexpr std::uint8_t kDynamicTableSizeUpdateMask = 0xE0;
        constexpr std::uint8_t kDynamicTableSizeUpdatePattern = 0x20;

        /// 带索引名的字面量里名字索引的前缀位数（01 两位模式 + 6 位索引）
        constexpr std::uint8_t kIndexedNamePrefixBitCount = 6;

        /// 名字也用字面量给出的前缀位数（0000 / 0001 四位模式 + 4 位索引）
        constexpr std::uint8_t kLiteralNamePrefixBitCount = 4;

        /// 字符串字面量里的 H 位（RFC 7541 §5.2）
        constexpr std::uint8_t kHuffmanFlag = 0x80;

        /**
         * @brief 在对外方法入口清空可选出参
         * @details 调用方常复用同一个字符串跨多次调用，若只在失败时写入，成功返回的调用会把上一次的
         *          失败原因留在出参里，「这次是不是失败」的判据随即失效。
         * @param errorText 出参指针，为空时什么都不做
         */
        void clearError(std::string *errorText) noexcept
        {
            if (errorText != nullptr)
            {
                errorText->clear();
            }
        }

        /**
         * @brief 把失败原因写入可选出参
         * @param errorText 出参指针，为空时什么都不做
         * @param reason 中文失败原因，须写清原因与替代做法
         */
        void writeError(std::string *errorText, std::string reason)
        {
            if (errorText != nullptr)
            {
                *errorText = std::move(reason);
            }
        }

        /**
         * @brief Huffman 前缀树的节点
         */
        struct HpackHuffmanDecodeNode
        {
            std::uint16_t childZeroIndex{0}; ///< 读到 0 位时走到的节点下标
            std::uint16_t childOneIndex{0};  ///< 读到 1 位时走到的节点下标
            std::int16_t symbol{-1};         ///< 叶子的符号值（0..255）；-1 表示内部节点
        };

        /// 前缀树节点容量：257 个码字构成的满二叉树最多 2*257-1 = 513 个节点，留出余量
        constexpr std::size_t kHpackHuffmanDecodeNodeCapacity = 2 * (kHpackHuffmanEndOfStringSymbol + 1) + 8;

        /**
         * @brief Huffman 解码表：前缀树节点数组与已用节点数
         */
        struct HpackHuffmanDecodingTable
        {
            std::array<HpackHuffmanDecodeNode, kHpackHuffmanDecodeNodeCapacity> nodes{};
            std::size_t nodeCount{0};
        };

        /**
         * @brief 由码表构造 Huffman 前缀树
         * @details 节点的孩子下标为 0 表示「还没有孩子」，因此根节点（下标 0）永远不会成为别人的孩子，
         *          0 可以安全地当「空」用。表是完整前缀码，构造过程中不可能出现「某个码字是另一个的前缀」。
         * @return HpackHuffmanDecodingTable 编译期常量表
         */
        constexpr HpackHuffmanDecodingTable buildHpackHuffmanDecodingTable()
        {
            HpackHuffmanDecodingTable table;
            table.nodeCount = 1;
            for (std::size_t symbol = 0; symbol < kHpackHuffmanCodeTable.size(); ++symbol)
            {
                const HpackHuffmanCode &code = kHpackHuffmanCodeTable[symbol];
                std::size_t nodeIndex = 0;
                // 码字按 MSB 到 LSB 写出，构造与解码必须同向，否则同一份字节会有两种解释
                for (int bitIndex = static_cast<int>(code.bitCount) - 1; bitIndex >= 0; --bitIndex)
                {
                    const bool isOneBit = ((code.code >> bitIndex) & 1U) != 0;
                    std::uint16_t &childIndex = isOneBit ? table.nodes[nodeIndex].childOneIndex : table.nodes[nodeIndex].childZeroIndex;
                    if (childIndex == 0)
                    {
                        // 用 at() 而不是 []：容量不够时在编译期就报错，不用等到运行期越界
                        childIndex = static_cast<std::uint16_t>(table.nodeCount);
                        table.nodes.at(table.nodeCount).symbol = -1;
                        ++table.nodeCount;
                    }
                    nodeIndex = childIndex;
                }
                table.nodes.at(nodeIndex).symbol = static_cast<std::int16_t>(symbol);
            }
            return table;
        }

        /// 编译期构造好的 Huffman 解码树：解码是热路径，不希望每次调用都重建
        constexpr HpackHuffmanDecodingTable kHpackHuffmanDecodingTable = buildHpackHuffmanDecodingTable();

        /**
         * @brief 算一项在动态表里占的字节数（RFC 7541 §4.1：名长 + 值长 + 32）
         * @param field 待算的头部
         * @return std::size_t 占用字节数
         */
        std::size_t dynamicTableEntrySizeByteCount(const HpackHeaderField &field) noexcept
        {
            return field.name.size() + field.value.size() + kHpackDynamicTableEntryOverheadBytes;
        }

        /**
         * @brief 在动态表里按「名 + 值」精确匹配，取索引空间里的绝对下标
         * @param dynamicTable 待查的动态表
         * @param name 头名
         * @param value 头值
         * @return std::size_t 绝对索引（62 起）；0 表示没有精确匹配
         */
        std::size_t findHpackDynamicTableIndex(const HpackDynamicTable &dynamicTable, const std::string_view name,
                                              const std::string_view value) noexcept
        {
            const std::deque<HpackHeaderField> &entries = dynamicTable.entries();
            for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex)
            {
                if (entries[entryIndex].name == name && entries[entryIndex].value == value)
                {
                    return kHpackFirstDynamicTableIndex + entryIndex;
                }
            }
            return 0;
        }

        /**
         * @brief 二分定位静态表里某个头名的同名段
         * @param name 头名，按字节精确匹配（HPACK 的字符串比较就是逐字节相等，RFC 7541 §5.4）
         * @return const HpackStaticNameRun * 命中时指向编译期索引表里的一项，未命中返回 nullptr
         */
        [[nodiscard]] const HpackStaticNameRun *findHpackStaticNameRun(const std::string_view name) noexcept
        {
            const auto location = std::ranges::lower_bound(kHpackStaticNameRuns, name, {}, &HpackStaticNameRun::name);
            if (location == kHpackStaticNameRuns.end() || location->name != name)
            {
                return nullptr;
            }
            return &*location;
        }

        /**
         * @brief 在静态表的同名段内查「名 + 值」精确匹配
         * @param run 该头名对应的同名段，nullptr 表示名字不在静态表里
         * @param value 头值
         * @return std::size_t 静态表索引（1 起）；0 表示没有精确匹配
         */
        [[nodiscard]] std::size_t findHpackStaticExactIndex(const HpackStaticNameRun *run, const std::string_view value) noexcept
        {
            if (run == nullptr)
            {
                return 0;
            }
            for (std::size_t offset = 0; offset < run->entryCount; ++offset)
            {
                if (kHpackStaticTable[run->firstEntryIndex + offset].value == value)
                {
                    return run->firstEntryIndex + offset + 1;
                }
            }
            return 0;
        }
    } // namespace

    std::size_t findHpackStaticTableIndex(const std::string_view name, const std::string_view value) noexcept
    {
        return findHpackStaticExactIndex(findHpackStaticNameRun(name), value);
    }

    std::size_t findHpackStaticTableNameIndex(const std::string_view name) noexcept
    {
        const HpackStaticNameRun *const run = findHpackStaticNameRun(name);
        // 同名段的首项即「最早出现的该项」，与原本从头扫表取首个命中的结果一致
        return run == nullptr ? 0 : run->firstEntryIndex + 1;
    }

    Http2ErrorCode toHttp2ErrorCode(const HpackErrorKind errorKind) noexcept
    {
        switch (errorKind)
        {
            case HpackErrorKind::None:
                return Http2ErrorCode::NoError;
            case HpackErrorKind::CompressionError:
                // 压缩上下文已经与对端不同步，继续通信只会解出错误的头部（RFC 7540 §4.3）
                return Http2ErrorCode::CompressionError;
            case HpackErrorKind::LimitExceeded:
                return Http2ErrorCode::EnhanceYourCalm;
        }
        return Http2ErrorCode::InternalError;
    }

    void appendHpackInteger(std::string &out, const std::uint64_t value, const std::uint8_t prefixBitCount,
                            const std::uint8_t firstByteHighBits)
    {
        if (prefixBitCount < 1 || prefixBitCount > 8)
        {
            throw Base::InvalidArgumentException(std::format("HPACK 整数表示的前缀位数 {} 不在 1..8 内（RFC 7541 §5.1）："
                                                            "请按表示的位数传 5（大小更新）、6/4（字面量的名字索引）或 7（索引与字符串长度）",
                                                            prefixBitCount));
        }

        const auto prefixMaximumValue = static_cast<std::uint64_t>((1ULL << prefixBitCount) - 1ULL);
        const std::uint8_t prefixMask = static_cast<std::uint8_t>(prefixMaximumValue);
        if ((firstByteHighBits & prefixMask) != 0)
        {
            // 模式位与整数前缀共用首字节：互相覆盖会让对端把表示识别成另一种类型
            throw Base::InvalidArgumentException(
                    std::format("HPACK 整数表示的首字节模式位 0x{:02X} 占用了低 {} 位前缀（RFC 7541 §5.1）：请让模式位只出现在高 {} 位",
                                firstByteHighBits, prefixBitCount, 8 - prefixBitCount));
        }

        if (value < prefixMaximumValue)
        {
            out.push_back(static_cast<char>(firstByteHighBits | static_cast<std::uint8_t>(value)));
            return;
        }

        // 前缀填满表示「后面还有续字节」，剩下的数值按 7 位一组、低位组在前地追加（RFC 7541 §5.1）
        out.push_back(static_cast<char>(firstByteHighBits | prefixMask));
        std::uint64_t remainingValue = value - prefixMaximumValue;
        while (remainingValue >= 128)
        {
            out.push_back(static_cast<char>(static_cast<std::uint8_t>((remainingValue % 128) + 128)));
            remainingValue /= 128;
        }
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(remainingValue)));
    }

    std::string encodeHpackInteger(const std::uint64_t value, const std::uint8_t prefixBitCount, const std::uint8_t firstByteHighBits)
    {
        std::string encoded;
        appendHpackInteger(encoded, value, prefixBitCount, firstByteHighBits);
        return encoded;
    }

    bool decodeHpackInteger(const std::string_view bytes, const std::uint8_t prefixBitCount, std::uint64_t &value,
                            std::size_t &consumedByteCount, std::string *const errorText)
    {
        clearError(errorText);
        value = 0;
        consumedByteCount = 0;
        if (prefixBitCount < 1 || prefixBitCount > 8)
        {
            writeError(errorText, std::format("HPACK 整数表示的前缀位数 {} 不在 1..8 内：请按表示的位数传 5、6、4 或 7", prefixBitCount));
            return false;
        }
        if (bytes.empty())
        {
            writeError(errorText, "HPACK 整数表示缺少首字节：头块已到末尾，请检查对端是否截断了头块");
            return false;
        }

        const std::uint64_t prefixMaximumValue = (1ULL << prefixBitCount) - 1ULL;
        const auto prefixMask = static_cast<std::uint8_t>(prefixMaximumValue);
        value = static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[0]) & prefixMask);
        consumedByteCount = 1;
        if (value < prefixMaximumValue)
        {
            return true;
        }

        // 前缀满值只是「还有续字节」的信号，真值要从后继字节里拼
        std::size_t shiftBitCount = 0;
        while (true)
        {
            if (consumedByteCount >= bytes.size())
            {
                writeError(errorText, std::format("HPACK 整数表示在续字节中间断开（已读 {} 字节）：请检查对端是否截断了头块", consumedByteCount));
                return false;
            }

            const auto continuationByte = static_cast<std::uint8_t>(bytes[consumedByteCount]);
            ++consumedByteCount;
            const auto payloadBits = static_cast<std::uint64_t>(continuationByte & 0x7FU);
            // 溢出判错而不是回绕：回绕后得到的是一个「看起来合法」的值，索引会指向另一条头部
            if (payloadBits > (std::numeric_limits<std::uint64_t>::max() >> shiftBitCount))
            {
                writeError(errorText, std::format("HPACK 整数表示超出 64 位可表示的范围（RFC 7541 §5.1 的整数没有上限，实现必须自己设限）："
                                                  "已读 {} 字节，请检查对端构造",
                                                  consumedByteCount));
                return false;
            }
            value += payloadBits << shiftBitCount;

            // 最高位为 0 表示这是最后一组
            if ((continuationByte & 0x80U) == 0)
            {
                return true;
            }
            shiftBitCount += 7;
            if (shiftBitCount > 63)
            {
                writeError(errorText, std::format("HPACK 整数表示的续字节超过 64 位可表示的范围（已读 {} 字节）：请检查对端构造", consumedByteCount));
                return false;
            }
        }
    }

    bool decodeHpackString(const std::string_view bytes, std::string &value, std::size_t &consumedByteCount,
                           std::string *const errorText)
    {
        clearError(errorText);
        value.clear();
        consumedByteCount = 0;
        if (bytes.empty())
        {
            writeError(errorText, "HPACK 字符串字面量缺少首字节：头块已到末尾，请检查对端是否截断了头块");
            return false;
        }

        // 首字节最高位是 H 位，其余 7 位是长度前缀（RFC 7541 §5.2）
        const bool isHuffmanEncoded = (static_cast<std::uint8_t>(bytes[0]) & kHuffmanFlag) != 0;
        std::uint64_t encodedLength = 0;
        std::size_t lengthByteCount = 0;
        if (!decodeHpackInteger(bytes, 7, encodedLength, lengthByteCount, errorText))
        {
            return false;
        }
        if (encodedLength > static_cast<std::uint64_t>(bytes.size() - lengthByteCount))
        {
            writeError(errorText, std::format("HPACK 字符串声明的长度 {} 字节超过头块剩余 {} 字节：请检查对端是否截断了头块",
                                              encodedLength, bytes.size() - lengthByteCount));
            return false;
        }

        const std::string_view encodedBytes = bytes.substr(lengthByteCount, static_cast<std::size_t>(encodedLength));
        if (isHuffmanEncoded)
        {
            if (!decodeHpackHuffmanString(encodedBytes, value, errorText))
            {
                return false;
            }
        }
        else
        {
            // 二进制安全：按「指针 + 长度」构造，值里可以有 NUL
            value.assign(encodedBytes.data(), encodedBytes.size());
        }
        consumedByteCount = lengthByteCount + static_cast<std::size_t>(encodedLength);
        return true;
    }

    bool decodeHpackHuffmanString(const std::string_view encodedBytes, std::string &value, std::string *const errorText)
    {
        clearError(errorText);
        value.clear();

        // 这三个量都是「自上一个完整符号起」的计数：收口一个符号就清零，结尾剩下的就是填充
        std::size_t pendingBitCount = 0;
        bool arePendingBitsAllOnes = true;
        std::size_t nodeIndex = 0;
        for (const char byteValue: encodedBytes)
        {
            const auto byte = static_cast<std::uint8_t>(byteValue);
            // 位流按 MSB 到 LSB 写：先读到的位是码字的高位
            for (int bitIndex = 7; bitIndex >= 0; --bitIndex)
            {
                const bool isOneBit = ((byte >> bitIndex) & 1U) != 0;
                nodeIndex = isOneBit ? kHpackHuffmanDecodingTable.nodes[nodeIndex].childOneIndex
                                     : kHpackHuffmanDecodingTable.nodes[nodeIndex].childZeroIndex;
                ++pendingBitCount;
                if (!isOneBit)
                {
                    arePendingBitsAllOnes = false;
                }

                const std::int16_t symbol = kHpackHuffmanDecodingTable.nodes[nodeIndex].symbol;
                if (symbol < 0)
                {
                    continue;
                }
                if (static_cast<std::size_t>(symbol) == kHpackHuffmanEndOfStringSymbol)
                {
                    writeError(errorText, "Huffman 编码里出现了 EOS 符号（RFC 7541 §5.2 规定 EOS 只作为填充位型，不得编码成数据）："
                                          "请检查对端的 HPACK 实现");
                    return false;
                }

                value.push_back(static_cast<char>(static_cast<unsigned char>(symbol)));
                // 一个符号收口，剩下的位属于下一个符号或结尾填充
                nodeIndex = 0;
                pendingBitCount = 0;
                arePendingBitsAllOnes = true;
            }
        }

        // 结尾残留的位只能是 EOS（30 位全 1）的最前若干位，且严格少于 8 位（RFC 7541 §5.2）
        if (pendingBitCount > 7)
        {
            writeError(errorText, std::format("Huffman 编码结尾残留 {} 位未成码，超出 7 位的填充上限（RFC 7541 §5.2）："
                                              "请检查对端是否截断了字符串",
                                              pendingBitCount));
            return false;
        }
        if (pendingBitCount > 0 && !arePendingBitsAllOnes)
        {
            writeError(errorText, std::format("Huffman 编码结尾残留 {} 位不是全 1（RFC 7541 §5.2 要求填充取自 EOS 码字的高位）："
                                              "请检查对端的 HPACK 实现",
                                              pendingBitCount));
            return false;
        }
        return true;
    }

    void appendHpackString(std::string &out, const std::string_view value)
    {
        // H 位恒为 0（不启用 Huffman）：长度前缀与字节原样写出，见 HpackEncoder 的取舍说明
        appendHpackInteger(out, value.size(), 7, 0);
        out.append(value);
    }

    HpackDynamicTable::HpackDynamicTable(const std::size_t maximumSizeByteCount) : m_maximumSizeByteCount(maximumSizeByteCount)
    {
        // 上限为 0 是合法状态（等价于禁用动态表），此时表里什么都放不下，构造不做额外处理
    }

    void HpackDynamicTable::setMaximumSizeByteCount(const std::size_t maximumSizeByteCount)
    {
        m_maximumSizeByteCount = maximumSizeByteCount;
        // 收小上限要立刻驱逐放不下的旧项：留着它们会让表大小与两端认知不一致（RFC 7541 §4.3）
        while (m_sizeByteCount > m_maximumSizeByteCount && !m_entries.empty())
        {
            m_sizeByteCount -= dynamicTableEntrySizeByteCount(m_entries.back());
            m_entries.pop_back();
        }
    }

    void HpackDynamicTable::insert(HpackHeaderField field)
    {
        const std::size_t entrySizeByteCount = dynamicTableEntrySizeByteCount(field);
        // 这一项本身就放不进空表：按 §4.4 清空整张表且不插入，否则它会立刻把别的项全挤走
        if (entrySizeByteCount > m_maximumSizeByteCount)
        {
            clear();
            return;
        }

        // 新项永远在最前面（索引 62），驱逐因此总从最旧的一项开始
        while (m_sizeByteCount + entrySizeByteCount > m_maximumSizeByteCount && !m_entries.empty())
        {
            m_sizeByteCount -= dynamicTableEntrySizeByteCount(m_entries.back());
            m_entries.pop_back();
        }

        m_sizeByteCount += entrySizeByteCount;
        m_entries.push_front(std::move(field));
    }

    bool HpackDynamicTable::tryGetEntry(const std::size_t entryIndex, HpackHeaderField &field) const
    {
        if (entryIndex >= m_entries.size())
        {
            return false;
        }
        field = m_entries[entryIndex];
        return true;
    }

    std::size_t HpackDynamicTable::sizeByteCount() const noexcept
    {
        return m_sizeByteCount;
    }

    std::size_t HpackDynamicTable::maximumSizeByteCount() const noexcept
    {
        return m_maximumSizeByteCount;
    }

    std::size_t HpackDynamicTable::entryCount() const noexcept
    {
        return m_entries.size();
    }

    const std::deque<HpackHeaderField> &HpackDynamicTable::entries() const noexcept
    {
        return m_entries;
    }

    void HpackDynamicTable::clear() noexcept
    {
        m_entries.clear();
        m_sizeByteCount = 0;
    }

    HpackDecoder::HpackDecoder(HpackDecoderLimits limits) : m_limits(limits), m_dynamicTable(limits.maximumDynamicTableSizeByteCount)
    {
        // 动态表的初始上限取本端通告的 SETTINGS_HEADER_TABLE_SIZE：对端按同一数值建表，
        // 此后只能由头块开头的「动态表大小更新」在限度内调整
    }

    bool HpackDecoder::decode(const std::string_view headerBlock, std::vector<HpackHeaderField> &headerFields,
                              std::string *const errorText)
    {
        clearError(errorText);
        headerFields.clear();
        // 粘滞错误态下不动动态表：它已经与对端不同步，继续解只会解出错的头部
        if (m_hasError)
        {
            writeError(errorText, m_errorMessage);
            return false;
        }

        // 本轮的结论要在入口处归零：上一轮可能因越限留下 errorKind 与文案，而越限不置粘滞标记，
        // 不清的话这一轮即使解好了，errorKind() 仍会报上一次的结论
        m_isBeyondHeaderLimits = false;
        m_errorKind = HpackErrorKind::None;
        m_errorMessage.clear();
        m_headerListByteCount = 0;
        m_hasSeenHeaderRepresentation = false;
        std::size_t consumed = 0;
        // 表示是自定界的：逐个解到末尾，中途任何失败都由 decodeRepresentation() 记下原因
        while (consumed < headerBlock.size())
        {
            if (!decodeRepresentation(headerBlock, consumed, headerFields))
            {
                // 失败时清空输出：解到一半的字段绝不能留在调用方手里——漏掉「丢弃」这一步的调用方
                // 会把半截头块当成真的用（宁可这里多清一次，也不留这种脚枪）
                headerFields.clear();
                writeError(errorText, m_errorMessage);
                return false;
            }
        }
        // 越限不中断解码循环（动态表必须与对端同步），因此结论在循环走完后才落地
        if (m_isBeyondHeaderLimits)
        {
            headerFields.clear();
            writeError(errorText, m_errorMessage);
            return false;
        }
        return true;
    }

    void HpackDecoder::reset()
    {
        m_dynamicTable = HpackDynamicTable(m_limits.maximumDynamicTableSizeByteCount);
        m_headerListByteCount = 0;
        m_hasSeenHeaderRepresentation = false;
        m_isBeyondHeaderLimits = false;
        m_hasError = false;
        m_errorKind = HpackErrorKind::None;
        m_errorMessage.clear();
    }

    bool HpackDecoder::hasError() const
    {
        return m_hasError;
    }

    HpackErrorKind HpackDecoder::errorKind() const
    {
        return m_errorKind;
    }

    bool HpackDecoder::isLimitExceeded() const noexcept
    {
        return m_isBeyondHeaderLimits;
    }

    std::string HpackDecoder::errorMessage() const
    {
        return m_errorMessage;
    }

    const std::deque<HpackHeaderField> &HpackDecoder::dynamicTableEntries() const noexcept
    {
        return m_dynamicTable.entries();
    }

    std::size_t HpackDecoder::dynamicTableSizeByteCount() const noexcept
    {
        return m_dynamicTable.sizeByteCount();
    }

    std::size_t HpackDecoder::dynamicTableMaximumSizeByteCount() const noexcept
    {
        return m_dynamicTable.maximumSizeByteCount();
    }

    bool HpackDecoder::decodeRepresentation(const std::string_view headerBlock, std::size_t &consumed,
                                            std::vector<HpackHeaderField> &headerFields)
    {
        const auto firstByte = static_cast<std::uint8_t>(headerBlock[consumed]);
        if ((firstByte & kIndexedRepresentationPattern) != 0)
        {
            // 1xxxxxxx：索引表示（RFC 7541 §6.1）
            std::uint64_t index = 0;
            std::size_t indexByteCount = 0;
            if (!decodeHpackInteger(headerBlock.substr(consumed), 7, index, indexByteCount, nullptr))
            {
                recordFailure(HpackErrorKind::CompressionError, "索引表示的整数解不开（RFC 7541 §5.1）：头块中该表示被截断或溢出，请检查对端");
                return false;
            }
            consumed += indexByteCount;
            if (index == 0)
            {
                recordFailure(HpackErrorKind::CompressionError, "索引 0 非法（RFC 7541 §6.1 规定索引从 1 起）：请检查对端的 HPACK 实现");
                return false;
            }

            HpackHeaderField field;
            if (!resolveIndexedField(static_cast<std::size_t>(index), field))
            {
                recordFailure(HpackErrorKind::CompressionError,
                              std::format("索引 {} 既不在静态表（1..{}）也不在动态表（当前 {} 项）里（RFC 7541 §2.3.3）："
                                          "两端的动态表已经不同步，请检查对端是否漏插或错插了条目",
                                          index, kHpackStaticTableEntryCount, m_dynamicTable.entryCount()));
                return false;
            }
            m_hasSeenHeaderRepresentation = true;
            appendField(std::move(field), headerFields);
            return true;
        }

        if ((firstByte & kLiteralIncrementalIndexingMask) == kLiteralIncrementalIndexingPattern)
        {
            // 01xxxxxx：带增量索引的字面量（RFC 7541 §6.2.1），这一项要进动态表
            return decodeLiteralRepresentation(kIndexedNamePrefixBitCount, true, headerBlock, consumed, headerFields);
        }

        if ((firstByte & kDynamicTableSizeUpdateMask) == kDynamicTableSizeUpdatePattern)
        {
            // 001xxxxx：动态表大小更新（RFC 7541 §6.3）
            if (m_hasSeenHeaderRepresentation)
            {
                recordFailure(HpackErrorKind::CompressionError,
                              "动态表大小更新出现在头部之后（RFC 7541 §4.2 要求它出现在头块开头）："
                              "在中间改表会让前后两段的索引指向不同的表状态，请检查对端构造");
                return false;
            }

            std::uint64_t maximumSizeByteCount = 0;
            std::size_t sizeByteCount = 0;
            if (!decodeHpackInteger(headerBlock.substr(consumed), 5, maximumSizeByteCount, sizeByteCount, nullptr))
            {
                recordFailure(HpackErrorKind::CompressionError, "动态表大小更新的整数解不开（RFC 7541 §5.1）：请检查对端");
                return false;
            }
            consumed += sizeByteCount;
            if (maximumSizeByteCount > m_limits.maximumDynamicTableSizeByteCount)
            {
                recordFailure(HpackErrorKind::CompressionError,
                              std::format("动态表大小更新要求 {} 字节，超过本端通告的 SETTINGS_HEADER_TABLE_SIZE {} 字节"
                                          "（RFC 7541 §6.3 要求超出限度即判为解码错误）：请调高 "
                                          "HpackDecoderLimits::maximumDynamicTableSizeByteCount，或让对端改用不大于该值的上限",
                                          maximumSizeByteCount, m_limits.maximumDynamicTableSizeByteCount));
                return false;
            }
            m_dynamicTable.setMaximumSizeByteCount(static_cast<std::size_t>(maximumSizeByteCount));
            return true;
        }

        // 0001xxxx（永不索引，§6.2.3）与 0000xxxx（不索引，§6.2.2）在解码侧完全等价：两者都不插动态表，
        // 区别只是「永不索引」是给中间设施的指示，本片只要求把它正确解析出来
        return decodeLiteralRepresentation(kLiteralNamePrefixBitCount, false, headerBlock, consumed, headerFields);
    }

    bool HpackDecoder::decodeLiteralRepresentation(const std::uint8_t nameIndexPrefixBitCount, const bool isIncrementalIndexing,
                                                   const std::string_view headerBlock, std::size_t &consumed,
                                                   std::vector<HpackHeaderField> &headerFields)
    {
        std::uint64_t nameIndex = 0;
        std::size_t nameIndexByteCount = 0;
        if (!decodeHpackInteger(headerBlock.substr(consumed), nameIndexPrefixBitCount, nameIndex, nameIndexByteCount, nullptr))
        {
            recordFailure(HpackErrorKind::CompressionError, "字面量表示的名字索引解不开（RFC 7541 §5.1）：请检查对端是否截断了头块");
            return false;
        }
        consumed += nameIndexByteCount;

        HpackHeaderField field;
        if (nameIndex == 0)
        {
            // 索引 0 表示名字也用字面量给出（RFC 7541 §6.2.1）
            std::size_t nameByteCount = 0;
            if (!decodeHpackString(headerBlock.substr(consumed), field.name, nameByteCount, nullptr))
            {
                recordFailure(HpackErrorKind::CompressionError,
                              std::format("字面量表示的头名解不开（已消费 {} 字节，头块共 {} 字节）："
                                          "请检查对端是否截断了头块或用了本端不支持的 Huffman 码",
                                          consumed, headerBlock.size()));
                return false;
            }
            consumed += nameByteCount;
        }
        else
        {
            if (!resolveIndexedField(static_cast<std::size_t>(nameIndex), field))
            {
                recordFailure(HpackErrorKind::CompressionError,
                              std::format("字面量表示引用的名字索引 {} 不存在（RFC 7541 §2.3.3）：两端的动态表已经不同步，请检查对端",
                                          nameIndex));
                return false;
            }
        }

        std::size_t valueByteCount = 0;
        if (!decodeHpackString(headerBlock.substr(consumed), field.value, valueByteCount, nullptr))
        {
            recordFailure(HpackErrorKind::CompressionError,
                          std::format("字面量表示的头值解不开（已消费 {} 字节，头块共 {} 字节）："
                                      "请检查对端是否截断了头块或用了本端不支持的 Huffman 码",
                                      consumed, headerBlock.size()));
            return false;
        }
        consumed += valueByteCount;

        if (isIncrementalIndexing)
        {
            // 带增量索引：对端的编码器已经把它加进了自己的表，本端必须同步插入，后续索引才能对上
            m_dynamicTable.insert(HpackHeaderField{field.name, field.value});
        }
        m_hasSeenHeaderRepresentation = true;
        appendField(std::move(field), headerFields);
        return true;
    }

    bool HpackDecoder::resolveIndexedField(const std::size_t index, HpackHeaderField &field) const
    {
        if (index <= kHpackStaticTableEntryCount)
        {
            const HpackStaticTableEntry &entry = kHpackStaticTable[index - 1];
            field.name.assign(entry.name);
            field.value.assign(entry.value);
            return true;
        }
        return m_dynamicTable.tryGetEntry(index - kHpackFirstDynamicTableIndex, field);
    }

    void HpackDecoder::appendField(HpackHeaderField field, std::vector<HpackHeaderField> &headerFields)
    {
        // 已经越限：这一项解出来了（动态表也照常插过），只是不再收集，也不重复记原因
        if (m_isBeyondHeaderLimits)
        {
            return;
        }

        // 单条长度先卡住：单条超长即使总量没超，上层拿到它也没法安全处理
        if (field.name.size() > m_limits.maximumHeaderFieldNameLength)
        {
            noteHeaderLimitExceeded(std::format("头名 {} 字节超出上限 {} 字节：请调高 HpackDecoderLimits::"
                                                "maximumHeaderFieldNameLength，或让对端不要发这么长的头名",
                                                field.name.size(), m_limits.maximumHeaderFieldNameLength));
            return;
        }
        if (field.value.size() > m_limits.maximumHeaderFieldValueLength)
        {
            noteHeaderLimitExceeded(std::format("头值 {} 字节超出上限 {} 字节：请调高 HpackDecoderLimits::"
                                                "maximumHeaderFieldValueLength，或让对端不要发这么长的头值",
                                                field.value.size(), m_limits.maximumHeaderFieldValueLength));
            return;
        }

        // 头列表总大小按 RFC 7540 §6.5.2 的算式累计：每项名长 + 值长 + 32
        m_headerListByteCount += field.name.size() + field.value.size() + kHpackDynamicTableEntryOverheadBytes;
        if (m_headerListByteCount > m_limits.maximumHeaderListByteCount)
        {
            noteHeaderLimitExceeded(std::format("本头块累计的头列表大小 {} 字节超出上限 {} 字节（RFC 7540 §6.5.2 的算式）："
                                                "请调高 HpackDecoderLimits::maximumHeaderListByteCount，或让对端少发头部",
                                                m_headerListByteCount, m_limits.maximumHeaderListByteCount));
            return;
        }

        headerFields.push_back(std::move(field));
    }

    void HpackDecoder::noteHeaderLimitExceeded(std::string reason)
    {
        // 刻意不置 m_hasError：越限的头块已经整块解完，两端动态表仍然同步，
        // 粘滞会让下一条无关的请求把整条连接带走（RFC 9113 §10.5.1 只要求按 431 应答这一条）
        m_isBeyondHeaderLimits = true;
        m_errorKind = HpackErrorKind::LimitExceeded;
        m_errorMessage = "HPACK 头块超出本端上限：" + std::move(reason);
    }

    void HpackDecoder::recordFailure(const HpackErrorKind errorKind, std::string reason)
    {
        m_hasError = true;
        m_errorKind = errorKind;
        // 前缀统一在这里补：调用点只写原因，文案风格不会因为某个分支漏写而不一致
        m_errorMessage = "HPACK 头块解码失败：" + std::move(reason);
    }

    HpackEncoder::HpackEncoder(const std::size_t maximumDynamicTableSizeByteCount) : m_dynamicTable(maximumDynamicTableSizeByteCount)
    {
    }

    void HpackEncoder::setMaximumDynamicTableSizeByteCount(const std::size_t maximumSizeByteCount)
    {
        if (maximumSizeByteCount == m_dynamicTable.maximumSizeByteCount())
        {
            // 上限没变就没有「变化」要通告，多打一个大小更新只是白占字节
            return;
        }
        m_dynamicTable.setMaximumSizeByteCount(maximumSizeByteCount);
        m_hasPendingTableSizeUpdate = true;
    }

    std::size_t HpackEncoder::maximumDynamicTableSizeByteCount() const noexcept
    {
        return m_dynamicTable.maximumSizeByteCount();
    }

    std::string HpackEncoder::encode(const std::vector<HpackHeaderField> &headerFields)
    {
        // owning 输入只为兼容仍按 HpackHeaderField 组织头块的调用方（解码侧往返、测试与微基准）：
        // 这里只拷视图，名与值的字节仍然原地读
        std::vector<HpackHeaderFieldView> headerFieldViews;
        headerFieldViews.reserve(headerFields.size());
        for (const HpackHeaderField &field: headerFields)
        {
            headerFieldViews.push_back(HpackHeaderFieldView{.name = field.name, .value = field.value});
        }
        return encode(std::span<const HpackHeaderFieldView>{headerFieldViews});
    }

    std::string HpackEncoder::encode(const std::span<const HpackHeaderFieldView> headerFieldViews)
    {
        std::string headerBlock;
        // 上层改过表上限时必须先通告：RFC 7541 §4.2 要求它出现在头块开头，放在别处对端会判错
        if (m_hasPendingTableSizeUpdate)
        {
            appendHpackInteger(headerBlock, m_dynamicTable.maximumSizeByteCount(), 5, kDynamicTableSizeUpdatePattern);
            m_hasPendingTableSizeUpdate = false;
        }

        for (const HpackHeaderFieldView &field: headerFieldViews)
        {
            // 一个头只做一次静态表定位：同名段内既查「名 + 值」精确匹配，也顺带给出「仅名」匹配
            const HpackStaticNameRun *const staticNameRun = findHpackStaticNameRun(field.name);
            const std::size_t staticExactIndex = findHpackStaticExactIndex(staticNameRun, field.value);
            if (staticExactIndex != 0)
            {
                appendHpackInteger(headerBlock, staticExactIndex, 7, kIndexedRepresentationPattern);
                continue;
            }

            const std::size_t dynamicIndex = findHpackDynamicTableIndex(m_dynamicTable, field.name, field.value);
            if (dynamicIndex != 0)
            {
                appendHpackInteger(headerBlock, dynamicIndex, 7, kIndexedRepresentationPattern);
                continue;
            }

            // 索引命中不了就发字面量：名字能命中静态表时只发值，否则名与值都发
            const std::size_t staticNameIndex = staticNameRun == nullptr ? 0 : staticNameRun->firstEntryIndex + 1;
            if (staticNameIndex != 0)
            {
                appendHpackInteger(headerBlock, staticNameIndex, kIndexedNamePrefixBitCount, kLiteralIncrementalIndexingPattern);
                appendHpackString(headerBlock, field.value);
            }
            else
            {
                // 名字索引 0 表示名字也是字面量（RFC 7541 §6.2.1）
                appendHpackInteger(headerBlock, 0, kIndexedNamePrefixBitCount, kLiteralIncrementalIndexingPattern);
                appendHpackString(headerBlock, field.name);
                appendHpackString(headerBlock, field.value);
            }

            // 带增量索引的字面量必须两端同时进表：对端的解码器会按同一规则插入并驱逐。
            // 进表这一份是必须拥有的（表要活过本次调用），也是本函数唯一的字符串构造
            m_dynamicTable.insert(HpackHeaderField{std::string(field.name), std::string(field.value)});
        }
        return headerBlock;
    }

    void HpackEncoder::reset()
    {
        m_dynamicTable.clear();
        m_hasPendingTableSizeUpdate = false;
    }

    const std::deque<HpackHeaderField> &HpackEncoder::dynamicTableEntries() const noexcept
    {
        return m_dynamicTable.entries();
    }

    std::size_t HpackEncoder::dynamicTableSizeByteCount() const noexcept
    {
        return m_dynamicTable.sizeByteCount();
    }
} // namespace AsynGyanis::Net
