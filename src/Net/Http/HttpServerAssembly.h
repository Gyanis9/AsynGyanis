/**
 * @file HttpServerAssembly.h
 * @brief 把 server 段的配置一次性落到一台 HTTP 服务器上：配置键与生效点只在这一处对接
 * @author Gyanis
 * @date 2026-09-29
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 此前 `HttpServerConfiguration` 的七个字段在库内没有任何消费方，全靠调用方逐台手接六七个
 *          setter——`expose_metrics` 因此是「配置里写了也不生效」的死键，而这正是最难查的一类失败：
 *          读配置的人以为打开了运维面，实际一个端点都没注册。这里把那层对接收进库内，配置与生效
 *          只此一处，键与语义同时可证。
 * @note 必须在 `start()` 之前调用（与其中各 setter 的契约一致）；中间件在路由器上的先后不影响
 *        它是否被调用，只影响它与别的中间件的执行顺序
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Http/HttpServerConfig.h"

#include <expected>
#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    class HttpServer;
    class HttpsServer;
    class PerIpConnectionLimiter;
    class TokenBucket;

    /**
     * @brief 装配时可以跨监听器共用的运行时对象
     * @details 留空即由本函数按配置里的标量新建一份。多条通道（h1/h2 与 h3）要共用同一道闸门或
     *          同一个桶时，必须由调用方把同一份对象传进来——各建一份会让「全局」限额实际变成
     *          每台一份，配置里的数被成倍放大却看不出来。
     */
    struct ASYN_NET_API HttpServerAssemblyContext
    {
        std::shared_ptr<PerIpConnectionLimiter> sharedPerIpLimiter;    ///< 跨监听器共用的按来源 IP 限额器
        std::shared_ptr<TokenBucket>            sharedRateLimitBucket; ///< 跨监听器共用的限流桶
    };

    /**
     * @brief 把 server 段的配置落到一台明文 HTTP 服务器上
     * @param server 目标服务器，尚未 start()
     * @param configuration 由 readHttpServerConfiguration() 读出来的配置
     * @param context 跨监听器共享的对象（可留空，见 HttpServerAssemblyContext）
     * @return std::expected<void, std::string> 成功为空；失败给出中文原因（当前只有一种：
     *         传进来的共享限额器与 `maximum_connections_per_ip` 不一致）
     */
    [[nodiscard]] ASYN_NET_API std::expected<void, std::string> applyHttpServerConfiguration(HttpServer &server, const HttpServerConfiguration &configuration,
                                                                                             const HttpServerAssemblyContext &context = {});

    /**
     * @brief 把 server 段的配置落到一台 HTTPS 服务器上
     * @param server 目标服务器，尚未 start()
     * @param configuration 由 readHttpServerConfiguration() 读出来的配置
     * @param context 跨监听器共享的对象（可留空，见 HttpServerAssemblyContext）
     * @return std::expected<void, std::string> 成功为空；失败给出中文原因
     */
    [[nodiscard]] ASYN_NET_API std::expected<void, std::string> applyHttpServerConfiguration(HttpsServer &server, const HttpServerConfiguration &configuration,
                                                                                             const HttpServerAssemblyContext &context = {});

} // namespace AsynGyanis::Net
