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
#include "Net/Http/Middleware.h"

#include <expected>
#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    class HttpServer;
    class HttpsServer;
    class HttpMemoryBudget;
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
        std::shared_ptr<PerIpConnectionLimiter> sharedPerIpLimiter; ///< 跨监听器共用的按来源 IP 限额器
        /**
         * @brief 跨监听器共用的限流桶
         * @details 传了它就必须是按 @c perProcessRateLimit 摊过的那一份：限流的整机口径与连接数同属
         *          「每进程只看得见自己这份账」，直接拿配置里的速率建桶，起 N 个进程就放行 N 倍。
         *          装配出口会把桶上的两个数与摊分结果比对，不一致当场拒。
         */
        std::shared_ptr<TokenBucket> sharedRateLimitBucket;
        /**
         * @brief 跨监听器共用的在途正文字节预算
         * @details 与限额器、限流桶同理：这台服务器上的 h1/h2 会话（以及调用方另起的 h3 服务端）要共用
         *          同一份账。传了它，其上限必须等于 @c perProcessShare 摊到本进程的那一份，否则当场拒——
         *          「配置写着 512 MiB、实际放行 512 MiB × 进程数」正是这一类看不出的偏差。
         *          只想让 h3 也吃到同一份账时，把本对象原样填进 `QuicServer::Configuration::memoryBudget`。
         */
        std::shared_ptr<HttpMemoryBudget> sharedMemoryBudget;
        /**
         * @brief 整机限额要摊到几个 worker 进程上（默认 1 = 单进程，不做摊分）
         * @details 每个进程只看得见自己这份账：`maximum_connections`、`maximum_connections_per_ip`、
         *          `memory_budget_bytes` 与 `rate_limit.rate` 配成整机的数、又起 N 个进程，实际放行的是
         *          N 倍。连接数与字节数按进程数向上取整摊到每台，速率按精确除法摊（速率可以是小数，
         *          取整会把 0.5 请求/s 抬成 1）。0 是用法错误（当场拒，不当「不限」）。
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
     * @brief 摊到本进程的一份限流口径
     */
    struct ASYN_NET_API PerProcessRateLimit
    {
        double requestsPerSecond{0.0}; ///< 本进程的补令牌速率；0 = 不限流
        double burstCapacity{1.0};     ///< 本进程的桶容量；最低 1.0，容量小于 1 的桶一个请求都放不出
    };

    /**
     * @brief 把整机口径的限流摊到每个 worker 进程上
     * @details 与连接数的摊分同属一条规则，单列一处是因为两个量的取整方向相反：速率可以是小数，
     *          向上取整会把 0.5 请求/s 抬成 1（一台进程被放行两倍）；而桶容量必须留够 1 枚令牌，
     *          整除到 0.25 会让桶永远攒不满一个令牌，TokenBucket 直接构造失败。
     * @param wholeMachineRequestsPerSecond 配置里的整机速率；0 表示不限流，原样返回 0
     * @param wholeMachineBurstCapacity 配置里的整机突发量，仅在速率非 0 时有意义
     * @param workerProcessCount 摊给几个进程，必须 ≥ 1；填 1 即不摊
     * @return PerProcessRateLimit 本进程应使用的速率与容量
     */
    [[nodiscard]] ASYN_NET_API PerProcessRateLimit perProcessRateLimit(double wholeMachineRequestsPerSecond, double wholeMachineBurstCapacity,
                                                                       std::size_t workerProcessCount) noexcept;

    /**
     * @brief 把运维端点（连同令牌闸门）挂到一台服务器上
     * @tparam ServerType HttpServer 或 HttpsServer（两者都有同名注册接口）
     * @details 这是那份注册逻辑的**唯一**实现：装配出口在 `metrics_port = 0` 时用它挂到业务口上，
     *          另起管理监听器的调用方用它挂到管理口上。分两处写的后果是「业务口撤了端点、管理口忘了加闸」，
     *          而那正是这一轮要消灭的形状。`exposeMetrics` 关掉时什么都不做。
     * @param host 目标服务器，必须尚未 start()
     * @param configuration 已读出的配置（起作用的是 `exposeMetrics` 与 `opsBearerToken`）
     */
    template<typename ServerType>
    void registerOperationEndpoints(ServerType &host, const HttpServerConfiguration &configuration)
    {
        if (!configuration.exposeMetrics)
        {
            return;
        }
        // 闸门与端点必须同进同出：只挂端点等于公开，只挂闸门等于保护不存在的东西
        if (!configuration.opsBearerToken.empty())
        {
            host.router().addMiddleware(opsAccessMiddleware(OpsAccessOptions{.bearerToken = configuration.opsBearerToken}));
        }
        host.enableMetricsEndpoint();
        host.enableHealthEndpoint();
        host.enableLoopDiagnosticsEndpoint();
    }

    /**
     * @brief 把 server 段的配置落到一台明文 HTTP 服务器上
     * @param server 目标服务器，尚未 start()
     * @param configuration 由 readHttpServerConfiguration() 读出来的配置
     * @param context 跨监听器共享的对象（可留空，见 HttpServerAssemblyContext）
     * @return std::expected<void, std::string> 成功为空；失败给出中文原因（两种：传进来的共享限额器
     *         与 @c maximum_connections_per_ip 不一致，或共享限流桶与 @c rate_limit 摊分结果不一致）
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
