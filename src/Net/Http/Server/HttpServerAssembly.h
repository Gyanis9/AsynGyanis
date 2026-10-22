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

#include "Net/Http/Config/HttpServerConfig.h"
#include "Net/Http/Routing/Middleware.h"
#include "Net/Http/Server/ReadinessCheck.h"

#include <expected>
#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class HttpServer;
    class HttpsServer;
    class Router;
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
         *          `memory_budget_bytes` 与 `rate_limit.requests_per_second` 配成整机的数、又起 N 个进程，实际放行的是
         *          N 倍。连接数与字节数按进程数向上取整摊到每台，速率按精确除法摊（速率可以是小数，
         *          取整会把 0.5 请求/s 抬成 1）。0 是用法错误（当场拒，不当「不限」）。
         * @note 摊分是近似：POSIX 侧内核按连接把新连接分散给各进程，长连接偏斜时某一台的瞬时并发仍可能
         *       高于份额。要精确的跨进程全局闸需要共享内存或外部存储，本层没做，别把这里当成那个东西
         */
        std::size_t workerProcessCount{1};

        /**
         * @brief 同一个进程里跑几条监听器（默认 1 = 一台；多个事件循环线程各绑一次同端口时填台数）
         * @details 为什么需要一个单独的分母：`TcpServer` 的并发计数是每台一份的，一条进程里跑 L 台
         *          就等于有 L 个各自独立的上限闸门，只按进程数摊会放行「配置值 × L」——
         *          `maximum_connections: 4096` 配四个循环线程实际收到 16384，而配置文件看着仍是 4096。
         *          因此这里分两种分母：每台自己计数的限额（连接数；以及本台自建的每来源限额器、
         *          限流桶、在途正文预算）按 `workerProcessCount × listenersPerProcess` 摊；
         *          调用方传进来的共享对象（一个进程里多台共用一份账）只按 `workerProcessCount` 摊，
         *          再乘台数就会把同一份账摊小 L 倍。两条比对判据也照各自的分母算，不混用。
         * @note 0 是用法错误（等于「本进程一台都不接」），装配当场拒，不当「不摊」
         */
        std::size_t listenersPerProcess{1};

        /**
         * @brief 就绪探针的依赖检查（可选）
         * @details 运维端点由本装配出口统一挂载；依赖检查也必须从同一个出口进，否则「业务口与
         *          管理口各挂一份」会分叉。不交即只判「还在不在接新连接」，与加它之前逐字相同。
         *          回调在服务器循环线程上被调用，必须是 O(1) 的快照读取（见 ReadinessCheck）
         */
        ReadinessCheck readinessCheck;
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
     * @details 这是那份注册逻辑的唯一实现：装配出口在 `metrics_port = 0` 时用它挂到业务口上，
     *          另起管理监听器的调用方用它挂到管理口上。分两处写的后果是「业务口撤了端点、管理口忘了加闸」，
     *          而那正是这一轮要消灭的形状。`exposeMetrics` 关掉时什么都不做。
     *          `QuicServer` 不在此列（它没有同名的注册接口，路由器是外部交给它的），因此 `metrics_port = 0`
     *          那档兜底只覆盖两条 TCP 通道：h3 业务口上不会长出这四个端点。要在开着 h3 的部署上暴露运维面，
     *          走管理监听器那一条（也是推荐形态——运维面不该跟着业务口一起公开出去）。
     * @param host 目标服务器，必须尚未 start()
     * @param configuration 已读出的配置（起作用的是 `exposeMetrics` 与 `opsBearerToken`）
     * @param readinessCheck 可选的依赖检查；不交即只判「还在不在接新连接」
     */
    template<typename ServerType>
    void registerOperationEndpoints(ServerType &host, const HttpServerConfiguration &configuration, const ReadinessCheck &readinessCheck = {})
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
        host.enableReadinessEndpoint("/readyz", readinessCheck);
        host.enableLoopDiagnosticsEndpoint();
        host.enableConnectionsEndpoint();
        // 改日志等级是写操作：没有令牌就不注册，宁可没有这个入口，也不留一个人人可改的开关
        if (!configuration.opsBearerToken.empty())
        {
            host.enableLogLevelEndpoint();
        }
        // `/healthz` 与 `/readyz` 都不进上面那道令牌闸（`OpsAccessOptions::protectedPaths` 的默认名单里
        // 只有 /metrics、/debug/loops、/debug/connections 与 /debug/log-level）：编排器的探针拿不到凭据，
        // 给探针加闸的结局是探针被人关掉
    }

    /**
     * @brief 按 `server.access_log` 把访问日志中间件挂到一台路由器上
     * @details 这是那条开关的唯一实现：h1/h2 由装配出口在服务器上挂，h3 的路由器是调用方自己交给
     *          `QuicServer` 的（装配出口碰不到它），必须调本函数挂同一份判断——分两处写的后果是
     *          「h1 有访问日志、h3 没有」，而这件事只有真打一条 h3 请求才看得出来。
     *          输出行的字段口径与不记哪些字段见 `loggingMiddleware`。
     * @param router 目标路由器，必须尚未开始服务
     * @param configuration 已读出的配置（只有 `accessLog` 这一项参与判断）
     */
    ASYN_NET_API void applyAccessLogMiddleware(Router &router, const HttpServerConfiguration &configuration);

    /**
     * @brief 按 `server.handler_timeout_ms` 把每请求的处理时限中间件挂到一台路由器上
     * @details 与 @c applyAccessLogMiddleware 同一形状：h1/h2 由装配出口挂，h3 的路由器交给
     *          `QuicServer`、装配出口碰不到，由调用方拿着自己那一侧的事件循环调本函数——分两处写
     *          的后果就是「h1 有处理时限、h3 没有」。时限为 0 时一条中间件都不挂：造一个「装了
     *          但永远不超时」的中间件只会多一层协程帧与一个定时器。
     * @note 挂上去的是软时限：到点把协作式取消交给业务、等业务返回那一刻把响应改写成 504。
     *       不配合取消又不返回的处理器拿不到这条 504（理由见 `HttpServerConfiguration::handlerTimeout`）
     * @param router 目标路由器，必须尚未开始服务
     * @param configuration 已读出的配置（只有 @c handlerTimeout 这一项参与判断）
     * @param loop 本路由器所属的事件循环：看门狗是每请求一个定时器，必须建在同一条循环上，
     *             挂到别的循环就成了跨线程碰循环对象（事件循环的线程契约）
     */
    ASYN_NET_API void applyRequestTimeoutMiddleware(Router &router, const HttpServerConfiguration &configuration, Core::EventLoop &loop);

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
