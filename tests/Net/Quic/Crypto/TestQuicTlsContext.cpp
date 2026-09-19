// TestQuicTlsContext.cpp —— QUIC 的 TLS 胶水（RFC 9001 §4.1）用例
//
// 覆盖四块：
//   1) 内存里跑完整握手：两端各一个上下文，把 TLS 记录**按级别**互相搬运，不碰 socket、不起网络，因此
//      结果与调度无关；轮数有上限，收敛不了就是失败而不是挂住；
//   2) 方向与级别：断言「服务端的写密钥 == 客户端的读密钥」（三个字段逐字节比），以及第一条
//      产出记录属于 Initial 级别。OpenSSL 的 crypto_send 回调**不带级别参数**，级别只能跟着
//      密钥切换自己维护（写方向定产出级别、读方向定入站缓冲），这两条断言正是钉住那个跟踪的；
//   3) transport parameters 原样往返：本类不解释其语义，只保证交回来的字节与对端设进去的一致；
//   4) 失败路径：喂进状态机不接受的消息要判 Failed（而不是继续等数据），且能拿到 TLS 告警码。
// 证书用仓库内的自签夹具（与 HTTPS 用例同一份），因此不依赖任何外部服务。

#include "Net/Quic/Crypto/QuicTlsContext.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;

        /// 夹具 SSL 上下文的持有者：作用域结束即释放，避免用例中途抛异常时漏掉
        class FixtureContext
        {
        public:
            FixtureContext() = default;
            FixtureContext(const FixtureContext &) = delete;
            FixtureContext &operator=(const FixtureContext &) = delete;

            /// 允许把建好的上下文交回调用方：删掉拷贝之后没有移动构造就 return 不出来
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

            /// 建一个客户端用的上下文：夹具证书是自签的，关掉校验才能握上手
            [[nodiscard]] static FixtureContext client()
            {
                FixtureContext holder;
                holder.m_context = SSL_CTX_new(TLS_client_method());
                SSL_CTX_set_min_proto_version(holder.m_context, TLS1_3_VERSION);
                SSL_CTX_set_verify(holder.m_context, SSL_VERIFY_NONE, nullptr);
                return holder;
            }

            /// 建一个服务端用的上下文，装上仓库内的自签证书与私钥
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

        /**
         * @brief 拼一份最小可辨识的 transport parameters 字节
         * @details 本类把这段字节当作不透明扩展交给 TLS，因此只需要形状像样：每项是
         *          「参数标识 + 长度 + 值」，都是变长整数（RFC 9000 §18）。
         * @param parameterId 首项的参数标识
         * @param valueHex 首项的值（十六进制）
         * @return std::vector<std::uint8_t> 编码结果
         */
        std::vector<std::uint8_t> makeTransportParameters(const std::uint64_t parameterId, const std::string_view valueHex)
        {
            const auto value = makeBytesFromHex(valueHex);
            std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(parameterId), static_cast<std::uint8_t>(value.size())};
            bytes.insert(bytes.end(), value.begin(), value.end());
            // 再挂一项 initial_max_data（标识 0x04），让参数不止一项，验证拼接不会只搬第一段
            const auto maximumData = makeBytesFromHex("40ffff");
            bytes.push_back(0x04);
            bytes.push_back(static_cast<std::uint8_t>(maximumData.size()));
            bytes.insert(bytes.end(), maximumData.begin(), maximumData.end());
            return bytes;
        }

        /**
         * @brief 两侧各推进一轮握手
         * @details 这里的返回值刻意不检查：收敛与失败由外面的断言判，本函数只负责「再走一步」。
         * @param first 一侧上下文
         * @param second 另一侧上下文
         */
        void pumpBoth(QuicTlsContext &first, QuicTlsContext &second)
        {
            static_cast<void>(first.drive());
            static_cast<void>(second.drive());
        }

        /**
         * @brief 把一侧产出的 TLS 记录按级别搬到另一侧的入站缓冲
         * @param from 产出方
         * @param to 接收方
         */
        void shuttle(QuicTlsContext &from, QuicTlsContext &to)
        {
            while (const auto record = from.takeOutboundRecord())
            {
                to.feedHandshakeData(record->level, record->data);
            }
        }
    } // namespace

    /**
     * @brief 内存里跑完一次握手：两侧都完成、无告警，且对端参数原样回来
     */
    TEST(QuicTlsContext, CompletesHandshakeInMemory)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr) << "夹具证书加载失败";
        ASSERT_NE(clientContext.get(), nullptr);

        const auto serverParameters = makeTransportParameters(0x00, "8394c8f03e515708");
        const auto clientParameters = makeTransportParameters(0x00, "f0eec687a7eb7f48");
        QuicTlsContext server(*serverContext.get(), true, serverParameters);
        QuicTlsContext client(*clientContext.get(), false, clientParameters);

        bool isBothCompleted = false;
        for (int round = 0; round < 20; ++round)
        {
            const QuicTlsProgress serverProgress = server.drive();
            const QuicTlsProgress clientProgress = client.drive();
            shuttle(client, server);
            shuttle(server, client);
            if (serverProgress == QuicTlsProgress::Failed || clientProgress == QuicTlsProgress::Failed)
            {
                break;
            }
            if (server.isHandshakeCompleted() && client.isHandshakeCompleted())
            {
                isBothCompleted = true;
                break;
            }
        }

        ASSERT_TRUE(isBothCompleted) << "握手没能在 20 轮内收敛，说明记录搬运或级别跟踪有问题";
        EXPECT_FALSE(server.alert().has_value());
        EXPECT_FALSE(client.alert().has_value());
        EXPECT_FALSE(server.peerTransportParameters().empty());
        // 本类只搬不解释：对端设进去的字节要原样回来
        EXPECT_EQ(std::vector<std::uint8_t>(server.peerTransportParameters().begin(), server.peerTransportParameters().end()),
                  clientParameters);
        EXPECT_EQ(std::vector<std::uint8_t>(client.peerTransportParameters().begin(), client.peerTransportParameters().end()),
                  serverParameters);
    }

    /**
     * @brief 写密钥与对端读密钥必须逐字节相同，且第一条产出记录属于 Initial 级别
     */
    TEST(QuicTlsContext, YieldsMatchingKeysPerLevelAndTracksTransmissionLevel)
    {
        const FixtureContext serverContext = FixtureContext::server();
        const FixtureContext clientContext = FixtureContext::client();
        ASSERT_NE(serverContext.get(), nullptr);
        const auto parameters = makeTransportParameters(0x00, "8394c8f03e515708");
        QuicTlsContext server(*serverContext.get(), true, parameters);
        QuicTlsContext client(*clientContext.get(), false, parameters);

        // 服务端第一条产出必然是 Initial：crypto_send 不带级别，级别由写方向的密钥切换驱动，
        // 若这个跟踪错位，第一个 Flight 会带着 Handshake 级别的密钥发出去，对端解不开
        static_cast<void>(client.drive());
        shuttle(client, server);
        const QuicTlsProgress firstProgress = server.drive();
        ASSERT_NE(firstProgress, QuicTlsProgress::Failed);
        const auto firstRecord = server.takeOutboundRecord();
        ASSERT_TRUE(firstRecord.has_value()) << "服务端该产出首批握手数据";
        EXPECT_EQ(firstRecord->level, QuicEncryptionLevel::Initial);
        EXPECT_FALSE(firstRecord->data.empty());
        // 取走的那条也要交回去，否则客户端缺了 ServerHello，后面的循环再怎么搬也握不完
        client.feedHandshakeData(firstRecord->level, firstRecord->data);

        for (int round = 0; round < 20; ++round)
        {
            pumpBoth(server, client);
            shuttle(client, server);
            shuttle(server, client);
            if (server.isHandshakeCompleted() && client.isHandshakeCompleted())
            {
                break;
            }
        }
        ASSERT_TRUE(server.isHandshakeCompleted());
        ASSERT_TRUE(client.isHandshakeCompleted());
        ASSERT_TRUE(server.cipherSuite().has_value());
        EXPECT_EQ(*server.cipherSuite(), *client.cipherSuite());

        for (const QuicEncryptionLevel level: {QuicEncryptionLevel::Handshake, QuicEncryptionLevel::Application})
        {
            const QuicPacketKeys *serverWriting = server.keys(level, QuicKeyDirection::Writing);
            const QuicPacketKeys *clientReading = client.keys(level, QuicKeyDirection::Reading);
            ASSERT_NE(serverWriting, nullptr);
            ASSERT_NE(clientReading, nullptr);
            EXPECT_EQ(serverWriting->encryptionKey, clientReading->encryptionKey) << "级别 " << static_cast<int>(level);
            EXPECT_EQ(serverWriting->initializationVector, clientReading->initializationVector);
            EXPECT_EQ(serverWriting->headerProtectionKey, clientReading->headerProtectionKey);
            // 两个方向必须不同：若密钥槽位方向写反，上面的相等断言仍会成立而这条会红
            EXPECT_NE(serverWriting->encryptionKey, server.keys(level, QuicKeyDirection::Reading)->encryptionKey);
        }
    }

    /**
     * @brief 对端发来当前状态不接受的消息要判失败，而不是继续等数据
     */
    TEST(QuicTlsContext, FailsOnUnparsableHandshakeData)
    {
        const FixtureContext serverContext = FixtureContext::server();
        ASSERT_NE(serverContext.get(), nullptr);
        const auto parameters = makeTransportParameters(0x00, "8394c8f03e515708");
        QuicTlsContext server(*serverContext.get(), true, parameters);

        // 一条 ServerHello（类型 0x02）：服务端在等 ClientHello，收到它就是 unexpected_message
        // 注意这里没有 TLS 记录头——本类的入站字节就是握手消息流本身
        server.feedHandshakeData(QuicEncryptionLevel::Initial, makeBytesFromHex("0200000403030000"));
        const QuicTlsProgress progress = server.drive();
        EXPECT_EQ(progress, QuicTlsProgress::Failed);
        EXPECT_TRUE(server.alert().has_value()) << "失败要靠告警码给收口原因，不能只报个失败";
        EXPECT_FALSE(server.isHandshakeCompleted());
    }
} // namespace AsynGyanis::Net
