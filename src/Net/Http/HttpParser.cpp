#include "Net/Http/HttpParser.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 按「0 表示关闭该项保护」的口径判定一个长度或条数是否越界
         * @param value 实测值（字节数或条数）
         * @param limit 该项上限，0 表示不设上限
         * @return true 表示越界
         */
        [[nodiscard]] constexpr bool exceedsLimit(const std::size_t value, const std::size_t limit) noexcept
        {
            // 0 是「关掉这道闸」而不是「不允许任何长度」：上限非 0 时才比较
            return limit != 0 && value > limit;
        }

        /**
         * @brief 判断字符是否为 RFC 9110 定义的 token 字符
         * @param character 待判断字节
         * @return true 表示可用作方法名或头部名（"!#$%&'*+-.^_`|~" 与字母数字）
         */
        bool isTokenCharacter(const unsigned char character) noexcept
        {
            if ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z'))
            {
                return true;
            }
            constexpr std::string_view kExtraTokenCharacters = "!#$%&'*+-.^_`|~";
            return kExtraTokenCharacters.find(static_cast<char>(character)) != std::string_view::npos;
        }

        /**
         * @brief 判断字符是否可以出现在请求目标里
         * @details 只接受可见 ASCII：空格是请求行的分隔符，控制字符与 DEL 都不允许。
         * @param character 待判断字节
         * @return true 表示合法
         */
        bool isTargetCharacter(const unsigned char character) noexcept
        {
            return character >= 0x21 && character <= 0x7E;
        }

        /**
         * @brief 判断字符是否可以出现在头部值里
         * @details 允许 HTAB、可见 ASCII 与 obs-text（0x80 以上，RFC 9110 允许接收）；
         *          其余控制字符（含 DEL）一律拒绝——它们既无法出现在合法报文里，
         *          又常被用来构造响应拆分之类的注入。
         * @param character 待判断字节
         * @return true 表示合法
         */
        bool isHeaderValueCharacter(const unsigned char character) noexcept
        {
            if (character == '\t' || (character >= 0x20 && character <= 0x7E) || character >= 0x80)
            {
                return true;
            }
            return false;
        }

        /**
         * @brief 判断字符是否为十进制数字
         * @param character 待判断字节
         * @return true 表示是 0-9
         */
        bool isDigit(const char character) noexcept
        {
            return character >= '0' && character <= '9';
        }

        /**
         * @brief 去掉首尾的可选空白（SP 与 HTAB）
         * @param text 待裁剪文本
         * @return std::string_view 裁剪后的视图
         */
        std::string_view trimOptionalWhitespace(const std::string_view text) noexcept
        {
            constexpr std::string_view kOptionalWhitespace = " \t";
            const std::size_t          first = text.find_first_not_of(kOptionalWhitespace);
            if (first == std::string_view::npos)
            {
                return {};
            }
            const std::size_t last = text.find_last_not_of(kOptionalWhitespace);
            return text.substr(first, last - first + 1);
        }

        /**
         * @brief ASCII 大小写不敏感比较
         * @param left 左操作数
         * @param right 右操作数
         * @return true 表示忽略大小写后相等
         */
        bool equalsIgnoringCase(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const auto leftCharacter  = static_cast<unsigned char>(left[index]);
                const auto rightCharacter = static_cast<unsigned char>(right[index]);
                // 只按 ASCII 折叠：locale 相关的 tolower 会让非 ASCII 字节产生平台差异
                const auto normalize = [](const unsigned char value) -> unsigned char
                {
                    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value - 'A' + 'a') : value;
                };
                if (normalize(leftCharacter) != normalize(rightCharacter))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 把十六进制位转成数值
         * @param character 待转换字节
         * @return int 0-15；不是十六进制位时为 -1
         */
        int hexadecimalDigitValue(const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            return -1;
        }

        /**
         * @brief 判断 Transfer-Encoding 的取值是否恰好是唯一的 chunked
         * @details RFC 9112 §6.1 要求 chunked 必须位于编码链末尾；本框架只实现它，因此取值里出现
         *          gzip 之类其它编码、或 chunked 重复出现，都判非法，绝不悄悄按 identity 处理。
         * @param listValue 各条 Transfer-Encoding 取值按到达顺序以 ", " 连接后的原文
         * @return true 仅有一个取值且忽略大小写等于 "chunked"
         */
        bool isSingleChunkedEncoding(const std::string_view listValue) noexcept
        {
            return equalsIgnoringCase(trimOptionalWhitespace(listValue), "chunked");
        }

        /**
         * @brief 校验块扩展（chunk-ext）的语法
         * @details 按 RFC 9112 §7.1.1：扩展是若干「;名字[=值]」段，名字必须是 token，值可以是 token
         *          或带引号字符串；本框架只校验语法，不解释扩展的语义。
         * @param text 从第一个 ';' 起的扩展原文
         * @return true 语法合法
         */
        bool areChunkExtensionsWellFormed(const std::string_view text) noexcept
        {
            const auto skipOptionalWhitespace = [&text](std::size_t &position) noexcept
            {
                while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
                {
                    ++position;
                }
            };

            std::size_t offset = 0;
            while (offset < text.size())
            {
                // 每段都以 ';' 开头：段与段之间只允许 BWS，多出来的字节一律判非法
                if (text[offset] != ';')
                {
                    return false;
                }
                ++offset;
                skipOptionalWhitespace(offset);

                // 扩展名必须是至少一个 token 字符
                const std::size_t nameBegin = offset;
                while (offset < text.size() && isTokenCharacter(static_cast<unsigned char>(text[offset])))
                {
                    ++offset;
                }
                if (offset == nameBegin)
                {
                    return false;
                }
                skipOptionalWhitespace(offset);

                if (offset < text.size() && text[offset] == '=')
                {
                    ++offset;
                    skipOptionalWhitespace(offset);
                    if (offset < text.size() && text[offset] == '"')
                    {
                        // 带引号字符串：内部允许 qdtext（HTAB、可见 ASCII、obs-text）与反斜杠转义
                        ++offset;
                        bool isClosed = false;
                        while (offset < text.size())
                        {
                            const unsigned char character = static_cast<unsigned char>(text[offset]);
                            if (character == '"')
                            {
                                ++offset;
                                isClosed = true;
                                break;
                            }
                            if (character == '\\')
                            {
                                // 引号对：反斜杠之后必须还有一个可打印字节，且不能是裸控制字符
                                if (offset + 1 >= text.size())
                                {
                                    return false;
                                }
                                const unsigned char escaped = static_cast<unsigned char>(text[offset + 1]);
                                if (escaped < 0x20 && escaped != '\t')
                                {
                                    return false;
                                }
                                offset += 2;
                                continue;
                            }
                            if (character == '\t' || (character >= 0x20 && character <= 0x7E) || character >= 0x80)
                            {
                                ++offset;
                                continue;
                            }
                            return false;
                        }
                        if (!isClosed)
                        {
                            return false;
                        }
                    } else
                    {
                        const std::size_t valueBegin = offset;
                        while (offset < text.size() && isTokenCharacter(static_cast<unsigned char>(text[offset])))
                        {
                            ++offset;
                        }
                        if (offset == valueBegin)
                        {
                            return false;
                        }
                    }
                    skipOptionalWhitespace(offset);
                }
            }
            return true;
        }
    } // namespace

    HttpParser::HttpParser(HttpParserLimits limits) :
        m_limits(limits)
    {
        // 上限只在构造时落定：解析按字节增量推进，若中途换一份更紧的配置，
        // 同一条报文的前后两段就会按不同尺子判定，出错位置不可预期
    }

    ParseStatus HttpParser::parse(const char *const data, const size_t length)
    {
        // 已收齐：一字节都不再吃。这些字节属于流水线里的下一条报文，喂进已完成的解析器
        // 会被当作新报文的开头，把上一条已定稿的结果改坏
        if (m_stage == Stage::Complete)
        {
            m_consumedByteCount = 0;
            return ParseStatus::Done;
        }

        // 错误粘滞：非 reset() 不能恢复，调用方要么重置要么断开连接
        if (m_stage == Stage::Failed)
        {
            m_consumedByteCount = 0;
            return ParseStatus::Error;
        }

        std::size_t consumed = 0;
        while (consumed < length && m_stage != Stage::Complete && m_stage != Stage::Failed)
        {
            if (m_stage == Stage::Body)
            {
                const std::size_t remainingBodyLength = m_contentLength - m_receivedBodyLength;
                const std::size_t chunkLength         = std::min(remainingBodyLength, length - consumed);

                // 正文按「已收 + 本次」的总量卡上限：单看 Content-Length 头不足以设防，
                // 声明 1 字节然后狂发数据同样能撑爆内存
                if (exceedsLimit(m_body.size() + chunkLength, m_limits.maximumBodySize))
                {
                    failBodyTooLarge(std::format("请求体超出上限 {} 字节", m_limits.maximumBodySize));
                    break;
                }

                m_body.append(data + consumed, chunkLength);
                consumed += chunkLength;
                m_receivedBodyLength += chunkLength;

                if (m_receivedBodyLength == m_contentLength)
                {
                    commitMessage();
                    m_stage = Stage::Complete;
                }
                continue;
            }

            if (m_stage == Stage::ChunkData)
            {
                // 块数据按字节数整段搬：内容可能含任意字节（含 CR/LF），因此只按长度拷、不扫描
                const std::size_t chunkLength = std::min(m_chunkRemainingBytes, length - consumed);

                // 上限按「解码后」的正文字节数判定：编码后的体积不能用来代替它
                if (exceedsLimit(m_body.size() + chunkLength, m_limits.maximumBodySize))
                {
                    failBodyTooLarge(std::format("分块解码后的请求体超出上限 {} 字节", m_limits.maximumBodySize));
                    break;
                }

                m_body.append(data + consumed, chunkLength);
                consumed += chunkLength;
                m_receivedBodyLength += chunkLength;
                m_chunkRemainingBytes -= chunkLength;

                // 本块收满：接着必须是一个 CRLF，改由 ChunkDataTerminator 逐字节核对
                if (m_chunkRemainingBytes == 0)
                {
                    m_stage                    = Stage::ChunkDataTerminator;
                    m_chunkTerminatorBytesSeen = 0;
                }
                continue;
            }

            if (m_stage == Stage::ChunkDataTerminator)
            {
                // 逐字节核对 CRLF：切在 CR 与 LF 之间时靠已收字节数续上，不需要另做暂存
                const char expectedCharacter = m_chunkTerminatorBytesSeen == 0 ? '\r' : '\n';
                if (data[consumed] != expectedCharacter)
                {
                    failMalformed("HTTP 报文解析失败：分块的块数据之后必须是 CRLF，请在块数据与下一个块大小之间补齐");
                    break;
                }
                ++consumed;
                ++m_chunkTerminatorBytesSeen;

                if (m_chunkTerminatorBytesSeen == 2)
                {
                    m_chunkTerminatorBytesSeen = 0;
                    m_stage                    = Stage::ChunkSize;
                }
                continue;
            }

            // 行导向阶段（请求行、头部行、块大小行、trailer 行）都按「整行」推进：
            // 行体跨两次输入时由 takeLine 拼接
            std::string_view line;
            if (!takeLine(data, length, consumed, line))
            {
                break;
            }

            if (m_stage == Stage::RequestLine)
            {
                if (!parseRequestLine(line))
                {
                    break;
                }
                m_stage = Stage::Headers;
                continue;
            }
            if (m_stage == Stage::Headers)
            {
                if (!parseHeaderLine(line))
                {
                    break;
                }
                continue;
            }
            if (m_stage == Stage::ChunkSize)
            {
                if (!parseChunkSizeLine(line))
                {
                    break;
                }
                continue;
            }
            // 只剩 Trailer：空行会在这里收尾整条报文，成功后阶段变为 Complete、循环随即退出
            if (!parseTrailerLine(line))
            {
                break;
            }
        }

        if (m_stage == Stage::Failed)
        {
            m_consumedByteCount = consumed;
            return ParseStatus::Error;
        }
        m_consumedByteCount = consumed;
        return m_stage == Stage::Complete ? ParseStatus::Done : ParseStatus::NeedMore;
    }

    void HttpParser::reset()
    {
        m_currentRequest.reset();
        clearMessageScratch();
        m_pendingLine.clear();
        m_isPendingLineHandedOut = false;

        m_stage    = Stage::RequestLine;
        m_hasError = false;
        m_errorKind = HttpParseErrorKind::None;
        m_errorMessage.clear();
        m_consumedByteCount = 0;
    }

    HttpRequest &HttpParser::request()
    {
        return m_currentRequest;
    }

    bool HttpParser::hasError() const
    {
        return m_hasError;
    }

    bool HttpParser::isLimitExceeded() const
    {
        // 「超限」是失败类别的一个子集：头部越界（431）与正文越界（413）都算，
        // 二者都说明报文形态合法、只是体量太大
        return m_errorKind == HttpParseErrorKind::HeaderTooLarge || m_errorKind == HttpParseErrorKind::BodyTooLarge;
    }

    std::string HttpParser::errorMessage() const
    {
        return m_errorMessage;
    }

    HttpParseErrorKind HttpParser::errorKind() const
    {
        return m_errorKind;
    }

    std::size_t HttpParser::consumedByteCount() const
    {
        return m_consumedByteCount;
    }

    std::size_t HttpParser::bufferedBodyByteCount() const noexcept
    {
        return m_body.size();
    }

    bool HttpParser::takeLine(const char *const data, const std::size_t length, std::size_t &consumed, std::string_view &line)
    {
        // 上一次慢路径交出去的视图按契约已经用完（调用方当场解析完），暂存可以清掉；
        // 而「还没等到 LF 的半行」必须留着继续拼，两者用一个标记区分
        if (m_isPendingLineHandedOut)
        {
            m_pendingLine.clear();
            m_isPendingLineHandedOut = false;
        }

        const char *const begin     = data + consumed;
        const std::size_t available = length - consumed;
        const void *const newline   = std::memchr(begin, '\n', available);

        if (m_pendingLine.empty())
        {
            // 快路径：整行落在本段输入里，直接在输入上切视图，零拷贝
            if (newline == nullptr)
            {
                // 本段凑不齐一行：整段并入暂存，等下一次调用继续拼。长度上限兜住
                // 「一行永远不结束」的输入：没有它，一个超长的头部行就能把内存一直撑下去
                if (!checkLineLength(available))
                {
                    return false;
                }
                m_pendingLine.assign(begin, available);
                consumed = length;
                return false;
            }

            const auto lineEnd = static_cast<const char *>(newline);
            if (lineEnd == begin || lineEnd[-1] != '\r')
            {
                failMalformed("HTTP 报文解析失败：行尾必须是 CRLF（不允许单独出现 LF）");
                return false;
            }

            line = std::string_view(begin, static_cast<std::size_t>(lineEnd - begin) - 1);
            consumed += static_cast<std::size_t>(lineEnd - begin) + 1;
            return true;
        }

        // 慢路径：行体跨在上一次的暂存与本次输入之间，先把本次输入里直到 LF 的部分并进来
        const std::size_t appendLength =
                newline == nullptr ? available : static_cast<std::size_t>(static_cast<const char *>(newline) - begin) + 1;
        if (!checkLineLength(m_pendingLine.size() + appendLength))
        {
            return false;
        }
        m_pendingLine.append(begin, appendLength);
        consumed += appendLength;

        if (newline == nullptr)
        {
            return false;
        }

        if (m_pendingLine.size() < 2 || m_pendingLine[m_pendingLine.size() - 2] != '\r')
        {
            failMalformed("HTTP 报文解析失败：行尾必须是 CRLF（不允许单独出现 LF）");
            return false;
        }

        // 视图指向暂存：调用方必须在下一次 takeLine() 之前解析完，本函数的开头会清掉它
        line                     = std::string_view(m_pendingLine.data(), m_pendingLine.size() - 2);
        m_isPendingLineHandedOut = true;
        return true;
    }

    bool HttpParser::checkLineLength(const std::size_t length)
    {
        if (m_stage == Stage::RequestLine)
        {
            // 整行上限由 URI 上限推出（URI + 方法名与版本串的固定余量）：放宽 URI 时这道闸门自动跟随
            const std::size_t requestLineLimit = m_limits.requestLineLengthLimit();
            if (exceedsLimit(length, requestLineLimit))
            {
                failHeaderTooLarge(std::format("请求行超出上限 {} 字节", requestLineLimit));
                return false;
            }
            return true;
        }

        if (m_stage == Stage::ChunkSize)
        {
            // 块大小行属于正文的帧结构而不是头部，超限按正文过大判定（上层回 413）
            if (exceedsLimit(length, m_limits.maximumChunkSizeLineLength))
            {
                failBodyTooLarge(std::format("分块块大小行超出上限 {} 字节", m_limits.maximumChunkSizeLineLength));
                return false;
            }
            return true;
        }

        const std::size_t headerLineLimit = headerLineLengthLimit();
        if (exceedsLimit(length, headerLineLimit))
        {
            failHeaderTooLarge(std::format("头部行超出上限 {} 字节", headerLineLimit));
            return false;
        }
        return true;
    }

    std::size_t HttpParser::headerLineLengthLimit() const noexcept
    {
        // 名与值任一项关闭保护（0）时整行也不设上限：否则推导值会先把被放宽的那一项卡住，
        // 与「0 表示关闭该项保护」的承诺相反
        if (m_limits.maximumHeaderFieldNameLength == 0 || m_limits.maximumHeaderFieldValueLength == 0)
        {
            return 0;
        }

        // ": " 与 CRLF 共 4 字节：名与值之外这一行的固定开销，不是可配置项
        return m_limits.maximumHeaderFieldNameLength + m_limits.maximumHeaderFieldValueLength + 4;
    }

    bool HttpParser::parseRequestLine(const std::string_view line)
    {
        // 三段由空格分隔，且目标里不允许再出现空格：用首个与末个空格切成三段后，
        // 中间那段自然就是「不含空格的目标」
        const std::size_t firstSpace = line.find(' ');
        const std::size_t lastSpace  = line.rfind(' ');
        if (firstSpace == std::string_view::npos || firstSpace == lastSpace)
        {
            failMalformed("HTTP 报文解析失败：请求行必须是「方法 目标 版本」三段，以空格分隔");
            return false;
        }

        const std::string_view methodText  = line.substr(0, firstSpace);
        const std::string_view targetText  = line.substr(firstSpace + 1, lastSpace - firstSpace - 1);
        const std::string_view versionText = line.substr(lastSpace + 1);

        if (methodText.empty() || methodText.size() > kMaximumMethodLength)
        {
            failMalformed(std::format("HTTP 报文解析失败：请求方法长度必须在 1 到 {} 字节之间", kMaximumMethodLength));
            return false;
        }
        for (const char character: methodText)
        {
            if (!isTokenCharacter(static_cast<unsigned char>(character)))
            {
                failMalformed("HTTP 报文解析失败：请求方法只能由 token 字符组成");
                return false;
            }
        }

        if (targetText.empty())
        {
            failMalformed("HTTP 报文解析失败：请求目标不能为空");
            return false;
        }
        if (exceedsLimit(targetText.size(), m_limits.maximumUriLength))
        {
            failHeaderTooLarge(std::format("请求 URI 超出上限 {} 字节", m_limits.maximumUriLength));
            return false;
        }
        for (const char character: targetText)
        {
            if (!isTargetCharacter(static_cast<unsigned char>(character)))
            {
                failMalformed("HTTP 报文解析失败：请求目标含非法字符（空格与控制字符都不允许）");
                return false;
            }
        }

        // 版本：HTTP/主.次，主版本只认 0 与 1、次版本一位十进制数字。这条不是保守取值而是
        // 协议事实：HTTP/2 及以上走完全不同的帧格式（二进制、不同握手），把它当 1.x 继续按
        // 文本解析等于用错误的语法去猜边界，因此这里当场判错，而不是收下版本号再装作能处理
        constexpr std::string_view kVersionPrefix = "HTTP/";
        const bool                 isVersionWellFormed =
                versionText.size() == kVersionPrefix.size() + 3 && versionText.starts_with(kVersionPrefix) &&
                (versionText[5] == '0' || versionText[5] == '1') && versionText[6] == '.' && isDigit(versionText[7]);
        if (!isVersionWellFormed)
        {
            failMalformed("HTTP 报文解析失败：版本必须是 HTTP/1.x 或 HTTP/0.x 的形式");
            return false;
        }

        m_method = HttpRequest::methodFromString(methodText);
        m_uri.assign(targetText);

        // 版本按收到的原文保存：上层要按 1.0/0.9 判定保活策略，重新拼装反而可能丢掉差异
        m_httpVersion.assign(versionText);
        return true;
    }

    bool HttpParser::parseFieldLine(const std::string_view line, const std::string_view fieldContextLabel,
                                    std::string_view &name, std::string_view &value)
    {
        // 折行（obs-fold）：RFC 9112 已把以空白开头的续行判为过时，这里明确拒绝而不是静默拼接，
        // 否则同一个头部名可能被两个来源写出不同含义（请求走私的经典入口）
        if (line.front() == ' ' || line.front() == '\t')
        {
            failMalformed("HTTP 报文解析失败：不支持折行（obs-fold）头部，请把值写在同一行");
            return false;
        }

        const std::size_t colonPosition = line.find(':');
        if (colonPosition == std::string_view::npos || colonPosition == 0)
        {
            failMalformed(std::format("HTTP 报文解析失败：{}行必须是「名: 值」的形式", fieldContextLabel));
            return false;
        }

        const std::string_view fieldName = line.substr(0, colonPosition);
        for (const char character: fieldName)
        {
            // 冒号前若有空白也会落到这里：token 字符集不含 SP 与 HTAB
            if (!isTokenCharacter(static_cast<unsigned char>(character)))
            {
                failMalformed(std::format("HTTP 报文解析失败：{}名只能由 token 字符组成（冒号前不得有空白）", fieldContextLabel));
                return false;
            }
        }
        if (exceedsLimit(fieldName.size(), m_limits.maximumHeaderFieldNameLength))
        {
            failHeaderTooLarge(std::format("{}名超出上限 {} 字节", fieldContextLabel, m_limits.maximumHeaderFieldNameLength));
            return false;
        }

        const std::string_view fieldValue = trimOptionalWhitespace(line.substr(colonPosition + 1));
        if (exceedsLimit(fieldValue.size(), m_limits.maximumHeaderFieldValueLength))
        {
            failHeaderTooLarge(std::format("{}值超出上限 {} 字节", fieldContextLabel, m_limits.maximumHeaderFieldValueLength));
            return false;
        }
        for (const char character: fieldValue)
        {
            if (!isHeaderValueCharacter(static_cast<unsigned char>(character)))
            {
                failMalformed(std::format("HTTP 报文解析失败：{}值含非法控制字符", fieldContextLabel));
                return false;
            }
        }

        // 头部块总长（名与值的净字节）与条数是两道独立的闸：单条名、单条值、条数各自合规，
        // 架不住上百条头部叠出来的总量。两道判定都在落库之前，拒绝路径不留半成品
        if (exceedsLimit(m_headerBlockLength + fieldName.size() + fieldValue.size(), m_limits.maximumHeaderBlockLength))
        {
            failHeaderTooLarge(std::format("{}总长超出上限 {} 字节", fieldContextLabel, m_limits.maximumHeaderBlockLength));
            return false;
        }
        if (m_limits.maximumHeaderCount != 0 && m_headerFieldCount >= m_limits.maximumHeaderCount)
        {
            failHeaderTooLarge(std::format("{}条数超出上限 {} 条", fieldContextLabel, m_limits.maximumHeaderCount));
            return false;
        }

        m_headerBlockLength += fieldName.size() + fieldValue.size();
        ++m_headerFieldCount;

        name  = fieldName;
        value = fieldValue;
        return true;
    }

    bool HttpParser::parseHeaderLine(const std::string_view line)
    {
        // 空行 = 头部块结束
        if (line.empty())
        {
            finishHeaderBlock();
            return m_stage != Stage::Failed;
        }

        std::string_view name;
        std::string_view value;
        if (!parseFieldLine(line, "请求头部", name, value))
        {
            return false;
        }

        // 两个定界头都要落到暂存上：Content-Length 决定长度，Transfer-Encoding 的取值要攒到
        // 头部块结束才能判「是否唯一的 chunked、有没有和它并列的其它编码」
        if (equalsIgnoringCase(name, "content-length"))
        {
            if (!parseContentLength(value))
            {
                return false;
            }
        } else if (equalsIgnoringCase(name, "transfer-encoding") && !appendTransferEncodingValue(value))
        {
            return false;
        } else if (equalsIgnoringCase(name, "expect"))
        {
            // 只记「对端在等 100」这一件事：头部收齐、正文未收时由 takeContinueRequest() 一次性交给上层
            m_hasContinueExpectation = isContinueExpected(value);
        }

        m_headers.push_back(ParsedHeader{std::string(name), std::string(value)});
        return true;
    }

    bool HttpParser::appendTransferEncodingValue(const std::string_view value)
    {
        if (value.empty())
        {
            failMalformed("HTTP 报文解析失败：Transfer-Encoding 的取值不能为空，请写 Transfer-Encoding: chunked");
            return false;
        }

        // 多条 Transfer-Encoding 按到达顺序拼成一份列表：要看到全部取值才能判 chunked 是否唯一
        if (m_hasTransferEncoding)
        {
            m_transferEncodingValue.append(", ");
        }
        m_transferEncodingValue.append(value);
        m_hasTransferEncoding = true;
        return true;
    }

    bool HttpParser::parseChunkSizeLine(const std::string_view line)
    {
        // 块扩展从第一个分号起：大小部分只到今天之前的那一段
        const std::size_t      semicolonPosition = line.find(';');
        const std::string_view sizeText          = line.substr(0, semicolonPosition);

        if (sizeText.empty())
        {
            failMalformed("HTTP 报文解析失败：分块块大小必须至少有一位十六进制数字");
            return false;
        }

        std::size_t chunkSize = 0;
        for (const char character: sizeText)
        {
            // 十六进制位手工转值：std::stoul 会抛异常，而本解析器对外的契约是「增量、无异常」
            const int digitValue = hexadecimalDigitValue(character);
            if (digitValue < 0)
            {
                failMalformed("HTTP 报文解析失败：分块块大小只能由十六进制数字组成，请检查块大小行");
                return false;
            }

            // 回绕判定放在移位之前：size_t 静默回绕会把一个天文数字读成「看起来正常」的小块，
            // 从而绕开下面那道上限。这一道与配置无关，正文上限关掉也仍然生效
            if (chunkSize > std::numeric_limits<std::size_t>::max() >> 4U)
            {
                failBodyTooLarge("分块单块大小超出可表示范围，无法解码：请把块大小写成实际要发送的字节数");
                return false;
            }
            chunkSize = (chunkSize << 4U) | static_cast<std::size_t>(digitValue);
        }
        if (exceedsLimit(chunkSize, m_limits.maximumBodySize))
        {
            failBodyTooLarge(std::format("分块单块大小超出上限 {} 字节", m_limits.maximumBodySize));
            return false;
        }

        // 扩展只校验语法、不解释语义：它不参与块边界计算，因此忽略内容而不忽略它的合法性
        if (semicolonPosition != std::string_view::npos && !areChunkExtensionsWellFormed(line.substr(semicolonPosition)))
        {
            failMalformed("HTTP 报文解析失败：分块块扩展语法非法，每一段应为「;名字」或「;名字=值」且名字由 token 字符组成");
            return false;
        }

        m_chunkRemainingBytes = chunkSize;
        // 零长度块是终止块：它之后只剩 trailer 段（若干字段行加一个空行）
        m_stage = chunkSize == 0 ? Stage::Trailer : Stage::ChunkData;
        return true;
    }

    bool HttpParser::parseTrailerLine(const std::string_view line)
    {
        // 空行 = trailer 段结束：解码后的正文与头部此刻才整体移交给对外请求对象
        if (line.empty())
        {
            commitMessage();
            m_stage = Stage::Complete;
            return true;
        }

        std::string_view trailerName;
        std::string_view trailerValue;
        // 语法与各项上限照头部行判，但解析结果有意丢弃：trailer 里出现 content-length 之类会与
        // 已解析头部形成两种解释，上层读到哪个都可能被对端利用（请求走私面）
        return parseFieldLine(line, "trailer 头部", trailerName, trailerValue);
    }

    bool HttpParser::parseContentLength(const std::string_view value)
    {
        if (value.empty())
        {
            failMalformed("HTTP 报文解析失败：Content-Length 不能为空");
            return false;
        }

        std::size_t          parsedLength = 0;
        const char *const    begin        = value.data();
        const char *const    end          = value.data() + value.size();
        const std::from_chars_result parseResult = std::from_chars(begin, end, parsedLength);

        // 只接受纯十进制数字：前导 '+'/'-'、空白、十六进制与任何非数字字符都会让 ptr 停在中间，
        // 溢出则返回 result_out_of_range
        if (parseResult.ec != std::errc{} || parseResult.ptr != end)
        {
            failMalformed("HTTP 报文解析失败：Content-Length 只能是十进制数字");
            return false;
        }

        // 重复出现时只允许取值一致：不一致意味着「同一份报文有两个长度解释」，
        // 收端各自按己方理解切包正是请求走私的温床。这一口径与上层定界器一致
        if (m_hasContentLength && parsedLength != m_contentLength)
        {
            failMalformed("HTTP 报文解析失败：Content-Length 出现多个不一致的取值");
            return false;
        }

        // 声明的长度本身就超限：现在判错，而不是等正文真的收满上限才判——
        // 那样等于按对端的声明替它预留内存，声明一个天文数字就能把缓冲区耗光
        if (exceedsLimit(parsedLength, m_limits.maximumBodySize))
        {
            failBodyTooLarge(std::format("请求体声明长度 {} 字节超出上限 {} 字节", parsedLength, m_limits.maximumBodySize));
            return false;
        }

        m_contentLength    = parsedLength;
        m_hasContentLength = true;
        return true;
    }

    void HttpParser::failMalformed(std::string message)
    {
        recordFailure(HttpParseErrorKind::Malformed, std::move(message));
    }

    void HttpParser::failHeaderTooLarge(std::string message)
    {
        recordFailure(HttpParseErrorKind::HeaderTooLarge, std::move(message));
    }

    void HttpParser::failBodyTooLarge(std::string message)
    {
        recordFailure(HttpParseErrorKind::BodyTooLarge, std::move(message));
    }

    void HttpParser::recordFailure(const HttpParseErrorKind errorKind, std::string message)
    {
        m_hasError     = true;
        m_errorKind    = errorKind;
        m_errorMessage = std::move(message);
        m_stage        = Stage::Failed;
    }

    void HttpParser::finishHeaderBlock()
    {
        // 分块请求体：先把「一条报文两个长度解释」的组合拒掉，再要求 Transfer-Encoding 只声明 chunked
        if (m_hasTransferEncoding)
        {
            // RFC 9112 §6.3：Transfer-Encoding 与 Content-Length 并存时收端必须拒绝（或只认一条并
            // 视另一条为错误），这里选拒绝——两侧各按己方理解切包正是请求走私的经典面
            if (m_hasContentLength)
            {
                failMalformed("HTTP 报文解析失败：Content-Length 与 Transfer-Encoding 不能同时出现，请只保留其中一个");
                return;
            }
            // 只实现 chunked：取值里出现 gzip 之类其它编码、或 chunked 重复出现都判错，
            // 绝不悄悄按 identity 处理（那等于按对端没声明的边界切包）
            if (!isSingleChunkedEncoding(m_transferEncodingValue))
            {
                failMalformed("HTTP 报文解析失败：Transfer-Encoding 只接受唯一的 chunked（不接受 gzip 等其它编码，"
                              "chunked 也不能与其它编码并列），请改写为 Transfer-Encoding: chunked 或改用 Content-Length");
                return;
            }
            m_stage = Stage::ChunkSize;
            return;
        }

        // 没有正文定界头的报文（GET/HEAD/DELETE 一类）在头部块结束时即完整，
        // 排在后面的字节归下一条报文，解析器一个都不吃
        if (m_contentLength == 0)
        {
            commitMessage();
            m_stage = Stage::Complete;
            return;
        }
        m_stage = Stage::Body;
    }

    void HttpParser::commitMessage()
    {
        // 移交：先把上一条报文留下的内容整体清掉（取消源一并重建，避免继承上一条的取消状态），
        // 再按解析结果逐项落进去。容器与串都按移动交付，不产生逐字节拷贝。
        // 移交之前对外请求对象一直是空壳，因此半成品阶段的 request() 读不出任何东西
        //（比「可读但不许放行」更强）
        m_currentRequest.reset();
        m_currentRequest.setMethod(m_method);
        m_currentRequest.setUri(std::move(m_uri));
        m_currentRequest.setHttpVersion(std::move(m_httpVersion));
        for (ParsedHeader &header: m_headers)
        {
            m_currentRequest.addHeader(std::move(header.name), std::move(header.value));
        }
        m_currentRequest.setBody(std::move(m_body));

        // 暂存清回初态供下一条报文复用：clear 保留容量，因此稳态下不再为它们分配内存
        clearMessageScratch();
    }

    bool HttpParser::takeContinueRequest() noexcept
    {
        if (!m_hasContinueExpectation || m_hasTakenContinue)
        {
            return false;
        }

        // 「正文还没收完」的四种阶段：定长正文、分块的大小行/块数据/块尾 CRLF。
        // Trailer 阶段正文已经收完（对端不再等本端表态），Complete/Failed 同理
        const bool isRequestBodyPending = m_stage == Stage::Body || m_stage == Stage::ChunkSize || m_stage == Stage::ChunkData
                                          || m_stage == Stage::ChunkDataTerminator;
        if (!isRequestBodyPending)
        {
            return false;
        }

        m_hasTakenContinue = true;
        return true;
    }

    void HttpParser::clearMessageScratch() noexcept
    {
        m_method = HttpMethod::UNKNOWN;
        m_uri.clear();
        m_httpVersion.clear();
        m_headers.clear();
        m_body.clear();

        m_contentLength      = 0;
        m_receivedBodyLength = 0;
        m_headerFieldCount   = 0;
        m_headerBlockLength  = 0;
        m_hasContentLength   = false;

        // 100-continue 的两项也随报文一起清：跨报文复用解析器时，上一条的「等 100」不能让
        // 下一条报文被多回一个 100
        m_hasContinueExpectation = false;
        m_hasTakenContinue       = false;

        // 分块解码的进度必须与正文一起清空：跨报文复用同一个解析器对象时，残留的
        // 「当前块剩余字节」会把下一条报文的正文按上一条的块边界切开
        m_hasTransferEncoding      = false;
        m_transferEncodingValue.clear();
        m_chunkRemainingBytes      = 0;
        m_chunkTerminatorBytesSeen = 0;
    }

} // namespace AsynGyanis::Net
