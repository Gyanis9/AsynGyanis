/**
 * @file HttpServerConfig.h
 * @brief 把 HTTP 服务器的可配置项接到配置文档的值上（限额、按 IP 限额、限流、指标开关）
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Config/ConfigValue.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpServerLimits.h"

#include <cstddef>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 服务器配置在文档里的段名
    inline constexpr std::string_view kHttpServerConfigSection = "server";

    /**
     * @brief 一台 HTTP 服务器的可配置项集合。
     *
     * @details 每个字段的默认值与对应结构体的默认值一致，因此配置里**只写要改的那几项**即可，
     *          没写的键保持默认——这也让「升级后新增的键」在旧配置上自动取到合理取值。
     * @note 这里只放「启动时定一次」的项：限额与开关都在 start() 之前落定，运行期不再变。
     * @see readHttpServerConfiguration(), HttpServerLimits, HttpParserLimits
     */
    /**
     * @brief 每监听器并发连接上限的内置默认值
     * @details 取有限值而不是 0：不限并发等于把连接表与每连接的缓冲交给对端，而默认值长得像「已经配好了」
     *          是最难发现的那类配置事故。要真的不限，显式写 0（语义没变，只是不再由默认值给）。
     */
    inline constexpr std::size_t kDefaultMaximumConnections = 4096;

    /**
     * @brief 单个来源并发连接上限的内置默认值
     * @details 取 256 而不是更小的数：一条默认值会误杀真实客户——运营商级 NAT 与企业出口让一群人共用一个地址，
     *          上限压到几十就会把其中一部分拒在门外。256 足够容纳这类共享出口，同时仍挡住「一个来源吃满整机名额」
     *          （每监听器总额 4096，即单来源最多占 1/16）。要真的不限，显式写 0。
     */
    inline constexpr std::size_t kDefaultMaximumConnectionsPerIp = 256;

    struct ASYN_NET_API HttpServerConfiguration
    {
        HttpServerLimits limits{};                                                 ///< 连接级限额：超时与单连接请求数上限
        HttpParserLimits parserLimits{};                                           ///< 单条报文的内存上限
        std::size_t      maximumConnections{kDefaultMaximumConnections};           ///< 全局并发连接上限；显式写 0 = 不限
        std::size_t      maximumConnectionsPerIp{kDefaultMaximumConnectionsPerIp}; ///< 单个来源的并发连接上限；显式写 0 = 不限
        double           requestsPerSecond{0.0};                                   ///< 全局请求速率上限（令牌桶速率），0 = 不限流
        double           rateLimitBurstCapacity{1.0};                              ///< 令牌桶容量，即瞬时允许的突发量；速率不为 0 时必须 ≥ 1
        /**
         * @brief 在途请求正文的总量上限，单位字节（整机口径，装配时摊到每个 worker 进程）
         * @details 补的是「单条报文的上限挡不住很多条连接」这一格：`parser_limits.maximum_body_size`
         *          限的是**一条**请求的正文，N 条连接各压着一条大正文时总占用与连接数同增，而每个
         *          分量看着都合规。超出本预算的新请求回 503 并收口，把额度留给已收下正文的连接。
         * @note 默认 0 = 不设这道账。刻意不给有限默认值：一个有限数会在「升级后什么都没改」的部署上
         *       突然开始回 503，而配置文件的每一行看着都对——这类默认值只能由部署方自己选
         * @note 账只覆盖三条 HTTP 通道（h1/h2/h3）的**请求正文**；WebSocket 帧缓冲、每连接接收窗口
         *       与响应正文都不在本预算之内
         */
        std::size_t memoryBudgetBytes{0};
        bool             exposeMetrics{false};                                     ///< 是否注册 /metrics 与 /healthz
        /**
         * @brief 运维端点的 Bearer 令牌；空 = 不鉴权
         * @details 只保护 `/metrics` 与 `/debug/loops`（内部计数与循环状态），`/healthz` 刻意不管：
         *          进程存活探针要能被编排器无凭据访问。端点开到 `0.0.0.0` 上而不给令牌，等于把
         *          「现在有多少连接、每条循环在干什么」公开发出去。
         * @note 这条只能写在配置文件里：命令行上的令牌会进 shell 历史与进程列表，等于把秘密交给运维通道
         */
        std::string opsBearerToken{};
        /**
         * @brief 运维端点单独听在哪个端口；0 = 端点仍留在业务口上
         * @details 两个用处：① 来源收口——管理口默认只听回环（`metricsAddress`），业务口可以继续开
         *          在 0.0.0.0；② 多进程部署时每个进程各听一个端口（调用方按进程序号错开），
         *          采集端就能按进程聚合而不是随机命中某一台。
         * @note 只在 `exposeMetrics` 打开时有意义，两者都不开等于配了个没人听的端口
         */
        std::uint16_t metricsPort{0};
        /// 管理口的监听地址，默认只听回环。写成 `0.0.0.0` 就是把指标公开到所有网卡上，请连同令牌一起想清楚
        std::string metricsAddress{"127.0.0.1"};
    };

    /**
     * @brief 从配置文档里读出 HTTP 服务器的可配置项。
     *
     * @details 读的是文档的 `server` 段，键名与 HttpServerConfiguration 的字段同名（超时一律带 `_ms`
     *          后缀，单位毫秒）。三层嵌套：`limits`（连接级）、`parser_limits`（单条报文）、
     *          `rate_limit`（令牌桶）。**没有任何配置时返回一份全默认值**，调用方因此不必区分
     *          「配置里没有 server 段」与「配置里有但没写这一项」。
     *
     * @param configurationRoot 配置文档的根值，即一个以 `server` 为键的对象。从
     *        Base::ConfigManager 取时用 `getSection("server")` 再补上段名这一层外壳
     *        （ConfigManager 的键是扁平的，get("server") 取不到中间层节点）
     * @return HttpServerConfiguration 读出来的配置；缺失的键取结构体默认值
     * @throws Base::ConfigValidationException 段或子段不是对象、出现未知键、类型不符或取值越界。
     *         未知键也算错是**刻意**的：把 `idle_timeout` 写成 `idle_timeout_ms` 时静默忽略
     *         等于配置没生效却看不出来，正是最难查的一类问题。
     * @note 只读不写，线程安全取决于调用方传入的值树是否可变（本函数不做任何修改）
     * @see HttpServerConfiguration, Base::ConfigManager
     */
    [[nodiscard]] ASYN_NET_API HttpServerConfiguration readHttpServerConfiguration(const Base::ConfigValue &configurationRoot);

} // namespace AsynGyanis::Net
