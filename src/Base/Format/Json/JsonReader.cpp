/**
 * @file JsonReader.cpp
 * @brief JSON 流式（SAX）事件读取器：push 喂入 / pull 取事件，支持跨分片边界的 token
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonReader.h"

#include "Base/Format/FormatError.h"
#include "Base/Format/TextEscapes.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 数字 token 的扫描结论
         */
        struct NumberScanOutcome
        {
            bool        isComplete{false}; ///< true 表示已能完整判定 token 结束
            std::size_t endIndex{0};       ///< token 结束偏移（相对传入视图起点）
        };

        /**
         * @brief 构造一条事件
         * @param type 事件类型
         * @param text 文本载荷
         * @param position 事件起始位置
         * @return JsonEvent 事件对象
         */
        JsonEvent makeEvent(const JsonEventType type, std::string text, const TextPosition &position)
        {
            JsonEvent event;
            event.type     = type;
            event.text     = std::move(text);
            event.position = position;
            return event;
        }

        /**
         * @brief 判断字符是否为 JSON 空白
         * @param character 待判定字符
         * @return true 属于空格、制表、换行或回车
         */
        bool isJsonWhitespace(const char character) noexcept
        {
            // RFC 8259 §2：JSON 空白只有这四个字符
            return character == ' ' || character == '\t' || character == '\n' || character == '\r';
        }

        /**
         * @brief 判断字符是否为十进制数字
         * @param character 待判定字符
         * @return true 属于 [0-9]
         */
        bool isDigit(const char character) noexcept
        {
            return character >= '0' && character <= '9';
        }

        /**
         * @brief 判断字节是否落在给定闭区间内
         * @param character 待判定字符
         * @param minimum 区间下界（含）
         * @param maximum 区间上界（含）
         * @return true 落在区间内
         */
        bool isByteInRange(const char character, const unsigned char minimum, const unsigned char maximum) noexcept
        {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= minimum && byte <= maximum;
        }

        /**
         * @brief 按 RFC 8259 §6 扫描一个数字 token
         * @details 只做语法扫描，不做类型与范围判定；缓冲在语法中间截断时返回
         *          `isComplete = false`，由调用方等更多输入后从 token 起点重扫。
         * @param text 以 token 起点开始的输入视图
         * @param isFinished 输入是否已声明结束（决定截断是「需更多输入」还是错误）
         * @param position token 起始位置，用于报错
         * @return NumberScanOutcome 扫描结论
         * @throws FormatError 数字语法非法
         */
        NumberScanOutcome scanNumberToken(const std::string_view text, const bool isFinished, const TextPosition &position)
        {
            std::size_t index = 0;

            if (text[index] == '-')
            {
                ++index;
            }

            // 整数部分：必须至少有一位数字
            if (index >= text.size())
            {
                if (!isFinished)
                {
                    return NumberScanOutcome{};
                }
                throw FormatError(FormatErrorKind::InvalidNumber, "数字必须以数字开头", position);
            }

            bool isSingleZero = false;
            if (text[index] == '0')
            {
                // 前导零：'0' 之后不得再跟数字（RFC 8259 §6）
                isSingleZero = true;
                ++index;
            } else if (isDigit(text[index]))
            {
                while (index < text.size() && isDigit(text[index]))
                {
                    ++index;
                }
            } else
            {
                throw FormatError(FormatErrorKind::InvalidNumber, "数字必须以数字开头", position);
            }

            if (isSingleZero && index < text.size() && isDigit(text[index]))
            {
                throw FormatError(FormatErrorKind::InvalidNumber, "数字不得有前导零", position);
            }

            if (index >= text.size())
            {
                if (!isFinished)
                {
                    return NumberScanOutcome{};
                }
                return NumberScanOutcome{.isComplete = true, .endIndex = index};
            }

            // 小数部分：小数点后必须至少有一位数字
            if (text[index] == '.')
            {
                ++index;
                if (index >= text.size() || !isDigit(text[index]))
                {
                    if (!isFinished && index >= text.size())
                    {
                        return NumberScanOutcome{};
                    }
                    throw FormatError(FormatErrorKind::InvalidNumber, "小数点后必须至少有一位数字", position);
                }
                while (index < text.size() && isDigit(text[index]))
                {
                    ++index;
                }

                if (index >= text.size())
                {
                    if (!isFinished)
                    {
                        return NumberScanOutcome{};
                    }
                    return NumberScanOutcome{.isComplete = true, .endIndex = index};
                }
            }

            // 指数部分：e/E 后可带正负号，但必须至少有一位数字
            if (text[index] == 'e' || text[index] == 'E')
            {
                ++index;
                if (index >= text.size())
                {
                    if (!isFinished)
                    {
                        return NumberScanOutcome{};
                    }
                    throw FormatError(FormatErrorKind::InvalidNumber, "指数部分必须至少有一位数字", position);
                }
                if (text[index] == '+' || text[index] == '-')
                {
                    ++index;
                }
                if (index >= text.size())
                {
                    if (!isFinished)
                    {
                        return NumberScanOutcome{};
                    }
                    throw FormatError(FormatErrorKind::InvalidNumber, "指数部分必须至少有一位数字", position);
                }
                if (!isDigit(text[index]))
                {
                    throw FormatError(FormatErrorKind::InvalidNumber, "指数部分必须至少有一位数字", position);
                }
                while (index < text.size() && isDigit(text[index]))
                {
                    ++index;
                }

                if (index >= text.size() && !isFinished)
                {
                    return NumberScanOutcome{};
                }
            }

            return NumberScanOutcome{.isComplete = true, .endIndex = index};
        }

        /**
         * @brief 校验数字 token 的取值是否可表示
         * @details 与 JsonParser 的取值级联保持一致：非负整数依次尝试 int64_t → uint64_t → double，
         *          负整数尝试 int64_t → double，全部失败视为超出范围。
         * @param token 数字原文
         * @param position token 起始位置，用于报错
         * @throws FormatError 超出可表示范围
         */
        void validateNumberRange(const std::string_view token, const TextPosition &position)
        {
            const bool isNegative = !token.empty() && token.front() == '-';

            if (!isNegative)
            {
                std::int64_t signedInteger = 0;
                if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), signedInteger);
                    errorCode == std::errc() && pointer == token.data() + token.size())
                {
                    return;
                }

                std::uint64_t unsignedInteger = 0;
                if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), unsignedInteger);
                    errorCode == std::errc() && pointer == token.data() + token.size())
                {
                    return;
                }
            } else
            {
                std::int64_t signedInteger = 0;
                if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), signedInteger);
                    errorCode == std::errc() && pointer == token.data() + token.size())
                {
                    return;
                }
            }

            // 退化为 double：能表示即接受（与 JsonParser 一致，避免凭空造数）
            double floatingPoint = 0.0;
            if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), floatingPoint);
                errorCode == std::errc() && pointer == token.data() + token.size())
            {
                return;
            }

            throw FormatError(FormatErrorKind::NumberOutOfRange, "数字超出范围或格式错误：" + std::string(token), position);
        }
    } // namespace

    JsonReader::JsonReader(const JsonParseOptions &options) :
        m_options(options)
    {
    }

    void JsonReader::feed(const std::string_view chunk)
    {
        if (chunk.empty())
        {
            return;
        }

        // 长度上限在追加前判定：累计缓冲一次性拦下超限输入
        if (m_options.maximumInputLength != 0 && m_buffer.size() + chunk.size() > m_options.maximumInputLength)
        {
            throw FormatError(FormatErrorKind::SizeExceeded,
                              std::format("输入长度超出上限：{} 字节", m_options.maximumInputLength),
                              currentPosition());
        }

        m_buffer.append(chunk);
    }

    void JsonReader::finish()
    {
        // 幂等：重复调用不会改变扫描结果
        m_finished = true;
    }

    bool JsonReader::hasNext()
    {
        if (m_pending.has_value())
        {
            return true;
        }

        // 预取一条事件放入单槽前瞻，使 nextEvent() 不会因为探测而丢失事件
        m_pending = produceNextEvent();
        return m_pending.has_value();
    }

    std::optional<JsonEvent> JsonReader::nextEvent()
    {
        if (m_pending.has_value())
        {
            std::optional<JsonEvent> event = std::move(m_pending);
            m_pending.reset();
            return event;
        }

        return produceNextEvent();
    }

    std::optional<JsonEvent> JsonReader::produceNextEvent()
    {
        // 流结束事件之后不再产生任何事件
        if (m_streamEnded)
        {
            return std::nullopt;
        }

        // 首事件恒为 StreamStart：与输入是否就绪无关，便于消费方统一处理流边界
        if (!m_streamStarted)
        {
            m_streamStarted = true;
            return makeEvent(JsonEventType::StreamStart, std::string{}, currentPosition());
        }

        if (!ensureScanReady())
        {
            return std::nullopt;
        }

        while (true)
        {
            const ScanPhase phase = currentPhase();

            switch (phase)
            {
                case ScanPhase::RootValue:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::EmptyInput, "输入为空", currentPosition());
                    }
                    return scanValueEvent();
                }

                case ScanPhase::RootEnd:
                {
                    // ensureScanReady 已跳过全部空白与注释，此处还有内容即为文档尾部残留
                    if (m_cursor < m_buffer.size())
                    {
                        throw FormatError(FormatErrorKind::TrailingContent, "文档结束后出现意外内容", currentPosition());
                    }
                    if (!m_finished)
                    {
                        return std::nullopt;
                    }

                    m_streamEnded = true;
                    return makeEvent(JsonEventType::StreamEnd, std::string{}, currentPosition());
                }

                case ScanPhase::ArrayFirst:
                case ScanPhase::ArrayElement:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::UnterminatedContainer, "数组未闭合", currentPosition());
                    }
                    // 空数组恒合法；',' 之后紧跟 ']' 只在宽松模式下合法（尾逗号）
                    if (peekByte(m_cursor) == ']' && (phase == ScanPhase::ArrayFirst || m_options.allowTrailingCommas))
                    {
                        return closeContainer();
                    }
                    // 其余情况都交给值扫描：值位置上的非法字节由它统一报 UnexpectedByte
                    return scanValueEvent();
                }

                case ScanPhase::ArrayDelimiter:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::UnterminatedContainer, "数组未闭合", currentPosition());
                    }

                    const char byte = peekByte(m_cursor);
                    if (byte == ',')
                    {
                        advanceTo(m_cursor + 1);
                        setCurrentPhase(ScanPhase::ArrayElement);
                    } else if (byte == ']')
                    {
                        return closeContainer();
                    } else
                    {
                        throw FormatError(FormatErrorKind::UnexpectedByte, "数组内应为 ',' 或 ']'", currentPosition());
                    }

                    if (!ensureScanReady())
                    {
                        return std::nullopt;
                    }
                    continue;
                }

                case ScanPhase::ObjectFirstKey:
                case ScanPhase::ObjectKey:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::UnterminatedContainer, "对象未闭合", currentPosition());
                    }
                    if (peekByte(m_cursor) == '}' && (phase == ScanPhase::ObjectFirstKey || m_options.allowTrailingCommas))
                    {
                        return closeContainer();
                    }
                    return scanKeyEvent();
                }

                case ScanPhase::ObjectColon:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::UnterminatedContainer, "对象未闭合", currentPosition());
                    }
                    if (peekByte(m_cursor) != ':')
                    {
                        throw FormatError(FormatErrorKind::UnexpectedByte, "对象键之后应为 ':'", currentPosition());
                    }

                    advanceTo(m_cursor + 1);
                    setCurrentPhase(ScanPhase::ObjectValue);
                    if (!ensureScanReady())
                    {
                        return std::nullopt;
                    }
                    continue;
                }

                case ScanPhase::ObjectValue:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::UnterminatedContainer, "对象未闭合", currentPosition());
                    }
                    return scanValueEvent();
                }

                case ScanPhase::ObjectDelimiter:
                {
                    if (m_cursor >= m_buffer.size())
                    {
                        if (!m_finished)
                        {
                            return std::nullopt;
                        }
                        throw FormatError(FormatErrorKind::UnterminatedContainer, "对象未闭合", currentPosition());
                    }

                    const char byte = peekByte(m_cursor);
                    if (byte == ',')
                    {
                        advanceTo(m_cursor + 1);
                        setCurrentPhase(ScanPhase::ObjectKey);
                    } else if (byte == '}')
                    {
                        return closeContainer();
                    } else
                    {
                        throw FormatError(FormatErrorKind::UnexpectedByte, "对象内应为 ',' 或 '}'", currentPosition());
                    }

                    if (!ensureScanReady())
                    {
                        return std::nullopt;
                    }
                    continue;
                }
            }

            // 枚举已穷尽，仅作兜底：内部状态机不应到此
            throw FormatError(FormatErrorKind::UnexpectedByte, "内部扫描状态异常", currentPosition());
        }
    }

    bool JsonReader::ensureScanReady()
    {
        if (!m_bomResolved && !resolveUtf8Bom())
        {
            return false;
        }

        while (true)
        {
            // 空白可以放心提交：单字节即可判定，不存在跨片歧义
            while (m_cursor < m_buffer.size() && isJsonWhitespace(m_buffer[m_cursor]))
            {
                advanceTo(m_cursor + 1);
            }

            if (m_cursor >= m_buffer.size())
            {
                // 输入未结束时「到此为止」只说明需要更多数据，不能提前判错
                return m_finished;
            }

            if (!m_options.allowComments || peekByte(m_cursor) != '/')
            {
                return true;
            }

            const char following = peekByte(m_cursor + 1);
            if (following != '/' && following != '*')
            {
                if (m_cursor + 1 < m_buffer.size() || m_finished)
                {
                    // 已能确定不是注释引导符：留给上层按「非法字节」报错
                    return true;
                }
                // 只喂到孤立的 '/'：可能是被切断的注释引导符，等更多输入
                return false;
            }

            std::size_t commentEnd = 0;
            if (scanComment(m_cursor, commentEnd) == ScanResult::NeedMore)
            {
                return false;
            }
            advanceTo(commentEnd);
        }
    }

    bool JsonReader::resolveUtf8Bom()
    {
        // 判定「当前缓冲是否为 BOM 的前缀」，用于处理 BOM 被切成多片的情形
        const auto isByteOrderMarkPrefix = [](const std::string_view text) noexcept
        {
            static constexpr unsigned char kByteOrderMark[] = {0xEFU, 0xBBU, 0xBFU};
            if (text.size() > sizeof(kByteOrderMark))
            {
                return false;
            }
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                if (static_cast<unsigned char>(text[index]) != kByteOrderMark[index])
                {
                    return false;
                }
            }
            return true;
        };

        if (!m_options.skipUtf8Bom)
        {
            m_bomResolved = true;
            return true;
        }

        if (m_buffer.size() >= 3)
        {
            m_bomResolved = true;
            // EF BB BF 属于编码层标记而非 JSON 空白；按 JsonParser 的做法跳过且不计入行列号
            if (static_cast<unsigned char>(m_buffer[0]) == 0xEFU &&
                static_cast<unsigned char>(m_buffer[1]) == 0xBBU &&
                static_cast<unsigned char>(m_buffer[2]) == 0xBFU)
            {
                m_cursor = 3;
            }
            return true;
        }

        if (isByteOrderMarkPrefix(m_buffer) && !m_finished)
        {
            // 只喂到 BOM 的前一两个字节：等更多输入再判定
            return false;
        }

        // 输入已结束或首字节与 BOM 不符：按无 BOM 处理
        m_bomResolved = true;
        return true;
    }

    JsonReader::ScanResult JsonReader::scanString(const std::size_t startIndex, std::string &decodedText, std::size_t &endIndex) const
    {
        const char           quote       = m_buffer[startIndex];
        const TextPosition stringStart = positionAt(startIndex);

        std::size_t index = startIndex + 1;
        while (true)
        {
            if (index >= m_buffer.size())
            {
                // 缓冲截断：先不判定未闭合，等 finish() 之后才能确定是错误
                if (!m_finished)
                {
                    return ScanResult::NeedMore;
                }
                throw FormatError(FormatErrorKind::UnterminatedString, "字符串未闭合", stringStart);
            }

            const char byte = m_buffer[index];
            if (byte == '\\')
            {
                // 转义引导符与紧随其后的字符一起越过：即便被转义的是收尾引号也不会误判；
                // 具体转义合法性交给 TextEscapes::decodeQuotedBody 统一判定
                if (index + 1 >= m_buffer.size())
                {
                    if (!m_finished)
                    {
                        return ScanResult::NeedMore;
                    }
                    throw FormatError(FormatErrorKind::InvalidEscape, "反斜杠后输入即结束", stringStart);
                }
                index += 2;
                continue;
            }

            if (byte == quote)
            {
                break;
            }

            // RFC 8259 §7：字符串内 U+0000..U+001F 必须以转义形式出现
            if (static_cast<unsigned char>(byte) < 0x20U)
            {
                throw FormatError(FormatErrorKind::ControlCharacter, "字符串内不允许出现原始控制字符", stringStart);
            }

            ++index;
        }

        const std::size_t bodyStart = startIndex + 1;
        const std::size_t bodyEnd   = index;

        // 长度上限按原文（含转义序列）字节数计，在解码分配之前拒绝
        if (m_options.maximumStringLength != 0 && bodyEnd - bodyStart > m_options.maximumStringLength)
        {
            throw FormatError(FormatErrorKind::SizeExceeded,
                              std::format("字符串长度超出上限：{} 字节", m_options.maximumStringLength),
                              stringStart);
        }

        validateStringUtf8(bodyStart, bodyEnd, stringStart);

        const std::string_view body(m_buffer.data() + bodyStart, bodyEnd - bodyStart);
        try
        {
            decodedText = TextEscapes::decodeQuotedBody(body, stringStart);
        } catch (const FormatError &error)
        {
            // TextEscapes 是 JSON 与 YAML 共用原语，不认识 JSON 的错误分类；
            // 这里按原因文本补上分类后重抛（与 JsonParser 同一做法）
            const std::string    &reason = error.reason();
            const FormatErrorKind kind   = reason.find("代理项") != std::string::npos
                                             ? FormatErrorKind::SurrogatePairError
                                             : FormatErrorKind::InvalidEscape;
            throw FormatError(kind, reason, stringStart);
        }

        endIndex = bodyEnd + 1;
        return ScanResult::Complete;
    }

    JsonReader::ScanResult JsonReader::scanNumber(const std::size_t startIndex, std::string &rawText, std::size_t &endIndex) const
    {
        const std::string_view text(m_buffer.data() + startIndex, m_buffer.size() - startIndex);
        const NumberScanOutcome outcome = scanNumberToken(text, m_finished, positionAt(startIndex));
        if (!outcome.isComplete)
        {
            return ScanResult::NeedMore;
        }

        rawText.assign(text.substr(0, outcome.endIndex));
        validateNumberRange(rawText, positionAt(startIndex));
        endIndex = startIndex + outcome.endIndex;
        return ScanResult::Complete;
    }

    JsonReader::ScanResult JsonReader::scanKeyword(const std::size_t startIndex, std::string &keyword, std::size_t &endIndex) const
    {
        const TextPosition keywordStart = positionAt(startIndex);

        // 首字符唯一确定候选字面量，无需逐个尝试
        std::string_view candidate;
        switch (m_buffer[startIndex])
        {
            case 't':
                candidate = "true";
                break;
            case 'f':
                candidate = "false";
                break;
            case 'n':
                candidate = "null";
                break;
            default:
                throw FormatError(FormatErrorKind::InvalidKeyword, "关键字必须是 true、false、null 之一", keywordStart);
        }

        const std::size_t available  = m_buffer.size() - startIndex;
        const std::size_t comparable = std::min(available, candidate.size());
        const std::string_view visible(m_buffer.data() + startIndex, comparable);
        if (visible != candidate.substr(0, comparable))
        {
            throw FormatError(FormatErrorKind::InvalidKeyword, "关键字必须是 true、false、null 之一", keywordStart);
        }

        if (comparable < candidate.size())
        {
            // 只有前几个字母可用：可能是被切断的关键字
            if (!m_finished)
            {
                return ScanResult::NeedMore;
            }
            throw FormatError(FormatErrorKind::InvalidKeyword, "关键字必须是 true、false、null 之一", keywordStart);
        }

        const std::size_t afterIndex = startIndex + candidate.size();
        if (afterIndex < m_buffer.size())
        {
            // 关键字必须是完整词：truely 之类的后续字母属于非法输入
            if (std::isalnum(static_cast<unsigned char>(m_buffer[afterIndex])) != 0)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "关键字格式错误", keywordStart);
            }
        } else if (!m_finished)
        {
            // 缓冲刚好停在关键字末尾，尚不能判定后面是否还有字母
            return ScanResult::NeedMore;
        }

        keyword.assign(candidate);
        endIndex = afterIndex;
        return ScanResult::Complete;
    }

    JsonReader::ScanResult JsonReader::scanComment(const std::size_t startIndex, std::size_t &endIndex) const
    {
        // 调用方已确认 startIndex 处是 '/' 且下一个字节是 '/' 或 '*'
        if (peekByte(startIndex + 1) == '/')
        {
            std::size_t index = startIndex + 2;
            while (index < m_buffer.size() && m_buffer[index] != '\n')
            {
                ++index;
            }

            if (index >= m_buffer.size())
            {
                // 行注释要吃满整行，未见到换行且输入未结束时不能提交游标
                if (!m_finished)
                {
                    return ScanResult::NeedMore;
                }
                endIndex = index;
                return ScanResult::Complete;
            }

            endIndex = index + 1; // 换行本身也是空白，一并消费
            return ScanResult::Complete;
        }

        std::size_t index = startIndex + 2;
        while (true)
        {
            // 需要同时看到 '*' 与 '/'，因此末尾不足两个字符即无法判定
            if (index + 1 >= m_buffer.size())
            {
                if (!m_finished)
                {
                    return ScanResult::NeedMore;
                }
                throw FormatError(FormatErrorKind::UnterminatedComment, "块注释未闭合", positionAt(startIndex));
            }
            if (m_buffer[index] == '*' && m_buffer[index + 1] == '/')
            {
                endIndex = index + 2;
                return ScanResult::Complete;
            }
            ++index;
        }
    }

    void JsonReader::validateStringUtf8(const std::size_t bodyStart, const std::size_t bodyEnd, const TextPosition &bodyStartPosition) const
    {
        // 与 JsonParser 同一套规则：正文中不可能出现换行，故出错位置按字节偏移平移列号
        const auto failurePosition = [bodyStart, &bodyStartPosition](const std::size_t failureIndex) noexcept
        {
            TextPosition position = bodyStartPosition;
            position.columnNumber += failureIndex - bodyStart;
            position.offset = failureIndex;
            return position;
        };

        std::size_t index = bodyStart;
        while (index < bodyEnd)
        {
            const auto leadByte = static_cast<unsigned char>(m_buffer[index]);
            if (leadByte < 0x80U)
            {
                // ASCII 快路径；U+007F（DEL）在 RFC 8259 §7 下合法，原样保留
                ++index;
                continue;
            }

            // RFC 3629：由首字节决定序列长度，并给出第二个字节的合法区间
            std::size_t   sequenceLength    = 0;
            unsigned char secondByteMinimum = 0x80U;
            unsigned char secondByteMaximum = 0xBFU;

            if (leadByte >= 0xC2U && leadByte <= 0xDFU)
            {
                sequenceLength = 2; // C0/C1 属于过长编码，必须拒绝
            } else if (leadByte == 0xE0U)
            {
                sequenceLength    = 3;
                secondByteMinimum = 0xA0U; // 排除过长编码
            } else if (leadByte >= 0xE1U && leadByte <= 0xECU)
            {
                sequenceLength = 3;
            } else if (leadByte == 0xEDU)
            {
                sequenceLength    = 3;
                secondByteMaximum = 0x9FU; // 排除以 UTF-8 编码的代理项 U+D800..U+DFFF
            } else if (leadByte >= 0xEEU && leadByte <= 0xEFU)
            {
                sequenceLength = 3;
            } else if (leadByte == 0xF0U)
            {
                sequenceLength    = 4;
                secondByteMinimum = 0x90U; // 排除过长编码
            } else if (leadByte >= 0xF1U && leadByte <= 0xF3U)
            {
                sequenceLength = 4;
            } else if (leadByte == 0xF4U)
            {
                sequenceLength    = 4;
                secondByteMaximum = 0x8FU; // 上限 U+10FFFF
            } else
            {
                // 0x80..0xC1 与 0xF5..0xFF 都不是合法的首字节
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", failurePosition(index));
            }

            if (index + sequenceLength > bodyEnd)
            {
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中的 UTF-8 序列不完整", failurePosition(index));
            }
            if (!isByteInRange(m_buffer[index + 1], secondByteMinimum, secondByteMaximum))
            {
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", failurePosition(index));
            }
            for (std::size_t offset = 2; offset < sequenceLength; ++offset)
            {
                if (!isByteInRange(m_buffer[index + offset], 0x80U, 0xBFU))
                {
                    throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", failurePosition(index + offset));
                }
            }

            index += sequenceLength;
        }
    }

    std::optional<JsonEvent> JsonReader::scanValueEvent()
    {
        const std::size_t      index          = m_cursor;
        const char             byte           = peekByte(index);
        const TextPosition   valuePosition  = currentPosition();

        // 容器开始：单字节即可判定，不存在「需要更多输入」的分支
        if (byte == '{' || byte == '[')
        {
            const bool isObject = byte == '{';
            // 先做深度判定（用括号所在位置报错），再推进游标与压栈
            beginContainer(isObject);
            advanceTo(index + 1);
            return makeEvent(isObject ? JsonEventType::ObjectStart : JsonEventType::ArrayStart, std::string{}, valuePosition);
        }

        if (byte == '"' || (m_options.allowSingleQuotedStrings && byte == '\''))
        {
            std::string decodedText;
            std::size_t endIndex = 0;
            if (scanString(index, decodedText, endIndex) == ScanResult::NeedMore)
            {
                return std::nullopt;
            }
            advanceTo(endIndex);
            completeValueInParent(false);
            return makeEvent(JsonEventType::String, std::move(decodedText), valuePosition);
        }

        if (byte == 't' || byte == 'f' || byte == 'n')
        {
            std::string keyword;
            std::size_t endIndex = 0;
            if (scanKeyword(index, keyword, endIndex) == ScanResult::NeedMore)
            {
                return std::nullopt;
            }
            advanceTo(endIndex);
            completeValueInParent(false);

            // null 不带文本载荷，bool 保留 "true"/"false" 原文便于消费方直接判定
            return makeEvent(keyword == "null" ? JsonEventType::Null : JsonEventType::Bool,
                             keyword == "null" ? std::string{} : std::move(keyword),
                             valuePosition);
        }

        if (byte == '-' || isDigit(byte))
        {
            std::string rawText;
            std::size_t endIndex = 0;
            if (scanNumber(index, rawText, endIndex) == ScanResult::NeedMore)
            {
                return std::nullopt;
            }
            advanceTo(endIndex);
            completeValueInParent(false);
            return makeEvent(JsonEventType::Number, std::move(rawText), valuePosition);
        }

        throw FormatError(FormatErrorKind::UnexpectedByte, "应为 JSON 值", valuePosition);
    }

    std::optional<JsonEvent> JsonReader::scanKeyEvent()
    {
        const std::size_t    index       = m_cursor;
        const char           byte        = peekByte(index);
        const TextPosition keyPosition = currentPosition();

        if (byte != '"' && !(m_options.allowSingleQuotedStrings && byte == '\''))
        {
            throw FormatError(FormatErrorKind::UnexpectedByte,
                              m_options.allowSingleQuotedStrings ? "对象键必须是字符串" : "对象键必须是双引号字符串",
                              keyPosition);
        }

        std::string decodedText;
        std::size_t endIndex = 0;
        if (scanString(index, decodedText, endIndex) == ScanResult::NeedMore)
        {
            return std::nullopt;
        }

        advanceTo(endIndex);
        // 成员个数在键确认后立即计数，与 JsonParser「解析下一个成员之前判定上限」的时机一致
        countElement();
        setCurrentPhase(ScanPhase::ObjectColon);
        return makeEvent(JsonEventType::Key, std::move(decodedText), keyPosition);
    }

    void JsonReader::beginContainer(const bool isObject)
    {
        // 容器所在层数 = 当前栈深，根容器为第 0 层，与解析/序列化两侧的深度定义一致
        if (m_options.maximumDepth != 0 && m_stack.size() >= m_options.maximumDepth)
        {
            throw FormatError(FormatErrorKind::DepthExceeded, "嵌套深度超出限制", currentPosition());
        }

        completeValueInParent(true);
        m_stack.push_back(ContainerFrame{
                .isObject     = isObject,
                .phase        = isObject ? ScanPhase::ObjectFirstKey : ScanPhase::ArrayFirst,
                .elementCount = 0});
    }

    JsonEvent JsonReader::closeContainer()
    {
        if (m_stack.empty())
        {
            throw FormatError(FormatErrorKind::UnexpectedByte, "内部扫描状态异常：容器栈为空", currentPosition());
        }

        // 调用方已确认当前字节是收尾括号；这里再校验一次，避免状态机与输入脱节
        const char expectedByte = m_stack.back().isObject ? '}' : ']';
        if (peekByte(m_cursor) != expectedByte)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte, "内部扫描状态异常：收尾括号与容器不匹配", currentPosition());
        }

        const TextPosition endPosition = currentPosition();
        const bool           isObject    = m_stack.back().isObject;
        advanceTo(m_cursor + 1);
        m_stack.pop_back();

        if (m_stack.empty())
        {
            // 根容器结束：根值已完成，后续只允许空白
            m_rootPhase = ScanPhase::RootEnd;
        }

        return makeEvent(isObject ? JsonEventType::ObjectEnd : JsonEventType::ArrayEnd, std::string{}, endPosition);
    }

    void JsonReader::completeValueInParent(const bool isContainerStart)
    {
        if (m_stack.empty())
        {
            // 根值为容器时先不置 RootEnd（等弹栈时再置），避免容器还没扫完就被当成已完成
            if (!isContainerStart)
            {
                m_rootPhase = ScanPhase::RootEnd;
            }
            return;
        }

        ContainerFrame &frame = m_stack.back();
        if (!frame.isObject)
        {
            countElement();
        }
        frame.phase = frame.isObject ? ScanPhase::ObjectDelimiter : ScanPhase::ArrayDelimiter;
    }

    void JsonReader::countElement()
    {
        ContainerFrame &frame = m_stack.back();
        if (m_options.maximumContainerElements != 0 && frame.elementCount >= m_options.maximumContainerElements)
        {
            throw FormatError(FormatErrorKind::SizeExceeded,
                              std::format("容器元素数量超出上限：{}", m_options.maximumContainerElements),
                              currentPosition());
        }
        ++frame.elementCount;
    }

    JsonReader::ScanPhase JsonReader::currentPhase() const noexcept
    {
        return m_stack.empty() ? m_rootPhase : m_stack.back().phase;
    }

    void JsonReader::setCurrentPhase(const ScanPhase phase) noexcept
    {
        if (m_stack.empty())
        {
            m_rootPhase = phase;
        } else
        {
            m_stack.back().phase = phase;
        }
    }

    void JsonReader::advanceTo(const std::size_t endIndex) noexcept
    {
        while (m_cursor < endIndex && m_cursor < m_buffer.size())
        {
            // 与 JsonParser 的行列推进规则一致：'\n' 换行归零，'\r' 只计列
            if (m_buffer[m_cursor] == '\n')
            {
                ++m_line;
                m_column = 1;
            } else
            {
                ++m_column;
            }
            ++m_cursor;
        }
    }

    char JsonReader::peekByte(const std::size_t index) const noexcept
    {
        return index < m_buffer.size() ? m_buffer[index] : '\0';
    }

    TextPosition JsonReader::currentPosition() const noexcept
    {
        return TextPosition{.lineNumber = m_line, .columnNumber = m_column, .offset = m_cursor};
    }

    TextPosition JsonReader::positionAt(const std::size_t index) const noexcept
    {
        // 游标之前的位置由行列号直接给出；只有向前推演时才逐字节累计（仅错误路径使用）
        TextPosition position = currentPosition();
        for (std::size_t cursor = m_cursor; cursor < index && cursor < m_buffer.size(); ++cursor)
        {
            if (m_buffer[cursor] == '\n')
            {
                ++position.lineNumber;
                position.columnNumber = 1;
            } else
            {
                ++position.columnNumber;
            }
        }
        position.offset = index;
        return position;
    }

    std::vector<JsonEvent> JsonReader::readAll(const std::string_view text, const JsonParseOptions &options)
    {
        JsonReader             reader(options);
        std::vector<JsonEvent> events;

        reader.feed(text);
        reader.finish();

        while (std::optional<JsonEvent> event = reader.nextEvent())
        {
            events.push_back(std::move(*event));
        }

        return events;
    }

    const JsonParseOptions &JsonReader::options() const noexcept
    {
        return m_options;
    }

    bool JsonReader::isFinished() const noexcept
    {
        return m_finished;
    }
} // namespace AsynGyanis::Base
