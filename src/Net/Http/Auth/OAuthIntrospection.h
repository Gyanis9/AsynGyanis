/**
 * @file OAuthIntrospection.h
 * @brief OAuth 2.0 令牌自省（RFC 7662）—— 把不透明令牌交给授权服务器的自省端点判定
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 不是所有令牌都是自证签名的 JWT：不透明令牌（opaque token）的唯一权威判定在授权
 *          服务器的自省端点——请求方把令牌原文 POST 过去，端点回 `{"active": true/false, …}`。
 *          本件就是那条请求的客户端：组表单、发 POST、把响应解析成结构体。与 JWT 一路的差别
 *          在信任模型：JWT 离线验签（公钥在手就能判），自省每问一次都要服务端点头——因此它
 *          天然是逐请求的往返，不适合放进同步的 verify 口逐请求调用，更适合会话建立时
 *          判定一次、或按缓存周期复核（怎么用是调用方的事，本件只把这一次往返做对）。
 *
 * @note 凭据按 RFC 6749 §2.3.1 走 HTTP Basic（client_id 作用户名、client_secret 作口令）；
 *       未给 clientId 时不发认证头（公开客户端或网关已代管的部署）。
 * @warning 自省端点必须在 TLS 之下：请求体里带着令牌原文，明文连接等于把令牌交给链路上的
 *          每一跳。构造时只放行 https 或回环地址上的 http，其余明文端点当场拒绝。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Auth/OAuthError.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    using Core::EventLoop;

    /**
     * @brief 自省客户端的取值
     */
    struct OAuthIntrospectionOptions
    {
        std::string               introspectionUrl;     ///< 自省端点（必填）
        std::string               clientId;             ///< 客户端标识；空 = 不发 Basic 认证头
        std::string               clientSecret;         ///< 客户端口令
        std::chrono::milliseconds requestTimeout{5000}; ///< 单次请求超时
    };

    /**
     * @brief 自省响应的常用字段（RFC 7662 §2.2）
     */
    struct OAuthTokenIntrospection
    {
        bool         active{false}; ///< 令牌当前是否有效——唯一的必填字段，其余字段只在 active 为真时可信
        std::string  scope;         ///< 空格分隔的授权范围
        std::string  clientId;      ///< 令牌的签发对象
        std::string  username;      ///< 资源所有者（若适用）
        std::string  subject;       ///< 主体标识（sub）
        std::string  tokenType;     ///< 令牌类型（如 bearer）
        std::int64_t expiresAt{0};  ///< 到期时刻（epoch 秒）；0 = 端点未给出
    };

    /**
     * @brief OAuth 2.0 令牌自省客户端
     *
     * @details 无状态：一次 `introspect()` 一条请求，不缓存判定结果（缓不缓是调用方的策略，
     *          本件不替调用方决定令牌判定的新鲜度）。
     */
    class ASYN_NET_API OAuthIntrospectionClient
    {
    public:
        /**
         * @brief 构造
         * @param loop 事件循环，只在这条循环上使用（与框架里每条连接归属一个循环的约定同口径）
         * @param options 取值；introspectionUrl 为空即抛
         * @throws Base::InvalidArgumentException introspectionUrl 为空，或不是 https 且不在回环地址上
         */
        OAuthIntrospectionClient(EventLoop &loop, OAuthIntrospectionOptions options);

        /**
         * @brief 查询一枚令牌的当前状态
         * @details POST `token=<百分号编码原文>&token_type_hint=access_token`（表单编码按
         *          RFC 6749 §4.1.3）；凭据走 Basic。响应必须是 2xx 且带布尔 `active`——
         *          RFC 7662 规定无效/过期的令牌也以 200 回答并置 `active: false`，
         *          因此非 2xx 与「缺 active」都算协议不符（返回失败原因），不是「不活跃」。
         * @param token 待查询的令牌原文
         * @return OAuthExpected<OAuthTokenIntrospection> 解析结果；失败为中文原因
         *         （网络失败、非 2xx、正文不是 JSON 对象、缺 active 字段）
         */
        [[nodiscard]] Core::Task<OAuthExpected<OAuthTokenIntrospection>> introspect(std::string_view token);

    private:
        EventLoop                &m_loop;
        OAuthIntrospectionOptions m_options;
    };

} // namespace AsynGyanis::Net
