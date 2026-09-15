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
     * @details 状态行 → 头部 → 正文。正文定界按 RFC 9112 §6 取信：有 Transfer-Encoding
     *          时按 chunked 解读，只有 Content-Length 时按长度，两者同时出现按非法拒绝
     *          （「挑一个信」正是响应走私的入口）。
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
        /// chunked 正文的读取阶段（RFC 9112 §7.1）
        enum class ChunkPhase
        {
            SizeLine,       ///< 块大小行（可带 ;ext 扩展）
            Data,           ///< 当前块的块数据
            DataTerminator, ///< 块数据之后的 CRLF
            Trailer         ///< 0 块之后的 trailer 段，空行收尾
        };

        Stage            m_stage{Stage::StatusLine};
        HttpResponseInfo m_result;
        std::string      m_lineBuffer;
        /// 上一次跨馈送取行把手里的视图交出去了：下一次取行时才能清暂存
        ///（交出去就清会让 std::string 在首字节写 NUL，调用方读到坏内容）
        bool             m_isLineHandedOut{false};
        std::size_t      m_expectedBodyBytes{0};
        bool             m_isChunked{false};
        bool             m_isCloseDelimited{false};
        bool             m_isHttp10{false};
        ChunkPhase       m_chunkPhase{ChunkPhase::SizeLine};
        std::size_t      m_chunkSize{0};
    };
} // namespace AsynGyanis::Net