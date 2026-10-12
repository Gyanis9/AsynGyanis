/**
 * @file OAuthDeviceCode.h
 * @brief OAuth 2.0 设备码流程（RFC 8628）—— 无浏览器或无输入能力设备的授权客户端
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 电视、CLI、嵌入式这类设备没有浏览器：授权拆成两步——①向设备授权端点要一对码
 *          （用户码给用户看、设备码留给设备轮询）；②用户在**另一台**机器的浏览器里打开
 *          verification_uri、输入用户码并确认；③设备按 interval 轮询令牌端点，直到发令牌
 *          或判失败。本件是这条流程的客户端：两步分别是 requestDeviceAuthorization() 与
 *          waitForToken()，两者之间的用户交互是调用方的事（把 user_code 与 verification_uri
 *          显示出去；verification_uri_complete 给了的话用户连码都不用输）。
 *
 * @note 轮询频率按 RFC 8628 §3.5：**每次新请求之前**都要等满当前间隔——第一次轮询也在等待
 *       之后（等满在任何读法下都不越线）；服务端回 slow_down 时间隔 +5 秒且此后一直用新值。
 * @note 本件按**公开客户端**形状发请求：client_id 走表单字段（§3.1 必填）；给了 clientSecret
 *       时凭据额外走 Basic（RFC 6749 §2.3.1）。不配 secret 的部署什么都不用改。
 * @warning 两个端点**必须**在 TLS 之下：来往上带着设备码与最终令牌，明文连接等于把它们交给
 *          链路上的每一跳。本件不替调用方拦明文端点——照发，但调用方要清楚这是它自己的选择。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    using Core::EventLoop;
    using Core::Timer;

    /**
     * @brief 设备码客户端的取值
     */
    struct OAuthDeviceCodeOptions
    {
        std::string               deviceAuthorizationUrl; ///< 设备授权端点（必填）
        std::string               tokenUrl;               ///< 令牌端点（必填）
        std::string               clientId;               ///< 客户端标识（必填；RFC 8628 §3.1）
        std::string               clientSecret;           ///< 客户端口令；空 = 公开客户端（不发 Basic 头）
        std::string               scope;                  ///< 请求的授权范围（空格分隔）；空 = 不请求特定范围
        std::chrono::milliseconds requestTimeout{5000};   ///< 单次请求超时
    };

    /**
     * @brief 设备授权响应（RFC 8628 §3.2）
     */
    struct OAuthDeviceAuthorization
    {
        std::string          deviceCode;              ///< 设备码：轮询令牌端点时用（自己留着，不显示给用户）
        std::string          userCode;                ///< 用户码：给用户看，在 verificationUri 上输入
        std::string          verificationUri;         ///< 用户交互页地址
        std::string          verificationUriComplete; ///< 带用户码的完整交互页地址（可空）
        std::chrono::seconds expiresIn{0};            ///< 两个码的有效期（秒）
        std::chrono::seconds interval{5};             ///< 轮询间隔（秒）；服务端未给时为 5（§3.2 的默认）
    };

    /**
     * @brief 令牌响应的常用字段（RFC 6749 §5.1）
     */
    struct OAuthDeviceToken
    {
        std::string          accessToken;  ///< 访问令牌（必填）
        std::string          tokenType;    ///< 令牌类型（如 Bearer；必填）
        std::string          refreshToken; ///< 刷新令牌（可空）
        std::string          scope;        ///< 实际授予的范围（可空）
        std::chrono::seconds expiresIn{0}; ///< 有效期（秒）；0 = 端点未给出
    };

    /**
     * @brief OAuth 2.0 设备码流程客户端（RFC 8628）
     *
     * @details 无状态：两次调用都是成员协程，期间的会话信息全在 `OAuthDeviceAuthorization`
     *          里由调用方保管——本件不替调用方记轮询进度。
     */
    class ASYN_NET_API OAuthDeviceCodeClient
    {
    public:
        /**
         * @brief 构造
         * @param loop 事件循环，只在这条循环上使用（与框架里每条连接归属一个循环的约定同口径）
         * @param options 取值；三个必填项（deviceAuthorizationUrl / tokenUrl / clientId）任一为空即抛
         * @throws Base::InvalidArgumentException 必填取值缺失，或 requestTimeout 非正
         */
        OAuthDeviceCodeClient(EventLoop &loop, OAuthDeviceCodeOptions options);

        /**
         * @brief 第一步：POST 设备授权端点，取回设备码与用户码（§3.1/§3.2）
         * @details 表单 `client_id=…[&scope=…]`；响应必须是 2xx JSON 对象，且带
         *          device_code / user_code / verification_uri / expires_in 四个必填字段
         *          （缺失、错类型或空串都按协议不符拒并点名）。interval 没给时回退默认 5，
         *          给了荒谬值（非正整数）按协议不符拒——「没给」与「给了坏值」是两回事。
         * @return std::expected<OAuthDeviceAuthorization, std::string> 解析结果；失败为中文原因
         */
        [[nodiscard]] Core::Task<std::expected<OAuthDeviceAuthorization, std::string>> requestDeviceAuthorization();

        /**
         * @brief 第二步：按 interval 轮询令牌端点，直到拿到令牌或判失败（§3.4/§3.5）
         * @details 每次请求（含第一次）之前先等满当前间隔；不越过 `expiresIn`（从本调用起算）——
         *          本地也设上界，不把有界性外包给对端（服务端此时通常回 expired_token）。
         *          对端问答的处置：authorization_pending 按现间隔续轮；slow_down 把间隔加 5 秒
         *          （此后都用新值）并续轮；access_denied / expired_token 终止并交回对端原因；
         *          其余错误码与传输失败一律终止（error 原文带在原因里，不猜可重试性）。
         * @param authorization requestDeviceAuthorization 的结果。**按值收**：协程到首次恢复才读
         *        入参，按引用/视图接临时量会静默读到已释放内存
         * @return std::expected<OAuthDeviceToken, std::string> 令牌；失败为中文原因
         */
        [[nodiscard]] Core::Task<std::expected<OAuthDeviceToken, std::string>> waitForToken(OAuthDeviceAuthorization authorization);

        /**
         * @brief §3.5 的轮询间隔规则：slow_down 时加 5 秒（此后每个请求都用新值），其余不动
         * @param currentInterval 当前生效的轮询间隔
         * @param errorCode 令牌端点回来的错误码（RFC 6749 §5.2 的 error 字段原文）
         * @return std::chrono::seconds 下一次轮询前应等待的间隔
         * @note 单独摆成一个静态函数是为了可直测：「此后所有请求都用新值」的链式 +5（1→6→11）
         *       在端到端里要等十几秒，纯函数一条断言就钉住
         */
        [[nodiscard]] static std::chrono::seconds pollingIntervalAfter(std::chrono::seconds currentInterval, std::string_view errorCode) noexcept;

    private:
        EventLoop             &m_loop;  ///< 所属循环（非拥有；比本对象活得久）
        Timer                  m_timer; ///< 挂在循环上的等待定时器
        OAuthDeviceCodeOptions m_options;
    };

} // namespace AsynGyanis::Net
