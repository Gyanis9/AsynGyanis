/**
 * @file SseEventParser.h
 * @brief text/event-stream 的增量解析器：把任意切分的字节流还原成一条条事件
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 一条已收齐的 SSE 事件
     *
     * @details 字段名与语义按 WHATWG HTML「服务器定义的事件」一节（§9.2.6）：`event:` 是事件类型、
     *          `data:` 是正文（多条以换行拼接）、`id:` 是最后一标识、`retry:` 是重连间隔毫秒数。
     */
    struct ASYN_NET_API SseEvent
    {
        std::string   type{};               ///< 事件类型；对端没写 `event:` 时为空（那就是默认的 message）
        std::string   data{};               ///< 正文；多条 `data:` 已按换行拼好
        std::string   lastEventId{};        ///< 本条带来的 `id:`；没写则为空（沿用上一条是调用方的事）
        bool          hasRetry{false};      ///< 本条是否带了 `retry:`
        std::uint64_t retryMilliseconds{0}; ///< `retry:` 的毫秒数；仅在 hasRetry 为真时有意义
    };

    /**
     * @brief SSE 流的增量解析器
     *
     * @details 为什么不一次把整份正文解析掉：`text/event-stream` 按设计不会结束，「整份」这个概念
     *          在这里不存在。解析器因此只认「喂进来的字节」与「能交出去的事件」，中间状态（半行、
     *          攒到一半的事件）都由它自己留着。
     * @note 按 §9.2.6 的规矩办事：行分隔符是 CRLF / LF / 单独的 CR；`field:value` 冒号后的**一个**
     *       空格属于分隔符不算进取值；未知字段与以 `:` 开头的注释行忽略；`data` 为空时那一行不算
     *       事件（不发）；`id` 里含 NUL 的整条忽略。
     * @see HttpClientRequest::responseBodyReceiver —— 喂进来的字节由它逐批给出
     */
    class ASYN_NET_API SseEventParser
    {
    public:
        /**
         * @brief 喂进一段到达的字节
         * @param chunk 本批字节；边界落在任何位置都可以（半行会留下一次再拼）
         */
        void feed(std::string_view chunk);

        /**
         * @brief 通知流已结束：把最后残留的半行按一整行处理（对端收线时最后一条事件常常没有空行收尾）
         */
        void endOfStream();

        /**
         * @brief 取下一条已收齐的事件
         * @return std::optional<SseEvent> 有事件则返回；暂时凑不齐一条就返回空
         * @note 事件按到达次序交出，取空之后可以反复再取（不是一次性的）
         */
        [[nodiscard]] std::optional<SseEvent> nextEvent();

        /**
         * @brief 此刻还留在解析器里的字节数（半行 + 攒到一半的事件）
         * @return std::size_t 字节数
         * @note 给观测用：这条流一直只喂不取时，这个数会涨，涨到不合理的值就是有人在灌没有结束的事件
         */
        [[nodiscard]] std::size_t pendingByteCount() const noexcept;

    private:
        /// 处理一整行（不含行分隔符），并按需把当前事件收进待发队列
        void handleLine(std::string_view line);

        std::string          m_lineBuffer{};         ///< 正在收的那一行（遇到分隔符才结清）
        std::deque<SseEvent> m_readyEvents{};        ///< 已收齐、等调用方取走的事件
        std::string          m_dataBuffer{};         ///< 当前事件累积的 data（多条以换行拼接）
        std::string          m_eventType{};          ///< 当前事件的 event: 值
        std::string          m_pendingId{};          ///< 当前事件带过来的 id:（没有就留空）
        bool                 m_hasRetry{false};      ///< 当前事件是否出现 retry:
        std::uint64_t        m_retryMilliseconds{0}; ///< retry: 的毫秒数（非数字按 0 处理，见实现）
    };
} // namespace AsynGyanis::Net
