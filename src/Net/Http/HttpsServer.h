/**
 * @file HttpsServer.h
 * @brief HTTPS 服务器：持有 TlsContext，为每条连接派生 TLS 包装的 Http2Session
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsContext.h"

#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/Http/StaticFileService.h"
#include "Net/Http2/Http2Connection.h"
#include "Net/Tcp/TcpServer.h"

#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTPS 服务器类。
     *
     * @details 与 HttpServer 的分工一致：接受循环与连接生命周期由 TcpServer 负责，
     *          本类额外持有一个 TLS 上下文，并在 createConnection() 里为每条新连接
     *          申请一个 SSL 对象、包成 Core::TlsSocket，再交给 Http2Session。
     *
     * @note 证书在构造时就加载完毕：加载失败直接抛异常，不会留下一个「看似在监听、
     *       每条连接都握手失败」的服务器。
     * @warning 一个 SSL_CTX 被本服务器的所有连接共享，OpenSSL 保证 SSL 对象可并发使用；
     *          会话本身仍全部跑在同一个事件循环线程上。
     * @see Http2Session, Core::TlsContext, HttpServer
     */
    class HttpsServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTPS 服务器并加载证书。
         * @param loop 事件循环，用于管理 I/O 事件与调度会话协程
         * @param address 监听的本地地址（IP 与端口）
         * @param certificateFile 服务器证书文件路径（PEM 格式，可含证书链）
         * @param keyFile 服务器私钥文件路径（PEM 格式）
         * @param policy TLS 策略（版本区间、套件与曲线、安全等级、信任库与校验深度、票据开关）；
         *        默认构造即本服务器既有档位，因此老调用方行为逐字不变
         * @throws Base::SystemException 基类创建监听套接字失败
         * @throws Base::Exception 证书或私钥加载失败（文件缺失、格式不对、口令不符）
         * @throws Core::CoreException 策略里某一项被当前 OpenSSL 拒绝（版本区间、套件列表、曲线、
         *         CA 信任库）：与证书失败一样属于启动期配置错误，当场抛而不是带着半生效的策略上线
         */
        HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile, const Core::TlsPolicy &policy = {});

        /**
         * @brief 用「已经在监听中的套接字」构造 HTTPS 服务器：零停机重启的接手侧
         * @param loop 事件循环，要求与按地址构造时相同
         * @param adoptedListeningDescriptor 已经在监听状态的套接字描述符，所有权随之转移
         * @param certificateFile 证书链文件路径
         * @param keyFile 私钥文件路径
         * @param policy TLS 策略，语义与按地址构造那一支相同
         * @throws Base::InvalidArgumentException 描述符无效
         * @throws Base::Exception 证书或私钥加载失败
         * @see TcpServer::TcpServer(Core::EventLoop &, int)
         */
        HttpsServer(Core::EventLoop &loop, int adoptedListeningDescriptor, const std::string &certificateFile, const std::string &keyFile, const Core::TlsPolicy &policy = {});

        /**
         * @brief 加载用于校验客户端证书的 CA，开启双向 TLS 的第一步
         * @details 纯转发，完整语义（可含多张、必须在开始接受连接之前调用、热轮换时按原路径复现）见
         *          Core::TlsContext::loadClientCertificateAuthority()。之所以要在这一层露出来：
         *          对端证书校验是「谁能连我」的问题，属服务器配置，不该逼调用方去摸内部上下文。
         * @param caFile CA 文件路径（PEM，即客户端证书的签发者或其根）
         * @return true 信任库已就位；false 加载不了（原因在 OpenSSL 错误栈里），此时不做任何降级
         * @note 要 CA 目录或校验深度就走构造期的 TlsPolicy：那条路径同时支持 CApath 与 verify_depth
         * @see setClientCertificateRequired(), TlsPolicy
         */
        bool loadClientCertificateAuthority(const std::string &caFile);

        /**
         * @brief 设置是否要求客户端出示并通过校验证书（mTLS 的第二步）
         * @param required true 要求（不出示即终止握手，不退化成可选校验）；false 关闭该校验
         * @throws Core::CoreException 传 true 但 CA 从未就绪：要求校验却没有 CA 会让每条连接都
         *         握手失败，属于配置错误，故当场拒绝
         * @note 必须在 start() 之前调用：校验模式挂在上下文上，此后新建的连接才看得到
         * @see loadClientCertificateAuthority()
         */
        void setClientCertificateRequired(bool required);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数与中间件。
         * @return Router& 路由器对象，生命周期跟随本服务器
         * @note 必须在 start() 之前完成注册
         */
        [[nodiscard]] Router &router();

        /**
         * @brief 设置（或关闭）静态文件目录，语义与明文侧逐字相同
         * @param directoryPath 静态文件的根目录（UTF-8 文本），相对或绝对均可；空串表示关闭
         * @details 与 `HttpServer::staticFileDir()` 共用同一份实现（StaticFileService），包括
         *          「目录在此刻规范化、失败即关闭并告警」与那条只登记一次的 `any("*")` 兜底路由。
         *          零拷贝发送只在 Linux 明文上发生（TLS 侧一律走聚合写），因此这一条在 HTTPS 上的
         *          性能形态与明文不同、语义相同。
         * @note 必须在 start() 之前调用
         */
        void staticFileDir(const std::string &directoryPath);

        /// @brief 读回当前生效的静态根目录（UTF-8 文本）；未启用时为空串
        [[nodiscard]] std::string staticFileDir() const;

        /// @brief 设置静态文件响应的 Cache-Control 值；空 optional 表示不发这条头
        void setStaticFileCacheControl(std::optional<std::string> cacheControl);

        /// @brief 读回当前生效的 Cache-Control 配置；未设置时为空 optional
        [[nodiscard]] std::optional<std::string> staticFileCacheControl() const;

        /**
         * @brief 为一条新连接创建 TLS 加密的 HTTP 会话。
         *
         * @details 重写 TcpServer::createConnection()：接管 socket 之前先向 TLS 上下文申请 SSL
         *          对象，失败时**抛异常**（基类会丢弃这条连接并记录中文错误）而不是返回空指针；
         *          socket 所有权随 Core::TlsSocket 转移，会话交给基类的是一条占位套接字；此处不做
         *          任何网络动作，握手留在会话协程里做，以免在事件循环线程上阻塞。
         *
         * @details ALPN 分流：ALPN 协商结果产生于握手过程，本函数在握手之前被调用，因此这里不按
         *          「读到的 ALPN」选类（那必然读到空串），统一创建 Http2Session —— 它继承
         *          除 HTTP/2 循环外还承载 TLS 传输与 HTTP/1.1 路径，在握手完成后按协商结果选协议：
         *          h2 走 HTTP/2 循环，其余交回 HTTP/1.1 循环。明文 h2c（前奏直发）不在本片。
         *
         * @param socket 已 accept 且已设为非阻塞的套接字，所有权就此转移
         * @return std::shared_ptr<Core::Connection> 实际类型为 Http2Session（内含 HTTP/2 与
         *         HTTP/1.1 两条路径），永不为空
         * @throws Base::Exception SSL 对象申请失败（此时 socket 随形参析构被关闭）
         * @see TcpServer::createConnection(), Http2Session, Http2Session::start(), Core::TlsContext::createSSL()
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

        /**
         * @brief 设置连接级限额（空闲 / 读 / 写超时与单连接请求上限）。
         *
         * @details 与 HttpServer::setLimits() 同一语义：整体换代而不是就地改写，
         *          会话在创建时取走一份共享的只读配置。TLS 连接同样受空闲清扫约束
         *          （清扫在 TcpServer 层，与传输层无关）。
         *
         * @param limits 新的限额，取值 0 的字段表示关闭对应保护（见 HttpServerLimits）
         * @note 必须在 start() 之前调用；已经建立的会话继续用创建时那份配置
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
         * @details 与 HttpServer::setParserLimits() 同一语义：连接级限额管时间与请求条数，
         *          本项管单条报文的内存占用，两者独立生效、互不覆盖；限额按值交给此后新建的会话。
         *
         * @param limits 新的解析上限，取值 0 的字段表示关闭对应保护（见 HttpParserLimits）
         * @note 必须在 start() 之前调用；已经建立的会话继续用创建时那份配置
         * @see HttpParserLimits, setLimits(), HttpServer::setParserLimits()
         */
        void setParserLimits(HttpParserLimits limits);

        /**
         * @brief 设置本服务器的在途正文字节预算
         * @param memoryBudget 预算对象，空指针表示不做该限制；语义与 HttpServer::setMemoryBudget() 一致
         * @note 同样必须在 start() 之前调用：会话在构造时取走这一份共享指针
         * @see HttpServer::setMemoryBudget(), HttpMemoryBudget
         */
        void setMemoryBudget(std::shared_ptr<HttpMemoryBudget> memoryBudget);

        /**
         * @brief 查询当前生效的解析器资源上限。
         * @return HttpParserLimits 构造时的默认值，或最后一次 setParserLimits() 设定的值
         */
        [[nodiscard]] HttpParserLimits parserLimits() const;

        /**
         * @brief 取本服务器的统计快照
         *
         * @details 与 HttpServer::stats() 同一口径：各字段分别原子读取，快照不是严格同一瞬间的
         *          一致切面；活跃连接数是采集端上的镜像量，由连接管理器在增删连接时同步写入。
         *
         * @return HttpServerStats 统计快照；尚未处理任何请求时各计数为零
         * @note 可从任意线程调用（计数都是原子量），运维线程或测试线程可直接采样，
         *       不必把动作投递到事件循环
         * @see HttpServerStats, HttpMetricsCollector, HttpServer::stats()
         */
        [[nodiscard]] HttpServerStats stats() const;

        /**
         * @brief 在本服务器上注册指标导出端点（Prometheus 文本格式）
         * @details 与 HttpServer::enableMetricsEndpoint() 同一形态与同一套指标口径：TLS 上的 h1/h2
         *          会话本来就向本服务器的采集端计数（见 stats()），只差把这个读数暴露出来。
         *          与 HTTP 侧共用一份采集端（metricsCollector()）时，两条服务路径的计数会并进同一份输出
         * @param path 端点路径，必须以 `/` 开头；默认 `/metrics`
         * @param metricNamePrefix 指标名前缀，用于同一进程内区分多套服务；空串表示不带前缀
         * @note **端点默认不开**（不调用本方法就没有任何暴露面）：本框架不做鉴权，公网可达的
         *       /metrics 等于把内部负载与错误率公开出去
         * @note 必须在 start() 之前调用：路由表在收到请求时读取，开机后再注册会让早到的请求拿不到
         * @throws Base::InvalidArgumentException 路径不以 `/` 开头
         * @see HttpMetricsEndpoint.h, HttpServer::enableMetricsEndpoint()
         */
        void enableMetricsEndpoint(std::string_view path = "/metrics", std::string_view metricNamePrefix = "asyn_http");

        /**
         * @brief 在本服务器上注册健康检查端点
         * @param path 端点路径，必须以 `/` 开头；默认 `/healthz`
         * @throws Base::InvalidArgumentException 路径不以 `/` 开头
         * @see HttpServer::enableHealthEndpoint(), kHealthCheckResponseBody
         */
        void enableHealthEndpoint(std::string_view path = "/healthz");

        /**
         * @brief 取本服务器的统计采集端
         * @details 采集端是共享对象：把它传给别的服务端（例如 QuicServer 的
         *          Configuration::metricsCollector），那条服务路径的计数就会并进本服务器的
         *          /metrics 与 stats()，一处抓取即可覆盖多条服务路径
         * @return std::shared_ptr<HttpMetricsCollector> 采集端，恒非空
         */
        [[nodiscard]] std::shared_ptr<HttpMetricsCollector> metricsCollector() const noexcept;

        /**
         * @brief 换用一份现成的统计采集端，让多台服务器把计数并进同一份口径
         * @details 与明文侧 `HttpServer::setMetricsCollector()` 同一用途与同一约束：TLS 侧与明文侧
         *          各持一份采集端时，抓哪一份就只能看到那一条通道的量。
         * @param collector 要并入的采集端，非空；活跃连接数也随之并进它的合计
         * @throws Base::InvalidArgumentException collector 为空
         * @note 必须在 start() 之前调用：会话按创建时那份采集端计数
         * @see HttpServer::setMetricsCollector(), metricsCollector()
         */
        void setMetricsCollector(std::shared_ptr<HttpMetricsCollector> collector);

        /**
         * @brief 取本服务器的 request-id 生成器
         * @details 与明文侧 `HttpServer::requestIdGenerator()` 同一个用途：传给别的服务路径
         *          （`QuicServer::Configuration::requestIdGenerator`）后，同一台机器上各条通道
         *          落定的 id 前缀一致
         * @return std::shared_ptr<HttpRequestIdGenerator> 生成器，恒非空
         */
        [[nodiscard]] std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator() const noexcept;

        /**
         * @brief 用当前证书路径重新加载证书与私钥，新连接立即改用新证书（热轮换）。
         *
         * @details 续期流程：ACM 客户端/certbot 把新证书覆盖到原路径 → 调用本方法。已建立的连接
         *          不受影响（各自持有旧上下文，见 Core::TlsContext::reloadCertificate()）。
         *          实现上先在新上下文把加固与 mTLS 配置整套重建、成功之后才换，因此**失败不会
         *          影响正在服务的旧证书**。
         *
         * @return true 新证书已生效；false 本次没换（新证书加载失败，或本服务器从未加载过证书），
         *         此时旧证书继续服务，调用方应记日志并稍后重试
         * @note 线程安全：与「为新连接创建 SSL」互斥；可从运维线程直接调用，不必投递到事件循环
         * @see Core::TlsContext::reloadCertificate()
         */
        bool reloadCertificate();

        /**
         * @brief 加载 OCSP 响应文件（DER 格式），此后 TLS 握手按客户端请求装订（stapling）。
         *
         * @details 与 reloadCertificate() 同为「路径即身份」的运维形态：此后每次证书热轮换
         *          都会按原路径重读响应、与证书一起换代；完整语义见 Core::TlsContext::loadOcspResponse()。
         *
         * @param ocspResponseFile OCSP 响应文件路径（DER；通常由 ACME 客户端随证书一并产出）
         * @return true 响应已生效；false 文件不可读或内容为空，此时保持原状态不装订
         * @note 可在服务运行中调用；已建立的连接不受影响
         * @see Core::TlsContext::loadOcspResponse()
         */
        bool loadOcspResponse(const std::string &ocspResponseFile);

        /**
         * @brief 装载会话票据密钥，让本服务器与别的进程、别的机器互相认得对方签发的票据。
         *
         * @details 纯转发，完整语义（长度校验、轮换、换代时按原路径重读）见
         *          Core::TlsContext::loadSessionTicketKeys()。之所以要在这一层露出来：多进程部署
         *          （SO_REUSEPORT 的 WorkerSupervisor）里每个 worker 各有一份 TlsContext，
         *          不装载同一份密钥文件的话，客户端第二次连接被分到别的 worker 就恢复不了会话，
         *          只能退回全量握手；证书换代前后同理。
         *
         * @param keyFiles 密钥文件路径列表（**二进制**，每份 48 或 80 字节），首份用于签发新票据、
         *                 其余只用于解开轮换窗口内的旧票据
         * @throws Core::CoreException 列表为空、某份文件读不出来或为空、某份长度既不是 48 也不是 80；
         *         消息点名是哪一份文件
         * @note 在开始接受连接之前调用；此后新建的连接用它，已建立的连接不受影响
         * @note 密钥文件按私钥同级保管：拿到它就能解开本服务签发的所有票据
         * @see Core::TlsContext::loadSessionTicketKeys(), reloadCertificate()
         */
        void loadSessionTicketKeys(const std::vector<std::string> &keyFiles);

        /**
         * @brief 设置 HTTP/2 连接层配置（SETTINGS 通告值与本端各项上限）。
         *
         * @details 本服务器是唯一跑 h2 的 TLS 入口，而配置此前改不动：连接层一律按缺省值构造，
         *          最大并发流数、头块上限这些只能在服务端 SETTINGS 里观测。取值交给此后新建的会话，
         *          已建立的连接继续用它握手时那份。完整判据见 HttpServer::setHttp2Configuration()，
         *          两端共用同一份 Http2ConnectionConfiguration，非法值都在设置时就抛。
         *
         * @param configuration 新的连接层配置
         * @throws Base::InvalidArgumentException 取值非法（见 Http2Connection::validateConfiguration()）
         * @see Http2ConnectionConfiguration, HttpServer::setHttp2Configuration()
         */
        void setHttp2Configuration(Http2ConnectionConfiguration configuration);

        /**
         * @brief 查询当前生效的 HTTP/2 连接层配置
         * @return Http2ConnectionConfiguration 最近一次设置值，未设置过则为缺省值
         */
        [[nodiscard]] const Http2ConnectionConfiguration &http2Configuration() const noexcept;

    private:
        /**
         * @brief 确保静态目录配置与 "*" 兜底路由已建立（幂等）
         * @details 建立动作在 StaticFileService::install() 里，与明文侧共用；这里只递本服务器的
         *          路由器与当时的映射缓存限额。四个静态方法先调它一次，配置顺序因此无所谓。
         */
        void ensureStaticFileSettings();

        /**
         * @brief 把本服务器的连接数镜像接到当前采集端上
         * @details 构造与 setMetricsCollector() 各接一次：换采集端不重接就会把连接数写进
         *          已经没人读的那一份
         */
        void attachActiveConnectionMirror() noexcept;

        Router                                  m_router;               ///< 路由器，存储 HTTP 路由表与处理函数
        StaticFileService                       m_staticFiles;          ///< 静态目录配置本体；四个静态方法都转发到它（与明文侧同一份实现）
        Core::TlsContext                        m_tlsContext;           ///< TLS 上下文，管理 SSL_CTX 与证书，被所有连接共享
        std::shared_ptr<const HttpServerLimits> m_limits;               ///< 连接级限额，按只读配置交给会话共享
        HttpParserLimits                        m_parserLimits{};       ///< 解析上限，按值交给每个新会话的解析器（构造时固定，无需共享）
        Http2ConnectionConfiguration            m_http2Configuration{}; ///< h2 连接层配置，按值交给每个新会话
        std::shared_ptr<HttpMemoryBudget>       m_memoryBudget;         ///< 在途正文字节的全局预算，交给会话共享；空指针表示不受该预算约束
        std::shared_ptr<HttpMetricsCollector>   m_metrics;              ///< 统计采集端，交给会话共享；本服务器所有会话向它累加计数
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator;   ///< request-id 生成器，交给会话共享；前缀标识本服务器实例
    };
} // namespace AsynGyanis::Net
