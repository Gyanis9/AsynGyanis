#include "Net/Http/Client/HttpResponseParser.h"
#include <cstdlib>
namespace AsynGyanis::Net
{
    namespace
    {
        /// 取一行直到 CRLF，返回行内容并推进 data，数据不足时返回 false
        bool takeLine(std::string_view &data, std::string_view &line)
        {
            const auto pos = data.find("\r\n");
            if (pos == std::string_view::npos) return false;
            line = data.substr(0, pos);
            data.remove_prefix(pos + 2);
            return true;
        }
        /// 取一行并拼入 lineBuffer（应对跨馈送调用的行拆分）
        bool feedLine(std::string_view &data, std::string &buffer, std::string_view &line)
        {
            if (buffer.empty())
            {
                if (takeLine(data, line)) return true;
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
            line = buffer;
            data.remove_prefix(pos + 2);
            buffer.clear();
            return true;
        }
        /// 小写化一个 string_view（只用于头部名比较）
        std::string toLower(std::string_view s)
        {
            std::string r;
            r.reserve(s.size());
            for (auto c: s) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return r;
        }
    }
    std::size_t HttpResponseParser::feed(const std::string_view raw)
    {
        auto data = raw;
        const auto startSize = raw.size();
        while (!data.empty() && m_stage != Stage::Complete && m_stage != Stage::Failed)
        {
            switch (m_stage)
            {
            case Stage::StatusLine:
            {
                std::string_view line;
                if (!feedLine(data, m_lineBuffer, line)) return startSize - data.size();
                // "HTTP/1.1 200 OK" 或 "HTTP/1.0 200 OK"
                if (line.size() < 12 || !line.starts_with("HTTP/1."))
                {
                    m_stage = Stage::Failed;
                    return startSize - data.size();
                }
                const auto sp1 = line.find(' ', 8);
                if (sp1 == std::string_view::npos) { m_stage = Stage::Failed; return startSize - data.size(); }
                auto codeStr = line.substr(sp1 + 1);
                if (codeStr.size() < 3) { m_stage = Stage::Failed; return startSize - data.size(); }
                const auto codeEnd = codeStr.find(' ');
                m_result.statusCode = std::atoi(std::string(codeStr.substr(0, 3)).c_str());
                m_result.reasonPhrase = codeEnd != std::string_view::npos ? std::string(codeStr.substr(codeEnd + 1)) : "";
                m_stage = Stage::Headers;
                break;
            }
            case Stage::Headers:
            {
                std::string_view line;
                if (!feedLine(data, m_lineBuffer, line)) return startSize - data.size();
                if (line.empty())
                {
                    // 头部收完，准备进入正文阶段
                    m_isChunked = false;
                    m_isCloseDelimited = false;
                    m_expectedBodyBytes = 0;
                    for (auto &[k, v]: m_result.headers)
                    {
                        auto lk = toLower(k);
                        if (lk == "content-length")
                            m_expectedBodyBytes = static_cast<std::size_t>(std::atoll(v.c_str()));
                        else if (lk == "transfer-encoding" && v.find("chunked") != std::string::npos)
                            m_isChunked = true;
                        else if (lk == "connection" && v.find("close") != std::string::npos)
                            m_isCloseDelimited = true;
                    }
                    if (m_expectedBodyBytes > 0)
                        m_stage = Stage::Body;
                    else if (m_isChunked)
                    { m_stage = Stage::Body; m_inChunk = false; }  // chunk 解析在 Body 分支里做
                    else
                    {
                        // 无正文 → 完成
                        m_stage = Stage::Complete;
                    }
                    break;
                }
                // 解析头部行 "Name: Value"
                const auto colon = line.find(':');
                std::string name, value;
                if (colon != std::string_view::npos)
                {
                    name = std::string(line.substr(0, colon));
                    value = std::string(line.substr(colon + 1));
                    // 去掉值前导空白
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                        value.erase(0, 1);
                } else
                {
                    name = std::string(line);
                }
                m_result.headers.emplace_back(std::move(name), std::move(value));
                break;
            }
            case Stage::Body:
            {
                if (m_isChunked)
                {
                    if (!m_inChunk)
                    {
                        // 读块大小行
                        std::string_view line;
                        if (!feedLine(data, m_lineBuffer, line))
                            return startSize - data.size();
                        m_chunkSize = static_cast<std::size_t>(std::strtoll(std::string(line).c_str(), nullptr, 16));
                        if (m_chunkSize == 0)
                        {
                            m_awaitingTrailer = true;
                            m_stage = Stage::Body; // 继续读 trailer 空行
                        } else
                        {
                            m_inChunk = true;
                        }
                        break;
                    }
                    // 读块数据
                    const auto available = data.size();
                    const auto toCopy = std::min(m_chunkSize, available);
                    m_result.body.append(data.data(), toCopy);
                    data.remove_prefix(toCopy);
                    m_chunkSize -= toCopy;
                    if (m_chunkSize == 0)
                    {
                        m_inChunk = false;
                        // 等块后的 CRLF
                        if (data.size() >= 2 && data[0] == '\r' && data[1] == '\n')
                        {
                            data.remove_prefix(2);
                        }
                        // 如果 size == 0 的块已经遇到过且处理好了
                    }
                    // 检查是不是刚完成一个 0 长度块
                    // 实际上在 feedLine 中处理 0 长度块后会设置 m_awaitingTrailer
                    // 这里利用标志检查是否完成
                    if (m_awaitingTrailer && data.size() >= 2 && data[0] == '\r' && data[1] == '\n')
                    {
                        data.remove_prefix(2);
                        m_stage = Stage::Complete;
                        m_awaitingTrailer = false;
                    }
                    break;
                }
                if (m_expectedBodyBytes > 0)
                {
                    const auto available = data.size();
                    const auto toCopy = std::min(m_expectedBodyBytes, available);
                    m_result.body.append(data.data(), toCopy);
                    data.remove_prefix(toCopy);
                    m_expectedBodyBytes -= toCopy;
                    if (m_expectedBodyBytes == 0) m_stage = Stage::Complete;
                    break;
                }
                if (m_isCloseDelimited)
                {
                    // 把所有剩余数据收作正文，连接关闭即「正文完成」
                    m_result.body.append(data.data(), data.size());
                    data = {};
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
    void HttpResponseParser::reset()
    {
        m_stage = Stage::StatusLine;
        m_result = {};
        m_lineBuffer.clear();
        m_expectedBodyBytes = 0;
        m_isChunked = false;
        m_isCloseDelimited = false;
        m_chunkSize = 0;
        m_inChunk = false;
        m_awaitingTrailer = false;
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