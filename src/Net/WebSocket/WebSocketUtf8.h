/**
 * @file WebSocketUtf8.h
 * @brief WebSocket 文本帧负载的 UTF-8 校验（RFC 6455 §5.6 + RFC 3629）
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 校验一段字节是否构成合法的 UTF-8 文本（RFC 3629）
     *
     * @details 严格按 RFC 3629 的形态判定：拒绝过长编码（overlong）、拒绝代理区码点
     *          U+D800~U+DFFF、拒绝超出 U+10FFFF 的码点、拒绝截断的多字节序列；`\0` 是合法字符。
     *
     * @param text 待校验字节，按「指针 + 长度」取，可含 NUL
     * @return true 整段合法，可作为 WebSocket 文本消息的负载（RFC 6455 §5.6）
     * @return false 存在违规字节，位置用 findInvalidWebSocketUtf8ByteOffset() 取
     * @note 一次调用校验整段、不留任何状态：帧解码层交付的是已重组的完整消息，跨分片的增量
     *       校验状态没有消费方，因此不引入有状态校验器（校验点见 WebSocketPeer::feedBytes()）
     * @see findInvalidWebSocketUtf8ByteOffset()
     */
    [[nodiscard]] bool isValidWebSocketUtf8(std::string_view text) noexcept;

    /**
     * @brief 找出首个违规字节的下标
     *
     * @details 判据与 isValidWebSocketUtf8() 完全一致（返回 npos 等价于它返回 true）；单独暴露
     *          是为了让上层能说出「第几个字节起违规」，而不是只报一句「不是合法 UTF-8」。
     * @param text 待校验字节，按「指针 + 长度」取，可含 NUL
     * @return std::size_t 违规序列的起始字节下标
     * @return std::string_view::npos 整段合法
     */
    [[nodiscard]] std::size_t findInvalidWebSocketUtf8ByteOffset(std::string_view text) noexcept;
} // namespace AsynGyanis::Net
