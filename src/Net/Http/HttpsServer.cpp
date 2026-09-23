#include "Net/Http/HttpsServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http2/Http2Session.h"

#include <openssl/ssl.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace AsynGyanis::Net
{
    HttpsServer::HttpsServer(Core::EventLoop &loop, const int adoptedListeningDescriptor, const std::string &certificateFile,
                             const std::string &keyFile) :
        TcpServer(loop, adoptedListeningDescriptor),
        m_limits(std::make_shared<const HttpServerLimits>()),
        m_metrics(std::make_shared<HttpMetricsCollector>()),
        m_requestIdGenerator(std::make_shared<HttpRequestIdGenerator>())
    {
        // 连接数镜像先接上：本服务器的采集端从这一刻起就是它的计数出口，证书失败与否都不影响这条线
        attachActiveConnectionMirror();

        // 证书必须在进入接受循环之前就位，理由见按地址构造的那一处：留着一个加载失败的上下文，
        // 表现是「端口开着、每条连接都握手失败」，比构造期直接抛异常更难排查
        if (!m_tlsContext.loadCertificate(certificateFile, keyFile))
        {
            throw Base::Exception("HttpsServer: 证书或私钥加载失败（certificate=" + certificateFile + ", key=" + keyFile + "）");
        }
    }

    HttpsServer::HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile) :
        TcpServer(loop, address),
        m_limits(std::make_shared<const HttpServerLimits>()),
        m_metrics(std::make_shared<HttpMetricsCollector>()),
        m_requestIdGenerator(std::make_shared<HttpRequestIdGenerator>())
    {
        // 限额、统计与 request-id 生成器都在此就绪：会话按 shared_ptr 共享持有它们，
        // 采集是常开行为，且它们的生命周期一定覆盖所有会话，创建路径上不必判空
        attachActiveConnectionMirror();

        // 证书必须在进入接受循环之前就位：留着一个加载失败的上下文，
        // 表现是「端口开着、每条连接都握手失败」，比构造期直接抛异常更难排查
        if (!m_tlsContext.loadCertificate(certificateFile, keyFile))
        {
            // 面向使用者的文本走异常消息（英文键名便于跨模块检索），具体原因留在 OpenSSL 错误栈里
            throw Base::Exception("HttpsServer: 证书或私钥加载失败（certificate=" + certificateFile + ", key=" + keyFile + "）");
        }

        // 服务器级信息用根日志器：此刻还没有属于某条连接的 logger，证书路径排障时也只会去翻全局日志
        LOG_INFO_FMT("HttpsServer: TLS 证书加载完成（certificate={}, key={}）", certificateFile, keyFile);
    }

    Router &HttpsServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpsServer::createConnection(Core::AsyncSocket socket)
    {
        // 与基类契约的差异见头文件 Doxygen：这里多了一步 SSL 对象申请，失败以异常而非空指针上报，
        // 让 TcpServer 走它既有的「丢弃这一条连接并继续接受」路径，而不是误判成子类实现有缺陷
        SSL *sslHandle = m_tlsContext.createSSL(socket.fileDescriptor());
        if (sslHandle == nullptr)
        {
            // 抛出去之前不碰 socket：形参析构会关掉描述符，SSL 对象本就没建起来，没有可释放的东西
            throw Base::Exception("HttpsServer: 创建 SSL 对象失败，已丢弃一条新连接");
        }

        // 描述符的所有权就此交给 TlsSocket（它是唯一所有者），socket 被移空只剩占位值；
        // 真正的 TLS 握手留给会话协程去做，这里绝不做任何网络动作。
        //
        // ALPN 分流：ALPN 协商结果产生于握手过程，而本函数在握手之前被同步调用，此刻读到的必然
        // 是空串（见 TlsSocket::selectedAlpnProtocol()）。因此这里统一创建 Http2Session —— 它继承
        // TLS 传输与 HTTP/1.1 路径都由它承载，由它在握手完成后按协商结果选协议：
        // 协商出 h2 就跑 HTTP/2 循环，否则（http/1.1 或客户端没提 ALPN）走同一份 HTTP/1.1 事务循环。
        // 明文 h2c（前奏直发、无 ALPN）不在本片：那条路径上没有任何 ALPN 可读，连接按 HTTP/1.1 处理
        Core::TlsSocket tlsSocket(sslHandle, m_loop, std::move(socket));
        return std::make_shared<Http2Session>(m_loop, std::move(tlsSocket), m_router, m_limits, m_metrics, m_requestIdGenerator, m_parserLimits,
                                              m_memoryBudget);
    }

    std::shared_ptr<HttpMetricsCollector> HttpsServer::metricsCollector() const noexcept
    {
        return m_metrics;
    }

    void HttpsServer::setMetricsCollector(std::shared_ptr<HttpMetricsCollector> collector)
    {
        // 与明文侧同一处置：空采集端等于让本服务器没有计数出口，而读出来全是零、
        // 与「没有流量」看不出差别，因此按用法错误直接拒绝
        if (collector == nullptr)
        {
            throw Base::InvalidArgumentException("HttpsServer: 统计采集端不能为空，请传入一份现成的采集端或调用 metricsCollector() 取本服务器的");
        }

        m_metrics = std::move(collector);
        // 换采集端要连同镜像一起重接，否则连接数会继续写进没人读的那一份
        attachActiveConnectionMirror();
    }

    void HttpsServer::attachActiveConnectionMirror() noexcept
    {
        // 活跃连接数由连接管理器在增删连接的临界区内写进采集端：每台只交出自己那一份，
        // 共用一份采集端时合起来的才是进程口径
        m_connectionManager.setSharedActiveCountMirror(&m_metrics->activeConnectionCountMirror());
    }

    std::shared_ptr<HttpRequestIdGenerator> HttpsServer::requestIdGenerator() const noexcept
    {
        return m_requestIdGenerator;
    }

    void HttpsServer::enableMetricsEndpoint(const std::string_view path, const std::string_view metricNamePrefix)
    {
        // 与 HttpServer 同一处置：路径形状先拦下来，静默注册会给人一个「调了却没有端点」的假象
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpsServer: 指标端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }

        // 前缀按值捕进处理函数：字符串是调用方的，可能比服务器先走；这里只留一份拷贝
        const std::string metricPrefix(metricNamePrefix);
        m_router.get(std::string(path),
                     [this, metricPrefix](HttpRequest &, HttpResponse &response) -> Core::Task<>
                     {
                         // 每次抓取现取一次快照：计数是原子的，不必把动作投递到事件循环
                         response.setStatus(200);
                         response.setHeader("content-type", kPrometheusTextContentType);
                         response.setBody(formatPrometheusMetrics(stats(), metricPrefix));
                         co_return;
                     });
    }

    void HttpsServer::enableHealthEndpoint(const std::string_view path)
    {
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpsServer: 健康检查端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }

        m_router.get(std::string(path),
                     [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                     {
                         response.setStatus(200);
                         response.setHeader("content-type", "application/json");
                         response.setBody(kHealthCheckResponseBody);
                         co_return;
                     });
    }

    void HttpsServer::setLimits(HttpServerLimits limits)
    {
        // 换一份新配置而不是改写原对象，理由同 HttpServer::setLimits()：会话按只读配置共享持有它
        m_limits = std::make_shared<const HttpServerLimits>(limits);
    }

    HttpServerLimits HttpsServer::limits() const
    {
        return *m_limits;
    }

    void HttpsServer::setMemoryBudget(std::shared_ptr<HttpMemoryBudget> memoryBudget)
    {
        // 理由同 HttpServer::setMemoryBudget()：预算跨连接共用一份账，按共享指针存
        m_memoryBudget = std::move(memoryBudget);
    }

    void HttpsServer::setParserLimits(HttpParserLimits limits)
    {
        // 按值保存，理由同 HttpServer::setParserLimits()：解析器在会话构造时取走一份副本，之后没有读者
        m_parserLimits = limits;
    }

    HttpParserLimits HttpsServer::parserLimits() const
    {
        return m_parserLimits;
    }

    HttpServerStats HttpsServer::stats() const
    {
        // 与 HttpServer 同一口径：活跃连接数是采集端上的镜像量，由本服务器的连接管理器在增删
        // 连接时写入，共用一份采集端的多台合起来才是进程口径
        return m_metrics->snapshot();
    }

    bool HttpsServer::reloadCertificate()
    {
        // 只做转发：换代的全部语义（先建后换、失败不碰旧上下文、复现 mTLS 与 OCSP）都在 TlsContext 里，
        // 这里再包一层是为了让运维调用方不必接触内部上下文对象
        return m_tlsContext.reloadCertificate();
    }

    bool HttpsServer::loadOcspResponse(const std::string &ocspResponseFile)
    {
        // 同 reloadCertificate()：纯转发，装订数据的存放与线程安全都由 TlsContext 负责
        return m_tlsContext.loadOcspResponse(ocspResponseFile);
    }

} // namespace AsynGyanis::Net
