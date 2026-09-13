/**
 * @file HttpServer.h
 * @brief HTTP 服务器：在 TcpServer 之上装配路由器、会话与静态文件服务
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"

#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/Tcp/TcpServer.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 静态文件服务的当前配置
     *
     * @details 单独成类型（而不是塞进 HttpServer 的私有成员）是因为通配路由的处理函数要读它：
     *          按值捕获一个共享的 settings 比捕获 `this` 更安全——处理函数可能在服务器之后才
     *          被销毁。rootDirectory 存的是规范化后的绝对路径，配置一次、每请求只读。
     * @note 只在事件循环线程上读写：静态服务开关与目录的变更不会与在途请求交错。
     */
    struct StaticFileSettings
    {
        bool isEnabled{false};                ///< 是否启用静态文件服务；根目录规范化失败即为 false
        std::filesystem::path rootDirectory;  ///< 规范化（weakly_canonical）之后的静态根目录，绝对路径
        std::optional<std::string> cacheControl; ///< 静态文件响应的 Cache-Control 值；空表示不发这条头
    };

    /**
     * @brief HTTP 服务器类。
     *
     * @details 继承 TcpServer：接受循环、连接计数、空闲清扫与优雅关闭都由基类负责，本类只补四件事——
     *          持有一张路由表、把每条新连接包成 HttpSession、可选的静态文件服务，以及两套限额：
     *          连接级限额（决定会话按什么相位刷新空闲截止时间、一条连接最多服务多少请求）与
     *          解析上限（单个会话的解析器按它为单条报文设内存闸门）。
     *
     * @note 用法：`server.router().get("/x", handler)` 注册业务路由，
     *       需要目录服务时再 `server.staticFileDir("./web")`；两者都必须在 start() 之前完成。
     * @see HttpSession, Router, staticFileDir()
     */
    class HttpServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTP 服务器。
         * @param loop 事件循环，用于管理 I/O 事件与调度会话协程
         * @param address 监听的本地地址（IP 与端口）
         * @throws Base::SystemException 基类创建监听套接字失败
         */
        HttpServer(Core::EventLoop &loop, const Core::InetAddress &address);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数与中间件。
         * @return Router& 路由器对象，生命周期跟随本服务器
         * @note 必须在 start() 之前完成注册；运行期间改路由表虽不会崩，但生效时机不可预期
         */
        [[nodiscard]] Router &router();

        /**
         * @brief 为一条新连接创建 HTTP 会话对象。
         *
         * @details 重写 TcpServer::createConnection()（基类为纯虚钩子）。与基类契约的差异：
         *          socket 的所有权随 HttpSession 转交 Core::Connection；本函数不做任何握手或
         *          阻塞动作（基类禁止在事件循环线程上阻塞），也不会抛异常，返回值永不为空，
         *          因此基类「丢弃连接」的分支不会走到。
         *
         * @param socket 已 accept 且已设为非阻塞的套接字，所有权就此转移
         * @return std::shared_ptr<Core::Connection> 实际类型为 HttpSession
         * @see TcpServer::createConnection(), HttpSession
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

        /**
         * @brief 设置（或关闭）静态文件目录。
         *
         * @details 语义是「配置当前值」，不是「追加一条路由」：首次配置（本方法或
         *          setStaticFileCacheControl()）注册一条 `any("*")` 兜底路由，处理函数只读取
         *          本服务器的当前配置；之后再调用只更新配置，**不会**注册第二条兜底路由，
         *          也不会出现「新目录不生效、旧目录仍在服务」的悬空状态。
         *
         * @param directoryPath 静态文件的根目录路径，相对或绝对均可；传入空串表示关闭静态服务
         *
         * @note 传入的目录在**此刻**被规范化（weakly_canonical）并落定：目录不存在或无权访问时
         *       直接判为关闭静态服务并记一条中文告警，不会推迟到请求到达时再报错。
         *       也就是说，进程启动后才被创建的目录需要再调一次本方法才会生效。
         * @warning 规范化结果会被缓存，因此运行期间把该目录改名或移动，行为不再可预期。
         */
        void staticFileDir(const std::string &directoryPath);

        /**
         * @brief 查询当前生效的静态文件根目录。
         * @return std::string 规范化后的绝对路径；未启用时返回空串
         */
        [[nodiscard]] std::string staticFileDir() const;

        /**
         * @brief 设置静态文件响应的 Cache-Control 头值
         *
         * @details 值随静态文件设置一起被每个静态响应（200/206/304）带上；传空 optional
         *          表示不发这条头。它是一项真实生效的设置，不是只存不用的配置字段。
         * @param cacheControl Cache-Control 值（如 "public, max-age=3600"）；空表示不发
         * @note 值里含 CR/LF/NUL 时整条设置被拒并记中文告警（这些字符会让调用方提前结束
         *       头部块，构成响应拆分），此时等同「不发这条头」
         */
        void setStaticFileCacheControl(std::optional<std::string> cacheControl);

        /**
         * @brief 查询当前配置的静态文件 Cache-Control 值。
         * @return 已设置的值；未设置或从未配置过静态目录时为空 optional
         */
        [[nodiscard]] std::optional<std::string> staticFileCacheControl() const;

        /**
         * @brief 设置连接级限额（空闲 / 读 / 写超时与单连接请求上限）。
         *
         * @details 语义是整体换代而不是就地改写：会话在创建时取走一份共享的只读配置，
         *          已建立的连接因此不会读到半新半旧的组合；要改配置就换一份新的。
         *
         * @param limits 新的限额，取值 0 的字段表示关闭对应保护（见 HttpServerLimits）
         * @note 必须在 start() 之前调用：它只影响此后的 createConnection()，
         *       已经建立的会话继续用创建时那份配置
         * @see HttpServerLimits, TcpServer::setIdleCheckInterval()
         */
        void setLimits(HttpServerLimits limits);

        /**
         * @brief 查询当前生效的连接级限额。
         * @return HttpServerLimits 构造时的默认值，或最后一次 setLimits() 设定的值
         */
        [[nodiscard]] HttpServerLimits limits() const;

        /**
         * @brief 设置解析器资源上限（请求行 / 头部 / 正文 / 分块行）。
         *
         * @details 与连接级限额的分工：setLimits() 管时间与请求条数（超时、单连接请求上限），
         *          本方法管单条报文的内存占用，两者独立生效、互不覆盖。
         *
         * @param limits 新的解析上限，取值 0 的字段表示关闭对应保护（见 HttpParserLimits）
         * @note 必须在 start() 之前调用：限额按值交给此后 createConnection() 新建的会话，
         *       已经建立的会话继续用它构造时那份
         * @see HttpParserLimits, setLimits(), HttpParser
         */
        void setParserLimits(HttpParserLimits limits);

        /**
         * @brief 设置本服务器的明文连接是否按 HTTP/2（h2c，先验知识）服务。
         *
         * @details 打开后本服务器的明文连接一律进入 HTTP/2 会话：对端必须直接发 HTTP/2 连接前奏
         *          （RFC 9113 §3.4 的先验知识），否则连接层按前奏校验失败回 GOAWAY。**不做前奏嗅探、
         *          不做已废弃的 HTTP/1.1 Upgrade 流程**，因此打开本开关就等于「这个端口只说 h2」，
         *          同一端口不再服务 HTTP/1.1。默认关闭，明文连接按 HTTP/1.1 服务。
         *
         * @param enabled true 表示明文连接按 h2c 服务
         * @note 必须在 start() 之前调用：它只影响此后的 createConnection()
         * @see Http2Session, kHttp2ConnectionPreface
         */
        void setHttp2CleartextEnabled(bool enabled) noexcept;

        /**
         * @brief 查询明文连接是否按 h2c 服务
         * @return true 表示明文连接进入 HTTP/2 会话
         */
        [[nodiscard]] bool isHttp2CleartextEnabled() const noexcept;

        /**
         * @brief 查询当前生效的解析器资源上限。
         * @return HttpParserLimits 构造时的默认值，或最后一次 setParserLimits() 设定的值
         */
        [[nodiscard]] HttpParserLimits parserLimits() const;

        /**
         * @brief 取本服务器的统计快照
         *
         * @details 各字段分别原子读取，因此快照不是严格同一瞬间的一致切面（跨字段求和可能与某次
         *          采样略有偏差）；活跃连接数在取快照这一刻从连接管理器读取，与其它字段同为近似同时刻的值。
         *
         * @return HttpServerStats 统计快照；尚未处理任何请求时各计数为零
         * @note 可从任意线程调用（计数是原子量、活跃连接数由连接管理器加锁读取），
         *       运维线程或测试线程可直接采样，不必把动作投递到事件循环
         * @see HttpServerStats, HttpMetricsCollector
         */
        [[nodiscard]] HttpServerStats stats() const;

        /**
         * @brief 在本服务器上注册指标导出端点（Prometheus 文本格式）
         *
         * @details 指标内容见 HttpMetricsEndpoint.h：计数器、状态码分类、耗时直方图、WebSocket 与
         *          HTTP/2 的专项计数，都是本服务器实例自己的口径。
         *
         * @param path 端点路径，必须以 `/` 开头；默认 `/metrics`
         * @param metricNamePrefix 指标名前缀，用于同一进程内区分多套服务；空串表示不带前缀
         * @note **端点默认不开**（不调用本方法就没有任何暴露面）：本框架不做鉴权，公网可达的
         *       /metrics 等于把内部负载与错误率公开出去。它是一条普通路由，会经过中间件，因此
         *       要鉴权就在路由器上挂中间件，或把端点限制在内网/交给反向代理屏蔽
         * @note 必须在 start() 之前调用：路由表在收到请求时读取，开机后再注册会让早到的请求拿不到
         * @see HttpMetricsEndpoint.h, enableHealthEndpoint(), stats()
         */
        void enableMetricsEndpoint(std::string_view path = "/metrics", std::string_view metricNamePrefix = "asyn_http");

        /**
         * @brief 在本服务器上注册健康检查端点
         *
         * @details 应答固定为 200 + `{"status":"ok"}`。它测的是**存活性**：这个响应由事件循环里的
         *          会话协程生成，能答出来就说明循环在转、连接还能被服务。它不表示「依赖都健康」
         *          （本框架不掌握下游依赖），也不区分「正在 drain」——那需要暴露停机期状态，留作细化项。
         *
         * @param path 端点路径，必须以 `/` 开头；默认 `/healthz`
         * @note 与指标端点同样默认不开，是否需要以及暴露给谁由部署方决定
         * @see enableMetricsEndpoint(), kHealthCheckResponseBody
         */
        void enableHealthEndpoint(std::string_view path = "/healthz");

        /**
         * @brief 设置本服务器的在途正文字节预算
         *
         * @details 单条报文的上限（`HttpParserLimits::maximumBodySize`）挡不住「很多条连接各压着一条大
         *          正文」：清点总量需要一个跨连接的账，这就是本方法传进来的对象。超预算的新请求
         *          回 503 并收口，把剩余额度留给已经收下正文的连接。
         *
         * @param memoryBudget 预算对象，空指针表示不做该限制
         * @note 同一台服务器上的所有连接共用传进来的这一份账；多个监听器（多线程）之间也要共享
         *       同一份，否则上限会随监听器数量成倍放大——与按 IP 限额同理
         * @note 必须在 start() 之前调用：会话在构造时取走指针，运行期不再回看服务器
         * @see HttpMemoryBudget
         */
        void setMemoryBudget(std::shared_ptr<HttpMemoryBudget> memoryBudget);

    private:
        /**
         * @brief 确保静态文件设置对象与 "*" 兜底路由已就绪（幂等）
         * @details 设置对象与兜底路由必须同时建立：只建对象不建路由会让后续 staticFileDir()
         *          误判「已注册过」而跳过注册，静态服务再也接不上请求
         */
        void ensureStaticFileSettings();

        Router m_router;                                ///< 路由器，存储路由表与处理函数
        std::shared_ptr<StaticFileSettings> m_staticFileSettings; ///< 静态文件配置；空指针表示还没调用过 staticFileDir()
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，按只读配置交给会话共享
        HttpParserLimits m_parserLimits{}; ///< 解析上限，按值交给每个新会话的解析器（构造时固定，无需共享）
        bool m_isHttp2CleartextEnabled{false}; ///< 明文连接是否按 h2c 服务（先验知识，见 setHttp2CleartextEnabled()）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端，交给会话共享；本服务器所有会话向它累加计数
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器，交给会话共享；前缀标识本服务器实例
        std::shared_ptr<HttpMemoryBudget> m_memoryBudget; ///< 在途正文字节的全局预算，交给会话共享；空指针表示不受该预算约束
    };
} // namespace AsynGyanis::Net
