// TestQuicConnectionCore.cpp —— QUIC 连接状态机用例
//
// 这块不需要 socket、不需要事件循环、也不需要真实网络：测试里带一个「最小客户端」
// （`InMemoryQuicClient`），它只做组包与解包，跨包状态自己记，把 `QuicConnectionCore` 当对端来打。
// 于是整条链（组包器 + 报文头/帧编解码 + 包保护 + TLS 胶水 + 状态机）在一次单测里跑完，断言全落在字节上。
//
// 覆盖：
//   1) 端到端握手到 Established，且对端传输参数被解出并过了 §7.3 的连接标识绑定校验；
//   2) 确认帧：服务端收到触发确认的包后要回 ACK，确认的就是那些包号，延迟字段按本端指数换算；
//   3) 该丢的丢：目的标识不合、垃圾字节、空数据报，都不报错也不产出；
//   4) 乱序 CRYPTO 分片先缓存、凑齐再按序喂 TLS（§19.6 + §7.5）——只喂后半段时服务端必须没反应；
//   5) 对端参数里的 initial_source_connection_id 与实际收到的不符 → 发 CONNECTION_CLOSE 收口；
//   6) HANDSHAKE_DONE 只在「握手完成 + 对端确认过 Handshake 空间的包」之后发且只发一次（§19.20）。
// 证书用仓库内的自签夹具（与 HTTPS、TLS 胶水用例同一份），因此不依赖任何外部服务。

#include "Net/Quic/QuicConnectionCore.h"

#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/Quic/Crypto/QuicHeaderProtection.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "Net/Quic/Crypto/QuicPacketKeys.h"
#include "Net/Quic/Crypto/QuicPacketProtection.h"
#include "Net/Quic/Crypto/QuicTlsContext.h"
#include "Net/Quic/QuicPacketBuilder.h"
#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;
        using Timestamp = QuicConnectionCore::Timestamp;

        /// 客户端自报的源连接标识（也是它参数里 ISCID 该填的值）
        const std::vector<std::uint8_t> kClientConnectionId = makeBytesFromHex("0610c9aedfc9e2edb1");
        /// 客户端凭空造的目的标识：Initial 密钥由它推导，服务端参数里的 ODCID 也是它
        const std::vector<std::uint8_t> kOriginalDestinationConnectionId = makeBytesFromHex("2b7e1234fab5d789");
        /// 本测试里服务端签发的连接标识
        const std::vector<std::uint8_t> kServerConnectionId = makeBytesFromHex("f0eec687a7eb7f48c311");

        /// 一个明显不等于任何已签发标识的连接标识
        const std::vector<std::uint8_t> kUnknownConnectionId = makeBytesFromHex("1122334455667788");

        /**
         * @brief 测试夹具的 SSL 上下文持有者
         * @details 与 TLS 胶水用例同一套写法：服务端装仓库内的自签证书，客户端关掉校验才握得上手
         */
        class FixtureContext
        {
        public:
            FixtureContext() = default;
            FixtureContext(const FixtureContext &) = delete;
            FixtureContext &operator=(const FixtureContext &) = delete;
            FixtureContext(FixtureContext &&other) noexcept : m_context(other.m_context)
            {
                other.m_context = nullptr;
            }
            ~FixtureContext()
            {
                if (m_context != nullptr)
                {
                    SSL_CTX_free(m_context);
                }
            }

            /// @return FixtureContext 客户端上下文
            [[nodiscard]] static FixtureContext client()
            {
                FixtureContext holder;
                holder.m_context = SSL_CTX_new(TLS_client_method());
                SSL_CTX_set_min_proto_version(holder.m_context, TLS1_3_VERSION);
                SSL_CTX_set_verify(holder.m_context, SSL_VERIFY_NONE, nullptr);
                return holder;
            }

            /// @return FixtureContext 服务端上下文；证书加载失败交出空指针，由用例断言
            [[nodiscard]] static FixtureContext server()
            {
                FixtureContext holder;
                holder.m_context = SSL_CTX_new(TLS_server_method());
                SSL_CTX_set_min_proto_version(holder.m_context, TLS1_3_VERSION);
                const std::string certificatePath = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem").string();
                const std::string keyPath = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem").string();
                if (SSL_CTX_use_certificate_chain_file(holder.m_context, certificatePath.c_str()) != 1 ||
                    SSL_CTX_use_PrivateKey_file(holder.m_context, keyPath.c_str(), SSL_FILETYPE_PEM) != 1)
                {
                    SSL_CTX_free(holder.m_context);
                    holder.m_context = nullptr;
                }
                return holder;
            }

            [[nodiscard]] SSL_CTX *get() const noexcept
            {
                return m_context;
            }

        private:
            SSL_CTX *m_context{nullptr}; ///< 底层 OpenSSL 上下文
        };

        /// @return std::span<const std::uint8_t> 把字符串按二进制字节看；空串给空视图而不是空指针
        std::span<const std::uint8_t> asBytes(const std::string &text)
        {
            return text.empty() ? std::span<const std::uint8_t>{}
                                : std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        }

        /// 客户端一侧的包号空间：包保护密钥 + 发包号 + 已收包号
        struct ClientSpace
        {
            std::optional<QuicPacketKeys> readKeys{};
            std::optional<QuicPacketKeys> writeKeys{};
            std::uint64_t nextPacketNumber{0};
            std::uint64_t nextExpectedPacketNumber{0};
        };

        /**
         * @brief 测试用的最小客户端
         * @details 只够把一次 TLS 1.3 握手跑完：按序发包、不重传、默认不发 ACK。它同时充当观测探头——
         *          解出来的帧在这里记账，用例直接读计数，不在用例里再解一遍包。
         */
        class InMemoryQuicClient
        {
        public:
            /**
             * @brief 建客户端
             * @param context 客户端 SSL 上下文
             * @param sourceConnectionId 本端在第一个 Initial 里的源标识
             * @param mismatchedSourceConnectionId 要不要把参数里的 ISCID 写成别的值，用于 §7.3 绑定用例
             */
            InMemoryQuicClient(SSL_CTX &context, std::vector<std::uint8_t> sourceConnectionId,
                               const bool mismatchedSourceConnectionId = false)
                : m_sourceConnectionId(std::move(sourceConnectionId))
            {
                m_initial.readKeys = deriveQuicInitialPacketKeys(kOriginalDestinationConnectionId, QuicPacketDirection::ServerToClient);
                m_initial.writeKeys = deriveQuicInitialPacketKeys(kOriginalDestinationConnectionId, QuicPacketDirection::ClientToServer);

                QuicTransportParameters parameters;
                parameters.initialMaximumData = 65536;
                parameters.initialMaximumBidirectionalStreams = 128;
                parameters.initialSourceConnectionId = mismatchedSourceConnectionId ? kUnknownConnectionId : m_sourceConnectionId;
                std::string encoded;
                appendQuicTransportParameters(encoded, parameters);
                m_tls = std::make_unique<QuicTlsContext>(context, false, asBytes(encoded));
            }

            /// 把 TLS 交出的 Handshake 级密钥补进本端空间（读密钥即服务端的写密钥）
            void adoptKeys()
            {
                if (const QuicPacketKeys *reading = m_tls->keys(QuicEncryptionLevel::Handshake, QuicKeyDirection::Reading);
                    reading != nullptr && !m_handshake.readKeys.has_value())
                {
                    m_handshake.readKeys = *reading;
                }
                if (const QuicPacketKeys *writing = m_tls->keys(QuicEncryptionLevel::Handshake, QuicKeyDirection::Writing);
                    writing != nullptr && !m_handshake.writeKeys.has_value())
                {
                    m_handshake.writeKeys = *writing;
                }
            }

            /**
             * @brief 之后发出的包都打这个目的标识，用来构造「路由不命中」的报文
             * @param destinationConnectionId 目的标识；交空即回到正常行为
             */
            void overrideDestinationConnectionId(std::vector<std::uint8_t> destinationConnectionId)
            {
                m_destinationOverride = std::move(destinationConnectionId);
            }

            /// 推进 TLS 并把产出的握手字节编成一批数据报
            [[nodiscard]] std::vector<std::vector<std::uint8_t>> buildFlight()
            {
                std::ignore = m_tls->drive();
                std::vector<std::vector<std::uint8_t>> flight;
                while (const auto record = m_tls->takeOutboundRecord())
                {
                    if (record->level == QuicEncryptionLevel::Application)
                    {
                        continue; // 客户端在 1-RTT 没东西要发
                    }
                    ClientSpace &space = record->level == QuicEncryptionLevel::Initial ? m_initial : m_handshake;
                    if (!space.writeKeys.has_value())
                    {
                        continue;
                    }
                    QuicCryptoFrame crypto;
                    crypto.data = record->data;
                    std::string frames;
                    appendQuicFrame(frames, QuicFrame{crypto});
                    flight.push_back(buildDatagram(space, record->level, frames));
                }
                return flight;
            }

            /**
             * @brief 把第一条 Initial 里的握手字节按 [后半段, 前半段] 的顺序分成两个数据报
             * @details 用来验「乱序早到的 CRYPTO 必须先缓存」这条路：只发第一个返回值时 TLS 拿不到
             *          完整的 ClientHello，服务端不该有任何反应
             * @return 两个数据报：前一个是偏移靠后的分片，后一个是从头开始的分片
             */
            [[nodiscard]] std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> buildReversedCryptoFragments()
            {
                std::ignore = m_tls->drive();
                std::vector<std::uint8_t> handshakeBytes;
                while (const auto record = m_tls->takeOutboundRecord())
                {
                    handshakeBytes.insert(handshakeBytes.end(), record->data.begin(), record->data.end());
                }
                const std::size_t splitPoint = handshakeBytes.size() / 2;
                if (splitPoint == 0)
                {
                    // 客户端连一条握手字节都没产出，用例后续的断言自然会红，这里只保证不越界
                    return {};
                }

                std::vector<std::uint8_t> firstFragment(handshakeBytes.begin() + static_cast<std::ptrdiff_t>(splitPoint),
                                                        handshakeBytes.end());
                std::vector<std::uint8_t> secondFragment(handshakeBytes.begin(),
                                                         handshakeBytes.begin() + static_cast<std::ptrdiff_t>(splitPoint));
                return {makeCryptoDatagram(m_initial, QuicEncryptionLevel::Initial, splitPoint, firstFragment),
                        makeCryptoDatagram(m_initial, QuicEncryptionLevel::Initial, 0, secondFragment)};
            }

            /// 手工发一条只含 ACK 的 Handshake 包，用来触发服务端的 HANDSHAKE_DONE
            void acknowledgeServerHandshakePacket(const std::uint64_t acknowledgedPacketNumber)
            {
                QuicAcknowledgementFrame acknowledgement;
                acknowledgement.largestAcknowledgedPacketNumber = acknowledgedPacketNumber;
                acknowledgement.ranges = {{acknowledgedPacketNumber, acknowledgedPacketNumber}};
                std::string frames;
                appendQuicFrame(frames, QuicFrame{acknowledgement});
                m_lastSentHandshakeDatagram = buildDatagram(m_handshake, QuicEncryptionLevel::Handshake, frames);
            }

            [[nodiscard]] const std::vector<std::uint8_t> &lastSentHandshakeDatagram() const noexcept
            {
                return m_lastSentHandshakeDatagram;
            }

            /**
             * @brief 收一条服务端数据报：逐包解密、记账、把 CRYPTO 字节按级别喂给 TLS
             * @param datagram 服务端产出的一条数据报
             */
            void consume(const std::vector<std::uint8_t> &datagram)
            {
                const std::span<const std::uint8_t> bytes(datagram);
                std::size_t offset = 0;
                while (offset < bytes.size())
                {
                    const auto remainder = bytes.subspan(offset);
                    const auto decodedHeader = decodeQuicPacketHeader(remainder, m_serverConnectionId.size());
                    if (!decodedHeader.has_value() || decodedHeader->packetByteCount == 0 ||
                        offset + decodedHeader->packetByteCount > bytes.size())
                    {
                        return;
                    }
                    consumePacket(remainder, *decodedHeader);
                    offset += decodedHeader->packetByteCount;
                }
                // 真实实现是「喂到就推进」：ServerHello 一被处理完就有 Handshake 密钥，
                // 同一批里紧跟着的 Handshake 包才解得开
                std::ignore = m_tls->drive();
                adoptKeys();
            }

            [[nodiscard]] bool isHandshakeCompleted() const noexcept
            {
                return m_tls->isHandshakeCompleted();
            }

            /// 服务端 ACK 帧里的最大确认值，取最后一次看到的
            [[nodiscard]] const std::optional<std::uint64_t> &largestServerAcknowledged() const noexcept
            {
                return m_largestServerAcknowledged;
            }

            /// 服务端 ACK 帧里的延迟字段（线上值）
            [[nodiscard]] const std::optional<std::uint64_t> &serverAcknowledgementDelay() const noexcept
            {
                return m_serverAcknowledgementDelay;
            }

            /// 收到过的 HANDSHAKE_DONE 帧数：整轮只该是 0 或 1
            [[nodiscard]] std::size_t handshakeDoneFrameCount() const noexcept
            {
                return m_handshakeDoneFrameCount;
            }

            [[nodiscard]] bool sawConnectionClose() const noexcept
            {
                return m_sawConnectionClose;
            }

            [[nodiscard]] std::size_t serverCryptoByteCount(const QuicEncryptionLevel level) const noexcept
            {
                return level == QuicEncryptionLevel::Initial ? m_initialCryptoByteCount : m_handshakeCryptoByteCount;
            }

        private:
            void consumePacket(const std::span<const std::uint8_t> remainder, const QuicPacketHeader &header)
            {
                const QuicEncryptionLevel level = header.isLongHeader
                        ? (header.longPacketType == QuicLongPacketType::Handshake ? QuicEncryptionLevel::Handshake
                                                                                  : QuicEncryptionLevel::Initial)
                        : QuicEncryptionLevel::Application;
                ClientSpace &space = level == QuicEncryptionLevel::Handshake ? m_handshake : m_initial;
                if (level == QuicEncryptionLevel::Application || !space.readKeys.has_value())
                {
                    return;
                }
                if (header.isLongHeader && !header.sourceConnectionId.empty())
                {
                    m_serverConnectionId = std::vector<std::uint8_t>(header.sourceConnectionId.begin(),
                                                                     header.sourceConnectionId.end());
                }
                std::vector<std::uint8_t> working(remainder.begin(),
                                                  remainder.begin() + static_cast<std::ptrdiff_t>(header.packetByteCount));
                const std::span<std::uint8_t> packet(working);
                const auto sample = extractQuicHeaderProtectionSample(packet, header);
                if (!sample.has_value())
                {
                    return;
                }
                const QuicHeaderProtectionMask mask = generateQuicHeaderProtectionMask(*space.readKeys, *sample);
                const auto unmasked = removeQuicHeaderProtection(packet, header, mask);
                if (!unmasked.has_value())
                {
                    return;
                }
                QuicPacketHeader refreshed = header;
                if (!refreshQuicPacketHeader(refreshed, *unmasked, packet).has_value())
                {
                    return;
                }
                const std::size_t headerByteCount = refreshed.packetNumberOffset + refreshed.packetNumberByteCount;
                if (packet.size() <= headerByteCount + kQuicAuthenticationTagByteLength)
                {
                    return;
                }
                std::vector<std::uint8_t> plaintext(packet.size() - headerByteCount - kQuicAuthenticationTagByteLength);
                const std::uint64_t packetNumber = space.nextExpectedPacketNumber;
                const auto opened = openQuicProtectedPayload(plaintext, *space.readKeys, packetNumber,
                                                             packet.subspan(0, headerByteCount), packet.subspan(headerByteCount));
                if (!opened.has_value())
                {
                    return;
                }
                plaintext.resize(*opened);
                ++space.nextExpectedPacketNumber;

                const auto frames = decodeQuicFrames(std::span<const std::uint8_t>(plaintext));
                if (!frames.has_value())
                {
                    return;
                }
                for (const QuicFrame &frame : *frames)
                {
                    observeFrame(frame, level);
                }
            }

            void observeFrame(const QuicFrame &frame, const QuicEncryptionLevel level)
            {
                if (const auto *crypto = std::get_if<QuicCryptoFrame>(&frame); crypto != nullptr)
                {
                    m_tls->feedHandshakeData(level, crypto->data);
                    if (level == QuicEncryptionLevel::Initial)
                    {
                        m_initialCryptoByteCount += crypto->data.size();
                    }
                    else
                    {
                        m_handshakeCryptoByteCount += crypto->data.size();
                    }
                }
                else if (const auto *acknowledgement = std::get_if<QuicAcknowledgementFrame>(&frame); acknowledgement != nullptr)
                {
                    m_largestServerAcknowledged = acknowledgement->largestAcknowledgedPacketNumber;
                    m_serverAcknowledgementDelay = acknowledgement->acknowledgementDelay;
                }
                else if (std::holds_alternative<QuicHandshakeDoneFrame>(frame))
                {
                    ++m_handshakeDoneFrameCount;
                }
                else if (std::holds_alternative<QuicConnectionCloseFrame>(frame))
                {
                    m_sawConnectionClose = true;
                }
            }

            [[nodiscard]] std::vector<std::uint8_t> makeCryptoDatagram(ClientSpace &space, const QuicEncryptionLevel level,
                                                                       const std::uint64_t offset, const std::vector<std::uint8_t> &data)
            {
                QuicCryptoFrame crypto;
                crypto.offset = offset;
                crypto.data = data;
                std::string frames;
                appendQuicFrame(frames, QuicFrame{crypto});
                return buildDatagram(space, level, frames);
            }

            [[nodiscard]] std::vector<std::uint8_t> buildDatagram(ClientSpace &space, const QuicEncryptionLevel level,
                                                                  const std::string &frames)
            {
                QuicOutboundPacket packet;
                packet.isLongHeader = true;
                packet.longPacketType = level == QuicEncryptionLevel::Initial ? QuicLongPacketType::Initial : QuicLongPacketType::Handshake;
                packet.destinationConnectionId = !m_destinationOverride.empty()
                        ? m_destinationOverride
                        : (m_serverConnectionId.empty() ? kOriginalDestinationConnectionId : m_serverConnectionId);
                packet.sourceConnectionId = m_sourceConnectionId;
                packet.packetNumber = space.nextPacketNumber++;
                packet.packetNumberByteCount = 1;
                packet.frames = asBytes(frames);

                std::string datagram;
                appendQuicPacket(datagram, packet, *space.writeKeys);
                return std::vector<std::uint8_t>(datagram.begin(), datagram.end());
            }

            std::vector<std::uint8_t> m_sourceConnectionId;          ///< 本端签发的连接标识
            std::vector<std::uint8_t> m_serverConnectionId{};         ///< 从服务端 Initial 的源标识学到的目的标识
            std::vector<std::uint8_t> m_lastSentHandshakeDatagram{};  ///< 最近手工发出去的那条 Handshake 包
            ClientSpace m_initial{};                                  ///< Initial 空间
            ClientSpace m_handshake{};                                ///< Handshake 空间
            std::unique_ptr<QuicTlsContext> m_tls;                    ///< 客户端 TLS 上下文
            std::optional<std::uint64_t> m_largestServerAcknowledged{}; ///< 服务端 ACK 到的最大包号
            std::optional<std::uint64_t> m_serverAcknowledgementDelay{};///< 服务端 ACK 的延迟字段
            std::size_t m_initialCryptoByteCount{0};                  ///< 收到的 Initial 级握手字节数
            std::size_t m_handshakeCryptoByteCount{0};                ///< 收到的 Handshake 级握手字节数
            std::size_t m_handshakeDoneFrameCount{0};                 ///< 收到过的 HANDSHAKE_DONE 帧数
            std::vector<std::uint8_t> m_destinationOverride{};         ///< 非空时覆盖发出包的目的标识
            bool m_sawConnectionClose{false};                         ///< 是否收到过 CONNECTION_CLOSE
        };

        /**
         * @brief 造一份服务端配置
         * @param tlsContext 服务端 SSL 上下文
         * @param peerConnectionId 回包要打的目的地标识，默认就是客户端自报的那个
         * @return QuicConnectionCoreConfiguration 填好的配置
         */
        QuicConnectionCoreConfiguration makeServerConfiguration(SSL_CTX &tlsContext,
                                                                std::vector<std::uint8_t> peerConnectionId = kClientConnectionId)
        {
            QuicConnectionCoreConfiguration configuration;
            configuration.tlsContext = &tlsContext;
            configuration.localConnectionId = kServerConnectionId;
            configuration.peerConnectionId = std::move(peerConnectionId);
            configuration.originalDestinationConnectionId = kOriginalDestinationConnectionId;
            configuration.transportParameters.initialMaximumData = 1048576;
            configuration.transportParameters.initialMaximumBidirectionalStreams = 1024;
            configuration.transportParameters.maximumIdleTimeoutMilliseconds = 30000;
            return configuration;
        }

        /// @return 核心当前产出的全部待发数据报
        std::vector<std::vector<std::uint8_t>> drain(QuicConnectionCore &core)
        {
            std::vector<std::vector<std::uint8_t>> datagrams;
            while (const auto datagram = core.takeOutboundDatagram())
            {
                datagrams.push_back(std::move(*datagram));
            }
            return datagrams;
        }
    } // namespace

    /**
     * @brief 内存里跑完一次握手：服务端进入 Established，对端参数解出并过了绑定校验
     */
    TEST(QuicConnectionCore, CompletesHandshakeInMemory)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr) << "夹具证书加载失败";
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);

        std::vector<std::vector<std::uint8_t>> serverDatagrams;
        // 客户端可能在 consume 里就把手握完了，最后一条飞行要等下一轮 buildFlight 才发出去，
        // 所以循环条件两侧都看：只有双方都到位才算跑完
        for (int round = 0; round < 6 && !(client.isHandshakeCompleted() && core.phase() == QuicConnectionPhase::Established); ++round)
        {
            for (const auto &datagram : client.buildFlight())
            {
                ASSERT_TRUE(core.onDatagramReceived(datagram, Timestamp{100 * round}).has_value());
                core.drive(Timestamp{100 * round + 5});
                const std::vector<std::vector<std::uint8_t>> produced = drain(core);
                serverDatagrams.insert(serverDatagrams.end(), produced.begin(), produced.end());
            }
            client.adoptKeys();
            for (const auto &datagram : serverDatagrams)
            {
                client.consume(datagram);
            }
            serverDatagrams.clear();
            client.adoptKeys();
        }

        ASSERT_TRUE(client.isHandshakeCompleted()) << "客户端没握上手，服务端产出的飞行有问题";
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Established);
        EXPECT_GT(client.serverCryptoByteCount(QuicEncryptionLevel::Initial), 0U);
        EXPECT_GT(client.serverCryptoByteCount(QuicEncryptionLevel::Handshake), 0U);

        const QuicTransportParameters *peerParameters = core.peerTransportParameters();
        ASSERT_NE(peerParameters, nullptr) << "参数没解出来，或 §7.3 的绑定校验没过";
        ASSERT_TRUE(peerParameters->initialSourceConnectionId.has_value());
        EXPECT_EQ(*peerParameters->initialSourceConnectionId, kClientConnectionId);
        EXPECT_EQ(peerParameters->initialMaximumData, 65536U);
        EXPECT_EQ(peerParameters->initialMaximumBidirectionalStreams, 128U);
    }

    /**
     * @brief 服务端产出的每条数据报都不能超过未做 PMTU 探测时的上限（RFC 9000 §14.1）
     */
    TEST(QuicConnectionCore, KeepsEveryOutboundDatagramWithinTheDefaultLimit)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        // 客户端可能在 consume 里就把手握完了，最后一条飞行要等下一轮 buildFlight 才发出去，
        // 所以循环条件两侧都看：只有双方都到位才算跑完
        for (int round = 0; round < 6 && !(client.isHandshakeCompleted() && core.phase() == QuicConnectionPhase::Established); ++round)
        {
            for (const auto &datagram : client.buildFlight())
            {
                ASSERT_TRUE(core.onDatagramReceived(datagram, Timestamp{0}).has_value());
                core.drive(Timestamp{5});
            }
            client.adoptKeys();
            for (const auto &datagram : drain(core))
            {
                // 证书那条飞行远超 1200，必须被切成多个报文而不是一条大数据报
                EXPECT_LE(datagram.size(), 1200U) << "第 " << round << " 轮产出了超长数据报";
                client.consume(datagram);
            }
            client.adoptKeys();
        }
        ASSERT_TRUE(client.isHandshakeCompleted());
    }

    /**
     * @brief 同一条数据报重复交来不能把握手字节喂给 TLS 第二次（§12.5）
     */
    TEST(QuicConnectionCore, IgnoresRetransmittedPackets)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        const std::vector<std::vector<std::uint8_t>> clientFlight = client.buildFlight();
        ASSERT_FALSE(clientFlight.empty());

        ASSERT_TRUE(core.onDatagramReceived(clientFlight.front(), Timestamp{0}).has_value());
        core.drive(Timestamp{5});
        ASSERT_FALSE(drain(core).empty()) << "第一条 Initial 该有回音";

        // 同一份字节再来一遍：包号重复，必须整个丢掉，不能再喂 TLS 也不再产出
        ASSERT_TRUE(core.onDatagramReceived(clientFlight.front(), Timestamp{100}).has_value());
        core.drive(Timestamp{105});
        EXPECT_TRUE(drain(core).empty()) << "重复包被当成了新数据，TLS 会收到两遍 ClientHello";
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Handshaking);
    }

    /**
     * @brief 服务端要确认客户端交来的包，延迟字段按本端声明的指数换算
     */
    TEST(QuicConnectionCore, AcknowledgesTheClientFirstFlight)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        const std::vector<std::vector<std::uint8_t>> clientFlight = client.buildFlight();
        ASSERT_FALSE(clientFlight.empty());

        ASSERT_TRUE(core.onDatagramReceived(clientFlight.front(), Timestamp{5000}).has_value());
        core.drive(Timestamp{10000});
        for (const auto &datagram : drain(core))
        {
            client.consume(datagram);
        }

        // 客户端只发了包号 0 的 Initial，服务端 ACK 的最大确认值必须就是它
        ASSERT_TRUE(client.largestServerAcknowledged().has_value()) << "收了触发确认的包却没回 ACK，对端会一直重发";
        EXPECT_EQ(*client.largestServerAcknowledged(), 0U);
        // 5000 微秒按本端 ack_delay_exponent 3 编码：5000 >> 3 = 625（§19.3 的线上值是除过指数的）
        ASSERT_TRUE(client.serverAcknowledgementDelay().has_value());
        EXPECT_EQ(*client.serverAcknowledgementDelay(), 625U);
    }

    /**
     * @brief 目的标识不落在本连接上的报文静默丢弃：不报错也不产出
     */
    TEST(QuicConnectionCore, DiscardsPacketForUnknownDestinationConnectionId)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        // 密钥照旧、目的标识换成客户端另一条连接上的值：只有路由这一关会拦下它
        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        client.overrideDestinationConnectionId(kUnknownConnectionId);
        for (const auto &datagram : client.buildFlight())
        {
            EXPECT_TRUE(core.onDatagramReceived(datagram, Timestamp{0}).has_value());
        }
        core.drive(Timestamp{10});
        EXPECT_TRUE(drain(core).empty()) << "目的标识不合的包不该触发任何回包";
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Handshaking);
    }

    /**
     * @brief 垃圾字节与截断数据报既不能让状态机崩，也不能让它产出
     */
    TEST(QuicConnectionCore, DropsGarbageWithoutProducingAnything)
    {
        const FixtureContext serverContext = FixtureContext::server();
        ASSERT_NE(serverContext.get(), nullptr);
        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));

        // 一个看起来像长头的开头（版本 1、目的标识 8 字节）后面全接垃圾
        const std::vector<std::uint8_t> garbage = makeBytesFromHex(
                "c40000000108112233445566778804" + std::string(64, 'f'));
        EXPECT_TRUE(core.onDatagramReceived(garbage, Timestamp{0}).has_value());
        EXPECT_TRUE(drain(core).empty());

        EXPECT_TRUE(core.onDatagramReceived({}, Timestamp{0}).has_value());
        EXPECT_TRUE(core.onDatagramReceived(std::span<const std::uint8_t>(garbage).subspan(0, 1), Timestamp{0}).has_value());
        core.drive(Timestamp{10});
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Handshaking);
        EXPECT_FALSE(core.isFinished());
    }

    /**
     * @brief 只交来后半段握手字节时服务端不该有反应，补齐了才产出飞行
     */
    TEST(QuicConnectionCore, BuffersOutOfOrderCryptoBytesUntilTheStreamIsComplete)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        const auto [lateFragment, headFragment] = client.buildReversedCryptoFragments();

        ASSERT_TRUE(core.onDatagramReceived(lateFragment, Timestamp{0}).has_value());
        core.drive(Timestamp{10});
        const std::vector<std::vector<std::uint8_t>> afterTail = drain(core);
        for (const auto &datagram : afterTail)
        {
            client.consume(datagram);
        }
        // 服务端可以（也应该）确认收到了这个包，但绝不能交出任何握手字节：ClientHello 还不完整
        EXPECT_EQ(client.serverCryptoByteCount(QuicEncryptionLevel::Initial), 0U);
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Handshaking);

        ASSERT_TRUE(core.onDatagramReceived(headFragment, Timestamp{1000}).has_value());
        core.drive(Timestamp{1010});
        const std::vector<std::vector<std::uint8_t>> serverDatagrams = drain(core);
        ASSERT_FALSE(serverDatagrams.empty()) << "两段补齐后服务端该产出首批飞行";
        for (const auto &datagram : serverDatagrams)
        {
            client.consume(datagram);
        }
        EXPECT_GT(client.serverCryptoByteCount(QuicEncryptionLevel::Initial), 0U);
    }

    /**
     * @brief 对端参数里的源标识与实际收到的不符，要发 CONNECTION_CLOSE 收口
     */
    TEST(QuicConnectionCore, ClosesWhenPeerSourceConnectionIdDoesNotMatch)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        // 参数里的 ISCID 写成另一个标识：§7.3 的绑定校验必须把它抓出来
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId, true);

        std::vector<std::vector<std::uint8_t>> serverDatagrams;
        for (int round = 0; round < 6 && core.phase() == QuicConnectionPhase::Handshaking; ++round)
        {
            for (const auto &datagram : client.buildFlight())
            {
                ASSERT_TRUE(core.onDatagramReceived(datagram, Timestamp{0}).has_value());
                core.drive(Timestamp{5});
                serverDatagrams = drain(core);
            }
            client.adoptKeys();
            for (const auto &datagram : serverDatagrams)
            {
                client.consume(datagram);
            }
            client.adoptKeys();
        }

        EXPECT_EQ(core.phase(), QuicConnectionPhase::Closing) << "绑定校验没过却没收口";
        EXPECT_EQ(core.peerTransportParameters(), nullptr) << "不合格的参数不该被交给调用方";
        EXPECT_TRUE(core.isFinished()) << "收口的同时要把 CONNECTION_CLOSE 排进待发队列";
    }

    /**
     * @brief HANDSHAKE_DONE 要等对端确认过 Handshake 包，且只发一次
     */
    TEST(QuicConnectionCore, SendsHandshakeDoneOnceAfterAcknowledgement)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);

        std::vector<std::vector<std::uint8_t>> serverDatagrams;
        // 客户端可能在 consume 里就把手握完了，最后一条飞行要等下一轮 buildFlight 才发出去，
        // 所以循环条件两侧都看：只有双方都到位才算跑完
        for (int round = 0; round < 6 && !(client.isHandshakeCompleted() && core.phase() == QuicConnectionPhase::Established); ++round)
        {
            for (const auto &datagram : client.buildFlight())
            {
                ASSERT_TRUE(core.onDatagramReceived(datagram, Timestamp{0}).has_value());
                core.drive(Timestamp{5});
                serverDatagrams = drain(core);
            }
            client.adoptKeys();
            for (const auto &datagram : serverDatagrams)
            {
                client.consume(datagram);
            }
            client.adoptKeys();
        }
        ASSERT_TRUE(client.isHandshakeCompleted());
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);

        // 到这一步服务端还没收到任何对本空间的确认，HANDSHAKE_DONE 不该出现
        for (const auto &datagram : serverDatagrams)
        {
            client.consume(datagram);
        }
        EXPECT_EQ(client.handshakeDoneFrameCount(), 0U) << "对端还没确认 Handshake 包就发了 HANDSHAKE_DONE";

        client.acknowledgeServerHandshakePacket(0);
        ASSERT_TRUE(core.onDatagramReceived(client.lastSentHandshakeDatagram(), Timestamp{8000}).has_value());
        core.drive(Timestamp{8000});
        const std::vector<std::vector<std::uint8_t>> afterAcknowledgement = drain(core);
        ASSERT_FALSE(afterAcknowledgement.empty()) << "确认之后该有产出（HANDSHAKE_DONE）";
        for (const auto &datagram : afterAcknowledgement)
        {
            client.consume(datagram);
        }
        EXPECT_EQ(client.handshakeDoneFrameCount(), 1U);

        // 再推几轮也不该重发：计数器不重置，重发会直接变成 2
        core.drive(Timestamp{9000});
        core.drive(Timestamp{10000});
        for (const auto &datagram : drain(core))
        {
            client.consume(datagram);
        }
        EXPECT_EQ(client.handshakeDoneFrameCount(), 1U) << "HANDSHAKE_DONE 发过之后不该再发第二次";
    }
} // namespace AsynGyanis::Net
