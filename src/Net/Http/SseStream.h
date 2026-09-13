/**
 * @file SseStream.h
 * @brief SSE（Server-Sent Events）事件流：在 chunked 流式响应上按 WHATWG 线格式发帧
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpResponse.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief SSE 事件流：把一条 HTTP 响应变成可持续追加的事件通道
     *
     * @details 构造即把响应切入 chunked 流式模式（200 + text/event-stream + no-cache），之后每次
     *          sendEvent()/sendComment() 立即产出一个完整帧：可选的 event/id/retry 行在前、data 行
     *          在后、末尾一个空行收尾（WHATWG HTML 的 SSE 线格式）。
     *
     * @note 本类不设 content-length，调用方也不要再设：SSE 的消息边界由分块帧给出（RFC 9112 §6），
     *       而 content-length 与 transfer-encoding 不得并存；头部随首段正文一起上线，上线后再改无效。
     * @note 长连接必须自己发心跳：代理与内核会按空闲时长切断静默连接，业务应周期性调用
     *       sendComment()（注释帧不会触发客户端的 onmessage）刷新连接的空闲截止时间；相邻两帧的间隔
     *       还受 HttpServerLimits::writeTimeout 约束，该项非 0 时心跳周期要短于它。
     * @warning 本对象只借用 HttpResponse，不持有它：响应对象必须比本对象活得久，
     *          否则写出时会访问已销毁的响应。
     */
    class SseStream
    {
    public:
        /// 单帧长度上限（1 MiB = 1024 × 1024 字节），指整帧序列化后的字节数（含字段名前缀与行尾）
        static constexpr std::size_t kMaximumFrameLength = 1024U * 1024U;

        /**
         * @brief 构造即把响应切到 SSE
         * @details 进入 chunked 流式模式并设好两个必备头部，不设 content-length（见类说明）。
         * @param response 目标响应对象，必须比本对象活得久
         * @throws Base::LogicException 响应已设过整块正文，或状态码不允许携带正文（startChunkedResponse 的契约）
         */
        explicit SseStream(HttpResponse &response);

        /**
         * @brief 本流是否仍可发送
         * @return true 仍可发送
         * @return false 连接已不可用（writeChunk 返回过 false）：此后所有发送接口直接短路返回 false，
         *         且不新增日志（收口原因已在 writeChunk 失败那一刻记过）
         */
        [[nodiscard]] bool isOpen() const noexcept;

        /**
         * @brief 发送一条注释帧：`: <comment>\n\n`
         * @details 注释文本里的换行按行拆开，每行一条注释行；注释帧只用于心跳与调试，
         *          客户端不会把它投给业务回调。
         * @param comment 注释文本
         * @return true 已写出
         * @return false 本帧未发出、连接不可再用、调用方应停止发送：两种来源与日志口径见
         *         HttpResponse::writeChunk()（本侧已收口不新增日志，传输失败本次记一条）
         * @throws Base::LogicException 整帧超过 kMaximumFrameLength
         * @note comment 指向的字节必须活到本次 co_await 结束：协程到首次 resume 才读取入参
         */
        Core::Task<bool> sendComment(std::string_view comment);

        /**
         * @brief 发送一条事件帧：可选 event/id/retry 行 + data 行 + 空行
         * @details 字段顺序固定为 event → id → retry → data。data 里的 `\n`、`\r\n` 与裸 `\r` 都按
         *          换行拆分，每段一行 `data:`、行内不留 `\r`；空 data 产出一条空 `data:` 行。
         * @param data 事件负载
         * @param eventName 事件名，写进 `event:` 行；空串表示不带该字段
         * @param eventId 事件标识，写进 `id:` 行；空串表示不带该字段
         * @param retry 重连建议间隔，写进 `retry:` 行；std::nullopt 表示不带该字段
         * @return true 已写出
         * @return false 本帧未发出、连接不可再用、调用方应停止发送：两种来源与日志口径见
         *         HttpResponse::writeChunk()（本侧已收口不新增日志，传输失败本次记一条）
         * @throws Base::LogicException eventName/eventId 含 CR、LF 或 NUL，或整帧超过 kMaximumFrameLength
         * @throws Base::InvalidArgumentException retry 为负数（用法错误，重试无用）
         * @note data/eventName/eventId 指向的字节必须活到本次 co_await 结束：协程到首次 resume 才读入参
         */
        Core::Task<bool> sendEvent(std::string_view data,
                                   std::string_view eventName = {},
                                   std::string_view eventId = {},
                                   std::optional<std::chrono::milliseconds> retry = std::nullopt);

    private:
        /**
         * @brief 把可能含换行的文本按 SSE 行拆开，每段写成「行前缀 + 段 + LF」
         * @param target 目标帧缓冲
         * @param linePrefix 行前缀（如 "data: "、": "），已含该行的固定部分
         * @param value 原始文本；`\n`、`\r\n` 与裸 `\r` 都算一个换行，行内不留 `\r`
         */
        static void appendTextLines(std::string &target, std::string_view linePrefix, std::string_view value);

        HttpResponse &m_response; ///< 被写入的响应对象（非拥有），必须比本对象活得久
        bool m_isOpen{true};      ///< 是否仍可发送；写出失败后粘滞为 false
    };
} // namespace AsynGyanis::Net
