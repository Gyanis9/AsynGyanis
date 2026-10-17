/**
 * @file OAuthAuthorizationCode.h
 * @brief OAuth 2.0 授权码流程（RFC 6749 §4.1）带 PKCE（RFC 7636）—— 有浏览器场景的换取客户端
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 三步走：①`beginAuthorization()` 组一条授权 URL 并交回本次的 state 与 PKCE verifier
 *          （都留着）；②调用方把 URL 交给浏览器，用户在授权页确认后浏览器被重定向到
 *          `redirectUri`，回调的接受方式由调用方决定（本地回环服务、自建端点、自定义协议
 *          或手工粘贴——本件不做这件事）；③`exchangeCode()` 把回调里的授权码连同 verifier 发到
 *          令牌端点换回令牌。状态核对与回调错误的拒绝都在换取之前完成，漏掉这两步的调用方
 *          拿不到令牌（state 不符会当场被拒）。
 *
 * @note PKCE 按 RFC 7636 §4.3：能力允许时必须用 S256（本件只发 S256，不发 plain）。
 * @note 授权码只能换一次、且要与发起时的 redirectUri 与 verifier 配套（RFC 6749 §4.1.3、
 *       RFC 7636 §4.5）——三者都由 `beginAuthorization()` 的产物与配置提供，调用方不必自己拼。
 * @warning 令牌端点必须在 TLS 之下：来往上带着授权码与最终令牌。本件不替调用方拦明文端点
 *          ——照发，但调用方要清楚这是它自己的选择。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/OAuthToken.h"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    using Core::EventLoop;

    /**
     * @brief 授权码客户端的取值
     */
    struct OAuthAuthorizationCodeOptions
    {
        std::string               authorizationUrl;     ///< 授权端点（必填）
        std::string               tokenUrl;             ///< 令牌端点（必填）
        std::string               clientId;             ///< 客户端标识（必填）
        std::string               clientSecret;         ///< 客户端口令；空 = 公开客户端（不发 Basic 头）
        std::string               redirectUri;          ///< 回调地址（必填；换取时原样再送一次，RFC 6749 §4.1.3）
        std::string               scope;                ///< 请求的授权范围（空格分隔）；空 = 不请求特定范围
        std::chrono::milliseconds requestTimeout{5000}; ///< 单次请求超时
    };

    /**
     * @brief 一次待用户确认的授权请求（beginAuthorization 的产物）
     */
    struct OAuthAuthorizationRequest
    {
        std::string url;      ///< 交给浏览器打开的授权 URL（含 state 与 PKCE 挑战）
        std::string state;    ///< 本次请求的 state：回调里的值必须与它逐字相同（CSRF 判据）
        std::string verifier; ///< 本次请求的 PKCE verifier：换取时随授权码一起送出
    };

    /**
     * @brief 回调里取回的一次授权响应（parseCallback 的产物）
     */
    struct OAuthAuthorizationCallback
    {
        std::string code;             ///< 授权码（成功回调时非空）
        std::string state;            ///< 对端回显的 state（换取时与发起值核对）
        std::string error;            ///< 失败回调的 error（如 access_denied；RFC 6749 §4.1.2.1）
        std::string errorDescription; ///< 失败回调的 error_description（可空）
    };

    /**
     * @brief OAuth 2.0 授权码流程客户端（RFC 6749 §4.1 + PKCE RFC 7636）
     *
     * @details 无状态：授权请求的 state 与 verifier 全在 `OAuthAuthorizationRequest` 里由调用方
     *          保管——本件不替调用方记「正在等哪一次回调」。
     */
    class ASYN_NET_API OAuthAuthorizationCodeClient
    {
    public:
        /**
         * @brief 构造
         * @param loop 事件循环，只在这条循环上使用（与框架里每条连接归属一个循环的约定同口径）
         * @param options 取值；四个必填项（authorizationUrl / tokenUrl / clientId / redirectUri）任一为空即抛
         * @throws Base::InvalidArgumentException 必填取值缺失，或 requestTimeout 非正
         */
        OAuthAuthorizationCodeClient(EventLoop &loop, OAuthAuthorizationCodeOptions options);

        /**
         * @brief 第一步：生成 state 与 PKCE 材料并组出授权 URL
         * @details state 与 verifier 都是 32 字节密码学随机（编成 43 个 base64url 字符）；
         *          挑战按 S256 计算（`code_challenge_method=S256`）。授权端点本来带查询串时正确接续 `&`。
         * @return std::expected<OAuthAuthorizationRequest, std::string> 待打开的 URL 与要留住的材料；
         *         失败为中文原因（密码学随机源不可用）
         */
        [[nodiscard]] std::expected<OAuthAuthorizationRequest, std::string> beginAuthorization() const;

        /**
         * @brief 第二步（可选）：从回调里取出 code / state / error
         * @details 接受整条回调 URL 或裸查询串；片段（`#...`）剥掉，参数按与入站表单同一套规则解码
         *          （`+` 表示空格、`%XX` 按字节）。既没有 `code` 也没有 `error` 按协议不符拒——
         *          page-not-found 一类的地址不会被猜成一次授权响应。
         * @param callback 回调原文
         * @return std::expected<OAuthAuthorizationCallback, std::string> 解析结果；失败为中文原因
         */
        [[nodiscard]] static std::expected<OAuthAuthorizationCallback, std::string> parseCallback(std::string_view callback);

        /**
         * @brief PKCE 的 S256 变换：BASE64URL(SHA256(verifier))，无填充（RFC 7636 §4.2/§4.6）
         * @param codeVerifier 授权请求时用的 verifier
         * @return std::expected<std::string, std::string> 挑战；失败为中文原因（SHA-256 计算不可用）
         * @note 单独摆成一个静态函数是为了可直测：RFC 7636 附录 B 的官方测试向量一条断言即可对照
         */
        [[nodiscard]] static std::expected<std::string, std::string> computePkceChallenge(std::string_view codeVerifier);

        /**
         * @brief 第三步：把授权码换回令牌（RFC 6749 §4.1.3 + RFC 7636 §4.5）
         * @details 换取之前先在本地做两道判：回调带 `error` 即拒（原样带上对端原因）、
         *          `callback.state` 与 `request.state` 不一致即拒（RFC 6749 §10.12）——
         *          两者都不向令牌端点发一个字节。表单固定为
         *          `grant_type=authorization_code&code=…&redirect_uri=…&client_id=…&code_verifier=…`；
         *          给了 clientSecret 时凭据走 Basic。响应按 RFC 6749 §5.1 的共用形状解析。
         * @param request beginAuthorization 的产物。按值收：协程到首次恢复才读入参，
         *        按引用/视图接临时量会静默读到已释放内存
         * @param callback 解析过的回调。按值收，口径同上
         * @return std::expected<OAuthAccessToken, std::string> 令牌；失败为中文原因
         */
        [[nodiscard]] Core::Task<std::expected<OAuthAccessToken, std::string>> exchangeCode(OAuthAuthorizationRequest request, OAuthAuthorizationCallback callback);

    private:
        EventLoop                    &m_loop; ///< 所属循环（非拥有；比本对象活得久）
        OAuthAuthorizationCodeOptions m_options;
    };

} // namespace AsynGyanis::Net
