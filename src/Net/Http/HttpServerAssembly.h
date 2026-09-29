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
        /**
         * @brief 整机限额要摊到几个 worker 进程上（默认 1 = 单进程，不做摊分）
         * @details 每个进程只看得见自己这份账：`maximum_connections` 与 `maximum_connections_per_ip`
         *          配成整机的数、又起 N 个进程，实际放行的是 N 倍。这里按进程数向上取整摊到每台，
         *          使「配置里写的是整机口径」这件事真的成立。0 是用法错误（当场拒，不当「不限」）。
         * @note 摊分是近似：POSIX 侧内核按连接把新连接分散给各进程，长连接偏斜时某一台的瞬时并发仍可能
         *       高于份额。要精确的跨进程全局闸需要共享内存或外部存储，本层没做，别把这里当成那个东西
         */
        std::size_t workerProcessCount{1};
    };

    /**
     * @brief 把整机口径的限额摊到每个 worker 进程上（向上取整）
     * @details 摊分规则只有这一处实现：装配出口用它，示例报生效值时也要用它，否则「打印的数」与
     *          「真正下发的数」会各说一套——那正是这一轮要消灭的那类问题
     * @param wholeMachineValue 配置里写的整机上限；0 表示显式不限，原样返回
     * @param workerProcessCount 摊给几个进程，必须 ≥ 1；填 1 即不摊
     * @return std::size_t 每进程上限
     */
    [[nodiscard]] ASYN_NET_API std::size_t perProcessShare(std::size_t wholeMachineValue, std::size_t workerProcessCount) noexcept;

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
