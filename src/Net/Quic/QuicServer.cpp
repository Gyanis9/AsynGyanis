#include "Net/Quic/QuicServer.h"

#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"
#include "Net/Http/Router.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 无状态重置令牌的密钥长度：够长即可，服务端级固定一份
        constexpr std::size_t kStatelessResetSecretLength = 32;

        /// 服务端要协商出的 ALPN：HTTP/3 约定用 h3
        constexpr std::string_view kHttp3ApplicationProtocol = "h3";

        /**
         * @brief 把 OpenSSL 的错误队列折成一行可读文本
         * @return std::string 最近一条错误；队列为空时给出占位文本
         */
        std::string lastOpenSslError()
        {
            const unsigned long errorCode = ERR_get_error();
            if (errorCode == 0)
            {
                return "(OpenSSL 未给出错误详情)";
            }
            char buffer[256]{};
            ERR_error_string_n(errorCode, buffer, sizeof(buffer));
            return buffer;
        }

        /**
         * @brief ALPN 选择回调：只接受 HTTP/3
         * @details 协商不出 h3 就按致命告警终止握手——放行别的协议会让后续按 h3 解析的字节流对不上，
         *          不如在握手期就明确拒绝。
         * @note 递给 SSL_select_next_proto 的服务端列表必须是**线格式**（首字节是长度、后面是协议名），
         *       只给裸 "h3" 一个字节串它永远匹配不上——握手会停在 ALPN 这一步
         */
        int selectApplicationProtocol(SSL *, const unsigned char **out, unsigned char *outLength, const unsigned char *clientList,
                                      const unsigned int clientListLength, void *)
        {
            // 线格式的服务端列表：{2, 'h', '3'}
            static constexpr unsigned char kServerProtocols[] = {2, 'h', '3'};
            if (SSL_select_next_proto(const_cast<unsigned char **>(out), outLength, kServerProtocols, sizeof(kServerProtocols), clientList,
                                      clientListLength) != OPENSSL_NPN_NEGOTIATED)
            {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            return SSL_TLSEXT_ERR_OK;
        }
    } // namespace

    QuicServer::QuicServer(Core::EventLoop &eventLoop, Configuration configuration) :
        m_eventLoop(eventLoop), m_configuration(std::move(configuration)), m_expiryTicker(eventLoop)
    {
        // ngtcp2 的 ossl 胶水要做一次全局初始化（它内部缓存 EVP 相关的句柄，跳过会明显拖慢性能）
        static const bool isCryptoInitialized = ngtcp2_crypto_ossl_init() == 0;
        if (!isCryptoInitialized)
        {
            throw Base::SystemException("QUIC 服务端启动失败：ngtcp2 的 OpenSSL 胶水初始化未成功");
        }

        m_tlsContext = SSL_CTX_new(TLS_server_method());
        if (m_tlsContext == nullptr)
        {
            throw Base::SystemException("QUIC 服务端启动失败：TLS 上下文创建失败（" + lastOpenSslError() + "）");
        }

        // QUIC 只用 TLS 1.3：低版本没有 QUIC 需要的握手接口
        if (SSL_CTX_set_min_proto_version(m_tlsContext, TLS1_3_VERSION) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：无法把 TLS 最低版本限到 1.3（" + lastOpenSslError() + "）");
        }
        if (SSL_CTX_use_certificate_chain_file(m_tlsContext, m_configuration.certificateFile.c_str()) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：证书加载失败（" + m_configuration.certificateFile + "）：" + lastOpenSslError());
        }
        if (SSL_CTX_use_PrivateKey_file(m_tlsContext, m_configuration.privateKeyFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：私钥加载失败（" + m_configuration.privateKeyFile + "）：" + lastOpenSslError());
        }
        if (SSL_CTX_check_private_key(m_tlsContext) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：私钥与证书不匹配：" + lastOpenSslError());
        }
        SSL_CTX_set_alpn_select_cb(m_tlsContext, selectApplicationProtocol, nullptr);

        m_statelessResetSecret.resize(kStatelessResetSecretLength);
        if (RAND_bytes(m_statelessResetSecret.data(), static_cast<int>(m_statelessResetSecret.size())) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：随机数不可用，无法生成无状态重置令牌的密钥");
        }
    }

    QuicServer::~QuicServer()
    {
        // 连接先于 SSL_CTX 销毁：连接析构里还要用上下文里的会话对象。
        // HTTP/3 会话内部指向各自的连接，别名索引持有的是裸指针——两者都要先于连接表清掉
        m_http3Sessions.clear();
        m_connectionsByAliasConnectionId.clear();
        m_connections.clear();
        if (m_tlsContext != nullptr)
        {
            SSL_CTX_free(m_tlsContext);
            m_tlsContext = nullptr;
        }
    }

    Core::Task<> QuicServer::listen(Core::InetAddress localAddress)
    {
        Platform::SocketAddress localSocketAddress;
        localSocketAddress.length = localAddress.nativeAddressLength();
        std::memcpy(&localSocketAddress.storage, localAddress.nativeAddress(), localAddress.nativeAddressLength());

        m_datagramSocket = Platform::DatagramSocket::bindTo(localSocketAddress);
        if (!m_datagramSocket.isValid())
        {
            throw Base::SystemException("QUIC 服务端启动失败：UDP 端口绑定失败（套接字错误码 " +
                                        std::to_string(Platform::PlatformError::lastSocketErrorCode()) + "）");
        }

        const Platform::SocketAddress boundAddress = m_datagramSocket.localAddress();
        sockaddr_in                   boundAddressV4{};
        std::memcpy(&boundAddressV4, &boundAddress.storage, sizeof(boundAddressV4));
        m_listeningPort = ntohs(boundAddressV4.sin_port);

        // 建连接时要拿本端地址进 ngtcp2 的 path，必须用**绑定后**的地址（端口给 0 时只有内核知道
        // 实际端口）。漏掉这一步 path.local 就是全零地址，ngtcp2 会拒绝写报文——服务端一条都发不出去
        m_localSocketAddress = boundAddress;

        m_socket = std::make_unique<Core::AsyncUdpSocket>(m_eventLoop, std::move(m_datagramSocket));
        LOG_INFO_FMT("QuicServer: 已在 UDP 端口 {} 上监听（ALPN {}）", m_listeningPort, m_configuration.applicationProtocol);

        // 两个循环并发跑：收报文的与驱动定时器的。定时器不能只挂在收报文上，否则空闲期（对端在等超时）
        // 就没人推进 PTO/空闲超时
        Core::Task<> expiryTask = runExpiryTicker();
        m_eventLoop.scheduler().schedule(expiryTask.handle());

        std::vector<std::uint8_t> receiveBuffer(Platform::DatagramSocket::kMaximumDatagramBytes);
        while (!m_isStopped.load(std::memory_order_acquire))
        {
            Platform::SocketAddress peerAddress;
            const ssize_t           receivedLength = co_await m_socket->asyncReceiveFrom(receiveBuffer.data(), receiveBuffer.size(), peerAddress);
            if (receivedLength < 0)
            {
                // 套接字被关（stop()）或读失败：退出收循环，收尾交给析构
                break;
            }
            if (receivedLength == 0)
            {
                // 空报文：QUIC 没有意义，丢掉即可（UDP 允许零长报文，收到它不算错误）
                continue;
            }

            co_await routeDatagram(peerAddress, std::span<const std::uint8_t>(receiveBuffer.data(), static_cast<std::size_t>(receivedLength)));
            reapClosedConnections();
        }

        // 等定时循环退出：它下次醒来就会发现停止标志
        stop();
        static_cast<void>(expiryTask);
        co_return;
    }

    void QuicServer::stop() noexcept
    {
        // 只置标记，不碰套接字与等待器：本方法可能从别的线程调用，而套接字与注册对象都只归
        // 所属事件循环线程。收尾由调用方的循环停止与随后的对象析构完成——两条路径都不需要
        // 在这里动描述符（在途探针还没收回来时销毁注册对象会让事件循环踩到已释放的对象）
        m_isStopped.store(true, std::memory_order_release);
    }

    void QuicServer::setStreamDataHandler(QuicConnection::StreamDataHandler handler)
    {
        m_streamDataHandler = std::move(handler);
    }

    void QuicServer::setRouter(Router &router) noexcept
    {
        m_router = &router;
    }

    Http3Session &QuicServer::http3SessionFor(QuicConnection &connection)
    {
        if (Http3Session *const existing = findHttp3Session(&connection); existing != nullptr)
        {
            return *existing;
        }

        // 会话的三个口子都绑到这条连接上：开单向流、写流数据、归还接收额度。
        // 这里捕获裸指针而不是引用，是为了让「会话指向哪条连接」在代码里显式可见
        QuicConnection *const rawConnection = &connection;
        auto                  session       = std::make_unique<Http3Session>(
                [rawConnection] { return rawConnection->openUnidirectionalStream(); },
                [rawConnection](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                { rawConnection->queueStreamData(streamId, data, isEndStream); },
                [rawConnection](const std::int64_t streamId, const std::size_t consumedByteCount)
                { rawConnection->extendReceiveWindow(streamId, consumedByteCount); },
                m_configuration.metricsCollector);
        if (m_router != nullptr)
        {
            session->attachRouter(*m_router);
        }
        // 解析上限必须显式交给会话：默认构造的上限虽然安全，但调用方在 Configuration 里
        // 调过的值（例如放宽正文上限）必须真的生效，否则「配置了却不生效」更难排查
        session->setParserLimits(m_configuration.parserLimits);

        Http3Session &createdSession = *session;
        m_http3Sessions.emplace(rawConnection, std::move(session));
        return createdSession;
    }

    HttpServerStats QuicServer::stats() const
    {
        HttpServerStats snapshot;
        if (m_configuration.metricsCollector != nullptr)
        {
            snapshot = m_configuration.metricsCollector->snapshot();
        }
        // 在线连接数与 h1/h2 侧同一口径：取快照这一刻的连接数（这里是近似值，不做一致性保证）
        snapshot.activeConnectionCount = static_cast<std::uint64_t>(connectionCount());
        return snapshot;
    }

    Http3Session *QuicServer::findHttp3Session(const QuicConnection *const connection) noexcept
    {
        const auto existing = m_http3Sessions.find(connection);
        return existing != m_http3Sessions.end() ? existing->second.get() : nullptr;
    }

    Core::Task<> QuicServer::pumpHttp3For(QuicConnection &connection)
    {
        if (Http3Session *const session = findHttp3Session(&connection); session != nullptr)
        {
            if (session->isBroken() || !session->isUsable())
            {
                // 会话建不起来（控制流/QPACK 开不出）或已被判协议错误：这条连接上再也不会有
                // 请求能完成，留着只会让对端干等到空闲超时。收口后交给清理循环摘除
                LOG_WARN_FMT("QuicServer: HTTP/3 会话不可用（{}），连接按收口处理", session->isBroken() ? "已判协议错误" : "初始化失败");
                connection.requestClose();
                co_return;
            }
            co_await session->pump();
            // 业务刚写下的响应此刻才排进连接的待发队列，必须再刷一次才会出网：
            // handleDatagram 里那次 flush 发生在业务之前（SETTINGS 那批因此出得去，
            // 而响应留在队列里等下一次定时器把它想起来——实测客户端就是干等超时）
            co_await connection.flush();
        }
        co_return;
    }

    std::size_t QuicServer::connectionCount() const noexcept
    {
        return m_connections.size();
    }

    std::uint16_t QuicServer::listeningPort() const noexcept
    {
        return m_listeningPort;
    }

    Core::Task<> QuicServer::routeDatagram(const Platform::SocketAddress &peerAddress, const std::span<const std::uint8_t> datagram)
    {
        if (m_isStopped.load(std::memory_order_acquire))
        {
            co_return;
        }

        // 路由键是报文里的目的连接标识；解不出来（太短、版本协商报文等）就丢掉。
        // 最后一个参数是 **Short 头报文的 DCID 长度**：Short 头不携带这个长度，必须告诉解码器本端
        // 自己的连接标识有多长。传 NGTCP2_MAX_CIDLEN 会让它把包号的头两字节也当成标识的一部分，
        // 从 1-RTT 起每条报文都命不中路由表（实测：握手全通、之后客户端所有报文整包被丢）
        ngtcp2_version_cid versionAndConnectionIds{};
        if (ngtcp2_pkt_decode_version_cid(&versionAndConnectionIds, datagram.data(), datagram.size(),
                                         QuicConnection::kSourceConnectionIdLength) != 0)
        {
            co_return;
        }
        const std::string destinationConnectionId(reinterpret_cast<const char *>(versionAndConnectionIds.dcid),
                                                  versionAndConnectionIds.dcidlen);
        if (const auto existing = m_connections.find(destinationConnectionId); existing != m_connections.end())
        {
            co_await existing->second->handleDatagram(peerAddress, datagram);
            registerConnectionIds(*existing->second);
            co_await pumpHttp3For(*existing->second);
            co_return;
        }

        // 别名索引：客户端重传 Initial 时，报文里的 DCID 仍是它最初选的那个（RFC 9000 §7.2 的首包
        // 连接标识固定到服务端回话为止），而按本端 SCID 建的键这时对不上——少了这一路，每条重传
        // 都会当成新连接（实测：一个客户端握手却建出 8 条连接，握手因此永远收不了口）。
        // ngtcp2 后续签发的额外连接标识也走这一路，见 registerConnectionIds
        if (const auto byAliasConnectionId = m_connectionsByAliasConnectionId.find(destinationConnectionId);
            byAliasConnectionId != m_connectionsByAliasConnectionId.end())
        {
            QuicConnection *matchedConnection = byAliasConnectionId->second;
            co_await matchedConnection->handleDatagram(peerAddress, datagram);
            registerConnectionIds(*matchedConnection);
            co_await pumpHttp3For(*matchedConnection);
            co_return;
        }

        if (m_connections.size() >= m_configuration.maximumConnections)
        {
            LOG_WARN_FMT("QuicServer: 在线连接已达上限 {}，新连接被拒绝", m_configuration.maximumConnections);
            co_return;
        }

        // 不认识的目的连接标识：只有「可开新连接的 Initial」才值得开一条新连接
        QuicConnection::Configuration connectionConfiguration;
        connectionConfiguration.tlsContext           = m_tlsContext;
        connectionConfiguration.statelessResetSecret = m_statelessResetSecret;
        connectionConfiguration.idleTimeout          = std::chrono::duration_cast<std::chrono::milliseconds>(m_configuration.idleTimeout);
        // 接上路由器就让 HTTP/3 接管：这时流里的字节是 h3 的帧，直通出口拿到的只会是看不懂的裸字节
        connectionConfiguration.onStreamData         = [this](QuicConnection &connection, const std::int64_t streamId,
                                                     const std::span<const std::uint8_t> data, const bool isEndStream)
        {
            if (m_router != nullptr)
            {
                http3SessionFor(connection).onStreamData(streamId, data, isEndStream);
                return;
            }
            if (m_streamDataHandler)
            {
                m_streamDataHandler(connection, streamId, data, isEndStream);
            }
        };
        connectionConfiguration.onConnectionIdIssued  = [this](QuicConnection &connection, const std::span<const std::uint8_t> connectionId)
        {
            // 标识一签发就进路由表：对端随时可能改用它寻址本端。只靠「处理完报文再同步一遍」会漏掉
            // 定时器驱动的 flush 里签发的那些，对端随后用它们发的报文会被整包丢掉
            m_connectionsByAliasConnectionId.insert_or_assign(
                    std::string(reinterpret_cast<const char *>(connectionId.data()), connectionId.size()), &connection);
        };
        connectionConfiguration.sendDatagram         = [this](const Platform::SocketAddress &targetAddress, const std::uint8_t *data,
                                                          const std::size_t length) -> Core::Task<bool>
        {
            // 出口只认「发出去多少字节」：整条发出为 true，出错（对端不可达、套接字已关）为 false
            const ssize_t sentLength = co_await m_socket->asyncSendTo(targetAddress, data, length);
            co_return sentLength == static_cast<ssize_t>(length);
        };

        std::unique_ptr<QuicConnection> connection =
                QuicConnection::accept(connectionConfiguration, m_localSocketAddress, peerAddress, datagram);
        if (connection == nullptr)
        {
            co_return;
        }

        const std::string sourceConnectionId = connection->sourceConnectionId();
        QuicConnection  *rawConnection       = connection.get();
        m_connections.emplace(sourceConnectionId, std::move(connection));
        // 同时按「客户端最初选的 DCID」登记一份：重传的 Initial 靠这一路认回同一条连接。
        // 这里存裸指针是因为连接的所有权仍在上面那张表里，本表只是别名查找索引
        m_connectionsByAliasConnectionId.emplace(destinationConnectionId, rawConnection);
        co_await rawConnection->handleDatagram(peerAddress, datagram);
        registerConnectionIds(*rawConnection);
        co_await pumpHttp3For(*rawConnection);
    }

    void QuicServer::registerConnectionIds(const QuicConnection &connection)
    {
        for (const std::string &connectionId: connection.sourceConnectionIds())
        {
            m_connectionsByAliasConnectionId.insert_or_assign(connectionId, const_cast<QuicConnection *>(&connection));
        }
    }

    void QuicServer::reapClosedConnections()
    {
        for (auto iterator = m_connections.begin(); iterator != m_connections.end();)
        {
            if (iterator->second->isClosed())
            {
                LOG_DEBUG_FMT("QuicServer: 连接已收口并从路由表摘除（剩 {} 条）", m_connections.size() - 1);
                // 三张表都要摘：别名索引存的是裸指针，HTTP/3 会话内部又指回这条连接——
                // 漏掉任何一处，都会把已销毁的连接留在表里（悬空指针）
                const QuicConnection *closedConnection = iterator->second.get();
                m_http3Sessions.erase(closedConnection);
                std::erase_if(m_connectionsByAliasConnectionId,
                              [closedConnection](const auto &entry) { return entry.second == closedConnection; });
                iterator = m_connections.erase(iterator);
                continue;
            }
            ++iterator;
        }
    }

    Core::Task<> QuicServer::runExpiryTicker()
    {
        while (!m_isStopped.load(std::memory_order_acquire))
        {
            co_await m_expiryTicker.waitFor(m_configuration.expiryTickInterval);
            if (m_isStopped.load(std::memory_order_acquire))
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();

            // 先取一份连接标识快照再逐个查表：本循环里每一步都可能挂起（flush 撞上窗口不足、
            // handleExpiry 等回包），而挂起期间收报文路径会 reapClosedConnections() 摘掉已收口的
            // 连接——直接按迭代器+引用遍历的话，恢复后手里的引用与隐藏迭代器都悬垂
            std::vector<std::string> connectionKeys;
            connectionKeys.reserve(m_connections.size());
            for (const auto &connectionEntry: m_connections)
            {
                connectionKeys.push_back(connectionEntry.first);
            }

            for (const std::string &connectionKey: connectionKeys)
            {
                const auto connectionEntry = m_connections.find(connectionKey);
                if (connectionEntry == m_connections.end())
                {
                    continue; // 已经收口摘掉了
                }
                // 业务协程可能在收报文路径之外写下响应（比如先 await 了一个定时器）：那时没人替它
                // flush，响应会一直躺在待发队列里。这里顺手补一刀，免得它等某个 ngtcp2 定时器
                if (connectionEntry->second->needsFlush())
                {
                    co_await connectionEntry->second->flush();
                }
                // 上面这次挂起期间它可能已经被摘掉：用之前重新查一次表
                const auto recheckedEntry = m_connections.find(connectionKey);
                if (recheckedEntry == m_connections.end())
                {
                    continue;
                }
                if (recheckedEntry->second->nextExpiry() <= now)
                {
                    co_await recheckedEntry->second->handleExpiry();
                }
            }
            reapClosedConnections();
        }
        co_return;
    }
} // namespace AsynGyanis::Net
