#include "Net/Http/MultipartForm.h"

#include "Net/Http/HttpHeaderRules.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        // CRLF：本类只认这一种行尾，裸 LF 会把分隔符判到正文里去
        constexpr std::string_view kLineBreak{"\r\n"};
        constexpr std::size_t      kLineBreakLength = 2;

        // 段头部块与正文的界线：一处空行
        constexpr std::string_view kEmptyLine{"\r\n\r\n"};
        constexpr std::size_t      kEmptyLineLength = 4;

        // RFC 2046 §5.1：结束分隔符在边界之后再带一对 "--"
        constexpr std::string_view kClosingMarker{"--"};
        constexpr std::size_t      kClosingMarkerLength = 2;

        // RFC 2046 §5.1.1：boundary 至多 70 字符
        constexpr std::size_t kMaximumBoundaryLength = 70;

        // RFC 2046 §5.1.1 的 bcharsnospace 里除字母数字之外的部分
        constexpr std::string_view kBoundaryPunctuation{"'()+_,-./:=?"};

        constexpr std::string_view kNameParameter{"name"};
        constexpr std::string_view kFileNameParameter{"filename"};
        constexpr std::string_view kBoundaryParameter{"boundary"};

        constexpr std::string_view kFormDataDisposition{"form-data"};
        constexpr std::string_view kContentDispositionHeader{"content-disposition"};
        constexpr std::string_view kContentTypeHeader{"content-type"};
        constexpr std::string_view kContentTransferEncodingHeader{"content-transfer-encoding"};

        // 「原样字节」的三种传输编码（RFC 2046 §4.6.1）。base64、quoted-printable 需要把段正文换成
        // 解码后的新字节序列，与「段正文是请求正文的视图」冲突，且 form-data 里极少见，故整份判失败
        constexpr std::array<std::string_view, 3> kIdentityTransferEncodings{"7bit", "8bit", "binary"};

        /**
         * @brief 头部参数表里的一条："name=value"
         */
        struct HeaderParameter
        {
            std::string name;  ///< 参数名，已折成小写 ASCII，便于大小写不敏感查找
            std::string value; ///< 取值，quoted-string 的反斜杠转义已展开
        };

        /**
         * @brief 分隔符行的解析结果
         */
        struct DelimiterLine
        {
            std::size_t nextStart{}; ///< 本行 CRLF 之后的下标，即下一段头部的起点
            bool        closing{};   ///< true 是结束分隔符（"--boundary--"）
        };

        /**
         * @brief 判断字节是不是 ASCII 字母或数字
         * @param character 待判字节
         * @return true 属于 [0-9A-Za-z]
         */
        [[nodiscard]] bool isAsciiLetterOrDigit(const char character) noexcept
        {
            return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        }

        /**
         * @brief 判断字节是不是控制字符
         * @details 空格不算控制字符：它正是 quoted-string 里合法的内容字节。高位字节（UTF-8 续字节）
         *          也不算，文件名与字段名的非 ASCII 写法要原样留给调用方。
         * @param character 待判字节
         * @return true 是 C0 控制字符或 DEL
         */
        [[nodiscard]] bool isControlCharacter(const char character) noexcept
        {
            const unsigned char byte = static_cast<unsigned char>(character);
            return byte < 0x20 || byte == 0x7F;
        }

        /**
         * @brief 校验 boundary 取值（RFC 2046 §5.1.1）
         * @details 语法是 0*69 bchars 加最后一位 bcharsnospace：末位不能是空格，否则它与分隔符行尾的
         *          transport padding 分不开，两条边界会长成同一条。
         * @param boundary Content-Type 里 boundary 参数的取值
         * @return true 可以拿它当边界用
         */
        [[nodiscard]] bool isValidBoundary(const std::string_view boundary) noexcept
        {
            if (boundary.empty() || boundary.size() > kMaximumBoundaryLength || boundary.back() == ' ')
            {
                return false;
            }
            for (const char character: boundary)
            {
                if (character != ' ' && !isAsciiLetterOrDigit(character) && kBoundaryPunctuation.find(character) == std::string_view::npos)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 解析 quoted-string（RFC 9110 §5.6.4）
         * @details 反斜杠按 quoted-pair 展开成后一个字符；引号里出现裸控制字符判失败，因为 filename
         *          带着 CR/LF 就等于让对端往段头部里插行。
         * @param text 含本参数的整段头部取值
         * @param from 起始引号的下标
         * @param value 输出：展开后的内容（仅成功时写入）
         * @return 结束引号之后的下标；缺闭合引号或引号内含控制字符时为 npos
         */
        [[nodiscard]] std::size_t parseQuotedString(std::string_view text, const std::size_t from, std::string &value)
        {
            // 内容先攒在局部串里：中途判失败时不能把半截东西留在出参上
            std::string scratch;
            std::size_t index = from + 1;
            while (index < text.size())
            {
                const char character = text[index];
                if (character == '\\')
                {
                    // 行尾的反斜杠没有可展开的对象，按未闭合处理
                    if (index + 1 >= text.size())
                    {
                        return std::string_view::npos;
                    }
                    scratch.push_back(text[index + 1]);
                    index += 2;
                    continue;
                }
                if (character == '"')
                {
                    value = std::move(scratch);
                    return index + 1;
                }
                if (isControlCharacter(character))
                {
                    return std::string_view::npos;
                }
                scratch.push_back(character);
                ++index;
            }
            // 一路走到取值末尾也没见到闭合引号
            return std::string_view::npos;
        }

        /**
         * @brief 逐段读取头部里的 "; name=value" 参数表
         *
         * @details 值支持 token 与 quoted-string 两种写法。判失败的情形都是「将就了就得猜」：
         *          @li 名字带 "*" 的扩展参数（RFC 2231 的分片与字符集）；
         *          @li 同名参数出现两次（两份取值没有规范规定的胜者）；
         *          @li 有 ';' 却没有 '='、参数名不是 token、引号后面还跟着垃圾、以 ';' 收尾。
         *
         * @param text 头部取值整体
         * @param from 第一个 ';' 的下标；等于 text.size() 表示没有参数
         * @return 参数序列（按出现顺序）；格式不合时为空
         */
        [[nodiscard]] std::optional<std::vector<HeaderParameter>> parseParameterList(std::string_view text, const std::size_t from)
        {
            std::vector<HeaderParameter> parameters;
            std::size_t                  separator = from;
            while (separator < text.size())
            {
                const std::size_t segmentStart = separator + 1;
                const std::size_t equals       = text.find('=', segmentStart);
                if (equals == std::string_view::npos)
                {
                    return std::nullopt;
                }

                const std::string_view rawName = trimOptionalWhitespace(text.substr(segmentStart, equals - segmentStart));
                if (rawName.empty() || rawName.back() == '*' || !containsOnlyTokenCharacters(rawName))
                {
                    return std::nullopt;
                }

                HeaderParameter parameter;
                parameter.name.reserve(rawName.size());
                for (const char character: rawName)
                {
                    parameter.name.push_back(toLowerAscii(character));
                }
                for (const HeaderParameter &existing: parameters)
                {
                    if (existing.name == parameter.name)
                    {
                        return std::nullopt;
                    }
                }

                const std::size_t valueStart = equals + 1;
                if (valueStart < text.size() && text[valueStart] == '"')
                {
                    const std::size_t afterQuote = parseQuotedString(text, valueStart, parameter.value);
                    if (afterQuote == std::string_view::npos)
                    {
                        return std::nullopt;
                    }
                    std::size_t next = afterQuote;
                    while (next < text.size() && (text[next] == ' ' || text[next] == '\t'))
                    {
                        ++next;
                    }
                    // 引号闭合后只允许再接 OWS、';' 或取值末尾
                    if (next < text.size() && text[next] != ';')
                    {
                        return std::nullopt;
                    }
                    separator = next;
                } else
                {
                    const std::size_t next       = text.find(';', valueStart);
                    const std::size_t segmentEnd = next == std::string_view::npos ? text.size() : next;
                    parameter.value              = std::string(trimOptionalWhitespace(text.substr(valueStart, segmentEnd - valueStart)));
                    separator                    = segmentEnd;
                }
                parameters.push_back(std::move(parameter));
            }
            return parameters;
        }

        /**
         * @brief 按小写名字查一条参数
         * @param parameters 参数序列
         * @param name 参数名（小写书写）
         * @return 指向参数表内条目的指针；没有时为空指针
         */
        [[nodiscard]] const HeaderParameter *findParameter(const std::vector<HeaderParameter> &parameters, const std::string_view name)
        {
            for (const HeaderParameter &parameter: parameters)
            {
                if (parameter.name == name)
                {
                    return &parameter;
                }
            }
            return nullptr;
        }

        /**
         * @brief 把对端送来的文件名收成单段名字
         *
         * @details Windows 与部分客户端会把整条路径写进 filename（`C:\\dir\\a.png`、`/tmp/a.png`），
         *          调用方一拼接就能穿越到别的目录，RFC 7578 §4.4 因此要求服务端只取最后一段。
         *          两种分隔符都当分隔用：对端来自哪个系统猜不准。
         *
         * @param rawFileName filename 的原样取值
         * @return 剥掉目录部分的名字；`filename=""` 为空串（那是「没选文件」的常见写法）；
         *         含控制字符或收成 "." / ".." 时为空，表示这份正文该整份拒掉
         */
        [[nodiscard]] std::optional<std::string> sanitizeFileName(const std::string_view rawFileName)
        {
            const std::size_t      lastSeparator = rawFileName.find_last_of("/\\");
            const std::string_view baseName      = lastSeparator == std::string_view::npos ? rawFileName : rawFileName.substr(lastSeparator + 1);
            for (const char character: baseName)
            {
                if (isControlCharacter(character))
                {
                    return std::nullopt;
                }
            }
            if (baseName.empty())
            {
                return std::string();
            }
            // 这两个名字指向父目录与自身：拿它们当落盘文件名，写出去的位置就不是调用方给的那个目录
            if (baseName == "." || baseName == "..")
            {
                return std::nullopt;
            }
            return std::string(baseName);
        }

        /**
         * @brief 解析段的 Content-Disposition
         * @param value 头部取值，形如 `form-data; name="field"; filename="a.txt"`
         * @return 填好 name 与 fileName 的段（正文待定）；处置类型不是 form-data、name 缺失或为空、
         *         文件名收成穿越写法时为空
         */
        [[nodiscard]] std::optional<MultipartPart> parseContentDisposition(const std::string_view value)
        {
            const std::size_t separator = value.find(';');
            if (!equalsIgnoringCase(trimOptionalWhitespace(value.substr(0, separator)), kFormDataDisposition))
            {
                return std::nullopt;
            }
            const std::optional<std::vector<HeaderParameter>> parameters = parseParameterList(value, separator == std::string_view::npos ? value.size() : separator);
            if (!parameters.has_value())
            {
                return std::nullopt;
            }

            // 没有 name 的段无法寻址：调用方按名字取段，取不到的段等于凭空丢掉一份正文
            const HeaderParameter *nameParameter = findParameter(*parameters, kNameParameter);
            if (nameParameter == nullptr || nameParameter->value.empty())
            {
                return std::nullopt;
            }

            MultipartPart part;
            part.name = nameParameter->value;
            if (const HeaderParameter *fileNameParameter = findParameter(*parameters, kFileNameParameter); fileNameParameter != nullptr)
            {
                const std::optional<std::string> fileName = sanitizeFileName(fileNameParameter->value);
                if (!fileName.has_value())
                {
                    return std::nullopt;
                }
                part.fileName = *fileName;
            }
            return part;
        }

        /**
         * @brief 判断段声明的传输编码是否等于「原样字节」
         * @param value Content-Transfer-Encoding 的取值（已去首尾 OWS）
         * @return true 无需解码即可直接用正文；空值与 base64 一类编码判否
         */
        [[nodiscard]] bool isIdentityTransferEncoding(const std::string_view value) noexcept
        {
            for (const std::string_view encoding: kIdentityTransferEncodings)
            {
                if (equalsIgnoringCase(value, encoding))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 解析一个段的头部块
         * @param headerBlock 头部块文本，不含收尾的空行
         * @return 填好 name/fileName/contentType 的段（正文待定）；没有 Content-Disposition、
         *         某行定不了界、上述三条头部重复或编码无法还原时为空
         */
        [[nodiscard]] std::optional<MultipartPart> parsePartHeaders(std::string_view headerBlock)
        {
            MultipartPart part;
            bool          hasDisposition = false;
            bool          hasContentType = false;

            std::size_t lineStart = 0;
            while (lineStart < headerBlock.size())
            {
                const std::size_t      lineEnd       = headerBlock.find(kLineBreak, lineStart);
                const std::size_t      nextLineStart = lineEnd == std::string_view::npos ? headerBlock.size() : lineEnd + kLineBreakLength;
                const std::string_view line          = lineEnd == std::string_view::npos ? headerBlock.substr(lineStart) : headerBlock.substr(lineStart, lineEnd - lineStart);
                lineStart                            = nextLineStart;

                const std::size_t colon = line.find(':');
                if (colon == std::string_view::npos)
                {
                    return std::nullopt;
                }
                const std::string_view fieldName  = trimOptionalWhitespace(line.substr(0, colon));
                const std::string_view fieldValue = trimOptionalWhitespace(line.substr(colon + 1));
                if (!isValidHeaderFieldName(fieldName))
                {
                    return std::nullopt;
                }

                if (equalsIgnoringCase(fieldName, kContentDispositionHeader))
                {
                    if (hasDisposition)
                    {
                        return std::nullopt;
                    }
                    const std::optional<MultipartPart> disposition = parseContentDisposition(fieldValue);
                    if (!disposition.has_value())
                    {
                        return std::nullopt;
                    }
                    // 段的 name/fileName 全在这条头部里，其余已解析到的字段（contentType）保留不动
                    part.name      = disposition->name;
                    part.fileName  = disposition->fileName;
                    hasDisposition = true;
                } else if (equalsIgnoringCase(fieldName, kContentTypeHeader))
                {
                    // RFC 2045 §3.1：Content-Type 这类头部在同一段里不得重复；空取值也无法当媒体类型用
                    if (hasContentType || fieldValue.empty())
                    {
                        return std::nullopt;
                    }
                    part.contentType = std::string(fieldValue);
                    hasContentType   = true;
                } else if (equalsIgnoringCase(fieldName, kContentTransferEncodingHeader))
                {
                    if (!isIdentityTransferEncoding(fieldValue))
                    {
                        return std::nullopt;
                    }
                }
                // 其余头部（Content-ID、Content-Description 一类元数据）放过，不影响取段
            }

            if (!hasDisposition)
            {
                return std::nullopt;
            }
            return part;
        }

        /**
         * @brief 解析分隔符行：dash-boundary [ "--" ] transport-padding CRLF
         * @param body 整份正文
         * @param position 分隔符起点（"--" + boundary 的 '-' 处）
         * @param dashBoundary dash-boundary 本身
         * @return 下一段起点与「是否结束分隔符」； padding 之后既不是 CRLF 也不是正文末尾时为空
         */
        [[nodiscard]] std::optional<DelimiterLine> parseDelimiterLine(std::string_view body, const std::size_t position, const std::string_view dashBoundary)
        {
            std::size_t index   = position + dashBoundary.size();
            bool        closing = false;
            if (body.size() - index >= kClosingMarkerLength && body.compare(index, kClosingMarkerLength, kClosingMarker) == 0)
            {
                closing = true;
                index += kClosingMarkerLength;
            }
            // transport padding 只允许 SP / HTAB，它不属于边界（RFC 2046 §5.1）
            while (index < body.size() && (body[index] == ' ' || body[index] == '\t'))
            {
                ++index;
            }
            if (index == body.size())
            {
                // 结束分隔符可以省略收尾 CRLF：它后面再没有别的东西，补不补都不改变任何段的边界。
                // 非结束分隔符省略就等于下一段没写完。
                if (closing)
                {
                    return DelimiterLine{index, closing};
                }
                return std::nullopt;
            }
            if (body[index] != '\r' || index + 1 >= body.size() || body[index + 1] != '\n')
            {
                return std::nullopt;
            }
            return DelimiterLine{index + kLineBreakLength, closing};
        }

        /**
         * @brief 找下一个位于行首的分隔符
         *
         * @details RFC 2046 §5.1 的分隔符是「CRLF + --boundary」，所以候选者要么在正文开头，要么前两字节
         *          正好是一对 CRLF。正文里恰好含 `--boundary` 文本、但不在行首的位置不算分隔符。
         * @param body 整份正文
         * @param from 起搜下标
         * @param lowerBound 允许命中的最小下标（段正文至少留出终结它的那对 CRLF）
         * @param dashBoundary dash-boundary 本身
         * @return 分隔符起点；找不到时为 npos
         */
        [[nodiscard]] std::size_t findDelimiterStart(std::string_view body, std::size_t from, const std::size_t lowerBound, const std::string_view dashBoundary)
        {
            if (from < lowerBound)
            {
                from = lowerBound;
            }
            std::size_t candidate = body.find(dashBoundary, from);
            while (candidate != std::string_view::npos)
            {
                const bool atLineStart = candidate == 0 || (candidate >= kLineBreakLength && body[candidate - 2] == '\r' && body[candidate - 1] == '\n');
                if (atLineStart)
                {
                    return candidate;
                }
                candidate = body.find(dashBoundary, candidate + 1);
            }
            return std::string_view::npos;
        }
    } // namespace

    MultipartFormData::MultipartFormData(std::vector<MultipartPart> &&parts) noexcept : m_parts(std::move(parts))
    {
    }

    std::optional<MultipartFormData> MultipartFormData::parse(const std::string_view body, const std::string_view contentType)
    {
        // 类型必须是 multipart/form-data：把别的正文按边界切段是凭空造数据
        if (!contentTypeIs(contentType, "multipart/form-data"))
        {
            return std::nullopt;
        }

        const std::size_t                                 typeSeparator = contentType.find(';');
        const std::optional<std::vector<HeaderParameter>> parameters =
                parseParameterList(contentType, typeSeparator == std::string_view::npos ? contentType.size() : typeSeparator);
        if (!parameters.has_value())
        {
            return std::nullopt;
        }
        const HeaderParameter *boundaryParameter = findParameter(*parameters, kBoundaryParameter);
        if (boundaryParameter == nullptr || !isValidBoundary(boundaryParameter->value))
        {
            return std::nullopt;
        }
        const std::string dashBoundary = std::string(kClosingMarker) + boundaryParameter->value;

        // preamble（首个分隔符之前的文本）按规范忽略，直接跳到第一个行首分隔符
        const std::size_t firstDelimiter = findDelimiterStart(body, 0, 0, dashBoundary);
        if (firstDelimiter == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::optional<DelimiterLine> opening = parseDelimiterLine(body, firstDelimiter, dashBoundary);
        // 结束分隔符打头是一份段落都没有；开头分隔符没写完则后面的段都无从定位
        if (!opening.has_value() || opening->closing)
        {
            return std::nullopt;
        }

        std::vector<MultipartPart> parts;
        std::size_t                cursor = opening->nextStart;
        while (true)
        {
            // 段的头部块到第一处空行为止
            const std::size_t headerBlockEnd = body.find(kEmptyLine, cursor);
            if (headerBlockEnd == std::string_view::npos)
            {
                return std::nullopt;
            }
            std::optional<MultipartPart> part = parsePartHeaders(body.substr(cursor, headerBlockEnd - cursor));
            if (!part.has_value())
            {
                return std::nullopt;
            }

            const std::size_t contentStart = headerBlockEnd + kEmptyLineLength;
            // 正文右边界是下一个分隔符，终结正文的那对 CRLF 属于分隔符行、不属于正文
            const std::size_t nextDelimiter = findDelimiterStart(body, contentStart, contentStart + kLineBreakLength, dashBoundary);
            if (nextDelimiter == std::string_view::npos)
            {
                return std::nullopt;
            }
            part->content = body.substr(contentStart, nextDelimiter - contentStart - kLineBreakLength);
            parts.push_back(std::move(*part));

            const std::optional<DelimiterLine> line = parseDelimiterLine(body, nextDelimiter, dashBoundary);
            if (!line.has_value())
            {
                return std::nullopt;
            }
            if (line->closing)
            {
                // epilogue 按 RFC 2046 §5.1 忽略，不再往里找段
                break;
            }
            cursor = line->nextStart;
        }

        // 段序列交进私有构造：optional 的 emplace 会从 std 的上下文里调用它，那条路看不见私有构造
        return MultipartFormData{std::move(parts)};
    }

    const std::vector<MultipartPart> &MultipartFormData::parts() const noexcept
    {
        return m_parts;
    }

    const MultipartPart *MultipartFormData::findPart(const std::string_view name) const noexcept
    {
        for (const MultipartPart &part: m_parts)
        {
            if (part.name == name)
            {
                return &part;
            }
        }
        return nullptr;
    }

    std::optional<std::string_view> MultipartFormData::fieldValue(const std::string_view name) const
    {
        for (const MultipartPart &part: m_parts)
        {
            if (part.isFile() || part.name != name)
            {
                continue;
            }
            return part.content;
        }
        return std::nullopt;
    }
} // namespace AsynGyanis::Net
