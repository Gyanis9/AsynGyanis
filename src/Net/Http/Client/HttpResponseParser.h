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
        /**
         * @brief 正文默认上限：与服务端请求解析器同一量级（8 MiB）
         * @details 客户端同样需要这道闸：chunked 与「读到连接关闭」两种定界方式下，
         *          正文长度由**对端**说了算，没有上限就是让对端决定本进程分配多少内存
         */
        static constexpr std::size_t kDefaultMaximumBodySize = 8ull * 1024 * 1024;

        /**
         * @brief 构造解析器
         * @param maximumBodySize 正文长度上限（字节，chunked 按解码后累计）；0 表示不限
         */
        explicit HttpResponseParser(std::size_t maximumBodySize = kDefaultMaximumBodySize) noexcept;

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

        /**
         * @brief 标记「接下来解析的是 HEAD 请求的应答」
         * @details RFC 9112 §6.3 第 1 条：对 HEAD 的应答一律在头块之后结束，无论带不带定界头，
         *          正文都为空。解析器本身不知道请求方法，由调用方在喂字节之前告知——不标记的话，
         *          「HEAD 应答不带 Content-Length」会被按「读到连接关闭」处理，客户端只能干等
         *          对端关闭（keep-alive 连接上是等不到结果的）
         */
        void markAsHeadResponse() noexcept { m_isHeadResponse = true; }

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
        bool             m_isHeadResponse{false}; ///< 接下来解析的是 HEAD 请求的应答（RFC 9112 §6.3 第 1 条）
        ChunkPhase       m_chunkPhase{ChunkPhase::SizeLine};
        std::size_t      m_chunkSize{0};
        std::size_t      m_maximumBodySize{kDefaultMaximumBodySize}; ///< 正文上限（0 表示不限）

        /**
         * @brief 已收正文是否越过上限
         * @return true 越界；调用方据此把解析置为失败（越界后不再接收任何正文）
         */
        [[nodiscard]] bool isBodyOverLimit() const noexcept
        {
            return m_maximumBodySize != 0 && m_result.body.size() > m_maximumBodySize;
        }
    };
} // namespace AsynGyanis::Net