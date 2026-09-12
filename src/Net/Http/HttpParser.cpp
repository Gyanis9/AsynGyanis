#include "Net/Http/HttpParser.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
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
         * @brief 判断逗号分隔的列表里是否含指定 token
         * @details 用于 Transfer-Encoding 这类列表值：只认整段匹配，避免 "xchunked"、
         *          "chunked-fake" 被当作分块传输蒙混过关。
         * @param listValue 列表值原文
         * @param token 目标 token（小写）
         * @return true 表示列表里确实有该 token
         */
        bool containsListToken(const std::string_view listValue, const std::string_view token) noexcept
        {
            std::size_t offset = 0;
            while (offset <= listValue.size())
            {
                const std::size_t comma   = listValue.find(',', offset);
                const std::size_t segment = comma == std::string_view::npos ? listValue.size() - offset : comma - offset;
                if (equalsIgnoringCase(trimOptionalWhitespace(listValue.substr(offset, segment)), token))
                {
                    return true;
                }
                if (comma == std::string_view::npos)
                {
                    return false;
                }
                offset = comma + 1;
            }
            return false;
        }
    } // namespace

    ParseStatus HttpParser::parse(const char *const data, const size_t length)
    {
        // 已收齐：一字节都不再吃。这些字节属于流水线里的下一条报文，喂进已完成的解析器
        // 会被当作新报文的开头，把上一条已定稿的结果改坏
        if (m_stage == Stage::Complete)
        {
            return ParseStatus::Done;
        }

        // 错误粘滞：非 reset() 不能恢复，调用方要么重置要么断开连接
        if (m_stage == Stage::Failed)
        {
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
                if (m_body.size() + chunkLength > kMaximumBodySize)
                {
                    failLimit(std::format("请求体超出上限 {} 字节", kMaximumBodySize));
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

            // 请求行与头部行都按「整行」推进：行体跨两次输入时由 takeLine 拼接
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
            if (!parseHeaderLine(line))
            {
                break;
            }
        }

        if (m_stage == Stage::Failed)
        {
            return ParseStatus::Error;
        }
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
        m_isLimitExceeded    = false;
        m_errorMessage.clear();
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
        return m_isLimitExceeded;
    }

    std::string HttpParser::errorMessage() const
    {
        return m_errorMessage;
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
                failProtocol("HTTP 报文解析失败：行尾必须是 CRLF（不允许单独出现 LF）");
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
            failProtocol("HTTP 报文解析失败：行尾必须是 CRLF（不允许单独出现 LF）");
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
            if (length > kMaximumRequestLineLength)
            {
                failLimit(std::format("请求行超出上限 {} 字节", kMaximumRequestLineLength));
                return false;
            }
            return true;
        }

        if (length > kMaximumHeaderLineLength)
        {
            failLimit(std::format("头部行超出上限 {} 字节", kMaximumHeaderLineLength));
            return false;
        }
        return true;
    }

    bool HttpParser::parseRequestLine(const std::string_view line)
    {
        // 三段由空格分隔，且目标里不允许再出现空格：用首个与末个空格切成三段后，
        // 中间那段自然就是「不含空格的目标」
        const std::size_t firstSpace = line.find(' ');
        const std::size_t lastSpace  = line.rfind(' ');
        if (firstSpace == std::string_view::npos || firstSpace == lastSpace)
        {
            failProtocol("HTTP 报文解析失败：请求行必须是「方法 目标 版本」三段，以空格分隔");
            return false;
        }

        const std::string_view methodText  = line.substr(0, firstSpace);
        const std::string_view targetText  = line.substr(firstSpace + 1, lastSpace - firstSpace - 1);
        const std::string_view versionText = line.substr(lastSpace + 1);

        if (methodText.empty() || methodText.size() > kMaximumMethodLength)
        {
            failProtocol(std::format("HTTP 报文解析失败：请求方法长度必须在 1 到 {} 字节之间", kMaximumMethodLength));
            return false;
        }
        for (const char character: methodText)
        {
            if (!isTokenCharacter(static_cast<unsigned char>(character)))
            {
                failProtocol("HTTP 报文解析失败：请求方法只能由 token 字符组成");
                return false;
            }
        }

        if (targetText.empty())
        {
            failProtocol("HTTP 报文解析失败：请求目标不能为空");
            return false;
        }
        if (targetText.size() > kMaximumUriLength)
        {
            failLimit(std::format("请求 URI 超出上限 {} 字节", kMaximumUriLength));
            return false;
        }
        for (const char character: targetText)
        {
            if (!isTargetCharacter(static_cast<unsigned char>(character)))
            {
                failProtocol("HTTP 报文解析失败：请求目标含非法字符（空格与控制字符都不允许）");
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
            failProtocol("HTTP 报文解析失败：版本必须是 HTTP/1.x 或 HTTP/0.x 的形式");
            return false;
        }

        m_method = HttpRequest::methodFromString(methodText);
        m_uri.assign(targetText);

        // 版本按收到的原文保存：上层要按 1.0/0.9 判定保活策略，重新拼装反而可能丢掉差异
        m_httpVersion.assign(versionText);
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

        // 折行（obs-fold）：RFC 9112 已把以空白开头的续行判为过时，这里明确拒绝而不是静默拼接，
        // 否则同一个头部名可能被两个来源写出不同含义（请求走私的经典入口）
        if (line.front() == ' ' || line.front() == '\t')
        {
            failProtocol("HTTP 报文解析失败：不支持折行（obs-fold）头部，请把值写在同一行");
            return false;
        }

        const std::size_t colonPosition = line.find(':');
        if (colonPosition == std::string_view::npos || colonPosition == 0)
        {
            failProtocol("HTTP 报文解析失败：头部行必须是「名: 值」的形式");
            return false;
        }

        const std::string_view name = line.substr(0, colonPosition);
        for (const char character: name)
        {
            // 冒号前若有空白也会落到这里：token 字符集不含 SP 与 HTAB
            if (!isTokenCharacter(static_cast<unsigned char>(character)))
            {
                failProtocol("HTTP 报文解析失败：头部名只能由 token 字符组成（冒号前不得有空白）");
                return false;
            }
        }
        if (name.size() > kMaximumHeaderFieldNameLength)
        {
            failLimit(std::format("请求头部名超出上限 {} 字节", kMaximumHeaderFieldNameLength));
            return false;
        }

        const std::string_view value = trimOptionalWhitespace(line.substr(colonPosition + 1));
        if (value.size() > kMaximumHeaderFieldValueLength)
        {
            failLimit(std::format("请求头部值超出上限 {} 字节", kMaximumHeaderFieldValueLength));
            return false;
        }
        for (const char character: value)
        {
            if (!isHeaderValueCharacter(static_cast<unsigned char>(character)))
            {
                failProtocol("HTTP 报文解析失败：头部值含非法控制字符");
                return false;
            }
        }

        // 头部块总长（名与值的净字节）与条数是两道独立的闸：单条名、单条值、条数各自合规，
        // 架不住上百条头部叠出来的总量。两道判定都在落库之前，拒绝路径不留半成品
        if (m_headerBlockLength + name.size() + value.size() > kMaximumHeaderBlockLength)
        {
            failLimit(std::format("请求头部总长超出上限 {} 字节", kMaximumHeaderBlockLength));
            return false;
        }
        if (m_headerFieldCount >= kMaximumHeaderCount)
        {
            failLimit(std::format("请求头部条数超出上限 {} 条", kMaximumHeaderCount));
            return false;
        }

        // Content-Length 决定正文边界；Transfer-Encoding 指到分块编码时本框架无法定界，
        // 当场判错而不是猜一个长度继续（上层定界器也在更早一步拦下分块请求体）
        if (equalsIgnoringCase(name, "content-length"))
        {
            if (!parseContentLength(value))
            {
                return false;
            }
        } else if (equalsIgnoringCase(name, "transfer-encoding") && containsListToken(value, "chunked"))
        {
            failProtocol("HTTP 报文解析失败：不支持分块请求体（Transfer-Encoding: chunked），请改用 Content-Length");
            return false;
        }

        m_headerBlockLength += name.size() + value.size();
        ++m_headerFieldCount;
        m_headers.push_back(ParsedHeader{std::string(name), std::string(value)});
        return true;
    }

    bool HttpParser::parseContentLength(const std::string_view value)
    {
        if (value.empty())
        {
            failProtocol("HTTP 报文解析失败：Content-Length 不能为空");
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
            failProtocol("HTTP 报文解析失败：Content-Length 只能是十进制数字");
            return false;
        }

        // 重复出现时只允许取值一致：不一致意味着「同一份报文有两个长度解释」，
        // 收端各自按己方理解切包正是请求走私的温床。这一口径与上层定界器一致
        if (m_hasContentLength && parsedLength != m_contentLength)
        {
            failProtocol("HTTP 报文解析失败：Content-Length 出现多个不一致的取值");
            return false;
        }

        m_contentLength    = parsedLength;
        m_hasContentLength = true;
        return true;
    }

    void HttpParser::failProtocol(std::string message)
    {
        m_hasError        = true;
        m_isLimitExceeded = false;
        m_errorMessage    = std::move(message);
        m_stage           = Stage::Failed;
    }

    void HttpParser::failLimit(std::string message)
    {
        m_hasError        = true;
        m_isLimitExceeded = true;
        m_errorMessage    = std::move(message);
        m_stage           = Stage::Failed;
    }

    void HttpParser::finishHeaderBlock()
    {
        // 没有 Content-Length 的报文（GET/HEAD/DELETE 一类）在头部块结束时即完整，
        // 上层定界器给出的边界与这里必须一致：多出来的字节归下一条报文
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
    }

} // namespace AsynGyanis::Net
