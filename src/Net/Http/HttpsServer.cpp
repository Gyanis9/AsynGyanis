#include "Net/Http/HttpsServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpsSession.h"
#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http2/Http2Session.h"

#include <openssl/ssl.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace AsynGyanis::Net
{
    HttpsServer::HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile) :
        TcpServer(loop, address),
        m_limits(std::make_shared<const HttpServerLimits>()),
        m_metrics(std::make_shared<HttpMetricsCollector>()),
        m_requestIdGenerator(std::make_shared<HttpRequestIdGenerator>())
    {
        // 限额、统计与 request-id 生成器都在此就绪：会话按 shared_ptr 共享持有它们，
        // 采集是常开行为，且它们的生命周期一定覆盖所有会话，创建路径上不必判空

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
        // HttpsSession 的全部传输层与 HTTP/1.1 路径，由它在握手完成后按协商结果选协议：
        // 协商出 h2 就跑 HTTP/2 循环，否则（http/1.1 或客户端没提 ALPN）原样交回 HttpsSession。
        // 明文 h2c（前奏直发、无 ALPN）不在本片：那条路径上没有任何 ALPN 可读，连接按 HTTP/1.1 处理
        Core::TlsSocket tlsSocket(sslHandle, m_loop, std::move(socket));
        return std::make_shared<Http2Session>(m_loop, std::move(tlsSocket), m_router, m_limits, m_metrics, m_requestIdGenerator, m_parserLimits,
                                              m_memoryBudget);
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
        HttpServerStats snapshot = m_metrics->snapshot();

        // 活跃连接数与 HttpServer 同源：现读连接管理器，避免另设一份计数与它漂移。
        // size_t 到 uint64_t 是加宽转换，32 位平台上也不会丢信息
        snapshot.activeConnectionCount = static_cast<std::uint64_t>(m_connectionManager.activeCount());
        return snapshot;
    }

    bool HttpsServer::reloadCertificate()
    {
        // 只做转发：换代的全部语义（先建后换、失败不碰旧上下文、复现 mTLS）都在 TlsContext 里，
        // 这里再包一层是为了让运维调用方不必接触内部上下文对象
        return m_tlsContext.reloadCertificate();
    }

} // namespace AsynGyanis::Net
