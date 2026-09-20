/**
 * @file HttpResponseParser.h
 * @brief 出站 HTTP 响应报文解析器：增量喂字节，把状态行、头部块与三种正文定界方式解成一条响应
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
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

        /// 头部条数上限：与服务端请求解析器同档，防对端用无限头部把客户端顶爆
        static constexpr std::size_t kDefaultMaximumHeaderCount = 100;

        /// 头部块净字节上限（名 + 值，不含分隔与 CRLF）：与服务端同档
        static constexpr std::size_t kDefaultMaximumHeaderBlockByteCount = 64ull * 1024;

        /// 单行上限（状态行、头部行、分块大小行共用）：一行永不含 CRLF 的字节流会让行缓冲
        /// 无界增长；闸门在 feed() 入口统一看行缓冲长度，越界即判失败
        static constexpr std::size_t kDefaultMaximumLineByteCount = 8ull * 1024;

        /**
         * @brief 构造解析器
         * @param maximumBodySize 正文长度上限（字节，chunked 按解码后累计）；0 表示不限
         */
        explicit HttpResponseParser(std::size_t maximumBodySize = kDefaultMaximumBodySize) noexcept;

        /// 解析推进到的阶段
        enum class Stage
        {
            StatusLine, ///< 状态行还没拿到
            Headers,    ///< 正在收头部块
            Body,       ///< 头部收完，正在按定界方式收正文
            Complete,   ///< 一条响应收齐
            Failed,     ///< 报文不合规或正文越出上限，本解析器不再推进
        };

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

        Stage            m_stage{Stage::StatusLine}; ///< 现在正在解哪一段
        HttpResponseInfo m_result;                   ///< 累积中的解析结果，收齐后才完整
        std::string      m_lineBuffer;               ///< 跨馈送的半行暂存，凑满一行才清
        /// 上一次跨馈送取行把手里的视图交出去了：下一次取行时才能清暂存
        ///（交出去就清会让 std::string 在首字节写 NUL，调用方读到坏内容）
        bool             m_isLineHandedOut{false};
        std::size_t      m_headerBlockByteCount{0}; ///< 已收头部块的净字节数（名 + 值）
        std::size_t      m_expectedBodyBytes{0};    ///< 还欠多少正文字节（Content-Length 或块边界给的）
        bool             m_isChunked{false};        ///< 正文按 chunked 分块定界
        bool             m_isCloseDelimited{false}; ///< 正文靠对端关闭连接定界
        bool             m_isHeadResponse{false}; ///< 接下来解析的是 HEAD 请求的应答（RFC 9112 §6.3 第 1 条）
        ChunkPhase       m_chunkPhase{ChunkPhase::SizeLine}; ///< chunked 读取当前停在哪一步
        std::size_t      m_chunkSize{0};            ///< 当前块还剩多少字节没收
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