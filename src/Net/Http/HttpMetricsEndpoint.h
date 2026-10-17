/**
 * @file HttpMetricsEndpoint.h
 * @brief 把统计快照渲染成 Prometheus 文本，以及健康检查端点的应答正文
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/ReadinessCheck.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    class Connection;
}

namespace AsynGyanis::Net
{
    class Router;
    /// /debug/loops 的 content-type：正文是 JSON
    inline constexpr std::string_view kLoopDiagnosticsContentType = "application/json";
    /// /metrics 的 content-type：Prometheus 文本展示格式 0.0.4；显式声明 UTF-8，因为 HELP 文本是中文
    inline constexpr std::string_view kPrometheusTextContentType = "text/plain; version=0.0.4; charset=utf-8";

    /// /healthz 的应答正文：固定的最小 JSON，探针与人都能一眼看懂
    inline constexpr std::string_view kHealthCheckResponseBody = R"({"status":"ok"})";

    /// /readyz 在「还在接受新连接」时的应答正文
    inline constexpr std::string_view kReadinessReadyResponseBody = R"({"status":"ready"})";
    /// /readyz 在「已经停止接受新连接、正在排空在途请求」时的应答正文（503）
    inline constexpr std::string_view kReadinessDrainingResponseBody = R"({"status":"draining"})";

    /**
     * @brief 把一次统计快照渲染成 Prometheus 文本格式（exposition format 0.0.4）
     *
     * @details 指标名一律 ASCII，HELP 文本是中文（该格式是 UTF-8，只对反斜杠与换行有转义要求）。
     *          计数器带 `_total` 后缀、直方图带 `_bucket`/`_sum`/`_count`，都按官方命名约定，
     *          否则采集侧推导不出 counter 与 histogram 两类语义。
     * @param stats 统计快照
     * @param metricNamePrefix 指标名前缀，用于同一进程内区分多套服务；空串表示不带前缀
     * @return std::string 可直接作为 200 响应正文的文本，以换行结尾
     * @note 行序固定（计数器、状态码分类、直方图、WebSocket、HTTP/2、发送路径、准入闸门、进程级读数），便于人眼比对两次抓取
     * @note 响应计数与延迟直方图都带 `route` 标签：取值是路由模式原文（如 `/user/:id`）或固定哨兵
     *       （`unmatched` / `static` / `merged`），绝不取请求的实际 URI。路由明细表有界，占满后新模式
     *       归并到 `merged`，归并条数见 `route_series_merged_total`。
     * @warning 快照的口径由传入的采集端决定：HTTP/1.1 与 HTTP/2 会话直接计入本服务器的
     *          采集端；HTTP/3 由独立的 QuicServer 服务，只有把同一个采集端交给它
     *          （QuicServer::Configuration::metricsCollector）才会一并出现在这里——只跑 h3 又没接
     *          采集端时会看到全零，那不是「没有流量」，是那条路径没接上。三条通道的口径现已一致：
     *          请求数、状态码类与耗时直方图都算，h3 的耗时同样从「会话收下这条请求」量到「响应排进
     *          待发字节」（见 Http3Session 构造函数的说明）；跨协议对照时唯一要记住的差别是 h3 不产生
     *          「写侧中止」那一族——QUIC 的发送由传输层自行重传，没有那个形态
     * @warning 同一端口由多台服务器共同监听（每循环线程一个）时，还要让它们共用一份采集端
     *          （HttpServer::setMetricsCollector()）：各持一份时抓取会随机命中其中一台，报出的是
     *          那台自己的量，计数器还能在两次抓取之间变小。跨进程没有这条通道——多个 worker 进程
     *          共用一个端口时，本端点报的只是被抓住的那个进程的口径，按进程各自暴露抓取端点再由
     *          采集侧汇总
     * @see HttpServer::enableMetricsEndpoint(), HttpServerStats
     */
    [[nodiscard]] ASYN_NET_API std::string formatPrometheusMetrics(const HttpServerStats &stats, std::string_view metricNamePrefix);

    /**
     * @brief 把进程内事件循环的自观测表渲染成 JSON，作为 /debug/loops 的应答正文
     *
     * @details 判停顿只看「工作相已经持续多久」：等事件的那一相再久也不是停顿（那只是空闲），
     *          已经停下的循环也不参与判定（它不再更新相位，读出来是一个停在原地的旧时刻）。
     * @param observedLoops Core::eventLoopSnapshots() 的返回值，按槽位号升序
     * @param unregisteredLoopCount 因观测槽位已满而没进表的循环条数；非零时表是不全的
     * @param nowMoment 算「这一相已持续多久」的基准时刻，由调用方给，渲染因此可复现可断言
     * @return std::string 紧凑 JSON：顶层给 stallThresholdMicroseconds 与 unregisteredLoopCount，
     *         loops 数组一条循环一行（serial/thread/running/phase/phaseMicroseconds/
     *         completedWorkingSegments/slowestWorkingSegmentMicroseconds/remotePendingCount/saturatedRemoteRejectionCount/skippedCompletedHandleCount/stalled）
     * @note stalled 与 Core 那条「工作段超阈值就落 ERROR」用的是同一个阈值（kSlowWorkingSegmentAlertThreshold），
     *       两边口径不同就会一个报警一个不报，那比没有更糟
     * @see Core::eventLoopSnapshots(), HttpServer::enableLoopDiagnosticsEndpoint()
     */
    [[nodiscard]] ASYN_NET_API std::string formatLoopDiagnosticsJson(const std::vector<Core::ObservedEventLoop> &observedLoops, std::size_t unregisteredLoopCount,
                                                                     std::chrono::steady_clock::time_point nowMoment);

    /**
     * @brief 把在途连接快照渲染成 `/debug/connections` 的聚合 JSON
     *
     * @details 只给聚合：按协议与状态分组计数、最老连接的存活秒数，以及收发字节的合计与单连接最大值。
     *          刻意不逐条列出对端地址——那等于把客户端清单暴露在运维端点上。数据来自
     *          `ConnectionManager::snapshot()`：取快照只在管理器锁内复制 shared_ptr，遍历与渲染都在
     *          锁外完成，不长时间占锁；字节数走连接上的原子累加，跨线程读不会碰传输层状态。
     * @param connections 连接快照
     * @param nowMoment 计算存活时长的基准时刻
     * @return std::string 紧凑 JSON：total / by_protocol / by_state / oldest_age_seconds /
     *         received_bytes_total / sent_bytes_total / max_received_bytes / max_sent_bytes
     * @note 字节口径是协议层交给传输层的量（h1/h2 的明文/帧字节），不是 TLS 记录层加密后的线上字节；
     *       h3 连接不在 ConnectionManager 里，因此不参与这份聚合
     */
    [[nodiscard]] ASYN_NET_API std::string formatConnectionsJson(const std::vector<std::shared_ptr<Core::Connection>> &connections,
                                                                 std::chrono::steady_clock::time_point                 nowMoment);

    /**
     * @brief 注册运行时日志等级端点：GET 读、PUT/POST 改 root 日志器等级
     * @details 必须与 `opsAccessMiddleware` 同批挂载（调用方把它加进 protectedPaths）：改等级是写操作，
     *          不加令牌闸等于把日志开关交给任何能访问运维口的人。改等级前先落一条 WARN 审计
     *          （新等级高于 WARN 时这条只能在旧等级下写出，因此先记审计再改）。
     * @param router 目标路由器，必须在 start() 之前注册
     * @param path 端点路径，形状校验由调用方做
     * @see Logger::setLevel, opsAccessMiddleware
     */
    ASYN_NET_API void registerLogLevelEndpoint(Router &router, std::string_view path);

    /**
     * @brief 注册就绪探针端点：还在接受新连接回 200，已经停了回 503
     *
     * @details HTTP 与 HTTPS 两台服务器共用这一份实现（两条各写一遍迟早有一处漏改）。判据只有一格：
     *          本端此刻还在不在接受新连接——`TcpServer::stop()` 置掉运行标志并关掉监听器，而已建立的
     *          连接继续跑到自然结束，那一段就是「排空」。编排器要在这段里把这台摘出负载，而不是等
     *          连接被强关才发现。
     *
     *          它与 `/healthz` 的分工是刻意的，两条不能互相顶替：存活性问「进程还在不在转」，停机排空
     *          期间答案仍是「在」（给探活口回 503 会让编排器直接把进程杀掉，比不报更糟）；就绪性问
     *          「还能不能接新活」，那正是先变的那一格。两条都不查运维令牌，理由同 `/healthz`：
     *          探针方拿不到凭据，加闸的结果是探针被人关掉。
     *
     * @param router 目标路由器；与另两个端点同样必须在 start() 之前注册
     * @param path 端点路径，形状校验由调用方做（报错文案要点名是哪个服务器）
     * @param isAccepting 本端此刻是否仍在接受新连接，端点在每次抓取时现读它，不做缓存
     * @param dependencyCheck 可选的依赖检查；不交时行为与加它之前逐字相同。非要就绪时回 503 并在
     *        正文里点名原因；回调在事件循环线程上调用，必须是 O(1) 的快照读取（见 ReadinessCheck）
     * @note 同 (方法, 路径) 的重复注册是就地替换（Router 的既有语义，由
     *       `Router.ReplacesHandlerForSameMethodAndPathInPlace` 钉住）：业务先注册过同一条 GET 路径再开
     *       这个开关，自己那条会被本端点顶掉；先开开关、后注册同路径的业务处理函数，赢的是后者。
     *       两条注册都发生在 start() 之前，所以「谁在后」就是路由器里看到的那个次序。
     * @see HttpServer::enableReadinessEndpoint(), HttpsServer::enableReadinessEndpoint()
     */
    ASYN_NET_API void registerReadinessEndpoint(Router &router, std::string_view path, std::function<bool()> isAccepting, ReadinessCheck dependencyCheck = {});

    /**
     * @brief 注册存活性探针端点：回固定的 200 JSON
     * @details 明文与 TLS 两台服务器共用这一份实现（各写一遍迟早有一处漏改）。不查运维令牌的理由
     *          与 registerReadinessEndpoint() 同一条：探针方拿不到凭据，加闸的结果是探针被人关掉。
     * @param router 目标路由器，必须在 start() 之前注册
     * @param path 端点路径，形状校验由调用方做（报错文案要点名是哪个服务器）
     * @see HttpServer::enableHealthEndpoint(), HttpsServer::enableHealthEndpoint()
     */
    ASYN_NET_API void registerHealthEndpoint(Router &router, std::string_view path);

    /**
     * @brief 注册事件循环观测端点：每次请求现取全表渲染成 JSON
     * @details 与上面同一条理由只留一份实现：读的是进程内每条循环自己的原子量，与走哪条通路无关。
     * @param router 目标路由器，必须在 start() 之前注册
     * @param path 端点路径，形状校验由调用方做
     * @see HttpServer::enableLoopDiagnosticsEndpoint(), HttpsServer::enableLoopDiagnosticsEndpoint()
     */
    ASYN_NET_API void registerLoopDiagnosticsEndpoint(Router &router, std::string_view path);

    /**
     * @brief 注册指标抓取端点：每次抓取现取一次快照再渲染
     * @details content-type 与渲染只留一份；取快照的动作由各台服务器交进来，因为在册连接数与
     *          状态码账目挂在自己的采集端上。
     * @param router 目标路由器，必须在 start() 之前注册
     * @param path 端点路径，形状校验与前缀合法性由调用方做（报错文案要点名是哪个服务器）
     * @param metricNamePrefix 已经过名字语法校验的指标名前缀，可为空
     * @param snapshotProvider 取本服务器当前统计快照的动作，每次抓取现调一次
     * @see HttpServer::enableMetricsEndpoint(), HttpsServer::enableMetricsEndpoint()
     */
    ASYN_NET_API void registerMetricsEndpoint(Router &router, std::string_view path, std::string_view metricNamePrefix, std::function<HttpServerStats()> snapshotProvider);

} // namespace AsynGyanis::Net
