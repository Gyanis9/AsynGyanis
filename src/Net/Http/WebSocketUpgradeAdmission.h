/**
 * @file WebSocketUpgradeAdmission.h
 * @brief 三条入站通道共用的 WebSocket 升级判定：把请求头折成策略判定所需的三个值
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/Message/HttpRequest.h"
#include "Net/WebSocket/WebSocketHandshake.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 按请求头与升级策略给出这次 WebSocket 升级的判定
     * @details 三条通道（h1 的 Upgrade、h2 的扩展 CONNECT、h3 的 CONNECT 隧道）读取的头部与同源
     *          拼接完全一致；收成一处，避免某条通道漏掉 origin 或子协议字段。
     * @param request 已通过协议校验的升级请求
     * @param policy 业务在 HttpResponse 上登记的升级策略
     * @return WebSocketUpgradeDecision 接受与否、拒绝码与文案、选中的子协议
     * @note 同源串取「传输层是否 TLS + 本连接的权威段」，不是对端自报的 :scheme
     */
    [[nodiscard]] inline WebSocketUpgradeDecision evaluateWebSocketUpgradeForRequest(const HttpRequest &request, const WebSocketUpgradePolicy &policy)
    {
        const std::optional<std::string> hostField    = request.getHeader("host");
        const std::optional<std::string> originField  = request.getHeader("origin");
        const std::optional<std::string> offeredField = request.getHeader(kWebSocketSubprotocolHeaderName);

        // 三条 Optional 都按「有才取」写成视图，不用 value_or：optional<string>::value_or 要求
        // 实参可转成 string，而 string_view → string 是显式构造（LWG-3424 把它写成了 static_assert）
        const std::string_view hostText    = hostField.has_value() ? std::string_view{*hostField} : std::string_view{};
        const std::string_view originText  = originField.has_value() ? std::string_view{*originField} : std::string_view{};
        const std::string_view offeredText = offeredField.has_value() ? std::string_view{*offeredField} : std::string_view{};

        const std::string sameOrigin = makeWebSocketSameOrigin(request.overTls(), hostText);
        return evaluateWebSocketUpgrade(originText, offeredText, sameOrigin, policy);
    }
} // namespace AsynGyanis::Net
