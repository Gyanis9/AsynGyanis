#include "Net/Http/TraceContext.h"

#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <random>
#include <ranges>
#include <thread>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// traceparent 各段的固定偏移：版本 0..1、'-'@2、trace-id 3..34、'-'@35、parent-id 36..51、'-'@52、flags 53..54
        constexpr std::size_t kVersionFieldStart      = 0U;
        constexpr std::size_t kHexPairLength          = 2U; ///< 版本与 flags 都是「两个十六进制字符」一段
        constexpr std::size_t kTraceIdFieldStart      = 3U;
        constexpr std::size_t kSpanIdFieldStart       = 36U;
        constexpr std::size_t kTraceFlagsFieldStart   = 53U;
        constexpr std::size_t kVersionSeparatorOffset  = 2U;
        constexpr std::size_t kTraceIdSeparatorOffset  = 35U;
        constexpr std::size_t kSpanIdSeparatorOffset   = 52U;
        /// 版本 0 字段的定长口径来自公开常量，校验与调用方截断因此共用一个数
        constexpr std::size_t kTraceparentLength       = kTraceparentCanonicalLength;

        /// 规范里点名作废的两个键（历史实现冲突），不接受也不生成
        constexpr std::string_view kInvalidTraceStateKeys[] = {"congo", "tircongo"};

        /// 小写字母表：产出侧只用这张表，因此本框架发出的字段永远是规范里的那一小串字符
        constexpr char kLowerHexDigits[] = "0123456789abcdef";

        /// 版本 `ff` 是规范里的「不得当正常版本处理」的哨兵值
        constexpr std::uint8_t kInvalidTraceparentVersion = 0xFFU;

        /// 十六进制字符折不出时的返回值（0..15 之外唯一使用的哨兵）
        constexpr std::uint8_t kNotAHexDigit = 0xFFU;

        /**
         * @brief 把一个字符折成十六进制位；不是小写十六进制时返回 kNotAHexDigit
         * @param character 待折的字符
         * @return std::uint8_t 0..15，或哨兵
         */
        constexpr std::uint8_t hexDigitValue(const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return static_cast<std::uint8_t>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<std::uint8_t>(character - 'a' + 10);
            }
            return kNotAHexDigit; // 大写与其余字符都算非法：规范的 ABNF 只收小写
        }

        /**
         * @brief 判定一段文本是否为「全 0」的十六进制段（全零的 trace-id 与 parent-id 都非法）
         * @param field 待判定的段
         * @return true 每一个字符都是 '0'
         */
        bool isAllZeroHexField(const std::string_view field) noexcept
        {
            return std::ranges::all_of(field, [](const char character)
                                      {
                                          return character == '0';
                                      });
        }

        /**
         * @brief 把一段小写十六进制折成字节值
         * @param field 恰好两个字符的段
         * @return std::uint8_t 折出的字节；任一字符非法时结果无意义，调用方须先用 areLowerHexDigits 判过
         */
        constexpr std::uint8_t hexByte(const std::string_view field) noexcept
        {
            return static_cast<std::uint8_t>((hexDigitValue(field[0]) << 4U) | hexDigitValue(field[1]));
        }

        /**
         * @brief 判定整段是否全是小写十六进制字符
         * @param field 待判定的段
         * @return true 每个字符都能折成十六进制位
         */
        bool areLowerHexDigits(const std::string_view field) noexcept
        {
            return std::ranges::all_of(field,
                                       [](const char character)
                                       {
                                           return hexDigitValue(character) != kNotAHexDigit;
                                       });
        }

        /**
         * @brief 把定长随机字节写成小写十六进制文本（含结尾 NUL）
         * @tparam DigitCount 十六进制位数，即源字节数的两倍
         * @tparam SourceSize  源字节数组长度
         * @param target 输出数组，长度必须是 DigitCount + 1
         * @param source 源字节
         */
        template<std::size_t DigitCount, std::size_t SourceSize>
        constexpr void writeHexText(std::array<char, DigitCount + 1U> &target, const std::array<std::uint8_t, SourceSize> &source) noexcept
        {
            for (std::size_t byteIndex = 0; byteIndex < SourceSize; ++byteIndex)
            {
                target[byteIndex * 2U]       = kLowerHexDigits[(source[byteIndex] >> 4U) & 0x0FU];
                target[byteIndex * 2U + 1U]  = kLowerHexDigits[source[byteIndex] & 0x0FU];
            }
            target[DigitCount] = '\0';
        }

        /**
         * @brief 线程局部的随机源：进程内首次调用时播种，之后每条新链路只要三次取数
         * @details 用 splitmix64 而不是 std::mt19937：状态只有 8 字节、每次取值一条乘法，
         *          而链路标识要的只是「跨进程不撞」。种子混入 random_device 与线程 id 和时钟，
         *          为的是多个线程同时首次进入时不共用同一条序列。
         * @return std::uint64_t 下一个 64 位随机值
         */
        std::uint64_t nextRandomWord() noexcept
        {
            thread_local std::uint64_t state = []
            {
                std::uint64_t seed = std::random_device{}();
                seed               = seed * 6364136223846793005ULL
                     ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                seed ^= std::hash<std::thread::id>{}(std::this_thread::get_id()) + 0x9E3779B97F4A7C15ULL;
                return seed;
            }();

            state += 0x9E3779B97F4A7C15ULL;
            std::uint64_t mixed = state;
            mixed ^= mixed >> 30U;
            mixed *= 0xBF58476D1CE4E5B9ULL;
            mixed ^= mixed >> 27U;
            mixed *= 0x94D049BB133111EBULL;
            mixed ^= mixed >> 31U;
            return mixed;
        }

        /**
         * @brief 取一段定长随机字节，全零时重取（全零的标识在规范里是非法值）
         * @tparam ByteCount 需要的字节数
         * @return std::array<std::uint8_t, ByteCount> 随机字节
         */
        template<std::size_t ByteCount>
        std::array<std::uint8_t, ByteCount> randomIdentifierBytes() noexcept
        {
            std::array<std::uint8_t, ByteCount> bytes{};
            do
            {
                for (std::size_t offset = 0; offset < ByteCount; offset += 8U)
                {
                    const std::uint64_t word = nextRandomWord();
                    for (std::size_t inner = 0; inner < 8U && offset + inner < ByteCount; ++inner)
                    {
                        bytes[offset + inner] = static_cast<std::uint8_t>(word >> (inner * 8U));
                    }
                }
            } while (std::ranges::all_of(bytes, [](const std::uint8_t byteValue)
                                        {
                                            return byteValue == 0U;
                                        }));
            return bytes;
        }

        /**
         * @brief 判定一个 tracestate 键字符是否在允许的字符集内
         * @param character 待判字符
         * @return true 属于 `a-z 0-9 _ - . @ / *`
         */
        constexpr bool isTraceStateKeyCharacter(const char character) noexcept
        {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_'
                   || character == '-' || character == '.' || character == '@' || character == '/' || character == '*';
        }
    } // namespace

    std::string_view TraceIdentifiers::traceIdText() const noexcept
    {
        return std::string_view(traceId.data(), kTraceIdHexDigitCount);
    }

    std::string_view TraceIdentifiers::parentIdText() const noexcept
    {
        return std::string_view(parentId.data(), kSpanIdHexDigitCount);
    }

    bool TraceIdentifiers::isSampled() const noexcept
    {
        return (flags & kSampledTraceFlag) != 0U;
    }

    void TraceIdentifiers::setSampled(const bool sampled) noexcept
    {
        flags = sampled ? static_cast<std::uint8_t>(flags | kSampledTraceFlag) : static_cast<std::uint8_t>(flags & ~kSampledTraceFlag);
    }

    std::optional<TraceIdentifiers> Traceparent::parse(const std::string_view value) noexcept
    {
        if (value.size() < kTraceparentLength)
        {
            return std::nullopt;
        }
        if (value[kVersionSeparatorOffset] != '-' || value[kTraceIdSeparatorOffset] != '-' || value[kSpanIdSeparatorOffset] != '-')
        {
            return std::nullopt;
        }

        const std::string_view versionField   = value.substr(kVersionFieldStart, kHexPairLength);
        const std::string_view traceIdField   = value.substr(kTraceIdFieldStart, kTraceIdHexDigitCount);
        const std::string_view spanIdField    = value.substr(kSpanIdFieldStart, kSpanIdHexDigitCount);
        const std::string_view traceFlagsField = value.substr(kTraceFlagsFieldStart, kHexPairLength);
        if (!areLowerHexDigits(versionField) || !areLowerHexDigits(traceIdField) || !areLowerHexDigits(spanIdField)
            || !areLowerHexDigits(traceFlagsField))
        {
            return std::nullopt;
        }

        const std::uint8_t version = hexByte(versionField);
        if (version == kInvalidTraceparentVersion)
        {
            return std::nullopt; // 规范：ff 不是版本，是「本字段不可用」的哨兵
        }

        // 版本 0 的字段就是那 55 个字节，多一个字符都是畸形；更高的版本允许在 flags 之后再挂
        // 由该版本自己定义的附加字段，本实现不认识其含义，但必须整条采信并原样传递（§3.2.2）
        if (value.size() > kTraceparentLength && (version == 0U || value[kTraceparentLength] != '-'))
        {
            return std::nullopt;
        }

        // 全零的 trace-id 与 parent-id 都是非法值：它们表示「没有标识」，不能被当成一条真链路
        if (isAllZeroHexField(traceIdField) || isAllZeroHexField(spanIdField))
        {
            return std::nullopt;
        }

        TraceIdentifiers identifiers;
        identifiers.version = version;
        identifiers.flags   = hexByte(traceFlagsField);
        identifiers.traceId.fill('\0');
        identifiers.parentId.fill('\0');
        std::ranges::copy(traceIdField, identifiers.traceId.begin());
        std::ranges::copy(spanIdField, identifiers.parentId.begin());
        return identifiers;
    }

    TraceIdentifiers Traceparent::generate(const bool isSampled) noexcept
    {
        TraceIdentifiers identifiers;
        identifiers.version = 0U;
        identifiers.flags   = isSampled ? kSampledTraceFlag : 0U;

        const auto traceIdBytes = randomIdentifierBytes<kTraceIdHexDigitCount / 2U>();
        const auto spanIdBytes  = randomIdentifierBytes<kSpanIdHexDigitCount / 2U>();
        writeHexText<kTraceIdHexDigitCount>(identifiers.traceId, traceIdBytes);
        writeHexText<kSpanIdHexDigitCount>(identifiers.parentId, spanIdBytes);
        return identifiers;
    }

    void Traceparent::renderInto(std::string &target, const TraceIdentifiers &identifiers)
    {
        target.resize(kTraceparentLength);
        std::size_t cursor = 0;
        const auto appendHexByte = [&target, &cursor](const std::uint8_t byteValue)
        {
            target[cursor++] = kLowerHexDigits[(byteValue >> 4U) & 0x0FU];
            target[cursor++] = kLowerHexDigits[byteValue & 0x0FU];
        };

        appendHexByte(identifiers.version);
        target[cursor++] = '-';
        cursor           = identifiers.traceIdText().copy(&target[cursor], kTraceIdHexDigitCount) + cursor;
        target[cursor++] = '-';
        cursor           = identifiers.parentIdText().copy(&target[cursor], kSpanIdHexDigitCount) + cursor;
        target[cursor++] = '-';
        appendHexByte(identifiers.flags);
    }

    std::string Traceparent::value(const TraceIdentifiers &identifiers)
    {
        std::string valueText;
        renderInto(valueText, identifiers);
        return valueText;
    }

    std::optional<TraceState> TraceState::parse(std::string_view value)
    {
        TraceState state;
        // 整条字段为空等价于「没有条目」：这是合法的缺席，不是畸形
        if (trimOptionalWhitespace(value).empty())
        {
            return state;
        }

        std::size_t cursor = 0;
        while (true)
        {
            const std::size_t separatorOffset = value.find(',', cursor);
            const std::size_t memberEnd       = separatorOffset == std::string_view::npos ? value.size() : separatorOffset;
            const std::string_view member     = trimOptionalWhitespace(value.substr(cursor, memberEnd - cursor));
            // 走到这里空条目就是畸形：多出来的一个逗号会在中间留下一个空成员
            const std::size_t equalOffset = member.find('=');
            if (member.empty() || equalOffset == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::string_view key        = trimOptionalWhitespace(member.substr(0, equalOffset));
            const std::string_view entryValue = trimOptionalWhitespace(member.substr(equalOffset + 1U));
            if (!isValidKey(key) || !isValidValue(entryValue))
            {
                return std::nullopt;
            }
            const bool isDuplicated = std::ranges::any_of(state.m_entries,
                                                          [&key](const TraceStateEntry &entry)
                                                          {
                                                              return entry.key == key;
                                                          });
            if (isDuplicated)
            {
                return std::nullopt; // 规范把重复键整条判废：半条可信比全都不可信更糟
            }
            state.m_entries.push_back(TraceStateEntry{std::string(key), std::string(entryValue)});

            if (separatorOffset == std::string_view::npos)
            {
                break;
            }
            cursor = separatorOffset + 1U;
        }
        return state;
    }

    bool TraceState::isValidKey(const std::string_view key) noexcept
    {
        if (key.empty() || key.size() > kMaximumTraceStateTokenLength)
        {
            return false;
        }
        if (const auto reserved = std::ranges::find(kInvalidTraceStateKeys, key); reserved != std::ranges::end(kInvalidTraceStateKeys))
        {
            return false;
        }

        // 首字符必须是字母或数字：`*` 与 `-` 打头的键在规范的 ABNF 里都不存在
        const char firstCharacter = key.front();
        if (!((firstCharacter >= 'a' && firstCharacter <= 'z') || (firstCharacter >= '0' && firstCharacter <= '9')))
        {
            return false;
        }
        return std::ranges::all_of(key,
                                   [](const char character)
                                   {
                                       return isTraceStateKeyCharacter(character);
                                   });
    }

    bool TraceState::isValidValue(const std::string_view value) noexcept
    {
        if (value.size() > kMaximumTraceStateTokenLength)
        {
            return false;
        }
        return std::ranges::all_of(value,
                                   [](const char character)
                                   {
                                       // 可打印 ASCII，且不含分隔符：逗号是条目边界，留在值里就会撕裂字段
                                       return character >= 0x20 && character <= 0x7E && character != ',';
                                   });
    }

    bool TraceState::upsertFront(const std::string_view key, const std::string_view value)
    {
        if (!isValidKey(key) || !isValidValue(value))
        {
            return false;
        }
        // 规范要求的三步：插到最前、丢掉后面的同名旧条目、按上限从尾部截断（§3.2.4.1）
        std::erase_if(m_entries,
                      [&key](const TraceStateEntry &entry)
                      {
                          return entry.key == key;
                      });
        m_entries.insert(m_entries.begin(), TraceStateEntry{std::string(key), std::string(value)});
        if (m_entries.size() > kMaximumTraceStateEntryCount)
        {
            m_entries.resize(kMaximumTraceStateEntryCount);
        }
        return true;
    }

    std::size_t TraceState::entryCount() const noexcept
    {
        return m_entries.size();
    }

    const std::vector<TraceStateEntry> &TraceState::entries() const noexcept
    {
        return m_entries;
    }

    void TraceState::renderInto(std::string &target, const std::size_t maximumEntryCount) const
    {
        target.clear();
        const std::size_t renderedCount = std::min(m_entries.size(), maximumEntryCount);
        for (std::size_t index = 0; index < renderedCount; ++index)
        {
            if (index != 0U)
            {
                target.push_back(',');
            }
            target.append(m_entries[index].key);
            target.push_back('=');
            target.append(m_entries[index].value);
        }
    }

    std::optional<TraceIdentifiers> extractTraceContext(const HttpRequest &request)
    {
        // 「缺席」「恰好一条」与「多于一条」必须分开：两条 traceparent 是有歧义的输入，
        // 猜首条还是猜末条都是替上游做决定，因此按「不在任何链路里」处理（中间件随即重起一条）。
        // 计数走的是权威记录，不拷贝任何取值
        if (request.headerFieldCount(kTraceparentHeaderName) != 1U)
        {
            return std::nullopt;
        }
        const std::optional<std::string_view> headerValue = request.firstHeaderValueView(kTraceparentHeaderName);
        if (!headerValue.has_value())
        {
            return std::nullopt;
        }
        return Traceparent::parse(trimOptionalWhitespace(*headerValue));
    }
} // namespace AsynGyanis::Net
