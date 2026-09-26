// QuicConnection 外壳的直测：这条壳不含协议逻辑，但它持有服务端全部的「什么时候该再刷一次、
// 什么时候可以摘掉」的判据——accept 的拒绝面、路由键长度、待刷标记、活动记账与到期时刻。
// 报文出口用假实现，因此这些用例不碰套接字，也不依赖任何对端实现。
#include "Net/Quic/QuicConnection.h"

#include "Core/Coroutine/Task.h"
#include "Platform/IO/Socket.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端签发标识的字节数：短头报文不携带目的标识长度，路由只能按这个定长截
        constexpr std::size_t kIssuedConnectionIdLength = QuicConnection::kSourceConnectionIdLength;

        /// 客户端自报的目的标识字节数：v1 允许 20 字节以内，取 RFC 9001 附录里那个 8 字节的形状
        constexpr std::size_t kClientDestinationConnectionIdLength = 8;

        /// 用例统一采用的空闲超时：也用来核对 nextExpiry() 是不是按本端宣告的 idleTimeout 算
        constexpr std::chrono::milliseconds kIdleTimeout{5000};

        /// 只用于 TLS 会话创建的空上下文：外壳的用例不做握手，因此不需要证书与 ALPN
        std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)> makeBareTlsContext()
        {
            SSL_CTX *const rawContext = SSL_CTX_new(TLS_server_method());
            return {rawContext, [](SSL_CTX *context)
                    {
                        if (context != nullptr)
                        {
                            SSL_CTX_free(context);
                        }
                    }};
        }

        /**
         * @brief 造一条能过包头解码的客户端 Initial
         * @details 外壳只解明文头部（不 decrypt），因此载荷可以全零：首字节 0xC3 = 长头固定位 +
         *          Initial 类型 + 包号 4 字节，版本 1，两条 8 字节连接标识，零长 Token，
         *          Length=5（包号 4 字节 + 1 字节载荷）。
         * @param clientChosenDestinationId 客户端凭空造的目的标识，Initial 密钥由它推导
         * @return std::vector<std::uint8_t> 一条完整的 Initial 数据报
         */
        std::vector<std::uint8_t> makeClientInitial(const std::uint8_t clientChosenDestinationId = 0xA5)
        {
            std::vector<std::uint8_t> datagram{
                    0xC3, 0x00, 0x00, 0x00, 0x01, static_cast<std::uint8_t>(kClientDestinationConnectionIdLength),
            };
            datagram.reserve(30U);
            for (std::size_t byteIndex = 0; byteIndex < kClientDestinationConnectionIdLength; ++byteIndex)
            {
                datagram.push_back(static_cast<std::uint8_t>(clientChosenDestinationId + byteIndex));
            }
            // 源连接标识：客户端自己的临时标识，8 字节
            datagram.push_back(8);
            for (std::uint8_t byteIndex = 0; byteIndex < 8; ++byteIndex)
            {
                datagram.push_back(static_cast<std::uint8_t>(0x10 + byteIndex));
            }
            datagram.push_back(0x00); // Token 长度
            datagram.push_back(0x05); // Length：包号 4 字节 + 载荷 1 字节
            datagram.resize(datagram.size() + 5U, 0x00);
            return datagram;
        }

        /**
         * @brief 一份可直接交给 accept() 的连接配置，报文出口把写出的长度记进台账
         * @param tlsContext 供会话创建用的上下文（生命周期由调用方持有）
         * @param sentDatagramLengths 出参：每次交给发送口的报文字节数
         * @return QuicConnection::Configuration 填好的配置
         */
        QuicConnection::Configuration makeConfiguration(SSL_CTX &tlsContext, std::vector<std::size_t> &sentDatagramLengths)
        {
            QuicConnection::Configuration configuration;
            configuration.tlsContext   = &tlsContext;
            configuration.idleTimeout  = kIdleTimeout;
            configuration.sendDatagram = [&sentDatagramLengths](const Platform::SocketAddress &, const std::uint8_t *, const std::size_t length) -> Core::Task<bool>
            {
                sentDatagramLengths.push_back(length);
                co_return true;
            };
            return configuration;
        }

        /// 造一个回环 IPv4 地址（外壳只把它当对端地址搬运，不做任何网络动作）
        Platform::SocketAddress makeLoopbackAddress(const std::uint16_t port)
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
         * @brief 一条已接受连接的夹具：持有 TLS 上下文、发送台账与被测连接
         *
         * @details 报文出口是假实现，因此协程不会真的挂起——`runToCompletion()` 用一次 resume
         *          就能跑到底，用例全程单线程，不需要事件循环。
         */
        class AcceptedConnectionFixture
        {
        public:
            explicit AcceptedConnectionFixture(const std::uint8_t clientChosenDestinationId = 0xA5) :
                m_tlsContext(makeBareTlsContext()), m_configuration(makeConfiguration(*m_tlsContext, m_sentDatagramLengths)),
                m_connection(QuicConnection::accept(m_configuration, makeLoopbackAddress(4433), makeLoopbackAddress(55000), makeClientInitial(clientChosenDestinationId)))
            {
            }

            AcceptedConnectionFixture(const AcceptedConnectionFixture &)            = delete;
            AcceptedConnectionFixture &operator=(const AcceptedConnectionFixture &) = delete;

            [[nodiscard]] QuicConnection &connection() const
            {
                return *m_connection;
            }

            [[nodiscard]] bool accepted() const noexcept
            {
                return m_connection != nullptr;
            }

            /**
             * @brief 把这条连接的 flush() 跑到底
             * @details 假发送口不会真的挂起，因此一次 resume 就该到底；跑不完说明这条链上出现了
             *          没被满足的等待，那本身就是待查的问题。
             */
            void flushUntilSettled()
            {
                const Core::Task<> flushTask = m_connection->flush();
                flushTask.handle().resume();
                EXPECT_TRUE(flushTask.handle().done()) << "flush 没跑完：假发送口上出现了不该有的挂起";
            }

            /**
             * @brief 把一段字节交给 handleDatagram() 跑到底
             * @param datagram 待交付的数据报
             */
            void deliver(const std::vector<std::uint8_t> &datagram)
            {
                const Core::Task<> handlingTask = m_connection->handleDatagram(makeLoopbackAddress(55000), std::span<const std::uint8_t>(datagram));
                handlingTask.handle().resume();
                EXPECT_TRUE(handlingTask.handle().done()) << "handleDatagram 没跑完";
            }

            /// 已经交给发送口的报文条数
            [[nodiscard]] std::size_t sentDatagramCount() const noexcept
            {
                return m_sentDatagramLengths.size();
            }

        private:
            std::vector<std::size_t>                      m_sentDatagramLengths; ///< 发送台账：每条报文的字节数
            std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)> m_tlsContext;          ///< 会话上下文，须比连接活得久
            QuicConnection::Configuration                 m_configuration;       ///< 交给 accept 的配置副本
            std::unique_ptr<QuicConnection>               m_connection;          ///< 被测连接
        };
    } // namespace

    TEST(QuicConnection, DoesNotBuildAStateMachineForUnacceptableDatagrams)
    {
        auto                                tlsContext = makeBareTlsContext();
        std::vector<std::size_t>            sentLengths;
        const QuicConnection::Configuration configuration = makeConfiguration(*tlsContext, sentLengths);

        // 一条都不该被接受：解不出头部、不是长头、版本不对、类型是 Retry/Handshake/0-RTT、
        // 标识长度越过 v1 上限、Length 与实际字节数不符
        const std::vector<std::pair<const char *, std::vector<std::uint8_t>>> rejectedCases = {
                {"空数据报", {}},
                {"只有首字节与版本", {0xC3, 0x00, 0x00, 0x00, 0x01}},
                {"固定位为 0 的长头", {0x83, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}},
                {"版本 2", {0xC3, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00}},
                {"版本为 0 的协商报文", {0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
                {"Retry 类型", {0xCD, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}},
        };
        for (const auto &[caseName, datagram]: rejectedCases)
        {
            EXPECT_EQ(QuicConnection::accept(configuration, makeLoopbackAddress(4433), makeLoopbackAddress(55000), datagram), nullptr) << "被拒的那条没生效：" << caseName;
        }

        // 头部形状合法、但 Length 字段与实际字节数不符的两条：一条声明得比实收长，一条声明为 0
        std::vector<std::uint8_t> overClaiming = makeClientInitial();
        overClaiming.back()                    = 0x7F;
        overClaiming.resize(overClaiming.size() - 3U);
        EXPECT_EQ(QuicConnection::accept(configuration, makeLoopbackAddress(4433), makeLoopbackAddress(55000), overClaiming), nullptr)
                << "Length 超出实收字节的数据报被当成了合法 Initial";

        // 目的标识长度越过 v1 的 20 字节上限：放行会让路由键长度失控
        std::vector<std::uint8_t> oversizedConnectionId = makeClientInitial();
        oversizedConnectionId[5]                        = 21;
        oversizedConnectionId.resize(oversizedConnectionId.size() + 13U, 0x00);
        EXPECT_EQ(QuicConnection::accept(configuration, makeLoopbackAddress(4433), makeLoopbackAddress(55000), oversizedConnectionId), nullptr)
                << "目的标识 21 字节的数据报被当成了合法 Initial";
        EXPECT_TRUE(sentLengths.empty()) << "接受阶段的判定不该碰发送口";
    }

    TEST(QuicConnection, RefusesToAcceptWithoutACompleteConfiguration)
    {
        auto                     tlsContext = makeBareTlsContext();
        std::vector<std::size_t> sentLengths;

        // 缺 TLS 上下文：连会话都建不起来，不该往下走
        QuicConnection::Configuration missingTlsContext = makeConfiguration(*tlsContext, sentLengths);
        missingTlsContext.tlsContext                    = nullptr;
        EXPECT_EQ(QuicConnection::accept(missingTlsContext, makeLoopbackAddress(4433), makeLoopbackAddress(55000), makeClientInitial()), nullptr);

        // 缺报文出口：握手字节能解出去却没有地方写，等价于建一条必然卡死的连接
        QuicConnection::Configuration missingSender = makeConfiguration(*tlsContext, sentLengths);
        missingSender.sendDatagram                  = nullptr;
        EXPECT_EQ(QuicConnection::accept(missingSender, makeLoopbackAddress(4433), makeLoopbackAddress(55000), makeClientInitial()), nullptr);
        EXPECT_TRUE(sentLengths.empty());
    }

    TEST(QuicConnection, IssuesAFixedLengthAndDistinctRoutingKeyPerConnection)
    {
        AcceptedConnectionFixture firstConnection;
        ASSERT_TRUE(firstConnection.accepted());
        AcceptedConnectionFixture secondConnection(0xB7);
        ASSERT_TRUE(secondConnection.accepted());

        // 长度必须是外壳自己声明的那个定长：入向短头按它截目的标识，短一分则路由不中、长一分则越界
        EXPECT_EQ(firstConnection.connection().sourceConnectionId().size(), kIssuedConnectionIdLength);

        // 两条连接的标识不能相同：路由表以它为键，撞车意味着一个客户端的报文被送给另一个的会话
        EXPECT_NE(firstConnection.connection().sourceConnectionId(), secondConnection.connection().sourceConnectionId());

        // 刚接受的连接既没收口也没有待发字节：服务端的定时拍因此不会为它空跑一次 flush
        EXPECT_FALSE(firstConnection.connection().isClosed());
        EXPECT_FALSE(firstConnection.connection().needsFlush());
        EXPECT_FALSE(firstConnection.connection().hasActivity());
    }

    /**
     * @brief 每个「攒下待发字节」的入口都要把待刷标记举起来
     * @details 收报文路径会顺手 flush，但业务协程可能在它之外写数据（先 await 了定时器或磁盘）：
     *          那时唯一的补刷通道就是服务端定时拍读到的这个标记。少举一次，响应就躺到下一次报文或
     *          PTO 才出去。
     */
    TEST(QuicConnection, EveryQueuingEntryPointRaisesThePendingFlushFlag)
    {
        AcceptedConnectionFixture fixture;
        ASSERT_TRUE(fixture.accepted());
        QuicConnection &connection = fixture.connection();

        // 对端的 transport parameters 还没到手，本端的开流额度按 0 算：此时开不出流是合规的
        EXPECT_EQ(connection.openUnidirectionalStream(), -1) << "对端额度未知就放行开流，等于无视 §18.2 的流数上限";

        // 后面的入口都挂在「对端发起的那条流」上：这类流不需要本端有额度就能记账
        constexpr std::int64_t peerInitiatedBidirectionalStreamId = 0;

        const std::string                   payload = "hello";
        const std::span<const std::uint8_t> payloadBytes(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
        EXPECT_EQ(connection.queueStreamData(peerInitiatedBidirectionalStreamId, payloadBytes, false), payload.size()) << "正文没被收下，后面的待刷判定就全是空转";
        EXPECT_TRUE(connection.needsFlush()) << "排进去的正文没被记成待发";

        // flush 之后标记归零：否则服务端的定时拍会每拍都为这条连接空跑一次
        fixture.flushUntilSettled();
        EXPECT_FALSE(connection.needsFlush()) << "flush 跑完仍报待发：这一条连接会被定时拍反复叫起来空转";

        // 归还接收额度：对端据此决定还发不发正文，漏刷的表现是「客户端安静地不再发数据」
        connection.extendReceiveWindow(peerInitiatedBidirectionalStreamId, payload.size());
        EXPECT_TRUE(connection.needsFlush()) << "归还的接收窗口没被记成待发";

        fixture.flushUntilSettled();
        EXPECT_FALSE(connection.needsFlush());

        // 收口一条流：RESET_STREAM 与 STOP_SENDING 都要发出去，否则对端只能干等
        connection.abortStream(peerInitiatedBidirectionalStreamId, 0x100);
        EXPECT_TRUE(connection.needsFlush()) << "作废一条流没被记成待发";

        fixture.flushUntilSettled();
        EXPECT_FALSE(connection.needsFlush());
    }

    TEST(QuicConnection, TracksHowManyCoroutinesAreHoldingTheConnection)
    {
        AcceptedConnectionFixture fixture;
        ASSERT_TRUE(fixture.accepted());
        QuicConnection &connection = fixture.connection();

        {
            const QuicConnection::ActivityGuard outerGuard(connection);
            EXPECT_TRUE(connection.hasActivity());
            {
                const QuicConnection::ActivityGuard innerGuard(connection);
                EXPECT_TRUE(connection.hasActivity());
            }
            // 计数不是布尔量：只归还不干净的那一半会在摘除时把还活着的协程丢在外面
            EXPECT_TRUE(connection.hasActivity());
        }
        EXPECT_FALSE(connection.hasActivity());
    }

    TEST(QuicConnection, StopsProducingOnceCloseIsRequested)
    {
        AcceptedConnectionFixture fixture;
        ASSERT_TRUE(fixture.accepted());
        QuicConnection &connection = fixture.connection();

        const std::string                   payload = "hello";
        const std::span<const std::uint8_t> payloadBytes(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
        static_cast<void>(connection.queueStreamData(0, payloadBytes, false));
        fixture.flushUntilSettled();
        const std::size_t datagramCountBeforeClose = fixture.sentDatagramCount();

        connection.requestClose();
        EXPECT_TRUE(connection.isClosed());

        // 只置标志、不发收口报文：这条连接此后再也不能往发送口交东西，也开不出新流
        fixture.flushUntilSettled();
        EXPECT_EQ(fixture.sentDatagramCount(), datagramCountBeforeClose) << "已请求收口的连接还在往发送口交报文";
        EXPECT_EQ(connection.openUnidirectionalStream(), -1) << "已收口的连接还能开流";
    }

    TEST(QuicConnection, NextExpiryFollowsTheAdvertisedIdleTimeout)
    {
        AcceptedConnectionFixture fixture;
        ASSERT_TRUE(fixture.accepted());
        const QuicConnection &connection = fixture.connection();

        const auto now = std::chrono::steady_clock::now();
        // 服务端的定时拍只在「到期时刻已过」时才去问这条连接该不该收口，因此一个没有截止时刻的连接
        // 等同于永远不被检查——它可能一个字节都没发出去过，也就没有任何重传定时器会来补这一刀
        const auto expiry = connection.nextExpiry();
        ASSERT_NE(expiry, (std::chrono::steady_clock::time_point::max())) << "新建连接没有截止时刻：只发过一份报文的对端可以把这条表项永久留在路由表里";
        EXPECT_GE(expiry, now + kIdleTimeout - std::chrono::seconds{1}) << "到期时刻比宣告的空闲超时早到太多";
        EXPECT_LE(expiry, now + kIdleTimeout + std::chrono::seconds{1}) << "到期时刻比宣告的空闲超时晚到太多";
    }

    /**
     * @brief 解不开的报文既不算活动、也不该让这条连接变成「无人可收」的表项
     * @details 反放大要求解不开的包不能拿来续命（RFC 9000 §10.1），但同一条规则的另一面是：
     *          这条表项必须仍然有收口的时候。两半合起来才是可接受的拒绝服务代价——
     *          只做到前一半，攻击者拿一份解不开的 Initial 就能占掉一个永久槽位。
     */
    TEST(QuicConnection, UndecryptableDatagramsNeitherExtendTheDeadlineNorClearIt)
    {
        AcceptedConnectionFixture fixture;
        ASSERT_TRUE(fixture.accepted());
        QuicConnection &connection = fixture.connection();

        const auto expiryAfterAccept = connection.nextExpiry();
        ASSERT_NE(expiryAfterAccept, (std::chrono::steady_clock::time_point::max()));

        // 载荷不是合法的受保护报文，解密一定失败：这类报文一条都不该被当成活动
        for (int round = 0; round < 20; ++round)
        {
            fixture.deliver(makeClientInitial());
        }

        EXPECT_FALSE(connection.isClosed());
        EXPECT_EQ(connection.nextExpiry(), expiryAfterAccept) << "解不开的报文给这条连接续了命";
    }
} // namespace AsynGyanis::Net
