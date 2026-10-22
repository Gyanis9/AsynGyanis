/**
 * @file Http2SessionSupport.h
 * @brief Http2Session 各翻译单元共用的响应头补齐规则
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 响应头自动补齐与 WebSocket 隧道拒绝应答共用同一组头名；常量只留这一份，
 *          避免发送路径与隧道路径对「哪些头由本层补」出现两种口径。
 */

#pragma once

#include "Net/Http/Message/HttpResponse.h"

#include <string_view>

namespace AsynGyanis::Net::Http2SessionDetail
{
    /// 响应头自动补齐规则要认出的四个头名（与 HttpResponse::appendHead() 同一套）
    inline constexpr std::string_view kContentTypeHeaderName   = "content-type";
    inline constexpr std::string_view kContentLengthHeaderName = "content-length";
    inline constexpr std::string_view kDateHeaderName          = "date";
    inline constexpr std::string_view kTrailerHeaderName       = "trailer";

    /// 自动补出的内容类型：与 HttpResponse 的兜底选择一致（不会被浏览器当脚本执行）
    inline constexpr std::string_view kDefaultContentTypeValue = kPlainTextContentType;
} // namespace AsynGyanis::Net::Http2SessionDetail
