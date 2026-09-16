/**
 * @file TestQuicServer.cpp
 * @brief QUIC 服务端的回环端到端用例：真 UDP 套接字、真证书、真握手
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 客户端的 QUIC 实现由测试自己用 ngtcp2 搭（与「用独立实现做验收」的精神一致：被测的是
 *          服务端这一侧，客户端只当对端）。握手与流数据都走真实回环 UDP，因此套接字封装、
 *          连接标识路由、定时器驱动、证书与 ALPN 协商全都在链路上。
 */

#include "Net/Quic/QuicServer.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/Socket.h"

#include <gtest/gtest.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 各用例共用的等待上限
        constexpr std::chrono::milliseconds kWaitTimeout{8000};

        /// 客户端单次收发的缓冲上限
        constexpr std::size_t kMaximumClientPacketBytes = 1500;

        /// 客户端提的 ALPN：必须与服务端配置一致才会被接受
        constexpr unsigned char kHttp3Alpn[] = {2, 'h', '3'};

        /// 服务器证书与私钥（与 TLS 用例共用同一份夹具）
        std::string certificatePath()
        {
            return std::string(TEST_FIXTURES_DIR) + "/test_cert.pem";
        }

        std::string privateKeyPath()
        {
            return std::string(TEST_FIXTURES_DIR) + "/test_key.pem";
        }

        /// ngtcp2 的时间戳单位是纳秒，用单调时钟（与 QuicConnection 的取值口径一致）
        ngtcp2_tstamp currentTimestamp()
        {
            return static_cast<ngtcp2_tstamp>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /**
         * @brief 回环上的 QUIC 客户端：一条 ngtcp2 客户端连接 + 一条真 UDP 套接字
         */
        class QuicTestClient
        {
        public:
            /// 服务端把流数据交给回调时的观测记录
            struct StreamObservation
            {
                std::int64_t             streamId{0};
                std::vector<std::uint8_t> payload;
                bool                      isEndStream{false};
                bool                      hasReceived{false};
            };

            ~QuicTestClient()
            {
                if (m_connection != nullptr)
                {
                    ngtcp2_conn_del(m_connection);
                }
                if (m_osslContext != nullptr)
                {
                    ngtcp2_crypto_ossl_ctx_del(m_osslContext);
                }
                if (m_ssl != nullptr)
                {
                    SSL_free(m_ssl);
                }
                // 上下文也要放：它是 initialize() 里新建的，漏了每跑一条用例就漏一份
                // （Windows 的 ASan 不带泄漏检测，这条是 Linux 门禁第一次跑这些用例时抓出来的）
                if (m_sslContext != nullptr)
                {
                    SSL_CTX_free(m_sslContext);
                    m_sslContext = nullptr;
                }
            }

            QuicTestClient(const QuicTestClient &) = delete;

            QuicTestClient &operator=(const QuicTestClient &) = delete;

            QuicTestClient() = default;

            /**
             * @brief 起一条真正的回环 UDP 套接字并与服务端地址建立 ngtcp2 客户端连接
             * @param serverAddress 服务端 UDP 地址
             * @param applicationProtocols 本端要提的 ALPN 列表（线格式）；默认提 h3
             * @return true 初始化完成（此时握手尚未开始，要靠 pumpOnce() 推进）
             */
            bool initialize(const Platform::SocketAddress &serverAddress, const std::span<const unsigned char> applicationProtocols = kHttp3Alpn)
            {
                m_serverAddress = serverAddress;
                m_socket        = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
                if (!m_socket.isValid())
                {
                    return false;
                }

                m_sslContext = SSL_CTX_new(TLS_client_method());
                if (m_sslContext == nullptr)
                {
                    return false;
                }
                // 只验「握手能否完成与数据是否送达」；证书校验在服务端用例里不参与本次断言
                SSL_CTX_set_verify(m_sslContext, SSL_VERIFY_NONE, nullptr);

                m_ssl = SSL_new(m_sslContext);
                if (m_ssl == nullptr)
                {
                    return false;
                }
                if (ngtcp2_crypto_ossl_configure_client_session(m_ssl) != 0)
                {
                    return false;
                }
                SSL_set_connect_state(m_ssl);
                if (SSL_set_alpn_protos(m_ssl, applicationProtocols.data(), static_cast<unsigned int>(applicationProtocols.size())) != 0)
                {
                    return false;
                }

                ngtcp2_settings        settings{};
                ngtcp2_transport_params parameters{};
                ngtcp2_settings_default(&settings);
                ngtcp2_transport_params_default(&parameters);
                settings.initial_ts = currentTimestamp();

                fillRandomConnectionId(m_destinationConnectionId);
                fillRandomConnectionId(m_sourceConnectionId);

                // path 的地址必须活到连接销毁：存成本对象的成员，这里只取地址
                m_localStorage  = m_socket.localAddress();
                m_remoteStorage = serverAddress;
                m_path          = makePath(m_localStorage, m_remoteStorage);

                const ngtcp2_callbacks callbacks = makeClientCallbacks();
                const int createResult = ngtcp2_conn_client_new(&m_connection, &m_destinationConnectionId, &m_sourceConnectionId, &m_path,
                                                                NGTCP2_PROTO_VER_V1, &callbacks, &settings, &parameters, nullptr, this);
                if (createResult != 0)
                {
                    return false;
                }

                if (ngtcp2_crypto_ossl_ctx_new(&m_osslContext, nullptr) != 0)
                {
                    return false;
                }
                ngtcp2_crypto_ossl_ctx_set_ssl(m_osslContext, m_ssl);
                ngtcp2_conn_set_tls_native_handle(m_connection, m_osslContext);

                // app data 必须是 conn_ref：ngtcp2 的 ossl 胶水会把它当函数指针调用取回连接
                m_connectionReference.user_data = this;
                m_connectionReference.get_conn  = [](ngtcp2_crypto_conn_ref *reference) -> ngtcp2_conn *
                {
                    return static_cast<QuicTestClient *>(reference->user_data)->m_connection;
                };
                SSL_set_app_data(m_ssl, &m_connectionReference);
                return true;
            }

            /**
             * @brief 推进一步：把待发字节发出去、把收到的报文喂回 ngtcp2、处理到期
             */
            void pumpOnce()
            {
                const ngtcp2_tstamp now = currentTimestamp();

                // 待发缓冲在整轮写循环里复用同一份：ngtcp2 要求同一轮里 dest 与 ts 保持一致
                std::array<std::uint8_t, kMaximumClientPacketBytes> packet{};
                while (true)
                {
                    // 带流数据时给出流号与数据；没有待发字节时必须用流号 -1 写控制帧，
                    // 否则带上流号而数据为空会构造出一个空 STREAM 帧
                    const bool        hasStreamData  = m_pendingStreamVector.len > 0;
                    const std::int64_t streamId      = hasStreamData ? m_pendingStreamId : -1;
                    const ngtcp2_vec *dataVectors    = hasStreamData ? &m_pendingStreamVector : nullptr;
                    const std::size_t dataVectorCount = hasStreamData ? 1 : 0;
                    ngtcp2_ssize       writtenStreamDataLength = 0;
                    // 不带 MORE 标志：一次调用最多构成一个完整报文，省掉「同一个包分多次装」那套状态机
                    // （带 MORE 时只要真装进了流数据，ngtcp2 就返回 NGTCP2_ERR_WRITE_MORE，而 -1/负值
                    //   在下面会被当成写错误直接收手——流数据因此一字节都发不出去）
                    const ngtcp2_ssize writtenByteCount =
                            ngtcp2_conn_writev_stream(m_connection, &m_path, nullptr, packet.data(), packet.size(), &writtenStreamDataLength,
                                                      NGTCP2_WRITE_STREAM_FLAG_NONE, streamId, dataVectors, dataVectorCount, now);
                    if (writtenStreamDataLength > 0)
                    {
                        m_writtenStreamByteCount += static_cast<std::size_t>(writtenStreamDataLength);
                        // 装进 STREAM 帧的字节要从待发窗口里划掉，否则下一轮会把同一段数据重发一遍
                        m_pendingStreamVector.base += writtenStreamDataLength;
                        m_pendingStreamVector.len -= static_cast<std::size_t>(writtenStreamDataLength);
                    }
                    if (writtenByteCount < 0)
                    {
                        // -1 是「这一轮没有可发的」以外的错误；本用例里只记录，不把它当成断言失败
                        m_lastWriteError = static_cast<int>(writtenByteCount);
                        return;
                    }
                    if (writtenByteCount == 0)
                    {
                        break;
                    }
                    if (m_socket.send(m_serverAddress, packet.data(), static_cast<std::size_t>(writtenByteCount)) < 0)
                    {
                        m_lastWriteError = -1;
                        return;
                    }
                    ++m_sentDatagramCount;
                }

                while (true)
                {
                    std::array<std::uint8_t, kMaximumClientPacketBytes> receivedPacket{};
                    Platform::SocketAddress                            peerAddress;
                    const ssize_t receivedByteCount = m_socket.receive(receivedPacket.data(), receivedPacket.size(), peerAddress);
                    if (receivedByteCount <= 0)
                    {
                        break;
                    }
                    ++m_receivedDatagramCount;
                    if (ngtcp2_conn_read_pkt(m_connection, &m_path, nullptr, receivedPacket.data(),
                                             static_cast<std::size_t>(receivedByteCount), now) != 0)
                    {
                        m_hasReadError = true;
                        break;
                    }
                }

                if (ngtcp2_conn_get_expiry(m_connection) <= now)
                {
                    static_cast<void>(ngtcp2_conn_handle_expiry(m_connection, now));
                }
            }

            /// 握手是否完成
            [[nodiscard]] bool isHandshakeCompleted() const noexcept
            {
                return m_isHandshakeCompleted;
            }

            /// 最近一次写失败的错误码（0 表示没出错）
            [[nodiscard]] int lastWriteError() const noexcept
            {
                return m_lastWriteError;
            }

            /// 读入服务端报文时是否失败过
            [[nodiscard]] bool hasReadError() const noexcept
            {
                return m_hasReadError;
            }

            /// 到目前为止收到过多少条服务端报文（分「服务端没发」与「发了但客户端处理不了」用）
            [[nodiscard]] std::size_t receivedDatagramCount() const noexcept
            {
                return m_receivedDatagramCount;
            }

            /// 到目前为止写出过多少字节的流数据（分「客户端没发」与「服务端没交」用）
            [[nodiscard]] std::size_t writtenStreamByteCount() const noexcept
            {
                return m_writtenStreamByteCount;
            }

            /// 到目前为止客户端发出过多少条报文
            [[nodiscard]] std::size_t sentDatagramCount() const noexcept
            {
                return m_sentDatagramCount;
            }

            /// 服务端协商出的 ALPN（握手完成后才有值）
            [[nodiscard]] std::string selectedApplicationProtocol() const
            {
                const unsigned char *protocol     = nullptr;
                unsigned int         protocolLength = 0;
                SSL_get0_alpn_selected(m_ssl, &protocol, &protocolLength);
                return std::string(reinterpret_cast<const char *>(protocol), protocolLength);
            }

            /**
             * @brief 开一条双向流并把负载排队待发（负载必须活到发送结束，这里由成员持有）
             * @param payload 要发的字节
             * @return true 流开出来了
             */
            bool openStreamAndQueuePayload(std::span<const std::uint8_t> payload)
            {
                if (ngtcp2_conn_open_bidi_stream(m_connection, &m_pendingStreamId, nullptr) != 0)
                {
                    return false;
                }
                m_pendingStreamPayload.assign(payload.begin(), payload.end());
                m_pendingStreamVector.base = m_pendingStreamPayload.data();
                m_pendingStreamVector.len  = m_pendingStreamPayload.size();
                return true;
            }

        private:
            /// 造一个回环 IPv4 地址（本用例的套接字都用它）
            static Platform::SocketAddress makeLoopbackAddress(const std::uint16_t port)
            {
                Platform::SocketAddress address;
                sockaddr_in             addressV4{};
                addressV4.sin_family      = AF_INET;
                addressV4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addressV4.sin_port        = htons(port);
                std::memcpy(&address.storage, &addressV4, sizeof(addressV4));
                address.length = sizeof(addressV4);
                return address;
            }

            /// 由两份平台地址拼出 ngtcp2 的 path（地址本体由调用方持有）
            static ngtcp2_path makePath(const Platform::SocketAddress &localAddress, const Platform::SocketAddress &remoteAddress)
            {
                ngtcp2_path path{};
                path.local.addr     = reinterpret_cast<ngtcp2_sockaddr *>(const_cast<sockaddr_storage *>(&localAddress.storage));
                path.local.addrlen  = localAddress.length;
                path.remote.addr    = reinterpret_cast<ngtcp2_sockaddr *>(const_cast<sockaddr_storage *>(&remoteAddress.storage));
                path.remote.addrlen = remoteAddress.length;
                return path;
            }

            /// 随机连接 ID（库里没有这个工具函数）
            static void fillRandomConnectionId(ngtcp2_cid &connectionId)
            {
                if (RAND_bytes(connectionId.data, NGTCP2_MAX_CIDLEN) == 1)
                {
                    connectionId.datalen = NGTCP2_MAX_CIDLEN;
                }
            }

            /// 客户端回调表：加密与 TLS 部分一律用 ngtcp2_crypto 的现成实现
            static ngtcp2_callbacks makeClientCallbacks()
            {
                ngtcp2_callbacks callbacks{};
                callbacks.client_initial           = ngtcp2_crypto_client_initial_cb;
                callbacks.recv_crypto_data         = ngtcp2_crypto_recv_crypto_data_cb;
                callbacks.recv_retry               = ngtcp2_crypto_recv_retry_cb;
                callbacks.encrypt                  = ngtcp2_crypto_encrypt_cb;
                callbacks.decrypt                  = ngtcp2_crypto_decrypt_cb;
                callbacks.hp_mask                  = ngtcp2_crypto_hp_mask_cb;
                callbacks.update_key               = ngtcp2_crypto_update_key_cb;
                callbacks.delete_crypto_aead_ctx   = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
                callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
                callbacks.version_negotiation      = ngtcp2_crypto_version_negotiation_cb;
                callbacks.get_path_challenge_data  = ngtcp2_crypto_get_path_challenge_data_cb;
                callbacks.rand                     = onRandom;
                callbacks.get_new_connection_id    = onGetNewConnectionId;
                callbacks.handshake_completed      = onHandshakeCompleted;
                return callbacks;
            }

            static void onRandom(std::uint8_t *destination, const std::size_t destinationLength, const ngtcp2_rand_ctx * /*context*/)
            {
                static_cast<void>(RAND_bytes(destination, static_cast<int>(destinationLength)));
            }

            static int onGetNewConnectionId(ngtcp2_conn * /*conn*/, ngtcp2_cid *connectionId, std::uint8_t *token, const std::size_t cidLength,
                                            void * /*userData*/)
            {
                if (RAND_bytes(connectionId->data, static_cast<int>(cidLength)) != 1)
                {
                    return -1;
                }
                connectionId->datalen = cidLength;
                return RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) == 1 ? 0 : -1;
            }

            static int onHandshakeCompleted(ngtcp2_conn * /*conn*/, void *userData)
            {
                static_cast<QuicTestClient *>(userData)->m_isHandshakeCompleted = true;
                return 0;
            }

            Platform::DatagramSocket  m_socket;                        ///< 客户端的 UDP 套接字
            SSL_CTX                  *m_sslContext{nullptr};           ///< 客户端 TLS 上下文
            SSL                      *m_ssl{nullptr};                  ///< 客户端 SSL（QUIC 模式）
            ngtcp2_crypto_ossl_ctx   *m_osslContext{nullptr};          ///< ngtcp2 的 ossl 上下文
            ngtcp2_conn              *m_connection{nullptr};           ///< 客户端连接
            ngtcp2_crypto_conn_ref    m_connectionReference{};         ///< SSL app data 用的连接引用
            ngtcp2_cid                m_destinationConnectionId{};     ///< 目的连接标识
            ngtcp2_cid                m_sourceConnectionId{};          ///< 本端连接标识
            ngtcp2_path               m_path{};                        ///< 网络路径（地址本体在下面两个成员里）
            Platform::SocketAddress   m_localStorage;                  ///< 本端地址本体
            Platform::SocketAddress   m_remoteStorage;                 ///< 服务端地址本体
            Platform::SocketAddress   m_serverAddress;                 ///< 服务端地址（发送用）
            std::int64_t              m_pendingStreamId{-1};           ///< 排队负载所属的流
            std::vector<std::uint8_t> m_pendingStreamPayload;          ///< 待发负载（必须活到确认）
            ngtcp2_vec                m_pendingStreamVector{};         ///< 待发负载的 ngtcp2 视图
            std::atomic<bool>         m_isHandshakeCompleted{false};   ///< 握手是否完成（回调里置位）
            std::atomic<int>          m_lastWriteError{0};             ///< 最近一次写失败的错误码
            std::atomic<bool>         m_hasReadError{false};           ///< 读入是否失败过
            std::atomic<std::size_t>  m_receivedDatagramCount{0};   ///< 收到过多少条服务端报文
            std::atomic<std::size_t>  m_sentDatagramCount{0};       ///< 发出过多少条报文
            std::size_t               m_writtenStreamByteCount{0};  ///< 已写出的流数据字节数
        };

        /**
         * @brief 把 QUIC 服务端跑在独立事件循环线程上的夹具
         */
        class RunningQuicServer
        {
        public:
            /**
             * @brief 起一台回环上的服务端
             * @param idleTimeout 空闲/握手超时；考「超时收口」的用例把它调小，免得干等默认的 30 秒
             */
            explicit RunningQuicServer(const std::chrono::seconds idleTimeout = std::chrono::seconds{30})
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile = certificatePath();
                configuration.privateKeyFile  = privateKeyPath();
                configuration.idleTimeout     = idleTimeout;

                m_server = std::make_unique<QuicServer>(m_loop, configuration);
                m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                // 在起线程之前把自己排进所属循环的就绪队列：调度器只在循环线程上跑，
                // 这里还在创建者线程上，于是这一次排入是「归属线程内」的合法调用
                m_loop.scheduler().schedule(m_listenTask->handle());

                m_loopThread = std::thread([this]
                {
                    m_loop.run();
                });

                // 绑定成功后 listeningPort() 才有值：端口由内核分配。**端口是循环线程写下的**，
                // 只能在循环线程上读——这里反复投递去采样（直接跨线程读就是与循环抢同一个成员，
                // TSan 报过这条），此后测试线程读的都是快照
                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (refreshListeningPort() == 0 && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            }

            ~RunningQuicServer()
            {
                m_server->stop();
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            RunningQuicServer(const RunningQuicServer &) = delete;

            RunningQuicServer &operator=(const RunningQuicServer &) = delete;

            [[nodiscard]] QuicServer &server() noexcept
            {
                return *m_server;
            }

            /// 服务端监听端口（0 表示还没绑上）：读的是循环线程写下的原子快照
            [[nodiscard]] std::uint16_t listeningPort() const noexcept
            {
                return m_listeningPort.load(std::memory_order_acquire);
            }

            /**
             * @brief 在循环线程上执行一段动作，并等它做完
             * @param action 待执行的动作
             * @details 服务端归它的循环所有：连接表、本端端口这些成员都只由循环线程读写，
             *          测试线程直接读就是与循环抢同一批数据（TSan 在并发用例集里报的正是这一类）。
             *          正路是投递（scheduler().postRemote()）——用具从测试线程取值时走这条
             */
            void runOnLoopAndWait(const std::function<void()> &action)
            {
                std::atomic<bool> isFinished{false};
                m_loop.scheduler().postRemote(
                        [&action, &isFinished]
                        {
                            action();
                            isFinished.store(true, std::memory_order_release);
                        });

                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (!isFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                EXPECT_TRUE(isFinished.load(std::memory_order_acquire)) << "投递到循环线程的动作没有在时限内完成";
            }

            /// 在循环线程上取一次本端端口，更新快照后返回
            std::uint16_t refreshListeningPort()
            {
                std::uint16_t port = 0;
                runOnLoopAndWait([this, &port] { port = m_server->listeningPort(); });
                m_listeningPort.store(port, std::memory_order_release);
                return port;
            }

            /**
             * @brief 在循环线程上取当前在线连接数
             * @return std::size_t 连接数
             */
            [[nodiscard]] std::size_t sampleConnectionCount()
            {
                std::size_t count = 0;
                runOnLoopAndWait([this, &count] { count = m_server->connectionCount(); });
                return count;
            }

        private:
            Core::EventLoop                 m_loop;             ///< 服务端所属事件循环
            std::unique_ptr<QuicServer>     m_server;           ///< 被测服务端
            std::optional<Core::Task<>>     m_listenTask;       ///< 监听协程（活到夹具析构；Task 没有默认构造，用 optional 托管）
            std::thread                     m_loopThread;       ///< 跑循环的线程
            std::atomic<std::uint16_t>      m_listeningPort{0}; ///< 循环线程写下的本端端口快照
        };

        /// 造服务端地址（回环 + 给定端口）
        Platform::SocketAddress makeServerAddress(const std::uint16_t port)
        {
            Platform::SocketAddress address;
            sockaddr_in             addressV4{};
            addressV4.sin_family      = AF_INET;
            addressV4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addressV4.sin_port        = htons(port);
            std::memcpy(&address.storage, &addressV4, sizeof(addressV4));
            address.length = sizeof(addressV4);
            return address;
        }

        /**
         * @brief 在时限内反复推进客户端，直到条件成立
         * @param client 客户端
         * @param predicate 条件
         * @return true 条件在时限内成立
         */
        template<typename Predicate>
        bool pumpUntil(QuicTestClient &client, Predicate predicate)
        {
            const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (predicate())
                {
                    return true;
                }
                client.pumpOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            return predicate();
        }
    } // namespace

    /**
     * @brief 客户端与服务端在真实回环 UDP 上完成握手，且协商出 h3
     */
    TEST(QuicServer, CompletesHandshakeOverLoopback)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        RunningQuicServer server;
        ASSERT_NE(server.listeningPort(), 0) << "服务端没有绑定成功（证书或套接字有问题）";

        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));

        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); }))
                << "握手没有在时限内完成（写错误码 " << client.lastWriteError() << "，服务端连接数 " << server.sampleConnectionCount() << "，读错 " << client.hasReadError() << "，收 " << client.receivedDatagramCount() << " 条 / 发 " << client.sentDatagramCount() << " 条）";
        EXPECT_STREQ(client.selectedApplicationProtocol().c_str(), "h3") << "协商出的 ALPN 不是 h3";
        EXPECT_EQ(server.sampleConnectionCount(), 1U) << "服务端应当正好有一条连接";
    }

    /**
     * @brief 握手之后客户端发流数据，服务端的流数据处理回调收到同样的字节
     */
    TEST(QuicServer, DeliversStreamDataToHandler)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        const std::vector<std::uint8_t> payload{'a', 's', 'y', 'n', '-', 'q', 'u', 'i', 'c'};

        RunningQuicServer server;
        ASSERT_NE(server.listeningPort(), 0);

        std::atomic<bool>         hasReceivedStreamData{false};
        std::vector<std::uint8_t> receivedPayload;
        std::mutex                observationMutex;

        // 回调在服务端循环线程上跑：跨线程共享的观测数据用锁保护
        server.server().setStreamDataHandler(
                [&hasReceivedStreamData, &receivedPayload, &observationMutex](QuicConnection & /*connection*/, const std::int64_t /*streamId*/,
                                                                             const std::span<const std::uint8_t> data,
                                                                             const bool /*isEndStream*/)
                {
                    const std::lock_guard<std::mutex> guard(observationMutex);
                    receivedPayload.assign(data.begin(), data.end());
                    hasReceivedStreamData.store(true, std::memory_order_release);
                });

        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "握手没有完成";
        ASSERT_TRUE(client.openStreamAndQueuePayload(payload)) << "流没有开出来";

        ASSERT_TRUE(pumpUntil(client, [&hasReceivedStreamData] { return hasReceivedStreamData.load(std::memory_order_acquire); }))
                << "服务端没有把流数据交给回调（客户端已写出流数据 " << client.writtenStreamByteCount() << " 字节）";

        const std::lock_guard<std::mutex> guard(observationMutex);
        EXPECT_EQ(receivedPayload, payload) << "送到的字节与发出去的不一致";
    }

    /**
     * @brief 乱码报文既不能建连接也不能把服务端搞坏：随后正常客户端仍能握手
     */
    TEST(QuicServer, IgnoresGarbageDatagramAndKeepsServing)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        RunningQuicServer server;
        ASSERT_NE(server.listeningPort(), 0);

        const Platform::DatagramSocket garbageSocket = Platform::DatagramSocket::bindTo(makeServerAddress(0));
        ASSERT_TRUE(garbageSocket.isValid());
        std::vector<std::uint8_t> garbage(1200, 0x5a); // 长得像 Initial 的随机字节，长头里的版本与标识都不合法
        ASSERT_GT(garbageSocket.send(makeServerAddress(server.listeningPort()), garbage.data(), garbage.size()), 0);

        // 服务端仍能正常服务：随后来的合法客户端照常握手。
        // 「乱码没建出连接」不用定时等待去赌——等这条合法连接真握上手，连接数恰好是 1，
        // 乱码若被当成了新连接，这里就会读到 2
        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); }))
                << "乱码报文之后服务端不再接受合法握手";
        EXPECT_EQ(server.sampleConnectionCount(), 1U) << "乱码报文不该建出连接";
    }

    /**
     * @brief 提非 h3 的 ALPN 必须被拒：握手完不成，且服务端照常服务下一位
     * @details 服务端的 ALPN 回调按约定是致命告警而非退让——放行别的协议会让后续按 h3 解析的字节流对不上，
     *          所以这里既钉「这次协商不成」，也钉「服务端没被这次拒绝带坏」。
     */
    TEST(QuicServer, RejectsClientWithoutHttp3Alpn)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        RunningQuicServer server;
        ASSERT_NE(server.listeningPort(), 0);

        static constexpr unsigned char kWrongApplicationProtocols[] = {2, 'h', '2'};
        QuicTestClient                wrongProtocolClient;
        ASSERT_TRUE(wrongProtocolClient.initialize(makeServerAddress(server.listeningPort()), kWrongApplicationProtocols));

        EXPECT_FALSE(pumpUntil(wrongProtocolClient, [&wrongProtocolClient] { return wrongProtocolClient.isHandshakeCompleted(); }))
                << "服务端不该接受非 h3 的 ALPN 协商";
        EXPECT_TRUE(wrongProtocolClient.selectedApplicationProtocol().empty()) << "没协商成的客户端不该拿到协议名";

        // 服务端不受影响：随后来的合法客户端照常握手并协商出 h3
        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); }))
                << "ALPN 被拒之后服务端不再接受合法握手";
        EXPECT_STREQ(client.selectedApplicationProtocol().c_str(), "h3") << "协商出的 ALPN 不是 h3";
    }

    /**
     * @brief 已收口但仍被协程持有的连接，要等在途动作结束才摘除
     * @details 收报文路径与定时循环都会 co_await 连接的方法（handleDatagram / flush / handleExpiry
     *          都可能在等网络时挂起）：挂起期间另一条路径若把「已收口」的连接摘掉销毁，恢复后手里
     *          那份引用与迭代器就是悬垂的。这里用 ActivityGuard 精确造出「有在途动作」这一状态——
     *          守卫在时清扫必须跳过（等满多个节拍），守卫一放立刻摘掉
     */
    TEST(QuicServer, KeepsClosedConnectionWhileAnActionIsInFlight)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        RunningQuicServer server;
        ASSERT_NE(server.listeningPort(), 0);

        // 握手后让服务端把连接交到流数据回调里：用例据此拿到服务端那一侧的 QuicConnection
        std::atomic<QuicConnection *> observedConnection{nullptr};
        server.server().setStreamDataHandler(
                [&observedConnection](QuicConnection &connection, const std::int64_t, const std::span<const std::uint8_t>, const bool)
                {
                    observedConnection.store(&connection, std::memory_order_release);
                });

        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "握手没有完成";

        const std::vector<std::uint8_t> payload{'x'};
        ASSERT_TRUE(client.openStreamAndQueuePayload(payload)) << "流没有开出来";
        ASSERT_TRUE(pumpUntil(client, [&observedConnection] { return observedConnection.load(std::memory_order_acquire) != nullptr; }))
                << "服务端没有把连接交到流数据回调里";
        QuicConnection *const serverConnection = observedConnection.load(std::memory_order_acquire);
        ASSERT_EQ(server.sampleConnectionCount(), 1U);

        // 「已收口 + 有在途动作」：守卫跨过下面整整一段等待
        std::optional<QuicConnection::ActivityGuard> activityGuard;
        server.runOnLoopAndWait(
                [serverConnection, &activityGuard]
                {
                    serverConnection->requestClose();
                    activityGuard.emplace(*serverConnection);
                });

        // 清扫节拍是 10ms：等 20 拍（200ms）看它会被摘掉几次。守卫在，一次都不该被摘
        const auto guardDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        while (std::chrono::steady_clock::now() < guardDeadline)
        {
            ASSERT_EQ(server.sampleConnectionCount(), 1U) << "有在途动作的已收口连接被提前摘掉了";
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }

        // 放开守卫：下一次节拍就该摘掉它——这一步同时证明清扫一直在跑，上面的「没被摘」不是假通过
        server.runOnLoopAndWait([&activityGuard] { activityGuard.reset(); });
        const auto reapDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (server.sampleConnectionCount() != 0 && std::chrono::steady_clock::now() < reapDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        EXPECT_EQ(server.sampleConnectionCount(), 0U) << "在途动作结束后连接没有被摘除";
    }

    /**
     * @brief 只发出首个 Initial 就消失的客户端：握手超时后服务端必须把这条连接收口
     * @details 失败面用例。对端半路消失时连接若一直留在路由表里，既占着连接上限，也让「在线连接数」
     *          永远不可信。超时调到 1 秒，否则要干等默认的 30 秒。
     */
    TEST(QuicServer, ReapsConnectionWhoseHandshakeNeverCompletes)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        RunningQuicServer server{std::chrono::seconds{1}};
        ASSERT_NE(server.listeningPort(), 0);

        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        // 只推一步：首个 Initial 发出去、服务端据此建起连接，此后客户端装死不再推进
        client.pumpOnce();

        // 先等这条连接真的建起来——否则下面那个「计数为 0」可能只是服务端还没处理那个 Initial
        const auto appearedDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (server.sampleConnectionCount() == 0 && std::chrono::steady_clock::now() < appearedDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ASSERT_EQ(server.sampleConnectionCount(), 1U) << "首个 Initial 没有建出连接";

        const auto reapedDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (server.sampleConnectionCount() != 0 && std::chrono::steady_clock::now() < reapedDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        EXPECT_EQ(server.sampleConnectionCount(), 0U) << "握手没完成的连接没有在超时后被收口";
    }
} // namespace AsynGyanis::Net
