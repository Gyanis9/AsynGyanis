#include "Net/Quic/QuicServer.h"
#include "Net/Quic/QuicOpenSslError.h"

#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/TimerQueue.h"
#include "Core/Tls/SessionTicketKeyRing.h"
#include "Core/Tls/TlsPolicy.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"
#include "Net/Http/Router.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <memory>
#include <string>

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
         * @brief 取来源地址的纯 IP 文本（不含端口），作为单来源限额的键
         * @details 带端口就等于按连接计数，限额永远碰不到（与 TcpServer 侧同一口径）
         * @param address 报文来源地址
         * @return std::optional<std::string> 可识别地址族的 IP 文本；地址族不认识时为空
         */
        [[nodiscard]] std::optional<std::string> peerIpKey(const Platform::SocketAddress &address)
        {
            switch (address.storage.ss_family)
            {
                case AF_INET:
                    return Core::InetAddress(*reinterpret_cast<const sockaddr_in *>(&address.storage)).ip();
                case AF_INET6:
                    return Core::InetAddress(*reinterpret_cast<const sockaddr_in6 *>(&address.storage)).ip();
                default:
                    // 地址族认不出来就不给键：硬编一个「未知」出来会把互不相干的来源并成一个名额，
                    // 比不按来源限制更糟
                    return std::nullopt;
            }
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

        /**
         * @brief 把 TLS 策略落到 QUIC 服务端这份 SSL_CTX 上，并把版本钉在 TLS 1.3
         * @details 先施加策略再钉下限，顺序反过来会得到一份「策略说了话而没人听」的配置：策略里的
         *          安全等级、1.3 套件、命名曲线都还在，唯独版本被后面那一句覆盖掉。下限低于 1.3 时
         *          钉回 1.3 只会更严，不算对配置说谎；把**上限**压到 1.3 以下则是 QUIC 满足不了的
         *          要求，这里当场拒掉而不是悄悄改回去——配置说的是「我最多说到 1.2」，实际却在 1.3 上
         *          服务，两边说法不一致，比启动失败难查得多。
         * @param context 目标 SSL_CTX，所有权不归本函数
         * @param policy 待施加的策略
         * @throws Base::SystemException 策略与 QUIC 的版本要求冲突，或 OpenSSL 拒绝了其中某一项
         */
        void applyTlsPolicyToQuicContext(SSL_CTX *const context, const Core::TlsPolicy &policy)
        {
            if (policy.maximumProtocolVersion == Core::TlsPolicy::ProtocolVersion::Tls1_2)
            {
                throw Base::SystemException(
                    "QUIC 服务端启动失败：TLS 策略把最高版本限定在 TLS 1.2，而 QUIC 只跑 TLS 1.3（RFC 9001）");
            }
            // 内置套件串传空：1.2 及以下的列表对一条只跑 1.3 的通路没有意义，
            // 套上去只会留下一份永远不会被用到的偏好
            Core::applyTlsPolicy(context, policy, nullptr);
            if (SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1)
            {
                throw Base::SystemException("QUIC 服务端启动失败：无法把 TLS 最低版本限到 1.3（" + quicOpenSslErrorText() + "）");
            }
        }
    } // namespace

    QuicServer::QuicServer(Core::EventLoop &eventLoop, Configuration configuration) :
        m_eventLoop(eventLoop), m_configuration(std::move(configuration)), m_expiryTicker(eventLoop)
    {
        // 构造期任一检查不过都要抛，而抛出去之后析构函数不会跑——成员那份裸指针就此无人认领。
        // 所以先让局部守卫持有，只有全部检查过了才交接给成员（一份 SSL_CTX 连带证书与私钥约 35 KiB）
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ownedTlsContext(SSL_CTX_new(TLS_server_method()), &SSL_CTX_free);
        if (ownedTlsContext == nullptr)
        {
            throw Base::SystemException("QUIC 服务端启动失败：TLS 上下文创建失败（" + quicOpenSslErrorText() + "）");
        }
        SSL_CTX *const tlsContext = ownedTlsContext.get();

        // TLS 策略与「QUIC 只用 TLS 1.3」这一条钉在一起施加：低版本没有 QUIC 需要的握手接口
        applyTlsPolicyToQuicContext(tlsContext, m_configuration.tlsPolicy);
        if (SSL_CTX_use_certificate_chain_file(tlsContext, m_configuration.certificateFile.c_str()) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：证书加载失败（" + m_configuration.certificateFile + "）：" + quicOpenSslErrorText());
        }
        if (SSL_CTX_use_PrivateKey_file(tlsContext, m_configuration.privateKeyFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：私钥加载失败（" + m_configuration.privateKeyFile + "）：" + quicOpenSslErrorText());
        }
        if (SSL_CTX_check_private_key(tlsContext) != 1)
        {
            throw Base::SystemException("QUIC 服务端启动失败：私钥与证书不匹配：" + quicOpenSslErrorText());
        }
        SSL_CTX_set_alpn_select_cb(tlsContext, selectApplicationProtocol, nullptr);

        // 票据密钥与证书一样在构造期就位：装不上就是配置错误，当场抛（消息点名是哪一份文件），
        // 而不是悄悄退回「每个上下文一份随机密钥」——那种形态的代价只在恢复命中率上体现，查起来最费时间
        if (!m_configuration.sessionTicketKeyFiles.empty())
        {
            std::vector<std::string> ticketKeys;
            Core::SessionTicketKeyRing::readKeyFiles(m_configuration.sessionTicketKeyFiles, ticketKeys);
            Core::SessionTicketKeyRing::install(tlsContext, std::move(ticketKeys));
        }
        // 到这里才算构造成功：所有权交给成员，由析构函数释放，守卫不再重复 free
        m_tlsContext = ownedTlsContext.release();
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
        const std::uint16_t boundPort = ntohs(boundAddressV4.sin_port);

        // 建连接时要拿本端地址写进回包，必须用**绑定后**的地址（端口给 0 时只有内核知道
        // 实际端口）。漏掉这一步日志与诊断里看到的就是一条全零地址
        m_localSocketAddress = boundAddress;

        m_socket = std::make_unique<Core::AsyncUdpSocket>(m_eventLoop, std::move(m_datagramSocket));

        // 端口最后发布：非 0 值就是「已经在监听」的唯一凭据，外部线程靠轮询它确认启动结果，
        // 因此读到非 0 时必须连带看到上面两项都已就位（release 与读侧 acquire 配对）
        m_listeningPort.store(boundPort, std::memory_order_release);
        LOG_INFO_FMT("QuicServer: 已在 UDP 端口 {} 上监听（ALPN {}）", boundPort, m_configuration.applicationProtocol);

        // 两个循环并发跑：收报文的与驱动定时器的。定时器不能只挂在收报文上，否则空闲期（对端在等超时）
        // 就没人推进 PTO/空闲超时
        Core::Task<> expiryTask = runExpiryTicker();
        m_eventLoop.scheduler().schedule(expiryTask.handle());

        std::vector<std::uint8_t> receiveBuffer(Platform::DatagramSocket::kMaximumDatagramBytes);
        while (!m_isStopped.load(std::memory_order_acquire))
        {
            // 结果按值回来（惰性协程不往调用方的引用里写：实参可能比 await 先亡）
            const Core::AsyncUdpSocket::DatagramReceiveResult received =
                    co_await m_socket->asyncReceiveFrom(receiveBuffer.data(), receiveBuffer.size());
            const ssize_t                  receivedLength = received.receivedByteCount;
            const Platform::SocketAddress &peerAddress    = received.peerAddress;
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

    Core::Task<> QuicServer::closeAllOpenConnections()
    {
        // 逐条带原因收口再统一摘除：closeNow() 会把 CONNECTION_CLOSE 刷出去（对端因此立刻知道
        // 连接没了，而不是等自己的空闲超时），真正的路由表清理靠 reapClosedConnections()
        // （它会跳过还有协程持有的连接）
        // 先取一份标识快照：每次 co_await 都可能挂起，而挂起期间收报文路径会摘掉已收口的连接
        std::vector<std::string> connectionKeys;
        connectionKeys.reserve(m_connections.size());
        for (const auto &connectionEntry: m_connections)
        {
            connectionKeys.push_back(connectionEntry.first);
        }

        for (const std::string &connectionKey: connectionKeys)
        {
            const auto connectionEntry = m_connections.find(connectionKey);
            if (connectionEntry == m_connections.end() || connectionEntry->second->isClosed())
            {
                continue; // 已经收口摘掉了
            }
            // 挂起期间它可能正被别的路径摘除：守卫让那次摘除推迟到本次收口结束
            const QuicConnection::ActivityGuard activityGuard(*connectionEntry->second);
            co_await connectionEntry->second->closeNow(0, "服务端正在收口");
        }
        reapClosedConnections();
    }

    Core::Task<> QuicServer::drain(const std::chrono::milliseconds drainTimeout)
    {
        // 第一步只挡新连接：不能直接 stop()——收报文那条循环会随之退出，在途请求的后续报文与
        // 对端的 ACK 就再也进不来，「等它做完」也就无从谈起
        m_isRefusingNewConnections.store(true, std::memory_order_release);

        try
        {
            // 给每条已有连接一次「告诉对端」的机会：h3 的收尾通告是控制流上的 GOAWAY。
            // 通告排进待发队列后立刻刷出去——关停路径上这是还能写字节的时刻
            for (auto &connectionEntry: m_connections)
            {
                QuicConnection &connection = *connectionEntry.second;
                if (connection.isClosed())
                {
                    continue;
                }
                if (Http3Session *const session = findHttp3Session(&connection); session != nullptr)
                {
                    static_cast<void>(session->beginGracefulShutdown());
                }
                // 挂起期间定时循环可能把这条连接判成收口并试图摘掉：守卫让那次摘除推迟到本迭代结束
                const QuicConnection::ActivityGuard activityGuard(connection);
                co_await connection.flush();
            }

            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + drainTimeout;
            while (drainTimeout > std::chrono::milliseconds::zero())
            {
                // 先取一份连接标识快照再逐个查表：下面那次 await 期间收报文路径可能摘掉它们
                std::vector<std::string> openConnectionKeys;
                openConnectionKeys.reserve(m_connections.size());
                for (const auto &connectionEntry: m_connections)
                {
                    if (!connectionEntry.second->isClosed())
                    {
                        openConnectionKeys.push_back(connectionEntry.first);
                    }
                }

                std::size_t busyConnectionCount = 0;
                for (const std::string &connectionKey: openConnectionKeys)
                {
                    const auto connectionEntry = m_connections.find(connectionKey);
                    if (connectionEntry == m_connections.end())
                    {
                        continue; // 已经收口摘掉了
                    }
                    if (const Http3Session *session = findHttp3Session(connectionEntry->second.get());
                        session != nullptr && session->hasOutstandingWork())
                    {
                        ++busyConnectionCount;
                    }
                }
                // 没有在途工作就是排空完成，不必把等待额度耗满
                if (busyConnectionCount == 0)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    LOG_INFO_FMT("QuicServer: 优雅收口等待超时，剩余 {} 条仍有在途工作的连接被强制关闭。等待时长 {}ms",
                                 busyConnectionCount, drainTimeout.count());
                    break;
                }
                // 等待期间的驱动不用本协程操心：定时循环只看 m_isStopped，它会继续刷包、
                // 继续按到期推进重传，业务协程也在同一个循环上被唤醒
                co_await m_expiryTicker.waitFor(kDrainPollInterval);
            }
        } catch (const std::exception &drainException)
        {
            // 本协程由调度器独立恢复：异常逃出去等于在事件循环线程上抛，会带走整个进程
            LOG_ERROR_EXCEPTION(drainException, "QuicServer: 优雅收口过程失败，已放弃等待并强制关闭剩余连接。原因：{}", drainException.what());
        } catch (...)
        {
            LOG_ERROR_FMT("QuicServer: 优雅收口过程失败，已放弃等待并强制关闭剩余连接。原因：非标准库异常");
        }

        // 三条出口（排空完成 / 到期 / 出错）的后置条件一样：本服务器不再留任何连接给调用方收尾
        stop();
        co_await closeAllOpenConnections();
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

        // 会话的四个口子都绑到这条连接上：开单向流、写流数据、归还接收额度、收口单条流。
        // 这里捕获裸指针而不是引用，是为了让「会话指向哪条连接」在代码里显式可见
        QuicConnection *const rawConnection = &connection;
        auto                  session       = std::make_unique<Http3Session>(
                [rawConnection] { return rawConnection->openUnidirectionalStream(); },
                [rawConnection](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                { return rawConnection->queueStreamData(streamId, data, isEndStream); },
                [rawConnection](const std::int64_t streamId, const std::size_t consumedByteCount)
                { rawConnection->extendReceiveWindow(streamId, consumedByteCount); },
                m_configuration.metricsCollector, m_configuration.memoryBudget, m_configuration.requestIdGenerator,
                [rawConnection](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                { rawConnection->abortStream(streamId, applicationErrorCode); });
        if (m_router != nullptr)
        {
            session->attachRouter(*m_router);
        }
        // 解析上限必须显式交给会话：默认构造的上限虽然安全，但调用方在 Configuration 里
        // 调过的值（例如放宽正文上限）必须真的生效，否则「配置了却不生效」更难排查
        session->setParserLimits(m_configuration.parserLimits);
        // 连接级限额同样要交下去：单连接请求条数到量后排空，靠的就是这一份
        session->setServerLimits(m_configuration.serverLimits);

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
        // 准入闸门：限额器可以与两条 TCP 通道共用同一份，因此这里报的同样是那道闸门的总量，
        // 而不是「h3 这一侧挡了多少」——采集端合并出来的数与 h1/h2 侧同源，不会重复计数
        snapshot.admissionRejectedConnectionCount =
            m_configuration.perIpConnectionLimiter == nullptr
                ? 0U
                : m_configuration.perIpConnectionLimiter->rejectedConnectionCount();
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
                co_await connection.closeNow(0, "HTTP/3 会话不可用");
                co_return;
            }
            co_await session->pump();
            // 业务刚写下的响应此刻才排进连接的待发队列，必须再刷一次才会出网：
            // handleDatagram 里那次 flush 发生在业务之前（SETTINGS 那批因此出得去，
            // 而响应留在队列里等下一次定时器把它想起来——实测客户端就是干等超时）
            co_await connection.flush();
            // 已排空且手上没活：这条连接的使命结束了（对端早收到过 GOAWAY，不会再往它上面发新请求）。
            // 单连接请求条数到量、或调用方主动 drain 过一条连接，都从这里收口
            if (session->isDrainedAndFinished())
            {
                co_await connection.closeNow(0, "已排空且无在途请求");
            }
        }
        co_return;
    }

    std::size_t QuicServer::connectionCount() const noexcept
    {
        return m_connections.size();
    }

    std::uint16_t QuicServer::listeningPort() const noexcept
    {
        // acquire 与写侧的 release 配对：读到非 0 就能确信本端地址与套接字封装都已就位
        return m_listeningPort.load(std::memory_order_acquire);
    }

    Core::Task<> QuicServer::routeDatagram(const Platform::SocketAddress peerAddress, const std::span<const std::uint8_t> datagram)
    {
        if (m_isStopped.load(std::memory_order_acquire))
        {
            co_return;
        }

        // 路由键是报文里的目的连接标识；解不出来（太短、版本协商报文等）就丢掉。最后一个参数是
        // **短头报文的 DCID 长度**：短头不带这个长度字段，必须告诉解码器本端自己的连接标识有多长，
        // 否则它会把包号的头几字节也算进标识，从 1-RTT 起每条报文都命不中路由表
        const std::expected<QuicPacketHeader, QuicDecodeError> decodedHeader =
                decodeQuicPacketHeader(datagram, QuicConnection::kSourceConnectionIdLength);
        if (!decodedHeader.has_value())
        {
            co_return;
        }
        // 用视图查表：两张表都是透明比较（std::less<>），而本端连接标识固定 18 字节、超过
        // 小字符串优化阈值——每个入向报文都为它分配一次 std::string 是白付的拷贝
        const std::string_view destinationConnectionId(reinterpret_cast<const char *>(decodedHeader->destinationConnectionId.data()),
                                                       decodedHeader->destinationConnectionId.size());
        if (const auto existing = m_connections.find(destinationConnectionId); existing != m_connections.end())
        {
            // 记账：下面几次 await 都可能在挂起中被定时循环判成「已收口」并试图摘掉它——守卫
            // 让那次摘除推迟（见 reapClosedConnections）。迭代器与本引用因此在整个区间内有效
            const QuicConnection::ActivityGuard activityGuard(*existing->second);
            co_await existing->second->handleDatagram(peerAddress, datagram);
            co_await pumpHttp3For(*existing->second);
            co_return;
        }

        // 别名索引：客户端重传 Initial 时，报文里的 DCID 仍是它最初选的那个（RFC 9000 §7.2 规定
        // 首包的目的标识固定到服务端回话为止），而按本端标识建的键这时对不上——少了这一路，每条
        // 重传都会被当成新连接（实测：一个客户端握手却建出 8 条连接，握手因此永远收不了口）
        if (const auto byAliasConnectionId = m_connectionsByAliasConnectionId.find(destinationConnectionId);
            byAliasConnectionId != m_connectionsByAliasConnectionId.end())
        {
            QuicConnection *const matchedConnection = byAliasConnectionId->second;
            // 与上面同一条记账：别名表里存的是裸指针，摘除会把这条一起抹掉
            const QuicConnection::ActivityGuard activityGuard(*matchedConnection);
            co_await matchedConnection->handleDatagram(peerAddress, datagram);
            co_await pumpHttp3For(*matchedConnection);
            co_return;
        }

        if (m_connections.size() >= m_configuration.maximumConnections)
        {
            LOG_WARN_FMT("QuicServer: 在线连接已达上限 {}，新连接被拒绝", m_configuration.maximumConnections);
            co_return;
        }

        // 排空期间不再接手新连接：对端会按 GOAWAY 或握手失败另找一台。这里只丢弃，
        // 不回 CONNECTION_CLOSE——本端还不认识这条连接，回什么都得先造一套密钥
        if (m_isRefusingNewConnections.load(std::memory_order_acquire))
        {
            co_return;
        }

        // 不认识的目的连接标识：只有「可开新连接的 Initial」才值得开一条新连接
        // 先占单来源名额再建连接：名额拿不到就不建，避免「建了又拆」白付一次握手成本
        std::optional<PerIpConnectionLimiter::Lease> perIpLease;
        if (m_configuration.perIpConnectionLimiter != nullptr)
        {
            if (const std::optional<std::string> ipKey = peerIpKey(peerAddress); ipKey.has_value())
            {
                perIpLease = m_configuration.perIpConnectionLimiter->tryAcquire(*ipKey);
                if (!perIpLease.has_value())
                {
                    LOG_WARN_FMT("QuicServer: 来源 {} 的并发连接已达上限，新的 Initial 被拒绝", *ipKey);
                    co_return;
                }
            }
        }

        QuicConnection::Configuration connectionConfiguration;
        connectionConfiguration.tlsContext           = m_tlsContext;
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
        // 对端取消了一条请求流（RESET_STREAM / STOP_SENDING）或传输层把它收尾了：h3 会话据此回收
        // 该流的状态。没有这一路的话，被取消的请求正文、流式等待者与隧道记录会一直留着
        connectionConfiguration.onPeerStreamClosed   = [this](QuicConnection &connection, const std::int64_t streamId)
        {
            // 只查表不现建会话：连 h3 一个字节都没跑过的连接没有请求状态可回收，此刻新建会话
            // 反而会替一条正在收尾的连接开出三条单向流
            if (Http3Session *const session = findHttp3Session(&connection); session != nullptr)
            {
                session->cancelStreamByPeer(streamId);
            }
        };
        // 待发队列被排空一些就催一次续交：h3 连接层留着的那段字节等的是「地方」而不是「对端发数据」，
        // 而只回窗口更新的连接不会触发任何接收回调
        connectionConfiguration.onSendSpaceAvailable = [this](QuicConnection &connection)
        {
            if (Http3Session *const session = findHttp3Session(&connection); session != nullptr)
            {
                session->flushPendingStreamData();
            }
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
        // 名额凭据与连接同寿命：摘连接时必须一起摘，否则那个来源的计数只增不减（等价于把
        // 限额变成了一次性配额）
        if (perIpLease.has_value())
        {
            m_perIpConnectionLeases.emplace(sourceConnectionId, std::move(*perIpLease));
        }
        // 同时按「客户端最初选的 DCID」登记一份：重传的 Initial 靠这一路认回同一条连接。
        // 这里存裸指针是因为连接的所有权仍在上面那张表里，本表只是别名查找索引
        m_connectionsByAliasConnectionId.emplace(destinationConnectionId, rawConnection);
        // 新连接同样要记账：它随时可能在下面几次 await 里被判成收口（会话层判定不可用、对端
        // 立刻发来 CONNECTION_CLOSE 等），而收报文路径收尾与清扫节拍都会尝试摘除它
        const QuicConnection::ActivityGuard activityGuard(*rawConnection);
        co_await rawConnection->handleDatagram(peerAddress, datagram);
        co_await pumpHttp3For(*rawConnection);
    }

    void QuicServer::reapClosedConnections()
    {
        for (auto iterator = m_connections.begin(); iterator != m_connections.end();)
        {
            if (iterator->second->isClosed())
            {
                if (iterator->second->hasActivity())
                {
                    LOG_DEBUG_FMT("QuicServer: 连接已收口但仍有在途动作，推迟摘除");
                    ++iterator;
                    continue;
                }
                // HTTP/3 会话：传输层收口不逐条流发信号，挂在正文或隧道上的业务协程只能由这里叫醒。
                // 摘掉会话等于把它们的协程帧一起销毁（等待之后的收尾代码就不再执行），所以先把账
                // 清下去，再等它们跑完——本函数每拍重来一次，直到这条会话没有在途工作
                if (Http3Session *const session = findHttp3Session(iterator->second.get()); session != nullptr)
                {
                    session->abandonPendingStreams();
                    if (session->hasOutstandingWork())
                    {
                        LOG_DEBUG_FMT("QuicServer: 连接已收口但 h3 业务协程还没跑完，推迟摘除");
                        ++iterator;
                        continue;
                    }
                }
                // 还有协程拿着它（收报文路径或定时循环正停在它的某个 co_await 上）：此刻销毁，
                // 那条协程恢复后手里的引用与迭代器就是悬垂的。推迟到它的在途动作结束——下一次
                // 收报文或下一次清扫节拍会回到这里（两条路径都在收尾处调本函数）
                LOG_DEBUG_FMT("QuicServer: 连接已收口并从路由表摘除（剩 {} 条）", m_connections.size() - 1);
                // 四张表都要摘：别名索引存的是裸指针，HTTP/3 会话内部又指回这条连接，单来源名额
                // 则要随连接归还——漏掉任何一处，都会把已销毁的连接留在表里（悬空指针）或让计数只增不减
                const QuicConnection *closedConnection = iterator->second.get();
                m_http3Sessions.erase(closedConnection);
                // 第四张表：单来源名额随连接一起归还（凭据析构即释放计数）
                m_perIpConnectionLeases.erase(iterator->first);
                std::erase_if(m_connectionsByAliasConnectionId,
                              [closedConnection](const auto &entry) { return entry.second == closedConnection; });
                iterator = m_connections.erase(iterator);
                continue;
            }
            ++iterator;
        }
    }

    std::chrono::steady_clock::time_point QuicServer::nextTickerWakePoint(const bool hasConnections,
                                                                         const std::chrono::steady_clock::time_point earliestExpiry,
                                                                         const std::chrono::steady_clock::time_point now,
                                                                         const std::chrono::milliseconds tickInterval)
    {
        // 一台没有在线连接的监听器没有任何到期要落实：这时按节拍轮询是纯白烧（实测空闲每秒 100 次唤醒，
        // 每次约两趟 epoll_wait）。退到空闲上界，新连接的头一拍最多延后这一档
        if (!hasConnections)
        {
            return now + kIdleTickerSleep;
        }
        // 节拍上限仍要留着：needsFlush 那一档补刀与 h3 请求的读时限都没有可查的截止时刻，只能靠轮询兜。
        // 节拍配成非正数时按「不设上限」处理，此时只剩各连接自己的截止时刻（armedDurationFor 会把
        // 已经过掉的时刻折成 1 毫秒，不会真的睡过去）
        if (tickInterval <= std::chrono::milliseconds::zero())
        {
            return earliestExpiry;
        }
        const std::chrono::steady_clock::time_point tickDeadline = now + tickInterval;
        if (earliestExpiry >= tickDeadline)
        {
            return tickDeadline;
        }
        // 早于节拍的截止按时到；已到期的（earliestExpiry <= now）留 now，由换算函数折成 1 毫秒的最近唤醒
        return earliestExpiry > now ? earliestExpiry : now;
    }

    std::chrono::steady_clock::time_point QuicServer::earliestConnectionExpiry() const
    {
        auto earliest = std::chrono::steady_clock::time_point::max();
        for (const auto &connectionEntry: m_connections)
        {
            if (const auto expiry = connectionEntry.second->nextExpiry(); expiry < earliest)
            {
                earliest = expiry;
            }
        }
        return earliest;
    }

    Core::Task<> QuicServer::runExpiryTicker()
    {
        while (!m_isStopped.load(std::memory_order_acquire))
        {
            // 先按当前状态算出这一觉睡到什么时候，再挂上去：唤醒点要么是所有连接里最早的交易截止，
            // 要么是配置的节拍（先到为准），零连接时退到空闲上界
            const std::chrono::steady_clock::time_point planningNow = std::chrono::steady_clock::now();
            const std::chrono::milliseconds sleepDuration = Core::detail::armedDurationFor(
                    nextTickerWakePoint(!m_connections.empty(), earliestConnectionExpiry(), planningNow,
                                        m_configuration.expiryTickInterval),
                    planningNow);
            co_await m_expiryTicker.waitFor(sleepDuration);
            if (m_isStopped.load(std::memory_order_acquire))
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();

            // 直接遍历连接表，不做标识快照：本循环里每一次挂起都被下面那枚守卫罩着，而摘除已收口
            // 的连接只有 reapClosedConnections() 一处、它见到守卫就跳过——这条因此不会在挂起期间被
            // 销毁；std::map 里别处摘除条目也不影响手里的迭代器。省下的是每拍一份快照：每条连接
            // 一次标识字符串分配，加一次按串的查表（空闲服务器上这项与流量无关，只随在线数走）
            for (const auto &connectionEntry: m_connections)
            {
                // 记账：下面几次 await 都可能挂起（flush 撞上发送缓冲满、handleExpiry 等回包），
                // 守卫把「本条连接的摘除」推迟到这次迭代结束
                const QuicConnection::ActivityGuard activityGuard(*connectionEntry.second);
                // 先按时限收口「收不全」的请求：h3 会话没有套接字可等，读时限只能靠这一拍落实。
                // 排在 flush 之前，被叫醒的处理器若因此产出了什么，这一拍就一起送出去
                if (Http3Session *const session = findHttp3Session(connectionEntry.second.get()); session != nullptr)
                {
                    session->expireStaleRequests(now);
                }
                // 业务协程可能在收报文路径之外写下响应（比如先 await 了一个定时器）：那时没人替它
                // flush，响应会一直躺在待发队列里。这里顺手补一刀，免得它一直等到下一次报文或定时器
                if (connectionEntry.second->needsFlush())
                {
                    co_await connectionEntry.second->flush();
                }
                if (connectionEntry.second->nextExpiry() <= now)
                {
                    co_await connectionEntry.second->handleExpiry();
                }
            }
            reapClosedConnections();
        }
        co_return;
    }
} // namespace AsynGyanis::Net
