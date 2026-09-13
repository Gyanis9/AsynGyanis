/**
 * @file PerMessageDeflate.h
 * @brief permessage-deflate（RFC 7692）的消息级压缩与扩展协商
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief permessage-deflate 的协商结论
     */
    struct PerMessageDeflateNegotiation
    {
        bool        accepted{false}; ///< 是否接受该扩展
        std::string responseValue;   ///< 接受时回给对端的 Sec-WebSocket-Extensions 取值；拒绝时为空
    };

    /**
     * @brief 协商 permessage-deflate 扩展（RFC 7692 §7.1）
     *
     * @details 在客户端提供的 Sec-WebSocket-Extensions 里找 permessage-deflate；找到就接受，并回一份
     *          **本端选定**的参数：`server_no_context_takeover` 与 `client_no_context_takeover`。
     *          两条都要求「每条消息重置压缩上下文」，因此本端收发都不需要跨消息保存 z_stream——
     *          这是刻意的取舍：省下的是连接级的压缩状态与它的生命周期管理，代价是压缩率略低
     *          （上下文复用能让后续消息压得更狠）。对端提供的其它参数（窗口位宽等）照 RFC 允许的
     *          方式忽略：它们是建议，最终以本端回复为准。
     * @param extensionsHeader Sec-WebSocket-Extensions 头部的值，缺头时传空串
     * @return PerMessageDeflateNegotiation 协商结论；未提供或提供了本端不认识的扩展名时不接受
     * @note 多个扩展可以逗号分隔并存（RFC 6455 §9.1），这里只挑出 permessage-deflate 那一个，
     *       其余扩展不参与协商、也不回进响应
     */
    [[nodiscard]] PerMessageDeflateNegotiation negotiatePerMessageDeflate(std::string_view extensionsHeader);

    /**
     * @brief 压缩一条 WebSocket 消息（RFC 7692 §7.2.1）
     *
     * @details 步骤固定为：负载后追加四字节 `0x00 0x00 0xFF 0xFF`，用 **裸 deflate**（windowBits = -15，
     *          不带 zlib 头尾）压到 Z_SYNC_FLUSH，再把输出末尾的四字节空块尾去掉——线上负载因此
     *          不含这四字节，解压侧自行补回。
     * @param payload 消息负载，可为空（空消息也会产出合法的压缩结果）
     * @return std::optional<std::string> 线上负载；zlib 失败（内存不足或输出未按预期收尾）时为空，
     *         调用方应放弃压缩并原样发送（README 同 HTTP 侧的「绝不发坏字节」口径）
     */
    [[nodiscard]] std::optional<std::string> deflateWebSocketMessage(std::string_view payload);

    /**
     * @brief 解压一条 WebSocket 消息
     *
     * @details 与 deflateWebSocketMessage() 互为逆运算：先补回四字节空块尾再裸 inflate。
     * @param payload 线上负载（对端发来的压缩字节）
     * @param maximumOutputBytes 解压输出的字节上限，0 表示不限
     * @return std::optional<std::string> 原始消息；数据非法或解压结果超过上限时为空——上限必须由
     *         调用方给出：压缩比可以做到几百倍，不设上限时一条小消息就能把内存撑爆（zip bomb）
     */
    [[nodiscard]] std::optional<std::string> inflateWebSocketMessage(std::string_view payload, std::size_t maximumOutputBytes);

    /// 每条消息的默认压缩级别：与 HTTP 响应压缩取同一档（zlib 的 6）
    inline constexpr int kWebSocketDeflateLevel = 6;
} // namespace AsynGyanis::Net
