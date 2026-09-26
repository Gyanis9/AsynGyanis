#include "Net/Http/Client/HttpResponseParser.h"

#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <cstdlib>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 取一行直到 CRLF，返回行内容并推进 data，数据不足时返回 false
        bool takeLine(std::string_view &data, std::string_view &line)
        {
            const auto pos = data.find("\r\n");
            if (pos == std::string_view::npos)
                return false;
            line = data.substr(0, pos);
            data.remove_prefix(pos + 2);
            return true;
        }
        /// 取一行并拼入 lineBuffer（应对跨馈送调用的行拆分）
        /// @note 慢路径交出的行指向 lineBuffer，调用方必须在下一次 feedLine 之前用完它；
        ///       清缓冲推迟到下一次调用（见 isLineHandedOut）——交出去就清会让 MSVC 的
        ///       std::string 在首字节写 NUL，调用方读到的是坏内容
        bool feedLine(std::string_view &data, std::string &buffer, std::string_view &line, bool &isLineHandedOut)
        {
            // 上一次慢路径交出去的行按契约已经用完，此刻才清暂存
            if (isLineHandedOut)
            {
                buffer.clear();
                isLineHandedOut = false;
            }
            if (buffer.empty())
            {
                if (takeLine(data, line))
                    return true;
            }
            // 跨段 CRLF：缓冲末字节是 '\r'、本段首字节是 '\n' 时，终止符正好被切开，
            // 下面按 "\r\n" 查找是找不到的——不单独识别，这一行会一直拼下去（两行并成一行）
            if (!buffer.empty() && buffer.back() == '\r' && !data.empty() && data.front() == '\n')
            {
                buffer.pop_back();
                line            = buffer;
                isLineHandedOut = true;
                data.remove_prefix(1);
                return true;
            }
            // 拼入已有缓冲
            const auto pos = data.find("\r\n");
            if (pos == std::string_view::npos)
            {
                buffer.append(data.data(), data.size());
                data = {};
                return false;
            }
            buffer.append(data.data(), pos);
            line            = buffer;
            isLineHandedOut = true;
            data.remove_prefix(pos + 2);
            return true;
        }
        /// 小写化一个 string_view（只用于头部名比较）
        std::string toLower(const std::string_view s)
        {
            std::string r;
            r.reserve(s.size());
            for (const char c: s)
                r.push_back(toLowerAscii(c));
            return r;
        }
        /// 取 Transfer-Encoding 的最后一个编码（RFC 9112 §6.1：chunked 必须是最后一个编码）
        std::string_view lastTransferEncoding(const std::string_view value) noexcept
        {
            std::string_view last;
            std::size_t      start = 0;
            while (start <= value.size())
            {
                const auto             comma = value.find(',', start);
                const std::string_view entry = value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
                const std::string_view token = trimOptionalWhitespace(entry);
                if (!token.empty())
                {
                    last = token;
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return last;
        }
        /// 严格解析十进制长度：只接受纯数字（不取整、不忽略后缀），超长按非法拒绝
        bool parseDecimalLength(const std::string_view text, std::size_t &length) noexcept
        {
            // 19 位十进制已覆盖现实里可能的长度，再多就是损坏或恶意的取值
            if (text.empty() || text.size() > 19)
                return false;
            std::size_t value = 0;
            for (const char character: text)
            {
                if (character < '0' || character > '9')
                    return false;
                value = value * 10 + static_cast<std::size_t>(character - '0');
            }
            length = value;
            return true;
        }
        /// 严格解析十六进制块大小：只接受 1 个以上的十六进制数字（RFC 9112 §7.1）
        bool parseChunkSize(const std::string_view text, std::size_t &chunkSize) noexcept
        {
            if (text.empty() || text.size() > 16)
                return false;
            std::size_t value = 0;
            for (const char character: text)
            {
                std::size_t digit = 0;
                if (character >= '0' && character <= '9')
                    digit = static_cast<std::size_t>(character - '0');
                else if (character >= 'a' && character <= 'f')
                    digit = static_cast<std::size_t>(character - 'a') + 10;
                else if (character >= 'A' && character <= 'F')
                    digit = static_cast<std::size_t>(character - 'A') + 10;
                else
                    return false;
                value = value * 16 + digit;
            }
            chunkSize = value;
            return true;
        }
        /// 204 / 304 一律没有正文（RFC 9112 §6.1）；1xx 另有分支处理（它是过渡响应，后面还有真正的响应）
        bool statusHasNoBody(const int statusCode) noexcept
        {
            return statusCode == 204 || statusCode == 304;
        }
    } // namespace

    std::size_t HttpResponseParser::feed(const std::string_view raw)
    {
        // 行长度闸门：取行路径有多处（状态行、头部行、块大小行…），把闸门放在入口一处就够——
        // 行缓冲只在这里增长，一行永不含 CRLF 的字节流因此不会无界吃内存
        if (kDefaultMaximumLineByteCount != 0 && m_lineBuffer.size() > kDefaultMaximumLineByteCount)
        {
            m_stage = Stage::Failed;
            return 0;
        }

        auto       data      = raw;
        const auto startSize = raw.size();
        while (!data.empty() && m_stage != Stage::Complete && m_stage != Stage::Failed)
        {
            switch (m_stage)
            {
                case Stage::StatusLine:
                {
                    std::string_view line;
                    if (!feedLine(data, m_lineBuffer, line, m_isLineHandedOut))
                        return startSize - data.size();
                    // "HTTP/1.1 200 OK" 或 "HTTP/1.0 200 OK"
                    if (line.size() < 12 || !line.starts_with("HTTP/1."))
                    {
                        m_stage = Stage::Failed;
                        return startSize - data.size();
                    }
                    // HTTP/1.1 与 1.0 在正文定界上不再区别对待：无定界头时一律读到连接关闭（RFC 9112 §6.3）
                    const auto sp1 = line.find(' ', 8);
                    if (sp1 == std::string_view::npos)
                    {
                        m_stage = Stage::Failed;
                        return startSize - data.size();
                    }
                    auto codeStr = line.substr(sp1 + 1);
                    if (codeStr.size() < 3)
                    {
                        m_stage = Stage::Failed;
                        return startSize - data.size();
                    }
                    const auto codeEnd    = codeStr.find(' ');
                    m_result.statusCode   = std::atoi(std::string(codeStr.substr(0, 3)).c_str());
                    m_result.reasonPhrase = codeEnd != std::string_view::npos ? std::string(codeStr.substr(codeEnd + 1)) : "";
                    m_stage               = Stage::Headers;
                    break;
                }
                case Stage::Headers:
                {
                    std::string_view line;
                    if (!feedLine(data, m_lineBuffer, line, m_isLineHandedOut))
                        return startSize - data.size();
                    if (line.empty())
                    {
                        // 头部收完：先按 RFC 9112 §6 把定界头核一遍，再决定正文形态
                        bool        hasContentLength    = false;
                        bool        hasTransferEncoding = false;
                        std::size_t declaredLength      = 0;
                        // 多字段 Transfer-Encoding 要按 §6.1 合并成一个列表再取最后一个编码：
                        // 「chunked 必须在末尾」是对合并结果说的，逐条看会把 chunked, gzip 误判成 chunked
                        std::string combinedTransferEncoding;
                        for (auto &[name, value]: m_result.headers)
                        {
                            const auto lowerName = toLower(name);
                            if (lowerName == "content-length")
                            {
                                // 重复出现只允许取值完全一致；取值必须是纯数字——长度有歧义就不猜
                                std::size_t thisLength = 0;
                                if (!parseDecimalLength(trimOptionalWhitespace(value), thisLength) || (hasContentLength && thisLength != declaredLength))
                                {
                                    m_stage = Stage::Failed;
                                    return startSize - data.size();
                                }
                                declaredLength   = thisLength;
                                hasContentLength = true;
                            } else if (lowerName == "transfer-encoding")
                            {
                                hasTransferEncoding = true;
                                if (!combinedTransferEncoding.empty())
                                {
                                    combinedTransferEncoding += ", ";
                                }
                                combinedTransferEncoding += trimOptionalWhitespace(value);
                            }
                        }

                        if (hasTransferEncoding && hasContentLength)
                        {
                            // 两者并存一律拒绝：挑一个信正是响应走私的入口
                            m_stage = Stage::Failed;
                            return startSize - data.size();
                        }
                        if (m_result.statusCode >= 100 && m_result.statusCode < 200)
                        {
                            // 1xx 是过渡响应（100 Continue、103 Early Hints）：它只是最终响应之前的一声招呼，
                            // 当成最终响应收尾会让调用方拿着 103 当结果、真正的响应被整条丢掉。
                            // 清掉本轮的状态与头部，回到状态行接着解析后面那一条
                            m_result.statusCode = 0;
                            m_result.reasonPhrase.clear();
                            m_result.headers.clear();
                            // 定界标志也要复位：过渡响应若带了 Transfer-Encoding，留在成员上会把
                            // 随后那条真正响应的正文按分块解读
                            m_isChunked            = false;
                            m_isCloseDelimited     = false;
                            m_headerBlockByteCount = 0;
                            m_expectedBodyBytes    = 0;
                            m_chunkSize            = 0;
                            m_chunkPhase           = ChunkPhase::SizeLine;
                            m_stage                = Stage::StatusLine;
                            break;
                        }
                        if (statusHasNoBody(m_result.statusCode))
                        {
                            // 无正文的状态码：即便带了 Content-Length 也不能据此等待正文
                            m_stage = Stage::Complete;
                            break;
                        }
                        if (m_isHeadResponse)
                        {
                            // 对 HEAD 的应答一律在头块之后结束（RFC 9112 §6.3 第 1 条），
                            // 无论带不带定界头都不该再去等正文
                            m_stage = Stage::Complete;
                            break;
                        }
                        if (hasContentLength)
                        {
                            // 声明的长度本身就是对端给的：先按上限判一次，免得为一条永远收不完的
                            // 响应白分配缓冲（chunked 与读到关闭两条路只能边收边判，见下面两处）
                            if (m_maximumBodySize != 0 && declaredLength > m_maximumBodySize)
                            {
                                m_isBodyLimitHit = true;
                                m_stage          = Stage::Failed;
                                break;
                            }
                            m_expectedBodyBytes = declaredLength;
                            m_stage             = declaredLength == 0 ? Stage::Complete : Stage::Body;
                        } else if (hasTransferEncoding)
                        {
                            if (equalsIgnoringCase(lastTransferEncoding(combinedTransferEncoding), "chunked"))
                            {
                                m_isChunked  = true;
                                m_chunkPhase = ChunkPhase::SizeLine;
                                m_stage      = Stage::Body;
                            } else
                            {
                                // 末尾编码不是 chunked：长度无法自定界，只能读到连接关闭（RFC 9112 §6.3）
                                m_isCloseDelimited = true;
                                m_stage            = Stage::Body;
                            }
                        } else
                        {
                            // 既无 Transfer-Encoding 也无 Content-Length 的响应（不论 1.0 还是 1.1）：
                            // 正文长度由「对端关闭连接前收到的字节数」决定（RFC 9112 §6.3 第 8 条）
                            m_isCloseDelimited = true;
                            m_stage            = Stage::Body;
                        }
                        break;
                    }
                    // 解析头部行 "Name: Value"
                    const auto  colon = line.find(':');
                    std::string name, value;
                    if (colon != std::string_view::npos)
                    {
                        name  = std::string(line.substr(0, colon));
                        value = std::string(line.substr(colon + 1));
                        // 去掉值前导空白
                        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                            value.erase(0, 1);
                    } else
                    {
                        name = std::string(line);
                    }
                    m_headerBlockByteCount += name.size() + value.size();
                    if ((kDefaultMaximumHeaderCount != 0 && m_result.headers.size() >= kDefaultMaximumHeaderCount) ||
                        (kDefaultMaximumHeaderBlockByteCount != 0 && m_headerBlockByteCount > kDefaultMaximumHeaderBlockByteCount))
                    {
                        // 与正文上限同源：头部也是「对端说了算」的字节数，没有闸门就是让对方
                        // 决定本端分配多少内存
                        m_stage = Stage::Failed;
                        break;
                    }
                    m_result.headers.emplace_back(std::move(name), std::move(value));
                    break;
                }
                case Stage::Body:
                {
                    if (m_isChunked)
                    {
                        // 分段读取（RFC 9112 §7.1）：块大小行 → 块数据 → CRLF，0 块之后是 trailer 段，
                        // 以空行收尾。每一步都可能跨多次 feed()，阶段因此记在成员里
                        while (!data.empty() && m_stage == Stage::Body)
                        {
                            if (m_chunkPhase == ChunkPhase::Data)
                            {
                                const auto toCopy = std::min(m_chunkSize, data.size());
                                m_result.body.append(data.data(), toCopy);
                                data.remove_prefix(toCopy);
                                m_chunkSize -= toCopy;
                                // 分块的长度由对端一块一块给：只有边收边判才拦得住「无限分块」
                                if (isAccumulatedBodyOverLimit())
                                {
                                    m_isBodyLimitHit = true;
                                    m_stage          = Stage::Failed;
                                    break;
                                }
                                if (m_chunkSize == 0)
                                {
                                    // 数据段收满：随后必须是 CRLF
                                    m_chunkPhase = ChunkPhase::DataTerminator;
                                }
                                continue;
                            }

                            std::string_view line;
                            if (!feedLine(data, m_lineBuffer, line, m_isLineHandedOut))
                                break;
                            if (m_chunkPhase == ChunkPhase::SizeLine)
                            {
                                // 块大小可带扩展（;ext=value）：只取分号前的十六进制数字
                                const auto             semicolonPosition = line.find(';');
                                const std::string_view sizeText          = semicolonPosition == std::string_view::npos ? line : line.substr(0, semicolonPosition);
                                std::size_t            chunkSize         = 0;
                                if (!parseChunkSize(sizeText, chunkSize))
                                {
                                    m_stage = Stage::Failed;
                                    break;
                                }
                                m_chunkSize = chunkSize;
                                // 0 块 = 正文到此为止，后面只剩 trailer 段
                                m_chunkPhase = chunkSize == 0 ? ChunkPhase::Trailer : ChunkPhase::Data;
                                continue;
                            }
                            if (m_chunkPhase == ChunkPhase::DataTerminator)
                            {
                                // 块数据之后必须是 CRLF（feedLine 已把 CRLF 吃掉，这一行必须为空）
                                if (!line.empty())
                                {
                                    m_stage = Stage::Failed;
                                    break;
                                }
                                m_chunkPhase = ChunkPhase::SizeLine;
                                continue;
                            }
                            // Trailer 段：空行表示报文完整；其余行按头字段形态校验后忽略
                            if (line.empty())
                            {
                                m_stage = Stage::Complete;
                                break;
                            }
                            if (line.find(':') == std::string_view::npos)
                            {
                                m_stage = Stage::Failed;
                                break;
                            }
                        }
                        break;
                    }
                    if (m_expectedBodyBytes > 0)
                    {
                        const auto available = data.size();
                        const auto toCopy    = std::min(m_expectedBodyBytes, available);
                        m_result.body.append(data.data(), toCopy);
                        data.remove_prefix(toCopy);
                        m_expectedBodyBytes -= toCopy;
                        if (m_expectedBodyBytes == 0)
                            m_stage = Stage::Complete;
                        break;
                    }
                    if (m_isCloseDelimited)
                    {
                        // 把所有剩余数据收作正文，连接关闭即「正文完成」。
                        // 超上限按失败收口：这条路的正文长度完全由对端决定（一直不关连接就一直收），
                        // 没有上限就是让对端决定本进程分配多少内存
                        m_result.body.append(data.data(), data.size());
                        data = {};
                        if (isAccumulatedBodyOverLimit())
                        {
                            m_isBodyLimitHit = true;
                            m_stage          = Stage::Failed;
                        }
                        break;
                    }
                    // 无正文
                    m_stage = Stage::Complete;
                    break;
                }
                default:
                    break;
            }
        }
        return startSize - data.size();
    }

    HttpResponseParser::HttpResponseParser(const std::size_t maximumBodySize) noexcept : m_maximumBodySize(maximumBodySize)
    {
    }

    void HttpResponseParser::reset()
    {
        m_stage  = Stage::StatusLine;
        m_result = {};
        m_lineBuffer.clear();
        m_isLineHandedOut      = false;
        m_expectedBodyBytes    = 0;
        m_isChunked            = false;
        m_isCloseDelimited     = false;
        m_headerBlockByteCount = 0;
        m_chunkPhase           = ChunkPhase::SizeLine;
        m_chunkSize            = 0;
        // 越界这一位也是「按条」的状态：留着它，下一条响应什么都不做也会被告知「正文超限」
        m_isBodyLimitHit = false;
        // HEAD 的标记也是「按请求」的状态：keep-alive 上复用同一个解析器时，漏了它会让第二条响应
        // 也在头块之后收口——正文被静默丢掉，而状态码看着完全正常
        m_isHeadResponse = false;
    }

    void HttpResponseParser::endOfStream()
    {
        if (m_stage == Stage::Body && m_isCloseDelimited)
        {
            m_stage = Stage::Complete;
        }
        // content-length 或 chunked 不完整 → 按失败处理
        if (m_stage == Stage::Body && !m_isCloseDelimited)
        {
            m_stage = Stage::Failed;
        }
    }
} // namespace AsynGyanis::Net
