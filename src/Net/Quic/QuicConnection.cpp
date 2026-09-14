#include "Net/Quic/QuicConnection.h"

#include "Base/Log/LogMacros.h"
#include "Platform/IO/DatagramSocket.h"

#include <cstring>

#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/rand.h>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端连接标识长度：够随机，又不会把报文头撑大
        constexpr std::size_t kConnectionIdLength = 18;

        /// 一次 flush 里最多写出多少条报文：防止窗口充裕时在一条连接上转太久
        constexpr std::size_t kMaximumPacketsPerFlush = 64;

        /**
         * @brief 当前单调时钟的纳秒读数
         * @details ngtcp2 的时间戳单位就是纳秒；用单调时钟而不是墙上时钟，避免系统时间跳变影响 PTO
         * @return ngtcp2_tstamp 纳秒计数
         */
        ngtcp2_tstamp currentTimestamp() noexcept
        {
            return static_cast<ngtcp2_tstamp>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /**
         * @brief 把回调拿到的 user_data 还原成 C++ 对象
         * @details user_data 是建立 ngtcp2 连接时随参数传进去的本对象指针（ngtcp2 会原样交给每个回调）
         * @param userData ngtcp2 交回的 user_data
         * @return QuicConnection* 对应的对象
         */
        QuicConnection *fromNative(void *const userData) noexcept
        {
            return static_cast<QuicConnection *>(userData);
        }

        /**
         * @brief 随机数回调：连接标识与各类随机量都由它供给
         * @note 随机失败没有安全降级路径（QUIC 的安全性建立在随机数上），这里写成全零让上层尽快失败
         */
        void randomBytesCallback(std::uint8_t *const destination, const std::size_t destinationLength, const ngtcp2_rand_ctx *) noexcept
        {
            if (RAND_bytes(destination, static_cast<int>(destinationLength)) != 1)
            {
                std::memset(destination, 0, destinationLength);
            }
        }

        /**
         * @brief 轮换连接标识时生成新标识与配套的无状态重置令牌
         */
        int newConnectionIdCallback(ngtcp2_conn *, ngtcp2_cid *const cid, ngtcp2_stateless_reset_token *const token,
                                    const std::size_t cidLength, void *const userData) noexcept
        {
            if (cidLength == 0 || cidLength > NGTCP2_MAX_CIDLEN)
            {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }

            std::vector<std::uint8_t> randomConnectionId(cidLength);
            if (RAND_bytes(randomConnectionId.data(), static_cast<int>(randomConnectionId.size())) != 1)
            {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            ngtcp2_cid_init(cid, randomConnectionId.data(), randomConnectionId.size());

            const QuicConnection *self = fromNative(userData);
            if (ngtcp2_crypto_generate_stateless_reset_token(token->data, self->statelessResetSecret().data(),
                                                             self->statelessResetSecret().size(), cid) != 0)
            {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            return 0;
        }

        /**
         * @brief 收到流数据：交给应用层（HTTP/3 层或首个里程碑的回显）
         */
        int receiveStreamDataCallback(ngtcp2_conn *, const std::uint32_t flags, const std::int64_t streamId, const std::uint64_t,
                                      const std::uint8_t *const data, const std::size_t dataLength, void *const userData, void *) noexcept
        {
            fromNative(userData)->deliverStreamData(streamId, std::span<const std::uint8_t>(data, dataLength),
                                                    (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0);
            return 0;
        }

        /// 流数据被确认送达：发送侧进度由本类的写循环自己记，这里只需报成功
        int acknowledgedStreamDataCallback(ngtcp2_conn *, const std::int64_t, const std::uint64_t, const std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 对端开了一条流：服务端不需要额外记账，收数据时带流号就够
        int streamOpenCallback(ngtcp2_conn *, const std::int64_t, void *) noexcept
        {
            return 0;
        }

        /// 一条流结束：把该流尚未发完的排队数据丢掉（对端已经不要了）
        int streamCloseCallback(ngtcp2_conn *, const std::uint32_t, const std::int64_t streamId, const std::uint64_t, const std::uint64_t,
                                void *const userData, void *) noexcept
        {
            fromNative(userData)->dropPendingStreamData(streamId);
            return 0;
        }

        /// 对端重置了一条流：同样丢掉该流的待发数据
        int streamResetCallback(ngtcp2_conn *, const std::int64_t streamId, const std::uint64_t, const std::uint64_t, void *const userData,
                                void *) noexcept
        {
            fromNative(userData)->dropPendingStreamData(streamId);
            return 0;
        }

        /// 对端要求本端停止发送：本端据此收手
        int stopSendingCallback(ngtcp2_conn *, const std::int64_t streamId, const std::uint64_t, void *const userData, void *) noexcept
        {
            fromNative(userData)->dropPendingStreamData(streamId);
            return 0;
        }

        /// 对端可以再开更多双向流了：流控由传输参数给出，无需追加通知
        int extendMaxRemoteStreamsBidiCallback(ngtcp2_conn *, const std::uint64_t, void *) noexcept
        {
            return 0;
        }

        /// 对端为我方某条流放宽了发送窗口：写循环本来就会重试，无需额外动作
        int extendMaxStreamDataCallback(ngtcp2_conn *, const std::int64_t, const std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 路径校验结束：服务端只用一条路径，直接接受
        int pathValidationCallback(ngtcp2_conn *, const std::uint32_t, const ngtcp2_path *, const ngtcp2_path *,
                                  const ngtcp2_path_validation_result, void *) noexcept
        {
            return 0;
        }

        /// 连接标识可以退休了：按 SCID 路由的那张表由服务端维护，这里无需动作
        int removeConnectionIdCallback(ngtcp2_conn *, const ngtcp2_cid *, void *) noexcept
        {
            return 0;
        }

        /// 握手完成：记一条日志（HTTP/3 层在下一个切片里会在这里初始化自己的会话）
        int handshakeCompletedCallback(ngtcp2_conn *, void *const userData) noexcept
        {
            const QuicConnection *self               = fromNative(userData);
            const unsigned char  *selectedProtocol   = nullptr;
            unsigned int          selectedProtocolLength = 0;
            self->selectedApplicationProtocol(selectedProtocol, selectedProtocolLength);
            LOG_INFO_FMT("QuicConnection: QUIC 握手完成（连接标识 {} 字节，协商协议 {}）", self->sourceConnectionId().size(),
                         selectedProtocol != nullptr ? std::string(reinterpret_cast<const char *>(selectedProtocol), selectedProtocolLength)
                                                     : std::string("(未协商)"));
            return 0;
        }
    } // namespace

    std::unique_ptr<QuicConnection> QuicConnection::accept(const Configuration &configuration, const Platform::SocketAddress &localAddress,
                                                           const Platform::SocketAddress &peerAddress,
                                                           const std::span<const std::uint8_t> clientInitial)
    {
        if (configuration.tlsContext == nullptr || !configuration.sendDatagram || configuration.statelessResetSecret.empty())
        {
            LOG_ERROR("QuicConnection: 连接配置不完整（缺 SSL_CTX、报文出口或重置密钥），连接未建立");
            return nullptr;
        }

        // 先按 ngtcp2 的规则判断这条报文能否起一条新连接（非 Initial、版本不支持、长度不足都会在这里被挡掉）
        ngtcp2_pkt_hd header{};
        if (ngtcp2_accept(&header, clientInitial.data(), clientInitial.size()) != 0)
        {
            LOG_DEBUG("QuicConnection: 报文不可接受（不是可开新连接的 Initial），已丢弃");
            return nullptr;
        }

        std::unique_ptr<QuicConnection> connection(new QuicConnection(configuration));
        connection->m_peerAddress  = peerAddress;
        connection->m_localAddress = localAddress;

        // 本端连接标识由本端生成：对端之后用报文里的目的连接标识指向它
        std::vector<std::uint8_t> sourceConnectionIdBytes(kConnectionIdLength);
        if (RAND_bytes(sourceConnectionIdBytes.data(), static_cast<int>(sourceConnectionIdBytes.size())) != 1)
        {
            LOG_ERROR("QuicConnection: 生成连接标识失败（随机数不可用），连接未建立");
            return nullptr;
        }
        ngtcp2_cid_init(&connection->m_sourceConnectionId, sourceConnectionIdBytes.data(), sourceConnectionIdBytes.size());

        ngtcp2_path_storage_init(&connection->m_path, reinterpret_cast<const sockaddr *>(&connection->m_localAddress.storage),
                                 connection->m_localAddress.length, reinterpret_cast<const sockaddr *>(&connection->m_peerAddress.storage),
                                 connection->m_peerAddress.length, nullptr);

        static constexpr ngtcp2_callbacks callbacks = ngtcp2_callbacks{
                .recv_client_initial            = ngtcp2_crypto_recv_client_initial_cb,
                .recv_crypto_data               = ngtcp2_crypto_recv_crypto_data_cb,
                .handshake_completed            = handshakeCompletedCallback,
                .encrypt                        = ngtcp2_crypto_encrypt_cb,
                .decrypt                        = ngtcp2_crypto_decrypt_cb,
                .hp_mask                        = ngtcp2_crypto_hp_mask_cb,
                .recv_stream_data               = receiveStreamDataCallback,
                .acked_stream_data_offset       = acknowledgedStreamDataCallback,
                .stream_open                    = streamOpenCallback,
                .rand                           = randomBytesCallback,
                .remove_connection_id           = removeConnectionIdCallback,
                .update_key                     = ngtcp2_crypto_update_key_cb,
                .path_validation                = pathValidationCallback,
                .stream_reset                   = streamResetCallback,
                .extend_max_remote_streams_bidi = extendMaxRemoteStreamsBidiCallback,
                .extend_max_stream_data         = extendMaxStreamDataCallback,
                .delete_crypto_aead_ctx         = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
                .delete_crypto_cipher_ctx       = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
                .stream_stop_sending            = stopSendingCallback,
                .version_negotiation            = ngtcp2_crypto_version_negotiation_cb,
                .get_new_connection_id2         = newConnectionIdCallback,
                .get_path_challenge_data2       = ngtcp2_crypto_get_path_challenge_data2_cb,
                .stream_close2                  = streamCloseCallback,
        };

        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts        = currentTimestamp();
        settings.cc_algo           = NGTCP2_CC_ALGO_CUBIC;
        settings.initial_rtt       = NGTCP2_DEFAULT_INITIAL_RTT;
        settings.max_window        = 24u * 1024u * 1024u;
        settings.max_stream_window = 8u * 1024u * 1024u;
        settings.handshake_timeout = static_cast<ngtcp2_duration>(configuration.idleTimeout.count()) * NGTCP2_MILLISECONDS;

        ngtcp2_transport_params parameters;
        ngtcp2_transport_params_default(&parameters);
        parameters.initial_max_stream_data_bidi_local  = 256u * 1024u;
        parameters.initial_max_stream_data_bidi_remote = 256u * 1024u;
        parameters.initial_max_stream_data_uni         = 256u * 1024u;
        parameters.initial_max_data                    = 1024u * 1024u;
        parameters.initial_max_streams_bidi            = 100;
        parameters.initial_max_streams_uni             = 100;
        parameters.max_idle_timeout                    = static_cast<ngtcp2_duration>(configuration.idleTimeout.count()) * NGTCP2_MILLISECONDS;
        parameters.active_connection_id_limit          = 7;
        parameters.grease_quic_bit                     = 1;

        // 「原始目的连接标识」与无状态重置令牌是服务端必须给的：对端据此把重置报文与这条连接对上
        parameters.original_dcid         = header.dcid;
        parameters.original_dcid_present = 1;
        if (ngtcp2_crypto_generate_stateless_reset_token(parameters.stateless_reset_token, configuration.statelessResetSecret.data(),
                                                         configuration.statelessResetSecret.size(),
                                                         &connection->m_sourceConnectionId) != 0)
        {
            LOG_ERROR("QuicConnection: 生成无状态重置令牌失败，连接未建立");
            return nullptr;
        }

        // 第三个参数是「本端发报文时填的目的连接标识」，ngtcp2 要求它取自对端 Initial 的**源**连接标识
        // （RFC 9000 §7.2 规定服务端的回包必须打到客户端自报的 SCID 上）。填成报文里的目的连接标识
        // 会让本端把所有回包送到一个客户端从未公布过的标识上，且此后每条重传的 Initial 都会因为
        // 「源连接标识对不上」被判为无主报文丢掉（ngtcp2 在首个 Initial 处理完后会校验 hd.scid）
        if (ngtcp2_conn_server_new(&connection->m_connection, &header.scid, &connection->m_sourceConnectionId, &connection->m_path.path,
                                   header.version, &callbacks, &settings, &parameters, nullptr, connection.get()) != 0)
        {
            LOG_ERROR("QuicConnection: ngtcp2 连接创建失败，连接未建立");
            return nullptr;
        }

        connection->m_tlsSession = SSL_new(configuration.tlsContext);
        if (connection->m_tlsSession == nullptr)
        {
            LOG_ERROR("QuicConnection: TLS 会话创建失败，连接未建立");
            return nullptr;
        }
        SSL_set_accept_state(connection->m_tlsSession);
        if (ngtcp2_crypto_ossl_configure_server_session(connection->m_tlsSession) != 0)
        {
            LOG_ERROR("QuicConnection: TLS 会话无法配置为 QUIC 服务端模式，连接未建立");
            return nullptr;
        }

        // ossl 后端要求把「每连接的加密上下文」交给 ngtcp2（不是裸 SSL）：数据保护与密钥轮换都经它
        if (ngtcp2_crypto_ossl_ctx_new(&connection->m_cryptoContext, connection->m_tlsSession) != 0)
        {
            LOG_ERROR("QuicConnection: 创建 QUIC 加密上下文失败，连接未建立");
            return nullptr;
        }
        ngtcp2_conn_set_tls_native_handle(connection->m_connection, connection->m_cryptoContext);

        // 反向引用同样不能少：crypto 胶水的回调是从 SSL 出发的，它把 app data 当 ngtcp2_crypto_conn_ref
        // 调用取回 ngtcp2 连接。这一步缺了，握手会停在原地——TLS 数据交不进 ngtcp2，双方都等对方
        connection->m_cryptoConnectionReference.user_data = connection.get();
        connection->m_cryptoConnectionReference.get_conn  = [](ngtcp2_crypto_conn_ref *reference) -> ngtcp2_conn *
        {
            return static_cast<QuicConnection *>(reference->user_data)->m_connection;
        };
        SSL_set_app_data(connection->m_tlsSession, &connection->m_cryptoConnectionReference);

        LOG_DEBUG_FMT("QuicConnection: 已接受一条连接（连接标识 {} 字节，版本 0x{:08x}）", connection->m_sourceConnectionId.datalen,
                      header.version);
        return connection;
    }

    QuicConnection::QuicConnection(Configuration configuration) :
        m_configuration(std::move(configuration))
    {
    }

    QuicConnection::~QuicConnection()
    {
        // 顺序有讲究：先删 ngtcp2 连接（回调都指向后两者），再删加密上下文与 TLS 会话
        if (m_connection != nullptr)
        {
            ngtcp2_conn_del(m_connection);
            m_connection = nullptr;
        }
        if (m_cryptoContext != nullptr)
        {
            ngtcp2_crypto_ossl_ctx_del(m_cryptoContext);
            m_cryptoContext = nullptr;
        }
        if (m_tlsSession != nullptr)
        {
            SSL_free(m_tlsSession);
            m_tlsSession = nullptr;
        }
    }

    std::string QuicConnection::sourceConnectionId() const
    {
        return std::string(reinterpret_cast<const char *>(m_sourceConnectionId.data), m_sourceConnectionId.datalen);
    }

    std::vector<std::string> QuicConnection::sourceConnectionIds() const
    {
        if (m_connection == nullptr)
        {
            return {};
        }

        // 先用空目标问一次条数，再按条数备好缓冲——ngtcp2 要求缓冲正好容纳 sizeof(ngtcp2_cid) * n 个
        const std::size_t issuedCount = ngtcp2_conn_get_scid2(m_connection, nullptr);
        if (issuedCount == 0)
        {
            return {};
        }

        std::vector<ngtcp2_cid> nativeConnectionIds(issuedCount);
        ngtcp2_conn_get_scid2(m_connection, nativeConnectionIds.data());

        std::vector<std::string> connectionIds;
        connectionIds.reserve(nativeConnectionIds.size());
        for (const ngtcp2_cid &nativeConnectionId: nativeConnectionIds)
        {
            connectionIds.emplace_back(reinterpret_cast<const char *>(nativeConnectionId.data), nativeConnectionId.datalen);
        }
        return connectionIds;
    }

    const std::vector<std::uint8_t> &QuicConnection::statelessResetSecret() const noexcept
    {
        return m_configuration.statelessResetSecret;
    }

    void QuicConnection::deliverStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
    {
        if (m_configuration.onStreamData)
        {
            m_configuration.onStreamData(streamId, data, isEndStream);
        }
    }

    void QuicConnection::dropPendingStreamData(const std::int64_t streamId)
    {
        m_pendingStreamData.erase(streamId);
    }

    void QuicConnection::selectedApplicationProtocol(const unsigned char *&protocol, unsigned int &protocolLength) const noexcept
    {
        protocol       = nullptr;
        protocolLength = 0;
        if (m_tlsSession != nullptr)
        {
            SSL_get0_alpn_selected(m_tlsSession, &protocol, &protocolLength);
        }
    }

    void QuicConnection::queueStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
    {
        PendingStreamData &pending = m_pendingStreamData[streamId];
        pending.bytes.append(reinterpret_cast<const char *>(data.data()), data.size());
        pending.isEndStream = pending.isEndStream || endStream;
    }

    Core::Task<> QuicConnection::handleDatagram(const Platform::SocketAddress &peerAddress, const std::span<const std::uint8_t> datagram)
    {
        if (m_connection == nullptr)
        {
            co_return;
        }

        // 对端地址变了（NAT 重绑定）：QUIC 允许迁移，把新地址写回 path 再继续
        if (peerAddress.length != m_peerAddress.length ||
            std::memcmp(&peerAddress.storage, &m_peerAddress.storage, peerAddress.length) != 0)
        {
            m_peerAddress = peerAddress;
            ngtcp2_path_storage_zero(&m_path);
            ngtcp2_path_storage_init(&m_path, reinterpret_cast<const sockaddr *>(&m_localAddress.storage), m_localAddress.length,
                                     reinterpret_cast<const sockaddr *>(&m_peerAddress.storage), m_peerAddress.length, nullptr);
        }

        ngtcp2_pkt_info packetInfo{};
        if (const int result = ngtcp2_conn_read_pkt(m_connection, &m_path.path, &packetInfo, datagram.data(), datagram.size(),
                                                    currentTimestamp());
            result != 0)
        {
            // 读失败多为对端违规或握手期的临时问题：记一条日志，待发字节（通常是 CONNECTION_CLOSE）由 flush 送出去
            LOG_WARN_FMT("QuicConnection: 报文处理失败（ngtcp2 错误 {}），连接将按协议收口", ngtcp2_strerror(result));
        }
        co_await flush();
    }

    Core::Task<> QuicConnection::flush()
    {
        if (m_connection == nullptr)
        {
            co_return;
        }

        std::vector<std::uint8_t> packetBuffer(Platform::DatagramSocket::kMaximumDatagramBytes);
        for (std::size_t packetIndex = 0; packetIndex < kMaximumPacketsPerFlush; ++packetIndex)
        {
            // 一次只带一条流的数据：ngtcp2 的写接口按流给数据；没有流数据可带时用流号 -1 写控制帧
            ngtcp2_vec   dataVector{};
            ngtcp2_vec  *dataVectors     = nullptr;
            std::size_t  dataVectorCount = 0;
            std::int64_t streamId        = -1;
            std::uint32_t flags          = NGTCP2_WRITE_STREAM_FLAG_NONE;

            for (auto &pendingEntry: m_pendingStreamData)
            {
                PendingStreamData &pending = pendingEntry.second;
                if (pending.offset >= pending.bytes.size())
                {
                    continue;
                }
                streamId        = pendingEntry.first;
                dataVector.base = reinterpret_cast<std::uint8_t *>(pending.bytes.data() + pending.offset);
                dataVector.len  = pending.bytes.size() - pending.offset;
                dataVectors     = &dataVector;
                dataVectorCount = 1;
                if (pending.isEndStream)
                {
                    flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
                }
                break;
            }

            ngtcp2_pkt_info    packetInfo{};
            ngtcp2_ssize       writtenStreamDataLength = 0;
            const ngtcp2_ssize writtenLength =
                    ngtcp2_conn_writev_stream(m_connection, &m_path.path, &packetInfo, packetBuffer.data(), packetBuffer.size(),
                                              &writtenStreamDataLength, flags, streamId, dataVectors, dataVectorCount, currentTimestamp());
            if (writtenLength < 0)
            {
                // 这几种都是常态（窗口不够、流已收尾、还有更多数据待写），继续下一轮
                if (writtenLength == NGTCP2_ERR_STREAM_DATA_BLOCKED || writtenLength == NGTCP2_ERR_STREAM_SHUT_WR ||
                    writtenLength == NGTCP2_ERR_WRITE_MORE)
                {
                    continue;
                }
                m_isClosed = true;
                LOG_WARN_FMT("QuicConnection: 写出失败（ngtcp2 错误 {}），连接收口", ngtcp2_strerror(writtenLength));
                co_return;
            }
            if (writtenLength == 0)
            {
                // 没有待发字节：本轮 flush 结束
                co_return;
            }

            if (streamId != -1 && writtenStreamDataLength > 0)
            {
                m_pendingStreamData[streamId].offset += static_cast<std::size_t>(writtenStreamDataLength);
                if (m_pendingStreamData[streamId].offset >= m_pendingStreamData[streamId].bytes.size() &&
                    !m_pendingStreamData[streamId].isEndStream)
                {
                    m_pendingStreamData.erase(streamId);
                }
            }

            if (!co_await m_configuration.sendDatagram(m_peerAddress, packetBuffer.data(), static_cast<std::size_t>(writtenLength)))
            {
                LOG_WARN("QuicConnection: 报文发送失败（对端可能已不可达），连接收口");
                co_return;
            }
        }
    }

    std::chrono::steady_clock::time_point QuicConnection::nextExpiry() const noexcept
    {
        if (m_connection == nullptr)
        {
            return std::chrono::steady_clock::time_point::max();
        }
        // UINT64_MAX 表示当前没有定时器（没有在途数据、也还没到空闲超时）
        if (const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(m_connection); expiry != UINT64_MAX)
        {
            return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(expiry));
        }
        return std::chrono::steady_clock::time_point::max();
    }

    Core::Task<> QuicConnection::handleExpiry()
    {
        if (m_connection == nullptr)
        {
            co_return;
        }

        if (const int result = ngtcp2_conn_handle_expiry(m_connection, currentTimestamp()); result != 0)
        {
            if (result == NGTCP2_ERR_IDLE_CLOSE)
            {
                m_isClosed = true;
                LOG_DEBUG("QuicConnection: 空闲超时，连接收口");
            } else
            {
                LOG_WARN_FMT("QuicConnection: 定时器处理失败（ngtcp2 错误 {}），连接收口", ngtcp2_strerror(result));
            }
            co_return;
        }
        co_await flush();
    }

    bool QuicConnection::isClosed() const noexcept
    {
        if (m_connection == nullptr || m_isClosed)
        {
            return true;
        }
        // 收口期与排空期都表示这条连接不再服务新数据；两者之外还要看本地标志，
        // 因为「空闲超时」这类收口在 ngtcp2 里未必立刻把状态推到收口期
        return ngtcp2_conn_in_closing_period(m_connection) != 0 || ngtcp2_conn_in_draining_period(m_connection) != 0;
    }
} // namespace AsynGyanis::Net
