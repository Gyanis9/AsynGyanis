#include "Net/Http/HttpsServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpsSession.h"

#include <openssl/ssl.h>

#include <memory>
#include <utility>

namespace AsynGyanis::Net
{
    HttpsServer::HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile) :
        TcpServer(loop, address)
    {
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
        // 真正的 TLS 握手留给会话协程去做，这里绝不做任何网络动作
        Core::TlsSocket tlsSocket(sslHandle, m_loop, std::move(socket));
        return std::make_shared<HttpsSession>(m_loop, std::move(tlsSocket), m_router);
    }

} // namespace AsynGyanis::Net
