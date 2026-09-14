/** @file HttpResponseParser.h 响应报文解析器 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace AsynGyanis::Net
{
    /// 一条已解析的响应头部
    struct ParsedStatus
    {
        int         statusCode{0};
        std::string reasonPhrase;
    };
    /// 响应解析结果
    struct HttpResponseInfo
    {
        int                                                     statusCode{0};
        std::string                                             reasonPhrase;
        std::vector<std::pair<std::string, std::string>>        headers;
        std::string                                             body;
    };
    /**
     * @brief 自顶向下解析 HTTP 响应报文
     * @details 状态行 → 头部 → 正文（Content-Length / Transfer-Encoding: chunked / Connection: close）
     */
    class HttpResponseParser
    {
    public:
        enum class Stage { StatusLine, Headers, Body, Complete, Failed };
        /// 喂入数据，返回本轮消费的字节数（返回 0 表示需要更多数据）
        std::size_t feed(std::string_view data);
        /// 解析是否已完成（所有期望的正文都收到了）
        [[nodiscard]] bool isComplete() const noexcept { return m_stage == Stage::Complete; }
        /// 当前状态是否为失败
        [[nodiscard]] bool hasFailed() const noexcept { return m_stage == Stage::Failed; }
        /// 取解析结果（仅有在 isComplete() 为 true 时内容完整）
        [[nodiscard]] const HttpResponseInfo &result() const noexcept { return m_result; }
        /// 通知对端已关闭（close-delimited 模式下据此完成解析）
        void endOfStream();
        /// 重置解析器状态
        void reset();
    private:
        Stage          m_stage{Stage::StatusLine};
        HttpResponseInfo m_result;
        std::string    m_lineBuffer;
        std::size_t    m_expectedBodyBytes{0};
        bool           m_isChunked{false};
        bool           m_isCloseDelimited{false};
        std::size_t    m_chunkSize{0};
        bool           m_inChunk{false};
        bool           m_awaitingTrailer{false};
    };
} // namespace AsynGyanis::Net