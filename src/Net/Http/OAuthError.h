/**
 * @file OAuthError.h
 * @brief OAuth 一族（授权码、设备码、令牌换取、令牌自省、JWKS）的失败类别与中文可操作文案
 * @author Gyanis
 * @date 2026-10-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 类别按「调用方拿到之后做什么」划分，不按机构回的错误码字符串划分：文案会改，分类才是契约。
 *          这族失败原先一律是裸 `std::string`，消费方只能对中文文案做子串匹配，文案一改判定就断。
 */

#pragma once

#include "AsynGyanisExport.h"

#include <expected>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief OAuth 通路的失败类别
     *
     * @details 新增类别一律追加在末尾：取值会进日志与指标，中间插入会让既有取值的含义整体位移。
     *          每一格都必须有真实产出点——加一个没人产生的类别，等于给调用方一个永远进不去的分支。
     */
    enum class OAuthErrorKind
    {
        Transport,             ///< 出站通路失败（DNS、TLS、时限、对端收线），一条应答都没拿到：可按退避重试
        RejectedByServer,      ///< 端点明确回了非 2xx 并带 OAuth 错误说明：重试通常无益，要改凭据、范围或让用户重授
        MalformedResponse,     ///< 应答形状不对：正文不是 JSON 对象、必填字段缺失/类型不合/为空——最常见的现场是把 tokenUrl 填成了登录页
        StateMismatch,         ///< 回调的 state 与本次授权请求不一致（RFC 6749 §10.12 要求逐字核对）：按 CSRF 处理，拒绝并记读数
        AuthorizationDenied,   ///< 用户在同意页点了拒绝（回调带 error=access_denied）：不是故障，把原因交回给用户即可
        MissingAuthorizationCode, ///< 回调里既没有 code 也没有 error：不是一次授权响应的形状，可疑的回跳
        InvalidLocalState,     ///< 本端状态或用法不自洽，未产生任何线上字节（缺 PKCE verifier、轮询间隔非正、设备码在本地就已过期）
        DeviceCodeExpired,     ///< 设备码有效期已过而用户仍未完成授权（RFC 8628 §3.5）：要重新发起一次设备授权
        CryptoFailed,          ///< 随机源或摘要接口算不出（PKCE verifier、state、SHA-256 挑战）：查本进程的 OpenSSL provider 配置
    };

    /**
     * @brief 一条 OAuth 失败：类别供分支，文案供人看
     *
     * @details 文案一律中文且写成「原因 + 下一步怎么做」，端点原文（含 HTTP 状态与 OAuth error/description）
     *          附在文案里作为证据，而不是替代分类。
     */
    struct ASYN_NET_API OAuthError
    {
        OAuthErrorKind kind;   ///< 失败类别，调用方据此分支
        std::string    message; ///< 中文可操作文案，含端点原文与出问题的字段名
    };

    /**
     * @brief OAuth 通路的结果形状：成功带 Value，失败侧固定为 OAuthError
     */
    template <typename Value>
    using OAuthExpected = std::expected<Value, OAuthError>;

    using OAuthStatus = std::expected<void, OAuthError>; ///< 只问成不成的 OAuth 结果
} // namespace AsynGyanis::Net
