// QUIC 服务端的回环端到端用例：真 UDP 套接字、真证书、真握手 客户端的 QUIC 实现由测试自己用 ngtcp2 搭（与「用独立实现做验收」的精神一致：被测的是
// 服务端这一侧，客户端只当对端）。握手与流数据都走真实回环 UDP，因此套接字封装、 连接标识路由、定时器驱动、证书与 ALPN 协商全都在链路上。
#include "Net/Quic/QuicServer.h"

#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http3/Http3Frame.h"
#include "Net/Http3/Qpack.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/Socket.h"

#include "CoreTestSupport.h"

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
             * @param sessionToResume 要带上的会话（TLS 1.3 票据）；空表示做全量握手。
             *        生命周期归调用方，本客户端只在握手前把它交给 SSL_set_session，不持有
             * @return true 初始化完成（此时握手尚未开始，要靠 pumpOnce() 推进）
             */
            bool initialize(const Platform::SocketAddress &serverAddress, const std::span<const unsigned char> applicationProtocols = kHttp3Alpn,
                            SSL_SESSION *sessionToResume = nullptr)
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
                if (sessionToResume != nullptr)
                {
                    // 只是登记候选会话：命不命中由握手本身决定，没命中就退回全量握手。
                    // 记下发没发出去，用例才能分辨「客户端根本没带会话」与「服务端解不开这张票据」
                    m_sessionApplied = SSL_set_session(m_ssl, sessionToResume) == 1;
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
                // 给服务端一点发送窗口：默认全 0 时服务端在本端流上一个字节都发不出来，
                // 「一条流被流控挡住」这类场景也就无从构造。流级只给 4 KiB：够小响应，
                // 大响应必定撞上窗口而停下
                parameters.initial_max_data                    = 256 * 1024;
                parameters.initial_max_stream_data_bidi_local  = 4 * 1024;
                parameters.initial_max_stream_data_uni         = 4 * 1024;
                // 服务端接上路由器后要开三条单向流（控制流 + QPACK 编/解码流）才建得起 HTTP/3
                // 会话，额度为 0 时会话直接判「不可用」，连接随即被收口：一条请求都派不出去
                parameters.initial_max_streams_uni             = 3;
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
                        if (writtenByteCount == NGTCP2_ERR_CLOSING || writtenByteCount == NGTCP2_ERR_DRAINING)
                        {
                            m_isPeerClosing = true;
                        }
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
                    const ngtcp2_ssize readResult = ngtcp2_conn_read_pkt(m_connection, &m_path, nullptr, receivedPacket.data(),
                                                                        static_cast<std::size_t>(receivedByteCount), now);
                    if (readResult != 0)
                    {
                        if (readResult == NGTCP2_ERR_CLOSING || readResult == NGTCP2_ERR_DRAINING)
                        {
                            m_isPeerClosing = true;
                        }
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

            /**
             * @brief 是否把调用方给的会话登记进了本次握手
             * @return true SSL_set_session 成功（不代表命中，命中看 isSessionReused()）
             */
            [[nodiscard]] bool isSessionApplied() const noexcept
            {
                return m_sessionApplied;
            }

            /**
             * @brief 本次握手是否走了会话恢复（客户端视角）
             * @return true 服务端接受了我们的 PSK，没有做全量握手
             */
            [[nodiscard]] bool isSessionReused() const
            {
                return SSL_session_reused(m_ssl) == 1;
            }

            /**
             * @brief 客户端是否已经拿到一张可恢复的会话票据
             * @details TLS 1.3 的 NewSessionTicket 在握手完成**之后**才到，所以握手完成不等于
             *          票据到手——调用方要 pumpUntil 到这里，再去取会话
             */
            [[nodiscard]] bool hasResumableSession() const
            {
                const std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session(SSL_get1_session(m_ssl), &SSL_SESSION_free);
                return session != nullptr && SSL_SESSION_is_resumable(session.get()) == 1;
            }

            /**
             * @brief 取走当前会话（加一份引用），供下一条连接恢复用
             * @return std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> 会话；没有则空
             */
            [[nodiscard]] std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> takeResumableSession() const
            {
                return {SSL_get1_session(m_ssl), &SSL_SESSION_free};
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

            /// 对端是否已发来 CONNECTION_CLOSE（ngtcp2 之后一律以 CLOSING/DRAINING 拒绝读写）
            [[nodiscard]] bool isPeerClosing() const noexcept
            {
                return m_isPeerClosing;
            }

            /**
             * @brief 某条流是否已收口，以及它的应用错误码
             * @param streamId 流号
             * @return std::optional<std::uint64_t> 还没收口时返回空
             */
            [[nodiscard]] std::optional<std::uint64_t> closedStreamErrorCodeOf(const std::int64_t streamId) const
            {
                const auto found = m_closedStreamCodes.find(streamId);
                if (found == m_closedStreamCodes.end())
                {
                    return std::nullopt;
                }
                return found->second;
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

            /// 最近一次 openStreamAndQueuePayload() 开出来的流号
            [[nodiscard]] std::int64_t lastOpenedStreamId() const noexcept
            {
                return m_pendingStreamId;
            }

            /**
             * @brief 指定一条「收到数据也不还窗口」的流，用于构造流控阻塞
             * @param streamId 目标流号；-1 表示所有流都照常还窗口
             * @note 必须在该流的数据到达之前设置
             */
            void setStreamToKeepFlowControlBlocked(const std::int64_t streamId) noexcept
            {
                m_streamKeptFlowControlBlocked = streamId;
            }

            /// 某条流上收到的字节（按到达顺序拼接）
            [[nodiscard]] std::string receivedPayloadOn(const std::int64_t streamId) const
            {
                const auto found = m_receivedStreamPayloads.find(streamId);
                return found == m_receivedStreamPayloads.end() ? std::string{} : found->second;
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
                callbacks.recv_stream_data         = onReceiveStreamData;
                callbacks.stream_close             = onStreamClose;
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

            /**
             * @brief 一条流收口了：记下它的应用错误码
             * @details 本端收到服务端的 RESET_STREAM 或 STOP_SENDING 时 ngtcp2 都走这里（收到停发
             *          请求时它自己会回一条复位），因此这也是「服务端那份帧对不对」的裁判点
             */
            static int onStreamClose(ngtcp2_conn * /*conn*/, const std::uint32_t /*flags*/, const std::int64_t streamId,
                                     const std::uint64_t applicationErrorCode, void *userData, void * /*streamUserData*/)
            {
                static_cast<QuicTestClient *>(userData)->m_closedStreamCodes[streamId] = applicationErrorCode;
                return 0;
            }

            /// 收下服务端发来的流数据：按流号累积，供用例断言「某条流的响应真的到了」
            static int onReceiveStreamData(ngtcp2_conn * /*conn*/, const std::uint32_t /*flags*/, const std::int64_t streamId,
                                           const std::uint64_t /*offset*/, const std::uint8_t *data, const std::size_t dataLength,
                                           void *userData, void * /*streamUserData*/)
            {
                QuicTestClient *const client = static_cast<QuicTestClient *>(userData);
                client->m_receivedStreamPayloads[streamId].append(reinterpret_cast<const char *>(data), dataLength);
                // 一般情形下收到即还窗口：不还的话服务端发满 4 KiB 就再也发不动
                //（用例若要构造「某条流被流控挡住」，就把该流登记为不还窗口，见 setStreamToKeepFlowControlBlocked）
                if (streamId == client->m_streamKeptFlowControlBlocked)
                {
                    return 0;
                }
                ngtcp2_conn_extend_max_stream_offset(client->m_connection, streamId, dataLength);
                ngtcp2_conn_extend_max_offset(client->m_connection, dataLength);
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
            std::map<std::int64_t, std::string> m_receivedStreamPayloads; ///< 各流上收到的字节（按流号累积）
            /// 各流的收口应用错误码：服务端发来 RESET_STREAM / STOP_SENDING 时由 ngtcp2 报进来
            std::map<std::int64_t, std::uint64_t> m_closedStreamCodes;
            std::int64_t m_streamKeptFlowControlBlocked{-1};              ///< 收到数据也不还窗口的流（-1 表示没有）
            std::vector<std::uint8_t> m_pendingStreamPayload;          ///< 待发负载（必须活到确认）
            ngtcp2_vec                m_pendingStreamVector{};         ///< 待发负载的 ngtcp2 视图
            std::atomic<bool>         m_isHandshakeCompleted{false};   ///< 握手是否完成（回调里置位）
            /// 调用方给的会话是否登记进了握手：只在 initialize() 里由测试线程写、用例读，不进回调
            bool m_sessionApplied{false};
            std::atomic<int>          m_lastWriteError{0};             ///< 最近一次写失败的错误码
            std::atomic<bool>         m_hasReadError{false};           ///< 读入是否失败过
            std::atomic<std::size_t>  m_receivedDatagramCount{0};   ///< 收到过多少条服务端报文
            bool                      m_isPeerClosing{false};       ///< 对端是否已发来 CONNECTION_CLOSE
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
             * @param perIpConnectionLimiter 单来源并发上限的限额器；空表示不按来源限制
             * @param sessionTicketKeyFiles 会话票据密钥文件列表；空表示按 OpenSSL 默认，
             *        每个 SSL_CTX 一份随机密钥（跨实例恢复因此必然落空，正是对照组的形态）
             */
            explicit RunningQuicServer(const std::chrono::seconds idleTimeout = std::chrono::seconds{30},
                                       std::shared_ptr<PerIpConnectionLimiter> perIpConnectionLimiter = nullptr,
                                       std::vector<std::string> sessionTicketKeyFiles = {})
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile = certificatePath();
                configuration.privateKeyFile  = privateKeyPath();
                configuration.idleTimeout     = idleTimeout;
                configuration.perIpConnectionLimiter = std::move(perIpConnectionLimiter);
                configuration.sessionTicketKeyFiles  = std::move(sessionTicketKeyFiles);

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

        /// 票据密钥的字节数：名 16 + HMAC 16 + AES-128 16
        constexpr std::size_t kTicketKeyBytes = 48;

        /**
         * @brief 在临时目录里写一份内容确定的会话票据密钥文件
         * @param directory 用例独占的临时目录
         * @param fileName 文件名
         * @param seed 密钥内容种子：同 seed 得到同一份密钥（跨服务端实例共享就是这么建模的）
         * @return std::string 文件路径文本
         * @details 走 writeBinaryFile()：密钥里有 0x0A，文本模式在 Windows 上会翻成 CRLF，
         *          长度当场就错，而长度合法正是被测前置。不用随机数是可复现的要求
         */
        std::string writeTicketKeyFile(const AsynGyanis::TestSupport::TemporaryDirectory &directory, const std::string &fileName,
                                       const unsigned int seed)
        {
            EXPECT_TRUE(directory.writeBinaryFile(fileName, AsynGyanis::TestSupport::makeBytePattern(seed, kTicketKeyBytes)))
                    << "密钥文件写不出来：" << fileName;
            return (directory.path() / fileName).string();
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
     * @brief 两台服务端实例装同一份票据密钥时，第二条连接在另一台实例上命中会话恢复
     * @details 多进程 worker 各持一份 SSL_CTX，而 SO_REUSEPORT 不保证第二次连接落回同一个进程：
     *          密钥不共享时票据解不开，只能退回全量握手。这里把「落到别的进程」直接建模成
     *          「换一台 QuicServer 握手」。判据取客户端视角的 SSL_session_reused，并额外断言
     *          会话确实被登记进了握手——只信「命中」会把「客户端根本没带会话」也看成恢复
     */
    TEST(QuicServer, ResumesSessionOnAnotherServerInstanceWithSharedTicketKeys)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        AsynGyanis::TestSupport::TemporaryDirectory directory("QuicTicketKeyShared");
        const std::string                           keyFile = writeTicketKeyFile(directory, "ticket.key", 21);

        RunningQuicServer firstServer(std::chrono::seconds{30}, nullptr, {keyFile});
        ASSERT_NE(firstServer.listeningPort(), 0) << "第一台服务端没有绑定成功（证书或套接字有问题）";

        QuicTestClient firstClient;
        ASSERT_TRUE(firstClient.initialize(makeServerAddress(firstServer.listeningPort())));
        ASSERT_TRUE(pumpUntil(firstClient, [&firstClient] { return firstClient.isHandshakeCompleted(); }))
                << "第一条连接的握手没有在时限内完成";

        // 握手完成不等于票据到手：TLS 1.3 的 NewSessionTicket 是握手之后才发的
        ASSERT_TRUE(pumpUntil(firstClient, [&firstClient] { return firstClient.hasResumableSession(); }))
                << "第一条连接没有拿到可恢复的会话票据";
        const std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session = firstClient.takeResumableSession();
        ASSERT_NE(session, nullptr);

        RunningQuicServer secondServer(std::chrono::seconds{30}, nullptr, {keyFile});
        ASSERT_NE(secondServer.listeningPort(), 0) << "第二台服务端没有绑定成功";

        QuicTestClient secondClient;
        ASSERT_TRUE(secondClient.initialize(makeServerAddress(secondServer.listeningPort()), kHttp3Alpn, session.get()));
        ASSERT_TRUE(secondClient.isSessionApplied()) << "SSL_set_session 登记失败，后面的判据无从谈起";
        ASSERT_TRUE(pumpUntil(secondClient, [&secondClient] { return secondClient.isHandshakeCompleted(); }))
                << "第二条连接的握手没有在时限内完成（即便解不开票据也应退回全量握手，不该失败）";
        EXPECT_TRUE(secondClient.isSessionReused()) << "换了一台实例就没认出来：票据密钥没有跨实例共享";
    }

    /**
     * @brief 对照组：不装密钥时，换实例必然恢复不了
     * @details 上一条用例的证伪判据。没有它，「命中恢复」可能只是同一进程里 OpenSSL 自己的会话缓存
     *          凑巧生效——两台服务端跑在同一进程的同一个测试里，各自一份随机密钥，
     *          跨实例本就不该解得开
     */
    TEST(QuicServer, ResumptionMissesOnAnotherServerInstanceWithoutSharedTicketKeys)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        RunningQuicServer firstServer;
        ASSERT_NE(firstServer.listeningPort(), 0) << "第一台服务端没有绑定成功";

        QuicTestClient firstClient;
        ASSERT_TRUE(firstClient.initialize(makeServerAddress(firstServer.listeningPort())));
        ASSERT_TRUE(pumpUntil(firstClient, [&firstClient] { return firstClient.isHandshakeCompleted(); }));
        ASSERT_TRUE(pumpUntil(firstClient, [&firstClient] { return firstClient.hasResumableSession(); }))
                << "第一条连接没有拿到可恢复的会话票据";
        const std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session = firstClient.takeResumableSession();
        ASSERT_NE(session, nullptr);

        RunningQuicServer secondServer;
        ASSERT_NE(secondServer.listeningPort(), 0) << "第二台服务端没有绑定成功";

        QuicTestClient secondClient;
        ASSERT_TRUE(secondClient.initialize(makeServerAddress(secondServer.listeningPort()), kHttp3Alpn, session.get()));
        ASSERT_TRUE(secondClient.isSessionApplied()) << "SSL_set_session 登记失败";
        ASSERT_TRUE(pumpUntil(secondClient, [&secondClient] { return secondClient.isHandshakeCompleted(); }));
        EXPECT_FALSE(secondClient.isSessionReused()) << "没共享密钥却命中了恢复，说明本组用例判不出共享语义";
    }

    /**
     * @brief 票据密钥不合格时构造当场失败，而不是悄悄退回「每个上下文一份随机密钥」
     * @details 退回随机密钥的表现是恢复命中率归零而服务一切正常——既没有错误日志也没有失败请求，
     *          是这类配置最贵的失败形态。异常文本还要点名是哪一份文件：本条失败没有 OpenSSL
     *          错误栈可查
     */
    TEST(QuicServer, RejectsInvalidTicketKeyFilesDuringConstruction)
    {
        AsynGyanis::TestSupport::TemporaryDirectory directory("QuicTicketKeyInvalid");
        const std::string                           wrongLengthPath = (directory.path() / "wrong-length.key").string();
        EXPECT_TRUE(directory.writeBinaryFile("wrong-length.key", AsynGyanis::TestSupport::makeBytePattern(31, 64)));

        const std::vector<std::string> keyFiles{wrongLengthPath};
        try
        {
            static_cast<void>(std::make_unique<RunningQuicServer>(std::chrono::seconds{30}, nullptr, keyFiles));
            FAIL() << "长度为 64 字节的密钥文件（既不是 48 也不是 80）应当让构造失败";
        } catch (const Base::Exception &failure)
        {
            const std::string message = failure.what();
            EXPECT_TRUE(message.find("wrong-length.key") != std::string::npos) << "异常没点名是哪一份文件：" << message;
        }

        // 文件根本不存在同样要抛，且不能留下半构造的对象：上面那台已经抛在构造期，
        // 这里的断言只判「会不会抛」，端口与握手都不参与
        EXPECT_THROW(static_cast<void>(std::make_unique<RunningQuicServer>(
                             std::chrono::seconds{30}, nullptr, std::vector<std::string>{(directory.path() / "missing.key").string()})),
                     Base::Exception);
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
     * @brief 钉住：连接被空闲收口时，还挂在正文上的 h3 处理器要醒来收尾，会话之后才被摘掉
     * @details 对端不再发任何报文 → 连接级空闲超时把连接收掉。这条路径上没有逐流的 RST，
     *          所以会话不会经由「对端取消」那条既有的收口口子里得到通知：处理器挂在
     *          `bodyStream()->readNext()` 上的帧若随会话一起销毁，它等待之后的代码全不执行，
     *          而任何已经投回循环的恢复动作会指向已释放的帧（外置到工作线程的响应压缩正是这样一个唤醒者）。
     *          判据两条：处理器跑到收尾、连接连同会话最终被摘除（后者同时防「为了不等业务而漏摘」）
     */
    TEST(QuicServer, WakesPendingHandlersWhenConnectionIsReapedByIdleTimeout)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        std::atomic<bool> isHandlerEntered{false};
        std::atomic<bool> isHandlerFinished{false};

        Router router;
        router.postStreaming("/upload",
                             [&isHandlerEntered, &isHandlerFinished](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 isHandlerEntered.store(true, std::memory_order_release);
                                 // 正文声明了 4 字节而客户端只发头部：这里必然挂起等下一段字节
                                 while (co_await request.bodyStream()->readNext())
                                 {
                                 }
                                 isHandlerFinished.store(true, std::memory_order_release);
                                 response.setStatus(204);
                                 co_return;
                             });

        // 空闲 1 秒：客户端停手后连接很快被服务端自己收掉（远早于 10 秒级的正文读超时，
        // 因此这条用例走的是「连接没了」而不是「这条流超时了」）
        RunningQuicServer server(std::chrono::seconds{1});
        ASSERT_NE(server.listeningPort(), 0);
        server.server().setRouter(router);

        std::vector<QpackHeaderField> fieldLines = {
                QpackHeaderField{":method", "POST"},
                QpackHeaderField{":scheme", "https"},
                QpackHeaderField{":authority", "example.com"},
                QpackHeaderField{":path", "/upload"},
                QpackHeaderField{"content-length", "4"},
        };
        std::string headerBlock;
        std::string encoderStreamBytes;
        QpackEncoder encoder(0, 0, 0);
        ASSERT_TRUE(encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock, encoderStreamBytes).has_value());
        ASSERT_TRUE(encoderStreamBytes.empty()) << "只用字面量编码就不该产生 QPACK 编码器指令";

        Http3HeadersFrame headersFrame;
        headersFrame.encodedFieldSection =
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(headerBlock.data()), headerBlock.size());
        std::string requestBytes;
        appendHttp3Frame(requestBytes, headersFrame);
        const std::span<const std::uint8_t> requestPayload(
                reinterpret_cast<const std::uint8_t *>(requestBytes.data()), requestBytes.size());

        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "握手没有完成";
        ASSERT_TRUE(client.openStreamAndQueuePayload(requestPayload)) << "流没有开出来";
        ASSERT_TRUE(pumpUntil(client, [&isHandlerEntered] { return isHandlerEntered.load(std::memory_order_acquire); }))
                << "服务端没有把这条请求派发给流式正文处理器，此时在线连接数=" << server.sampleConnectionCount();

        // 此后不再替客户端发任何报文：连接静默 → 服务端按空闲上限收口
        const auto finishDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!isHandlerFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < finishDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        EXPECT_TRUE(isHandlerFinished.load(std::memory_order_acquire))
                << "连接被收口时没唤醒挂在正文上的处理器：它的帧被连会话一起销毁了";

        const auto reapDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (server.sampleConnectionCount() != 0 && std::chrono::steady_clock::now() < reapDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        EXPECT_EQ(server.sampleConnectionCount(), 0U) << "会话收口后连接没有被摘除（在途动作的账没还干净）";
    }


    /**
     * @brief 一条流被流控挡住时，同连接其它流的响应照样出得去
     * @details 选流此前是「按流号顺序取第一条还有待发数据的流」，而窗口不够时只 continue 重新进
     *          循环——又选中同一条，64 轮全空转：流号更小的那条被挡住时，同连接其它流（含服务端
     *          自己的控制流与 QPACK 流）一个字节都出不去。客户端窗口只给 4 KiB：流 0 上排 32 KiB
     *          必定被挡住，流 4 上排一小段则该出得去
     */
    TEST(QuicServer, DoesNotStarveOtherStreamsWhenOneIsFlowControlBlocked)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        const std::string bigPayload(32 * 1024, 'B');
        const std::string smallPayload = "small-response";

        RunningQuicServer server;
        ASSERT_NE(server.listeningPort(), 0);

        // 服务端每收到一条流的字节就往该流排一份响应：第一条流排大响应（撑爆窗口），
        // 其余排小响应。回调在服务端循环线程上跑，用一个原子记录「第一条流」即可
        std::atomic<std::int64_t> firstStreamId{-1};
        server.server().setStreamDataHandler(
                [&firstStreamId, &bigPayload, &smallPayload](QuicConnection &connection, const std::int64_t streamId,
                                                             const std::span<const std::uint8_t>, const bool)
                {
                    const bool isFirstStream = firstStreamId.exchange(streamId) == -1;
                    const std::string &payload = isFirstStream ? bigPayload : smallPayload;
                    connection.queueStreamData(streamId,
                                               std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(
                                                                                     const_cast<char *>(payload.data())),
                                                                             payload.size()),
                                               false);
                });

        QuicTestClient client;
        ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
        ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "握手没有完成";

        const std::vector<std::uint8_t> requestByte{'r'};
        ASSERT_TRUE(client.openStreamAndQueuePayload(requestByte)) << "第一条流没有开出来";
        const std::int64_t blockedStreamId = client.lastOpenedStreamId();
        // 这一条流收到数据也不还窗口：服务端发满它的 4 KiB 流窗口后就再也发不动——
        // 正是「一条流被流控挡住」这条前提
        client.setStreamToKeepFlowControlBlocked(blockedStreamId);

        // 等第一条流被服务端收下并排上大响应（服务端此时已撞上 4 KiB 的流窗口）
        ASSERT_TRUE(pumpUntil(client, [&firstStreamId] { return firstStreamId.load(std::memory_order_acquire) != -1; }))
                << "服务端没有收到第一条流的字节";

        // 再开一条流：它的响应必须出得去，而不是被第一条流的窗口堵死
        ASSERT_TRUE(client.openStreamAndQueuePayload(requestByte)) << "第二条流没有开出来";
        const std::int64_t laterStreamId = client.lastOpenedStreamId();
        ASSERT_NE(laterStreamId, blockedStreamId);

        const bool isDelivered = pumpUntil(client, [&client, laterStreamId, &smallPayload]
                                         { return client.receivedPayloadOn(laterStreamId) == smallPayload; });
        EXPECT_TRUE(isDelivered) << "一条流被流控挡住后，同连接其它流的响应一字节都没出去（实现仍在空转选同一条流）";
        EXPECT_LT(client.receivedPayloadOn(blockedStreamId).size(), bigPayload.size())
                << "被挡住的那条流不该整份发完（用例前提：它的窗口只有 4 KiB）";
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
/**
 * @brief drain 会等在途做完再收口全部连接，并且此后不再服务任何新连接
 * @details 关停路径最怕两件事：把还在做事的连接当场掐掉，以及「以为收了」其实连接表还在。
 *          这条用例两头都钉：握手完成的连接被 drain 收掉（计数归零），随后一条新连接的握手
 *          在时限内完不成
 */
TEST(QuicServer, DrainClosesOpenConnectionsAndStopsServing)
{
    ASSERT_TRUE(Platform::Socket::initialize());

    // 协程帧的存放声明在 fixture 之前：循环停止（fixture 析构）之后才轮到它销毁
    std::optional<Core::Task<>> drainTask;

    RunningQuicServer server;
    ASSERT_NE(server.listeningPort(), 0) << "服务端没有绑定成功";

    QuicTestClient client;
    ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
    ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "用例前提：先让一条连接握手完成";
    ASSERT_EQ(server.sampleConnectionCount(), 1U);

    drainTask.emplace(server.server().drain(std::chrono::milliseconds{1500}));
    // 本服务器只在其所属循环上被触碰：排入 drain 这个动作本身也投递过去做
    Core::Task<> *const drainTaskPointer = &drainTask.value();
    server.runOnLoopAndWait([drainTaskPointer]
                            {
                                // 这次已经在循环线程上，直接首次恢复就是「归属线程内」的合法调用；
                                // 之后它挂在定时器上，由循环自己唤醒
                                drainTaskPointer->handle().resume();
                            });

    const auto drainedDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (server.sampleConnectionCount() != 0 && std::chrono::steady_clock::now() < drainedDeadline)
    {
        client.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_EQ(server.sampleConnectionCount(), 0U) << "drain 返回后不应再留任何连接";

    QuicTestClient lateClient;
    ASSERT_TRUE(lateClient.initialize(makeServerAddress(server.listeningPort())));
    EXPECT_FALSE(pumpUntil(lateClient, [&lateClient] { return lateClient.isHandshakeCompleted(); }))
            << "服务端已经收口，不该再接手新连接";
}

/**
 * @brief 单来源并发上限对 h3 同样生效：超出的那条握手建不起连接
 * @details TcpServer 早就有这道闸（--max-connections-per-ip），h3 此前完全不按来源计数，
 *          于是同一个来源换条协议就能绕过限额
 */
TEST(QuicServer, RefusesNewConnectionBeyondPerIpLimit)
{
    ASSERT_TRUE(Platform::Socket::initialize());

    RunningQuicServer server(std::chrono::seconds{30}, std::make_shared<PerIpConnectionLimiter>(1));
    ASSERT_NE(server.listeningPort(), 0) << "服务端没有绑定成功";

    QuicTestClient firstClient;
    ASSERT_TRUE(firstClient.initialize(makeServerAddress(server.listeningPort())));
    ASSERT_TRUE(pumpUntil(firstClient, [&firstClient] { return firstClient.isHandshakeCompleted(); })) << "用例前提：第一条连接要能握手";
    ASSERT_EQ(server.sampleConnectionCount(), 1U);

    QuicTestClient secondClient;
    ASSERT_TRUE(secondClient.initialize(makeServerAddress(server.listeningPort())));
    EXPECT_FALSE(pumpUntil(secondClient, [&secondClient] { return secondClient.isHandshakeCompleted(); }))
            << "同一来源的第二条连接应当被限额器挡在门外";
    EXPECT_EQ(server.sampleConnectionCount(), 1U) << "被拒的握手不该留下连接记录";
}

/**
 * @brief 收口连接时会给对端一个 CONNECTION_CLOSE，而不是让对端干等
 * @details 只置标志的收口对端什么都收不到，只能等自己的空闲超时——这对「排空后关闭」与
 *          「会话不可用即关」两条路径都成立，是关停语义的最后一环
 */
TEST(QuicServer, AnnouncesConnectionCloseWhenDraining)
{
    ASSERT_TRUE(Platform::Socket::initialize());

    // 协程帧的存放声明在 fixture 之前：drain 里会 co_await 发包，帧必须活过循环停止
    std::optional<Core::Task<>> drainTask;

    RunningQuicServer server;
    ASSERT_NE(server.listeningPort(), 0) << "服务端没有绑定成功";

    QuicTestClient client;
    ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
    ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "用例前提：先让连接握手完成";
    ASSERT_FALSE(client.isPeerClosing()) << "刚握手完不该已处于收口态";

    drainTask.emplace(server.server().drain(std::chrono::milliseconds{0}));
    Core::Task<> *const drainTaskPointer = &drainTask.value();
    server.runOnLoopAndWait([drainTaskPointer] { drainTaskPointer->handle().resume(); });

    EXPECT_TRUE(pumpUntil(client, [&client] { return client.isPeerClosing(); }))
            << "服务端收口时对端没收到任何 CONNECTION_CLOSE，只能等自己的空闲超时";
    EXPECT_EQ(server.sampleConnectionCount(), 0U) << "收口后连接表应当清空";
}

/**
 * @brief 服务端收口一条流时，那份 RESET_STREAM / STOP_SENDING 要能被另一个实现接受
 * @details 单元用例只能证明「自己编的自己解得回去」；收尾长度这类字段是否合规范，只有别的实现
 *          愿意收才算数。这里让服务端一收到数据就把那条流两头收掉，客户端（ngtcp2）必须既不报
 *          读错、又看到这条流以指定错误码收口
 */
TEST(QuicServer, AnnouncesStreamAbortToCrossImplementationClient)
{
    ASSERT_TRUE(Platform::Socket::initialize());

    RunningQuicServer server;
    ASSERT_NE(server.listeningPort(), 0) << "服务端没有绑定成功";

    std::atomic<QuicConnection *> observedConnection{};
    std::atomic<std::int64_t>     observedStreamId{-1};
    server.server().setStreamDataHandler(
            [&observedConnection, &observedStreamId](QuicConnection &connection, const std::int64_t streamId,
                                                    const std::span<const std::uint8_t> /*data*/, const bool /*isEndStream*/)
            {
                observedStreamId.store(streamId, std::memory_order_release);
                // 错误码取 RFC 9114 §8.1 里那一档的一个值：这里只关心线上那份帧对不对，不套 h3 语义
                connection.abortStream(streamId, 0x010b);
                // 标记放在收口之后：用例据此保证「等到的那一趟里，宣告已经排进待发」
                observedConnection.store(&connection, std::memory_order_release);
            });

    QuicTestClient client;
    ASSERT_TRUE(client.initialize(makeServerAddress(server.listeningPort())));
    ASSERT_TRUE(pumpUntil(client, [&client] { return client.isHandshakeCompleted(); })) << "握手没有完成";

    const std::vector<std::uint8_t> payload{'p', 'i', 'n', 'g'};
    ASSERT_TRUE(client.openStreamAndQueuePayload(payload)) << "客户端流没开出来";
    ASSERT_TRUE(pumpUntil(client, [&observedConnection] { return observedConnection.load(std::memory_order_acquire) != nullptr; }))
            << "服务端没把流数据交给回调";
    const std::int64_t abortedStreamId = observedStreamId.load(std::memory_order_acquire);
    ASSERT_GE(abortedStreamId, 0) << "用例前提：回调里确实看到了一条流";

    ASSERT_TRUE(pumpUntil(client, [&] { return client.closedStreamErrorCodeOf(abortedStreamId).has_value(); }))
            << "客户端没接受服务端收口这条流：那份帧不合裁判的意（读错标志 " << (client.hasReadError() ? "有" : "无") << "）";
    EXPECT_EQ(client.closedStreamErrorCodeOf(abortedStreamId).value_or(0U), 0x010bU) << "对端看到的收口错误码";
    EXPECT_FALSE(client.hasReadError()) << "客户端读这些帧时出错：编码不合 ngtcp2 的裁判";
}

/**
 * @brief 钉住：构造期证书或私钥不合规时当场抛出，且不留下一份无人认领的 TLS 上下文
 * @details SSL_CTX 是构造里第一件拿到的资源，其后四道检查任一不过都抛——那时析构函数不会跑，
 *          旧写法存在成员里的裸指针就此没人负责（实测一份 1784 字节直漏 + 连带 35 KiB 的间接量，
 *          每次构造失败漏一整份）。判据分两层：抛与不抛由这里钉，漏与不漏由容器的 LSan 门禁钉，
 *          两层缺一都挡不住「改成不抛而是吞掉」这种倒退。
 */
TEST(QuicServer, ReleasesTlsContextWhenCertificateValidationFailsDuringConstruction)
{
    // 构造与销毁都在本线程：这个循环没交给别的线程，本线程就是它的归属线程
    Core::EventLoop loop;

    QuicServer::Configuration missingCertificate;
    missingCertificate.certificateFile = std::string(TEST_FIXTURES_DIR) + "/no-such-cert.pem";
    missingCertificate.privateKeyFile  = privateKeyPath();
    EXPECT_THROW(QuicServer server(loop, missingCertificate), Base::SystemException);

    QuicServer::Configuration missingPrivateKey;
    missingPrivateKey.certificateFile = certificatePath();
    missingPrivateKey.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/no-such-key.pem";
    EXPECT_THROW(QuicServer server(loop, missingPrivateKey), Base::SystemException);

    // 两个文件各自都在、也能各自加载，只有配对检查拦得住：这一道是构造里最后一道抛点
    QuicServer::Configuration mismatchedPair;
    mismatchedPair.certificateFile = certificatePath();
    mismatchedPair.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/test_ip_key.pem";
    EXPECT_THROW(QuicServer server(loop, mismatchedPair), Base::SystemException);

    // 反复构造失败也要抛，且不留残留：以上三道各来一轮，退出时 LSan 不许报任何东西
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        EXPECT_THROW(QuicServer server(loop, missingCertificate), Base::SystemException);
    }
}

} // namespace AsynGyanis::Net
