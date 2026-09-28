/**
 * @file AcmeHttp01ChallengeStore.h
 * @brief HTTP-01 自证令牌的暂存处，兼作那条 well-known 路由的处理者
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AsynGyanis::Net
{
    class HttpRequest;
    class HttpResponse;
    class Router;

    /**
     * @brief 一组「现在正在被机构校验」的 http-01 令牌
     *
     * @details 令牌只在一条挑战的生命期内该被答出来：机构判出终局之后就把它撤掉。留着一串永久
     *          可取的 token 等于给本机挂一个长期的凭据出口，而 ACME 的设计里它是一次性的。
     *
     * @note 写入发生在续期循环所属的线程，读取发生在应答机构那次 GET 的线程——两者通常不是同一条
     *       循环，因此内部加锁。临界区只有查表与拼串，锁内不做任何 IO
     */
    class AcmeHttp01ChallengeStore
    {
    public:
        AcmeHttp01ChallengeStore() = default;

        AcmeHttp01ChallengeStore(const AcmeHttp01ChallengeStore &) = delete;

        AcmeHttp01ChallengeStore &operator=(const AcmeHttp01ChallengeStore &) = delete;

        /**
         * @brief 挂出一个令牌，此后该路径会答出对应的 keyAuthorization
         * @param token 机构在授权记录里给出的令牌
         * @param accountKeyThumbprint 账户公钥的 RFC 7638 指纹
         * @note 同一令牌重复挂出是覆盖：机构重发一次挑战不该拿到两份不同的答案
         */
        void present(std::string_view token, std::string_view accountKeyThumbprint);

        /**
         * @brief 撤掉一个令牌（挑战到终局之后就该撤）
         * @param token 待撤的令牌；没挂过则无事发生
         */
        void withdraw(std::string_view token);

        /**
         * @brief 查这条令牌当前该答什么
         * @param token 请求路径里的令牌原文
         * @return std::optional<std::string> 命中时的 keyAuthorization；没挂或已撤时为空
         */
        [[nodiscard]] std::optional<std::string> keyAuthorizationFor(std::string_view token) const;

        /// 当前挂着的令牌条数（观测与用例都读它）
        [[nodiscard]] std::size_t presentedCount() const noexcept;

        /**
         * @brief 把那条 well-known 路由注册到给定路由器上
         * @param router 目标路由表；须在服务 start() 之前完成注册（Router 的既有时限）
         * @details 路径固定为 `/.well-known/acme-challenge/:token`（RFC 8555 §8.3）。
         *          没挂出的令牌一律 404——那不是「拒绝服务」而是「这条挑战已经不该再被答」。
         */
        void registerRoutes(Router &router);

    private:
        /**
         * @brief 路由处理体：按令牌查答案，查到就按 text/plain 交出
         * @param request 请求（取路径参数里的令牌）
         * @param response 响应（本函数写状态码与正文）
         * @return Core::Task<void> 写完响应即返回
         */
        Core::Task<void> answer(HttpRequest &request, HttpResponse &response) const;

        mutable std::mutex                           m_mutex;             ///< 保护下面这张表
        std::unordered_map<std::string, std::string> m_keyAuthorizations; ///< 令牌 → keyAuthorization
    };
} // namespace AsynGyanis::Net
