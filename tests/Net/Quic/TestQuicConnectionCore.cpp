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
//   6) HANDSHAKE_DONE 只在「握手完成 + 对端确认过 Handshake 空间的包」之后发且只发一次（§19.20）；
//   7) 恢复层接进来之后的四条补发路：探测超时补在途字节、时间阈值判丢后补发、重复 ACK 也要判丢补发、
//      没字节可补时退化成一个 PING；以及在途清空后定时器跟着撤掉（RFC 9002 §6.1.2、§6.2、§A.7）；
//   8) 出流量的两道闸：与已交字节重叠的 CRYPTO 分片剪掉再交给 TLS（§7.5），以及地址验证之前回量
//      夹在三倍已收字节以内、连重发与探针也不例外（RFC 9000 §8.1）；
//   9) 解到对端的 Handshake 报文后 Initial 空间整体退休：不再补发它的字节，也不再为它亮定时器
//      （RFC 9001 §4.9.1 + RFC 9002 §A.11）；
//  10) 空闲超时：有效值取两端宣告里较小的那一个，到点静默关闭（不收口、不留待发），且探测期间
//      把自己撑住不关（RFC 9000 §10.1）。
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

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

        /// 客户端一侧的包号空间：包保护密钥、发包号，以及已收包号与握手字节的偏移
        struct ClientSpace
        {
            std::optional<QuicPacketKeys> readKeys{};
            std::optional<QuicPacketKeys> writeKeys{};
            std::uint64_t nextPacketNumber{0};
            std::optional<std::uint64_t> largestReceived{};   ///< 已解密通过的最大包号，还原截断包号要靠它
            std::set<std::uint64_t> receivedPacketNumbers{};  ///< 已解密通过的包号，回 ACK 的原料
            std::size_t receivedCryptoByteCount{0};           ///< 已交给 TLS 的握手字节数，也就是下一个期望偏移
            std::map<std::uint64_t, std::vector<std::uint8_t>> laterCryptoFragments{}; ///< 早到的乱序段，按偏移存
            bool isAcknowledgementPending{false};             ///< 收过触发确认的包还没回 ACK
            bool suppressesAcknowledgements{false};          ///< 本空间从此不再回 ACK，用来制造永久在途
        };

        /// @return std::optional<QuicEncryptionLevel> 报文头对应的加密级别；本端不收的形态返回空
        std::optional<QuicEncryptionLevel> levelOf(const QuicPacketHeader &header)
        {
            if (!header.isLongHeader)
            {
                return QuicEncryptionLevel::Application;
            }
            switch (header.longPacketType)
            {
            case QuicLongPacketType::Initial:
            case QuicLongPacketType::ZeroRtt: return QuicEncryptionLevel::Initial;
            case QuicLongPacketType::Handshake: return QuicEncryptionLevel::Handshake;
            case QuicLongPacketType::Retry: return std::nullopt;
            }
            return std::nullopt;
        }

        /**
         * @brief 测试用的最小客户端
         * @details 够把一次 TLS 1.3 握手跑完，并且像真实对端那样按级别收字节、回 ACK：它同时充当观测
         *          探头——解出来的帧在这里记账，用例直接读计数，不在用例里再解一遍包。
         *          不做乱序缓存，也不发流相关帧，那是上层替身的事。
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
            /**
             * @brief 建客户端
             * @param context 客户端 SSL 上下文
             * @param sourceConnectionId 本端在第一个 Initial 里的源标识
             * @param mismatchedSourceConnectionId 要不要把参数里的 ISCID 写成别的值，用于 §7.3 绑定用例
             * @param maximumIdleTimeoutMilliseconds 本端宣告的 max_idle_timeout，0 表示不宣告（§18.2）
             */
            InMemoryQuicClient(SSL_CTX &context, std::vector<std::uint8_t> sourceConnectionId,
                               const bool mismatchedSourceConnectionId = false,
                               const std::uint64_t maximumIdleTimeoutMilliseconds = 0)
                : m_sourceConnectionId(std::move(sourceConnectionId))
            {
                ClientSpace &initial = spaceOf(QuicEncryptionLevel::Initial);
                initial.readKeys = deriveQuicInitialPacketKeys(kOriginalDestinationConnectionId, QuicPacketDirection::ServerToClient);
                initial.writeKeys = deriveQuicInitialPacketKeys(kOriginalDestinationConnectionId, QuicPacketDirection::ClientToServer);

                QuicTransportParameters parameters;
                parameters.initialMaximumData = 65536;
                parameters.initialMaximumBidirectionalStreams = 128;
                parameters.maximumIdleTimeoutMilliseconds = maximumIdleTimeoutMilliseconds;
                parameters.initialSourceConnectionId = mismatchedSourceConnectionId ? kUnknownConnectionId : m_sourceConnectionId;
                std::string encoded;
                appendQuicTransportParameters(encoded, parameters);
                m_tls = std::make_unique<QuicTlsContext>(context, false, asBytes(encoded));
            }

            /// 把 TLS 交出的 Handshake 与 Application 级密钥补进本端对应空间（读密钥即服务端的写密钥）
            void adoptKeys()
            {
                for (const QuicEncryptionLevel level : {QuicEncryptionLevel::Handshake, QuicEncryptionLevel::Application})
                {
                    ClientSpace &space = spaceOf(level);
                    if (const QuicPacketKeys *reading = m_tls->keys(level, QuicKeyDirection::Reading);
                        reading != nullptr && !space.readKeys.has_value())
                    {
                        space.readKeys = *reading;
                    }
                    if (const QuicPacketKeys *writing = m_tls->keys(level, QuicKeyDirection::Writing);
                        writing != nullptr && !space.writeKeys.has_value())
                    {
                        space.writeKeys = *writing;
                    }
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

            /**
             * @brief 让本端在收到 Handshake 级报文时直接丢弃，模拟在途丢包
             * @param drop true 表示这些包一律不看
             */
            void dropHandshakePackets(const bool drop) noexcept
            {
                m_dropsAllHandshakePackets = drop;
            }

            /**
             * @brief 只跳过某个级别里某一个包号的报文，用来造「中间空一包」的局面
             * @param level 要跳过的加密级别
             * @param packetNumber 要跳过的包号；交空回到正常行为
             */
            void skipPacketNumber(const QuicEncryptionLevel level, const std::optional<std::uint64_t> packetNumber) noexcept
            {
                m_skippedPacketLevel = level;
                m_skippedPacketNumber = packetNumber;
            }

            /// 推进 TLS 并把产出的握手字节编成一批数据报
            [[nodiscard]] std::vector<std::vector<std::uint8_t>> buildFlight()
            {
                std::ignore = m_tls->drive();
                adoptKeys();
                std::vector<std::vector<std::uint8_t>> flight;
                while (const auto record = m_tls->takeOutboundRecord())
                {
                    if (record->level == QuicEncryptionLevel::Application)
                    {
                        continue; // 客户端在 1-RTT 没东西要发
                    }
                    ClientSpace &space = spaceOf(record->level);
                    if (!space.writeKeys.has_value())
                    {
                        continue;
                    }
                    QuicCryptoFrame crypto;
                    crypto.data = record->data;
                    std::string frames;
                    appendQuicFrame(frames, QuicFrame{crypto});
                    flight.push_back(buildDatagram(record->level, frames));
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
                const std::vector<std::uint8_t> handshakeBytes = takeHandshakeBytes();
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
                return {makeCryptoDatagram(QuicEncryptionLevel::Initial, splitPoint, firstFragment),
                        makeCryptoDatagram(QuicEncryptionLevel::Initial, 0, secondFragment)};
            }

            /**
             * @brief 把 ClientHello 分成 [0, 半) 与 [四分之一, 末尾) 两条**重叠**的数据报
             * @details 用来验「重传换了分片大小时要剪掉重叠的那一段」（§7.5）：不剪就会把重复字节
             *          喂给 TLS，握手当场报错收口
             * @return 两条数据报，前一条短、后一条从中间开始且盖住前一条的后半段
             */
            [[nodiscard]] std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> buildOverlappingCryptoFragments()
            {
                const std::vector<std::uint8_t> handshakeBytes = takeHandshakeBytes();
                const std::size_t halfPoint = handshakeBytes.size() / 2;
                const std::size_t quarterPoint = handshakeBytes.size() / 4;
                if (halfPoint == 0 || quarterPoint == 0)
                {
                    return {};
                }
                const std::vector<std::uint8_t> head(handshakeBytes.begin(),
                                                     handshakeBytes.begin() + static_cast<std::ptrdiff_t>(halfPoint));
                const std::vector<std::uint8_t> overlappingTail(handshakeBytes.begin() + static_cast<std::ptrdiff_t>(quarterPoint),
                                                                handshakeBytes.end());
                return {makeCryptoDatagram(QuicEncryptionLevel::Initial, 0, head),
                        makeCryptoDatagram(QuicEncryptionLevel::Initial, quarterPoint, overlappingTail)};
            }

            /**
             * @brief 只发 ClientHello 的第一个字节：包合法、触发确认，但 TLS 拼不出任何消息
             * @return std::vector<std::uint8_t> 一条数据报，反放大额度的用例用它把服务端卡住
             */
            [[nodiscard]] std::vector<std::uint8_t> buildStubCryptoDatagram()
            {
                const std::vector<std::uint8_t> handshakeBytes = takeHandshakeBytes();
                if (handshakeBytes.empty())
                {
                    return {};
                }
                return makeCryptoDatagram(QuicEncryptionLevel::Initial, 0, {handshakeBytes.front()});
            }

            /**
             * @brief 为每个「收过触发确认的包、且已有写密钥」的空间各回一条只含 ACK 的包
             * @details 真实对端就是这么做的：服务端的发包记账、RTT 样本与 HANDSHAKE_DONE 都以它为依据。
             * @param leaveLargestUnacknowledged 这个级别不确认它收到的最新一包，用来制造「就差一包没被确认」
             * @return 至多三条数据报，按 Initial、Handshake、Application 的顺序
             */
            [[nodiscard]] std::vector<std::vector<std::uint8_t>> buildAcknowledgements(
                    const std::optional<QuicEncryptionLevel> leaveLargestUnacknowledged = std::nullopt)
            {
                std::vector<std::vector<std::uint8_t>> acknowledgements;
                for (const QuicEncryptionLevel level : {QuicEncryptionLevel::Initial, QuicEncryptionLevel::Handshake,
                                                        QuicEncryptionLevel::Application})
                {
                    ClientSpace &space = spaceOf(level);
                    if (space.suppressesAcknowledgements || !space.isAcknowledgementPending ||
                        !space.writeKeys.has_value() || !space.largestReceived.has_value())
                    {
                        continue;
                    }
                    std::set<std::uint64_t> acknowledged = space.receivedPacketNumbers;
                    std::uint64_t largest = *space.largestReceived;
                    if (level == leaveLargestUnacknowledged && acknowledged.size() > 1)
                    {
                        acknowledged.erase(std::prev(acknowledged.end()));
                        largest = *acknowledged.rbegin();
                    }
                    space.isAcknowledgementPending = false;
                    acknowledgements.push_back(makeAcknowledgementDatagram(level, std::move(acknowledged), largest));
                }
                return acknowledgements;
            }

            /**
             * @brief 让本端从此不再确认某个级别收到的包
             * @details 真实实现里「一条确认都没发出去」是会出现的（确认被丢、或本端攒着没发）；
             *          用它把某个空间的包永久留在对端的在途账里
             * @param level 要闭嘴的级别
             */
            void suppressAcknowledgements(const QuicEncryptionLevel level) noexcept
            {
                spaceOf(level).suppressesAcknowledgements = true;
            }

            /**
             * @brief 只把某个级别已收到的包再确认一次，不管有没有新包要确认
             * @details 重复 ACK 是合法且常见的（周期性确认）。本端用它来走「确认本身就判丢」那条不依赖
             *          定时器的路（RFC 9002 §A.7 的 OnAckReceived 第 5 步是无条件的）
             * @param level 用哪个级别发，同时决定用哪个空间与哪套密钥
             * @return std::vector<std::uint8_t> 一条只含 ACK 的数据报
             */
            [[nodiscard]] std::vector<std::uint8_t> buildRepeatedAcknowledgement(const QuicEncryptionLevel level)
            {
                ClientSpace &space = spaceOf(level);
                return makeAcknowledgementDatagram(level, space.receivedPacketNumbers, *space.largestReceived);
            }

            /**
             * @brief 手工发一条只含 PING 的包
             * @details 用来让服务端「解得开对端的 Handshake 报文」，从而脱离 §8.1 的反放大额度；
             *          它自己也是触发确认的包，服务端必须回一条确认。
             * @param level 用哪个级别发（同时决定空间与密钥）
             * @return std::vector<std::uint8_t> 一条数据报
             */
            [[nodiscard]] std::vector<std::uint8_t> buildPing(const QuicEncryptionLevel level)
            {
                std::string frames;
                appendQuicFrame(frames, QuicFrame{QuicPingFrame{}});
                return buildDatagram(level, frames);
            }

            /**
             * @brief 手工发一条只含 ACK 的包，确认服务端的某个包号
             * @details 与 `buildAcknowledgements` 的区别是完全由用例指定确认值，用来单点验证
             *          「确认到第几包才发 DONE」这类判据。
             * @param level 用哪个级别发（同时决定用哪个空间与哪套密钥）
             * @param acknowledgedPacketNumber 要确认的最大包号
             * @param acknowledgeEverythingBeforeToo 是否把 0 到该包号之间的全部包一并确认
             */
            void acknowledgeServerPacket(const QuicEncryptionLevel level, const std::uint64_t acknowledgedPacketNumber,
                                         const bool acknowledgeEverythingBeforeToo = false)
            {
                QuicAcknowledgementFrame acknowledgement;
                acknowledgement.largestAcknowledgedPacketNumber = acknowledgedPacketNumber;
                acknowledgement.ranges = {acknowledgeEverythingBeforeToo
                                              ? QuicAcknowledgementRange{0, acknowledgedPacketNumber}
                                              : QuicAcknowledgementRange{acknowledgedPacketNumber, acknowledgedPacketNumber}};
                std::string frames;
                appendQuicFrame(frames, QuicFrame{acknowledgement});
                m_lastSentAcknowledgement = buildDatagram(level, frames);
            }

            [[nodiscard]] const std::vector<std::uint8_t> &lastSentAcknowledgement() const noexcept
            {
                return m_lastSentAcknowledgement;
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
                    // 短头没有长度字段：服务端发来的 1-RTT 包里目的标识是**本端自报**的那一条
                    const auto decodedHeader = decodeQuicPacketHeader(remainder, m_sourceConnectionId.size());
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

            /// 收到过的 PING 帧数：探测超时后没有可重发的东西时就该靠它续命
            [[nodiscard]] std::size_t pingFrameCount() const noexcept
            {
                return m_pingFrameCount;
            }

            /// 本端解出来的服务端某个级别的包条数（重发与确认的用例按它挑确认值）
            [[nodiscard]] std::size_t serverReceivedPacketCount(const QuicEncryptionLevel level) const noexcept
            {
                return spaceOf(level).receivedPacketNumbers.size();
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

            /// @return std::size_t 本端交给 TLS 的某个级别握手字节数
            [[nodiscard]] std::size_t serverCryptoByteCount(const QuicEncryptionLevel level) const noexcept
            {
                return spaceOf(level).receivedCryptoByteCount;
            }

        private:
            /// 级别 → 空间下标：0-RTT 借用 Initial 的空间（RFC 9000 §17.2.3）
            [[nodiscard]] static std::size_t spaceIndexOf(const QuicEncryptionLevel level) noexcept
            {
                switch (level)
                {
                case QuicEncryptionLevel::Initial:
                case QuicEncryptionLevel::ZeroRtt: return 0;
                case QuicEncryptionLevel::Handshake: return 1;
                case QuicEncryptionLevel::Application: return 2;
                }
                return 0;
            }

            [[nodiscard]] ClientSpace &spaceOf(const QuicEncryptionLevel level) noexcept
            {
                return m_spaces[spaceIndexOf(level)];
            }

            [[nodiscard]] const ClientSpace &spaceOf(const QuicEncryptionLevel level) const noexcept
            {
                return m_spaces[spaceIndexOf(level)];
            }

            /// 这条报文是不是本端故意不看的：整级丢弃，或只挑掉某个级别的某一个包号
            [[nodiscard]] bool isPacketToSkip(const QuicEncryptionLevel level, const std::uint64_t packetNumber) const noexcept
            {
                if (level == QuicEncryptionLevel::Handshake && m_dropsAllHandshakePackets)
                {
                    return true;
                }
                return m_skippedPacketNumber.has_value() && level == m_skippedPacketLevel &&
                       packetNumber == *m_skippedPacketNumber;
            }

            void consumePacket(const std::span<const std::uint8_t> remainder, const QuicPacketHeader &header)
            {
                const std::optional<QuicEncryptionLevel> level = levelOf(header);
                if (!level.has_value())
                {
                    return;
                }
                ClientSpace &space = spaceOf(*level);
                if (!space.readKeys.has_value())
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
                // 包号在线上是截断的，而且中间可能有整包被丢弃，必须按 §A.3 还原而不是自己数序号
                const std::uint64_t packetNumber = restoreQuicPacketNumber(space.largestReceived.value_or(0),
                                                                          refreshed.packetNumber, refreshed.packetNumberByteCount);
                if (isPacketToSkip(*level, packetNumber))
                {
                    return;
                }
                const auto opened = openQuicProtectedPayload(plaintext, *space.readKeys, packetNumber,
                                                             packet.subspan(0, headerByteCount), packet.subspan(headerByteCount));
                if (!opened.has_value())
                {
                    return;
                }
                plaintext.resize(*opened);
                space.receivedPacketNumbers.insert(packetNumber);
                if (!space.largestReceived.has_value() || packetNumber > *space.largestReceived)
                {
                    space.largestReceived = packetNumber;
                }

                const auto frames = decodeQuicFrames(std::span<const std::uint8_t>(plaintext));
                if (!frames.has_value())
                {
                    // 解不出帧的包仍然算「收到过」：STREAM 这类帧本来就是触发确认的
                    space.isAcknowledgementPending = true;
                    return;
                }
                for (const QuicFrame &frame : *frames)
                {
                    observeFrame(frame, *level);
                    if (!std::holds_alternative<QuicAcknowledgementFrame>(frame) &&
                        !std::holds_alternative<QuicPaddingFrame>(frame))
                    {
                        space.isAcknowledgementPending = true;
                    }
                }
            }

            /// 按偏移把握手字节续上：重叠的重传剪掉、早到的先缓存，接上了才交给 TLS（真实对端都这么做）
            void acceptCryptoBytes(ClientSpace &space, const QuicEncryptionLevel level, std::uint64_t offset,
                                   std::span<const std::uint8_t> bytes)
            {
                if (offset < space.receivedCryptoByteCount)
                {
                    const std::size_t alreadyReceivedByteCount = static_cast<std::size_t>(space.receivedCryptoByteCount - offset);
                    if (alreadyReceivedByteCount >= bytes.size())
                    {
                        return;
                    }
                    offset += alreadyReceivedByteCount;
                    bytes = bytes.subspan(alreadyReceivedByteCount);
                }
                if (bytes.empty())
                {
                    return;
                }
                if (offset > space.receivedCryptoByteCount)
                {
                    space.laterCryptoFragments.emplace(offset, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
                    return;
                }
                m_tls->feedHandshakeData(level, bytes);
                space.receivedCryptoByteCount += bytes.size();
                for (auto fragment = space.laterCryptoFragments.find(space.receivedCryptoByteCount);
                     fragment != space.laterCryptoFragments.end();
                     fragment = space.laterCryptoFragments.find(space.receivedCryptoByteCount))
                {
                    m_tls->feedHandshakeData(level, fragment->second);
                    space.receivedCryptoByteCount += fragment->second.size();
                    space.laterCryptoFragments.erase(fragment);
                }
            }

            void observeFrame(const QuicFrame &frame, const QuicEncryptionLevel level)
            {
                if (const auto *crypto = std::get_if<QuicCryptoFrame>(&frame); crypto != nullptr)
                {
                    acceptCryptoBytes(spaceOf(level), level, crypto->offset, crypto->data);
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
                else if (std::holds_alternative<QuicPingFrame>(frame))
                {
                    ++m_pingFrameCount;
                }
            }

            /// 推进 TLS 并把它交出的握手字节按顺序拼成一整段（取走即出队，本端后续没有它们）
            [[nodiscard]] std::vector<std::uint8_t> takeHandshakeBytes()
            {
                std::ignore = m_tls->drive();
                std::vector<std::uint8_t> handshakeBytes;
                while (const auto record = m_tls->takeOutboundRecord())
                {
                    handshakeBytes.insert(handshakeBytes.end(), record->data.begin(), record->data.end());
                }
                return handshakeBytes;
            }

            [[nodiscard]] std::vector<std::uint8_t> makeCryptoDatagram(const QuicEncryptionLevel level,
                                                                      const std::uint64_t offset,
                                                                      const std::vector<std::uint8_t> &data)
            {
                QuicCryptoFrame crypto;
                crypto.offset = offset;
                crypto.data = data;
                std::string frames;
                appendQuicFrame(frames, QuicFrame{crypto});
                return buildDatagram(level, frames);
            }

            /**
             * @brief 按给定的包号集合编一条只含 ACK 的包
             * @param level 用哪个级别发，同时决定空间与密钥
             * @param acknowledged 要确认的包号集合
             * @param largest 帧里的最大确认值
             * @return std::vector<std::uint8_t> 一条完整的 UDP 净字节
             */
            [[nodiscard]] std::vector<std::uint8_t> makeAcknowledgementDatagram(const QuicEncryptionLevel level,
                                                                                const std::set<std::uint64_t> &acknowledged,
                                                                                const std::uint64_t largest)
            {
                QuicAcknowledgementFrame acknowledgement;
                acknowledgement.largestAcknowledgedPacketNumber = largest;
                acknowledgement.ranges = buildQuicAcknowledgementRanges(acknowledged, largest);
                std::string frames;
                appendQuicFrame(frames, QuicFrame{acknowledgement});
                return buildDatagram(level, frames);
            }

            /**
             * @brief 把一条数据报的明文帧序列封成包：长头按级别定类型，1-RTT 用短头
             * @param level 本包用的加密级别，同时决定空间与密钥
             * @param frames 明文帧序列
             * @return std::vector<std::uint8_t> 一条完整的 UDP 净字节
             */
            [[nodiscard]] std::vector<std::uint8_t> buildDatagram(const QuicEncryptionLevel level, const std::string &frames)
            {
                ClientSpace &space = spaceOf(level);
                const std::vector<std::uint8_t> &destination =
                        !m_destinationOverride.empty() ? m_destinationOverride
                                                       : (m_serverConnectionId.empty() ? kOriginalDestinationConnectionId
                                                                                       : m_serverConnectionId);
                QuicOutboundPacket packet;
                packet.isLongHeader = level != QuicEncryptionLevel::Application;
                packet.longPacketType = level == QuicEncryptionLevel::Initial ? QuicLongPacketType::Initial
                                                                             : QuicLongPacketType::Handshake;
                packet.destinationConnectionId = destination;
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
            std::vector<std::uint8_t> m_lastSentAcknowledgement{};    ///< 最近手工发出的那条只含 ACK 的包
            std::array<ClientSpace, 3> m_spaces{};                     ///< Initial、Handshake、Application 三个包号空间
            std::unique_ptr<QuicTlsContext> m_tls;                    ///< 客户端 TLS 上下文
            std::optional<std::uint64_t> m_largestServerAcknowledged{}; ///< 服务端 ACK 到的最大包号
            std::optional<std::uint64_t> m_serverAcknowledgementDelay{};///< 服务端 ACK 的延迟字段
            std::size_t m_handshakeDoneFrameCount{0};                 ///< 收到过的 HANDSHAKE_DONE 帧数
            std::vector<std::uint8_t> m_destinationOverride{};         ///< 非空时覆盖发出包的目的标识
            bool m_sawConnectionClose{false};                         ///< 是否收到过 CONNECTION_CLOSE
            bool m_dropsAllHandshakePackets{false};                 ///< 是否把 Handshake 级报文一律不看
            std::optional<std::uint64_t> m_skippedPacketNumber{};       ///< 要跳过的那个包号，空表示不挑
            QuicEncryptionLevel m_skippedPacketLevel{QuicEncryptionLevel::Initial}; ///< 上面那个包号属于哪个级别
            std::size_t m_pingFrameCount{0};                          ///< 收到过的 PING 帧数
        };

        /**
         * @brief 造一份服务端配置
         * @param tlsContext 服务端 SSL 上下文
         * @param peerConnectionId 回包要打的目的地标识，默认就是客户端自报的那个
         * @param maximumIdleTimeoutMilliseconds 本端宣告的 max_idle_timeout；默认 0 表示不宣告，
         *        免得空闲超时定时器混进那些只盯着丢包与拥塞的用例（§18.2）
         * @return QuicConnectionCoreConfiguration 填好的配置
         */
        QuicConnectionCoreConfiguration makeServerConfiguration(SSL_CTX &tlsContext,
                                                                std::vector<std::uint8_t> peerConnectionId = kClientConnectionId,
                                                                const std::uint64_t maximumIdleTimeoutMilliseconds = 0)
        {
            QuicConnectionCoreConfiguration configuration;
            configuration.tlsContext = &tlsContext;
            configuration.localConnectionId = kServerConnectionId;
            configuration.peerConnectionId = std::move(peerConnectionId);
            configuration.originalDestinationConnectionId = kOriginalDestinationConnectionId;
            configuration.transportParameters.initialMaximumData = 1048576;
            configuration.transportParameters.initialMaximumBidirectionalStreams = 1024;
            configuration.transportParameters.maximumIdleTimeoutMilliseconds = maximumIdleTimeoutMilliseconds;
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

        /// 一轮往返里服务端出包的延迟：ACK 的延迟字段与 RTT 样本都以此为基准，取 1 毫秒便于口算
        constexpr Timestamp kServerSendLatency{1000};

        /**
         * @brief 跑一轮往返：客户端的飞行与 ACK 交给服务端，服务端的产出交回客户端
         * @details 客户端对上一轮收到的包回 ACK（真实对端的行为），因此服务端的发包记账会随轮次
         *          推进到「全部确认」。时刻按轮递增，RTT 样本才是正数。
         * @param core 服务端状态机
         * @param client 客户端替身
         * @param arrivalTime 本轮客户端报文到达服务端的时刻
         * @param leaveLargestUnacknowledged 转发给 `buildAcknowledgements`，用来故意留一包不确认
         */
        void exchange(QuicConnectionCore &core, InMemoryQuicClient &client, const Timestamp arrivalTime,
                      const std::optional<QuicEncryptionLevel> leaveLargestUnacknowledged = std::nullopt)
        {
            std::vector<std::vector<std::uint8_t>> toServer = client.buildFlight();
            const std::vector<std::vector<std::uint8_t>> acknowledgements = client.buildAcknowledgements(leaveLargestUnacknowledged);
            toServer.insert(toServer.end(), acknowledgements.begin(), acknowledgements.end());
            for (const auto &datagram : toServer)
            {
                ASSERT_TRUE(core.onDatagramReceived(datagram, arrivalTime).has_value());
            }
            core.drive(arrivalTime + kServerSendLatency);
            for (const auto &datagram : drain(core))
            {
                client.consume(datagram);
            }
        }

        /**
         * @brief 补发之后把握手跑完：不再跳包，客户端的飞行该让双方都到位
         * @param core 服务端状态机
         * @param client 客户端替身
         */
        void finishAfterHandshakeGap(QuicConnectionCore &core, InMemoryQuicClient &client)
        {
            client.skipPacketNumber(QuicEncryptionLevel::Handshake, std::nullopt);
            for (int round = 2; round < 6; ++round)
            {
                exchange(core, client, Timestamp{10000 * round});
            }
            EXPECT_TRUE(client.isHandshakeCompleted()) << "补发之后握手仍没接上";
            EXPECT_EQ(core.phase(), QuicConnectionPhase::Established);
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

        client.acknowledgeServerPacket(QuicEncryptionLevel::Handshake, 0);
        ASSERT_TRUE(core.onDatagramReceived(client.lastSentAcknowledgement(), Timestamp{8000}).has_value());
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

    /**
     * @brief 握手包在途丢失后，探测超时期要把仍未确认的握手字节重发出去
     * @details 客户端故意不看服务端的 Handshake 级报文：补齐重发之后握手才继续得下去，
     *          这正是「丢一个包就卡死」那条老路的对照面
     */
    TEST(QuicConnectionCore, RetransmitsDroppedHandshakePacketsOnProbeTimeout)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        client.dropHandshakePackets(true);
        for (int round = 0; round < 4; ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        ASSERT_FALSE(client.isHandshakeCompleted()) << "Handshake 字节被丢光了才该停在这儿";
        ASSERT_EQ(client.serverReceivedPacketCount(QuicEncryptionLevel::Handshake), 0U);

        // Initial 包已被确认，因此有了一个 RTT 样本；Handshake 包全都没收，定时器只能靠探测超时
        const std::optional<Timestamp> deadline = core.nextTimeout();
        ASSERT_TRUE(deadline.has_value()) << "仍有未确认的握手包在途，必须武装定时器";
        // 没采到样本时 PTO 约是 999 毫秒（初值 333 + 4×166），采到 9 毫秒的样本后应在 5 万微秒以内
        EXPECT_LT(*deadline, Timestamp{500000}) << "有了 RTT 样本之后 PTO 不该还停在没采过样本的初值上";

        client.dropHandshakePackets(false);
        core.onTimeout(*deadline);
        const std::vector<std::vector<std::uint8_t>> retransmitted = drain(core);
        ASSERT_FALSE(retransmitted.empty()) << "探测超时期该重发仍未确认的握手字节（RFC 9002 §6.2.2）";
        for (const auto &datagram : retransmitted)
        {
            client.consume(datagram);
        }
        ASSERT_TRUE(client.isHandshakeCompleted()) << "重发的那段字节没能让握手继续";

        for (int round = 4; round < 6; ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Established);
        EXPECT_GT(client.serverReceivedPacketCount(QuicEncryptionLevel::Handshake), 0U);
    }

    /**
     * @brief 中间空一包时，定时器到点按时间阈值判丢并补发那一段字节
     * @details 与「整级丢弃」那条用例的分工：这条走 RFC 9002 §6.1.2 的时间阈值判丢，
     *          那条走 §6.2.2 的探测超时——两条路的触发者与记账都不一样
     */
    TEST(QuicConnectionCore, RetransmitsHandshakeBytesJudgedLostByTimeThreshold)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        client.skipPacketNumber(QuicEncryptionLevel::Handshake, std::uint64_t{0});

        exchange(core, client, Timestamp{0});
        exchange(core, client, Timestamp{10000});
        ASSERT_FALSE(client.isHandshakeCompleted()) << "第 0 包被跳过，后面的字节只能先堵在乱序缓存里";

        // 最大确认值抬到 1 之后，下面的空洞才有资格被判丢：时刻是发出时刻 1000 加上 9/8×最新 RTT
        const std::optional<Timestamp> deadline = core.nextTimeout();
        ASSERT_TRUE(deadline.has_value()) << "有空洞未被确认，必须武装时间阈值定时器";
        EXPECT_EQ(*deadline, Timestamp{11125}) << "判丢时刻应是 1000 + 9/8×9000（§6.1.2）";

        const std::size_t handshakeBytesBefore = client.serverCryptoByteCount(QuicEncryptionLevel::Handshake);
        core.onTimeout(*deadline);
        const std::vector<std::vector<std::uint8_t>> retransmitted = drain(core);
        ASSERT_FALSE(retransmitted.empty()) << "判丢之后该按原偏移补发那一段字节";
        for (const auto &datagram : retransmitted)
        {
            client.consume(datagram);
        }
        EXPECT_GT(client.serverCryptoByteCount(QuicEncryptionLevel::Handshake), handshakeBytesBefore)
                << "补发的字节没有续上 Handshake 流的空洞";
        finishAfterHandshakeGap(core, client);
    }

    /**
     * @brief 没有新确认的重复 ACK 也要判丢并补发，不必等定时器（RFC 9002 §A.7 第 5 步无条件）
     * @details 只认「有新确认才判丢」会让补发晚一个定时器粒度：这条用例整个跳过定时器
     */
    TEST(QuicConnectionCore, RetransmitsOnRedundantAcknowledgementWithoutWaitingForTimer)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        client.skipPacketNumber(QuicEncryptionLevel::Handshake, std::uint64_t{0});

        exchange(core, client, Timestamp{0});
        exchange(core, client, Timestamp{10000});
        ASSERT_FALSE(client.isHandshakeCompleted());

        const std::size_t handshakeBytesBefore = client.serverCryptoByteCount(QuicEncryptionLevel::Handshake);
        ASSERT_TRUE(core.onDatagramReceived(client.buildRepeatedAcknowledgement(QuicEncryptionLevel::Handshake),
                                            Timestamp{30000}).has_value());
        core.drive(Timestamp{30000});
        const std::vector<std::vector<std::uint8_t>> retransmitted = drain(core);
        ASSERT_FALSE(retransmitted.empty()) << "重复的确认里也该判丢并补发，而不是干等定时器";
        for (const auto &datagram : retransmitted)
        {
            client.consume(datagram);
        }
        EXPECT_GT(client.serverCryptoByteCount(QuicEncryptionLevel::Handshake), handshakeBytesBefore)
                << "补发的字节没有续上 Handshake 流的空洞";
        finishAfterHandshakeGap(core, client);
    }

    /**
     * @brief 没有可重发的握手字节时，探针退化成一条 PING
     * @details 此刻在途的是只带 HANDSHAKE_DONE 的那个包：它触发确认但不带字节，除了再探一条别无可做
     */
    TEST(QuicConnectionCore, ProbesWithPingWhenNoUnacknowledgedCryptoRemains)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        for (int round = 0; round < 3 && !(client.isHandshakeCompleted() && core.phase() == QuicConnectionPhase::Established); ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        ASSERT_TRUE(client.isHandshakeCompleted());
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);
        ASSERT_GT(client.handshakeDoneFrameCount(), 0U) << "握手已确认，DONE 该跟着发出来";

        // 故意漏掉最新收到的 Handshake 包（就是带 DONE 的那一个），让本端留一包在途
        exchange(core, client, Timestamp{40000}, QuicEncryptionLevel::Handshake);
        ASSERT_TRUE(core.nextTimeout().has_value());
        const Timestamp deadline = *core.nextTimeout();
        EXPECT_EQ(client.pingFrameCount(), 0U) << "还在等待窗口里就不该发探针";

        core.onTimeout(deadline);
        for (const auto &datagram : drain(core))
        {
            client.consume(datagram);
        }
        EXPECT_GE(client.pingFrameCount(), 1U) << "无可重发内容时必须靠 PING 维持探测（RFC 9002 §6.2.2）";
        EXPECT_GT(core.nextTimeout().value_or(Timestamp{0}), deadline) << "连续探测要按退避倍数拉长等待（§6.2.1）";
    }

    /**
     * @brief 在途被全部确认之后定时器该撤掉，不再骚扰对端
     */
    TEST(QuicConnectionCore, ClearsTheProbeOnceEverythingIsAcknowledged)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        ASSERT_FALSE(core.nextTimeout().has_value()) << "一个包都没发出去之前不该武装定时器";
        // 头两轮把飞行与 DONE 发完，后两轮是静默往返：客户端把最后收到的包确认掉，服务端才算「全部确认」
        for (int round = 0; round < 4; ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        ASSERT_TRUE(client.isHandshakeCompleted());
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);
        EXPECT_FALSE(core.nextTimeout().has_value()) << "在途清空了还留着定时器，对端会白挨探针";
    }

    /**
     * @brief 重传换了分片大小：与已交字节重叠的那一段剪掉再交给 TLS（RFC 9000 §7.5）
     * @details 两段分片刻意让后一段从头一段的中间开始。不剪的话 TLS 会看到重复字节，握手当场报错
     */
    TEST(QuicConnectionCore, TrimsOverlappingCryptoFragmentsBeforeFeedingTls)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        const auto [head, overlappingTail] = client.buildOverlappingCryptoFragments();
        ASSERT_FALSE(head.empty());
        ASSERT_FALSE(overlappingTail.empty());

        ASSERT_TRUE(core.onDatagramReceived(head, Timestamp{0}).has_value());
        core.drive(Timestamp{1000});
        ASSERT_TRUE(core.onDatagramReceived(overlappingTail, Timestamp{2000}).has_value());
        core.drive(Timestamp{3000});

        const std::vector<std::vector<std::uint8_t>> flight = drain(core);
        EXPECT_NE(core.phase(), QuicConnectionPhase::Closing) << "重叠的那一段没剪掉，TLS 收到了重复字节";
        ASSERT_FALSE(flight.empty()) << "两段合起来才是完整的 ClientHello，服务端该交出它的飞行";
        for (const auto &datagram : flight)
        {
            client.consume(datagram);
        }
        for (int round = 0; round < 6 && !(client.isHandshakeCompleted() && core.phase() == QuicConnectionPhase::Established); ++round)
        {
            exchange(core, client, Timestamp{10000 * (round + 1)});
        }
        EXPECT_TRUE(client.isHandshakeCompleted());
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Established);
    }

    /**
     * @brief 地址验证之前的回量夹在三倍已收字节以内，重发也不能越过这条线（RFC 9000 §8.1）
     * @details 客户端发完 ClientHello 就闭嘴：服务端既没解到 Handshake 报文（地址就没验证过），
     *          又得一次次重发自己的飞行。第二趟重发就已经超出三倍额度，之后每一趟都该被夹住
     */
    TEST(QuicConnectionCore, HoldsEarlyOutputBackAtThreeTimesTheBytesReceived)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        std::size_t receivedByteCount = 0;
        for (const auto &datagram : client.buildFlight())
        {
            receivedByteCount += datagram.size();
            ASSERT_TRUE(core.onDatagramReceived(datagram, Timestamp{0}).has_value());
        }
        core.drive(Timestamp{1000});
        std::size_t sentByteCount = 0;
        for (const auto &datagram : drain(core))
        {
            sentByteCount += datagram.size();
            client.consume(datagram);
        }
        ASSERT_GT(sentByteCount, receivedByteCount) << "服务端交出的飞行比收到的是多些，才谈得上夹三倍";
        ASSERT_LE(sentByteCount, 3 * receivedByteCount);

        // 一路探测超时打下去：额度用光之后连探针也发不出去（§8.1 排在 §7.5 的豁免之前）
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const std::optional<Timestamp> deadline = core.nextTimeout();
            ASSERT_TRUE(deadline.has_value());
            core.onTimeout(*deadline);
            for (const auto &datagram : drain(core))
            {
                sentByteCount += datagram.size();
            }
            EXPECT_LE(sentByteCount, 3 * receivedByteCount) << "第 " << attempt << " 趟重发越过了 §8.1 的三倍额度";
        }
    }

    /**
     * @brief 在途把拥塞窗口占满时，新数据（票据）先被压住；确认把在途销干净之后才放行
     * @details 探针豁免窗口（§7.5），于是一路「重发却没人确认」会把在途抬过初始窗口 12000。
     *          之后握手完成而产生的 NewSessionTicket 是新数据，不豁免，就该等在窗口外面
     */
    TEST(QuicConnectionCore, WithholdsNewCryptoWhileTheCongestionWindowIsFull)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        client.dropHandshakePackets(true);
        exchange(core, client, Timestamp{0});
        exchange(core, client, Timestamp{10000});
        ASSERT_EQ(client.serverReceivedPacketCount(QuicEncryptionLevel::Handshake), 0U) << "Handshake 包都该被丢掉";
        ASSERT_TRUE(core.nextTimeout().has_value());

        // 客户端在 Handshake 空间打一条 PING：服务端解得开它，地址就算验证过了，§8.1 的额度不再卡重发
        ASSERT_TRUE(core.onDatagramReceived(client.buildPing(QuicEncryptionLevel::Handshake), Timestamp{20000}).has_value());
        core.drive(Timestamp{21000});
        drain(core);

        // 十四趟探测超时：Initial 已经确认干净，欠的只剩 Handshake。每趟整段重发约 1.2KB，
        // 累计 16KB 以上，越过慢启动涨到 13200 的拥塞窗口
        Timestamp probeTime{0};
        for (int attempt = 0; attempt < 14; ++attempt)
        {
            const std::optional<Timestamp> deadline = core.nextTimeout();
            ASSERT_TRUE(deadline.has_value());
            probeTime = *deadline;
            core.onTimeout(probeTime);
            ASSERT_FALSE(drain(core).empty()) << "探针豁免拥塞窗口，第 " << attempt << " 趟也该发得出东西（§7.5）";
        }

        // 最后一趟不再丢包：客户端这才拿到服务端的 Finished，但那些探针它一条都没确认
        client.dropHandshakePackets(false);
        const std::optional<Timestamp> deadline = core.nextTimeout();
        ASSERT_TRUE(deadline.has_value());
        probeTime = *deadline;
        core.onTimeout(probeTime);
        for (const auto &datagram : drain(core))
        {
            client.consume(datagram);
        }
        ASSERT_TRUE(client.isHandshakeCompleted()) << "补发的字节该让客户端把握手走完";

        // 只交飞行、不交确认：服务端这时已经在满窗里，新数据该被压住
        for (const auto &datagram : client.buildFlight())
        {
            ASSERT_TRUE(core.onDatagramReceived(datagram, probeTime + Timestamp{1000}).has_value());
        }
        core.drive(probeTime + Timestamp{2000});
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);
        for (const auto &datagram : drain(core))
        {
            client.consume(datagram);
        }
        EXPECT_EQ(client.serverCryptoByteCount(QuicEncryptionLevel::Application), 0U)
                << "在途已经压满窗口，票据这种新数据不该再往上叠";

        // 把在途确认干净，窗口腾出来，同一批字节就该放行
        exchange(core, client, probeTime + Timestamp{20000});
        EXPECT_GT(client.serverCryptoByteCount(QuicEncryptionLevel::Application), 0U)
                << "窗口腾出来后，被压住的新数据该跟着下一轮产出";
    }

    /**
     * @brief 解到对端的 Handshake 报文之后，Initial 空间整体退休：不再补发、也不再为它亮定时器
     * @details 客户端一条 Initial 确认都没发出去，服务端的 Initial 在途因此永远清不干净。
     *          空间一退休就该连账一起销（RFC 9001 §4.9.1 + RFC 9002 §A.11）
     */
    TEST(QuicConnectionCore, RetiresTheInitialSpaceOnceThePeerSendsHandshakePackets)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get()));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        exchange(core, client, Timestamp{0});
        client.suppressAcknowledgements(QuicEncryptionLevel::Initial);
        for (int round = 1; round < 6 && !(client.isHandshakeCompleted() && core.phase() == QuicConnectionPhase::Established); ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        ASSERT_TRUE(client.isHandshakeCompleted()) << "Initial 少一份确认也该握得手——同一批字节在 Handshake 里还有一份";
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);

        const std::size_t initialPackets = client.serverReceivedPacketCount(QuicEncryptionLevel::Initial);
        ASSERT_GT(initialPackets, 1U) << "至少要留下一包没被确认的 Initial，这条用例才不是空转";
        // 再跑两轮静默往返：把 DONE 这些最后一批收到的包确认干净，只剩 Initial 那几包永远悬着
        exchange(core, client, Timestamp{100000});
        exchange(core, client, Timestamp{110000});
        EXPECT_FALSE(core.nextTimeout().has_value()) << "退休了的空间还留着在途账，定时器会一直亮着";
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::optional<Timestamp> deadline = core.nextTimeout();
            if (!deadline.has_value())
            {
                break;
            }
            core.onTimeout(*deadline);
            for (const auto &datagram : drain(core))
            {
                client.consume(datagram);
            }
        }
        EXPECT_EQ(client.serverReceivedPacketCount(QuicEncryptionLevel::Initial), initialPackets)
                << "Initial 空间退休后不该再补发它的握手字节";
    }

    /**
     * @brief 空闲到点就静默关闭：不发 CONNECTION_CLOSE，也不留任何待发
     * @details 对端本来就没在说话，收口报文发过去也没人接；本端把状态丢了就完事（RFC 9000 §10.1）
     */
    TEST(QuicConnectionCore, ClosesSilentlyWhenTheConnectionGoesIdle)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get(), kClientConnectionId, 30000));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        for (int round = 0; round < 4; ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        ASSERT_TRUE(client.isHandshakeCompleted());
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);

        // 用一条明确的 PING 把「最后一次收到包」的时刻钉死，之后只谈 30 秒的额度怎么算
        const Timestamp lastActivity{50000};
        ASSERT_TRUE(core.onDatagramReceived(client.buildPing(QuicEncryptionLevel::Application), lastActivity).has_value());
        core.drive(lastActivity + Timestamp{1000});
        for (const auto &datagram : drain(core))
        {
            client.consume(datagram);
        }

        const std::optional<Timestamp> deadline = core.nextTimeout();
        ASSERT_TRUE(deadline.has_value()) << "宣告了 max_idle_timeout 就该武装空闲定时器";
        // 服务端只回了 ACK：不触发确认的出包不该把计时推后（§10.1 只认「第一个触发确认的包」）
        EXPECT_EQ(*deadline, lastActivity + Timestamp{30000000});

        core.onTimeout(*deadline);
        EXPECT_EQ(core.phase(), QuicConnectionPhase::Closing);
        EXPECT_TRUE(core.isFinished()) << "静默关闭不留收口报文";
        EXPECT_TRUE(drain(core).empty()) << "空闲超时不该发出 CONNECTION_CLOSE（§10.2）";
    }

    /**
     * @brief 有效空闲超时取两端宣告里较小的那一个（§10.1）
     */
    TEST(QuicConnectionCore, UsesTheSmallerIdleTimeoutOfTheTwoPeers)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get(), kClientConnectionId, 30000));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId, false, 10000);
        for (int round = 0; round < 4; ++round)
        {
            exchange(core, client, Timestamp{10000 * round});
        }
        ASSERT_EQ(core.phase(), QuicConnectionPhase::Established);

        const Timestamp lastActivity{50000};
        ASSERT_TRUE(core.onDatagramReceived(client.buildPing(QuicEncryptionLevel::Application), lastActivity).has_value());
        core.drive(lastActivity + Timestamp{1000});
        drain(core);

        const std::optional<Timestamp> deadline = core.nextTimeout();
        ASSERT_TRUE(deadline.has_value());
        EXPECT_EQ(*deadline, lastActivity + Timestamp{10000000}) << "对端只宣告 10 秒，本端就不该按 30 秒留着连接";
    }

    /**
     * @brief 一路探测期间不该把自己判空闲：本端发起的触发确认的包也算活动（§10.1）
     * @details 对端一条确认都不发时，「最后一次收到包」的时刻永远不动；只看它的实现会在第 30 秒
     *          把还在努力恢复的连接关掉
     */
    TEST(QuicConnectionCore, KeepsProbingInsteadOfIdlingOut)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        ASSERT_NE(clientContext.get(), nullptr);

        QuicConnectionCore core(makeServerConfiguration(*serverContext.get(), kClientConnectionId, 30000));
        InMemoryQuicClient client(*clientContext.get(), kClientConnectionId);
        client.skipPacketNumber(QuicEncryptionLevel::Application, std::nullopt);
        client.dropHandshakePackets(true);
        exchange(core, client, Timestamp{0});
        exchange(core, client, Timestamp{10000});
        ASSERT_FALSE(client.isHandshakeCompleted()) << "Handshake 包被丢光，握手该停在半路";
        // 一条 Handshake 级的 PING 让服务端确认对端能收发包，§8.1 的额度不再挡住后面的重发
        ASSERT_TRUE(core.onDatagramReceived(client.buildPing(QuicEncryptionLevel::Handshake), Timestamp{20000}).has_value());
        core.drive(Timestamp{21000});
        drain(core);

        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const std::optional<Timestamp> deadline = core.nextTimeout();
            ASSERT_TRUE(deadline.has_value());
            core.onTimeout(*deadline);
            ASSERT_FALSE(drain(core).empty()) << "第 " << attempt << " 趟探测没东西可发，连接已经死了";
            EXPECT_NE(core.phase(), QuicConnectionPhase::Closing) << "还在恢复中就把自己判空闲关掉了";
        }
    }
} // namespace AsynGyanis::Net
