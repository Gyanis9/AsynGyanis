/**
 * @file OAuthToken.h
 * @brief RFC 6749 §5.1 令牌响应的形状与解析 —— 设备码与授权码两条换取流程共用
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 令牌端点对成功请求回的都是同一张形状（RFC 6749 §5.1）：access_token 与 token_type
 *          必填，expires_in / refresh_token / scope 可选。两条换取流程（设备码、授权码）解析的是
 *          同一份字节，收紧在一处；对端字节不可信，读取一律不抛异常（口径见 OAuthSupport.h）。
 */
#pragma once

#include "AsynGyanisExport.h"
#include "Net/Http/OAuthError.h"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 令牌端点一次成功响应的常用字段（RFC 6749 §5.1）
     */
    struct OAuthAccessToken
    {
        std::string          accessToken;  ///< 访问令牌（必填）
        std::string          tokenType;    ///< 令牌类型（如 Bearer；必填）
        std::string          refreshToken; ///< 刷新令牌（可空）
        std::string          scope;        ///< 实际授予的范围（可空）
        std::chrono::seconds expiresIn{0}; ///< 有效期（秒）；0 = 端点未给出
    };

    /**
     * @brief 解析令牌端点（或刷新流程）的 2xx 响应正文
     * @details access_token 与 token_type 必填（缺失、错类型、空串各自点名拒）；expires_in 给了
     *          必须是正整数（没给按 0 即「端点未给出」）；refresh_token / scope 可选，但类型不符即拒。
     * @param responseBody 响应正文原文
     * @return OAuthExpected<OAuthAccessToken> 解析结果；失败为中文原因
     */
    [[nodiscard]] ASYN_NET_API OAuthExpected<OAuthAccessToken> parseOAuthTokenResponse(std::string_view responseBody);

} // namespace AsynGyanis::Net
