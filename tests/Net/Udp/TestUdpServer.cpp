// UdpServer 的公开面直测：绑定与启动凭据、逐条交付（含零长报文与缓冲截断）、按来源回包、
// 主动下发、处理器抛异常与读数报错都不带走收循环、超限应答整条拒发、收口叫醒挂在读数上的协程。
//
// 对端是本线程上一条**真实**的 UDP 套接字，不是事件循环里的封装：数据报的线上语义要用真套接字量
// （createPair 在 POSIX 是 AF_UNIX、Windows 是环回 TCP，都不算 UDP），而「有没有回包」由字节本身裁定。
// 用例一律不等「没有回包」——那种判据只能靠赌时序；该发而没发的出口全部改看服务端的计数，
// 以及「下一次往返仍然拿到应答」这个正面信号。
#include "Net/Udp/UdpServer.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "CoreTestSupport.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Core::TestSupport::captureCoroutineFailure;
        using AsynGyanis::Core::TestSupport::EventLoopThread;
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /// 对端等一条报文的有界上限：回环上的应答本该在毫秒级到达，超了这个上限就是没回来
        constexpr std::chrono::milliseconds kPeerWaitTimeout{5000};

        /// 读数报回的非零错误码：各平台取值不同（WSAECONNRESET / EHOSTUNREACH …），
        /// 而被测的那条判据只看「是不是零」，这里给一个非零值即可
        constexpr int kPeerGoneErrorCode{1};

        /**
         * @brief 文本与报文字节之间换算（用例正文按文本写，收发按字节走）
         * @param text 文本
         * @return std::vector<std::uint8_t> 同一串字节
         */
        [[nodiscard]] std::vector<std::uint8_t> toBytes(const std::string_view text)
        {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(text.size());
            for (const char character: text)
            {
                bytes.push_back(static_cast<std::uint8_t>(character));
            }
            return bytes;
        }

        /**
         * @brief 报文字节转回文本，让失败原因读得出人话
         * @param bytes 报文净字节
         * @return std::string 同一串字符
         */
        [[nodiscard]] std::string toText(const std::vector<std::uint8_t> &bytes)
        {
            std::string text;
            text.reserve(bytes.size());
            for (const std::uint8_t byte: bytes)
            {
                text.push_back(static_cast<char>(byte));
            }
            return text;
        }

        /**
         * @brief 驱动 listen() 直到收口，并把「收循环已退出」写成原子标记
         * @param listenTask 服务端交出来的收循环协程
         * @param isListenFinished 出参：收循环退出后最后置的标记
         * @return Core::Task<void> 收循环退出时完成
         * @note 「stop() 有没有叫醒挂在读数上的协程」在外部只能这样观测：帧留在夹具里活到
         *       循环线程 join 之后，标记按 release 发布、读侧 acquire 配对
         */
        Core::Task<void> driveListenTask(Core::Task<> listenTask, std::atomic<bool> &isListenFinished)
        {
            co_await std::move(listenTask);
            isListenFinished.store(true, std::memory_order_release);
        }

        /**
         * @brief 后台循环线程上的一台 UdpServer，加上本线程一条真实 UDP 套接字当对端
         */
        class UdpServerFixture
        {
        public:
            /**
             * @brief 起服务端并等到它进入监听
             * @param handler 报文处理器
             * @param maximumDatagramByteCount 收包缓冲容量
             * @param adoptedBoundSocket 给出时走「接手别人已绑好的套接字」那条路：服务端不再自己 bind，
             *        端口由这份套接字给出（所有权归服务端）
             */
            UdpServerFixture(UdpServer::MessageHandler handler, std::size_t maximumDatagramByteCount = Platform::DatagramSocket::kMaximumDatagramBytes,
                             std::optional<Platform::DatagramSocket> adoptedBoundSocket = std::nullopt)
            {
                UdpServer::Configuration configuration;
                configuration.maximumDatagramByteCount = maximumDatagramByteCount;
                configuration.onMessage                = std::move(handler);

                const bool isAdopting = adoptedBoundSocket.has_value();
                m_server              = isAdopting ? std::make_unique<UdpServer>(m_loop, std::move(configuration), std::move(*adoptedBoundSocket))
                                                   : std::make_unique<UdpServer>(m_loop, std::move(configuration));
                m_peer                = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
                // 夹具里一律用 EXPECT_ 而不是 ASSERT_：构造函数返回不了值，ASSERT 宏展开成的
                // `return;` 在这里直接编不过（C2534）。没起来的话后面每条断言都会红，
                // 而红的位置已经带上了「应答没到」这句原因
                EXPECT_TRUE(m_peer.isValid()) << "对端套接字绑定失败（错误码 " << Platform::PlatformError::lastSocketErrorCode() << "）";

                // 首次恢复交给循环线程：收循环会在第一次读数时就地注册观察者
                m_listenDriver.emplace(driveListenTask(isAdopting ? m_server->listen() : m_server->listen(Core::InetAddress("127.0.0.1", 0)), m_isListenFinished));
                m_loop.scheduler().scheduleRemote(m_listenDriver->handle());
                EXPECT_TRUE(waitForCondition([this] { return m_server->listeningPort() != 0U; })) << "服务端没在时限内进入监听";
            }

            /**
             * @brief 析构时收口：把 stop() 投到循环线程上执行并等它做完
             */
            ~UdpServerFixture()
            {
                stopOnLoopThread();
            }

            UdpServerFixture(const UdpServerFixture &) = delete;

            UdpServerFixture &operator=(const UdpServerFixture &) = delete;

            /**
             * @brief 在循环线程上执行 stop() 并等它返回
             * @details stop() 要销毁那份注册对象，只有所属循环线程能碰它；用例自己调过之后
             *          析构不再重复投（重复调用只生效一次）
             */
            void stopOnLoopThread()
            {
                if (m_isStopRequested)
                {
                    return;
                }
                m_isStopRequested = true;

                std::atomic<bool> isStopFinished{false};
                m_loop.scheduler().postRemote(
                        [this, &isStopFinished]
                        {
                            m_server->stop();
                            isStopFinished.store(true, std::memory_order_release);
                        });
                static_cast<void>(waitForCondition([&isStopFinished] { return isStopFinished.load(std::memory_order_acquire); }));
            }

            /**
             * @brief 服务端实际监听的地址
             * @return Core::InetAddress 回环地址 + 内核分配的端口
             */
            [[nodiscard]] Core::InetAddress serverAddress() const
            {
                return Core::InetAddress("127.0.0.1", m_server->listeningPort());
            }

            /**
             * @brief 对端自己的地址（服务端主动下发时发到这里）
             * @return Core::InetAddress 回环地址 + 内核分配的端口
             */
            [[nodiscard]] Core::InetAddress peerAddress() const
            {
                const Platform::SocketAddress address = m_peer.localAddress();
                return Core::InetAddress(address.storage, address.length);
            }

            /**
             * @brief 对端发一条报文给服务端
             * @param payload 报文净字节；空区间就是合法的零长报文
             * @return true 整条已交给内核
             */
            [[nodiscard]] bool sendToServer(const std::span<const std::uint8_t> payload) const
            {
                const Platform::SocketAddress target = serverAddress().platformAddress();
                return m_peer.send(target, payload.empty() ? nullptr : payload.data(), payload.size()) >= 0;
            }

            /**
             * @brief 对端收一条报文（有界轮询直到字节到齐）
             * @param timeout 等待上限
             * @return std::optional<std::vector<std::uint8_t>> 收到的字节；空 optional 表示超时没收到
             * @note 零长报文是「收到了」的结果之一（size 为 0），与没收到分得很清
             */
            [[nodiscard]] std::optional<std::vector<std::uint8_t>> receiveFromServer(std::chrono::milliseconds timeout = kPeerWaitTimeout) const
            {
                std::vector<std::uint8_t> buffer(Platform::DatagramSocket::kMaximumDatagramBytes);
                const auto                deadline = std::chrono::steady_clock::now() + timeout;
                do
                {
                    Platform::SocketAddress source;
                    const ssize_t           received = m_peer.receive(buffer.data(), buffer.size(), source);
                    if (received >= 0)
                    {
                        return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + received);
                    }
                    // 这里只有两类来路：暂时没有数据，或 ICMP 替一个已消失的对端捎回的错误码。
                    // 后者不影响这条套接字继续收，与收循环同一判据，照旧等到期限
                    static_cast<void>(std::this_thread::sleep_for(std::chrono::milliseconds{1}));
                } while (std::chrono::steady_clock::now() < deadline);
                return std::nullopt;
            }

            /**
             * @brief 被测服务端
             * @return UdpServer& 挂在后台循环上的那一台
             */
            [[nodiscard]] UdpServer &server() noexcept
            {
                return *m_server;
            }

            /**
             * @brief 有界等待一条判据成立（计数与处理器侧的观测值都由循环线程写）
             * @param predicate 希望成立的判据
             * @return true 在时限内成立
             */
            template<typename Predicate>
            [[nodiscard]] bool waitUntil(Predicate predicate) const
            {
                return waitForCondition(std::move(predicate), kPeerWaitTimeout);
            }

            /**
             * @brief 收循环是否已退出
             * @return true stop() 之后那条协程真的收手了
             */
            [[nodiscard]] bool isListenFinished() const noexcept
            {
                return m_isListenFinished.load(std::memory_order_acquire);
            }

            /**
             * @brief 承载本服务端的后台循环
             * @return EventLoopThread& 用于把主动下发投给循环线程并等它做完
             */
            [[nodiscard]] EventLoopThread &loopThread() noexcept
            {
                return m_loopThread;
            }

        private:
            Core::EventLoop            m_loop;                    ///< 被测服务端与后台循环共用同一个事件循环
            std::unique_ptr<UdpServer> m_server;                  ///< 被测服务端
            Platform::DatagramSocket   m_peer;                    ///< 对端：本线程上一条真实 UDP 套接字
            std::atomic<bool>          m_isListenFinished{false}; ///< 收循环退出标记（在所有观测之后发布）
            bool                       m_isStopRequested{false};  ///< 是否已请求收口（只在测试线程读写）

            /// 驱动帧必须晚于循环线程 join 才能销毁：EventLoopThread 声明在最后（因而最先析构、
            /// 在析构里 join），帧在它之前声明、在它之后销毁
            std::optional<Core::Task<void>> m_listenDriver;
            EventLoopThread                 m_loopThread{m_loop};
        };

        /**
         * @brief 把 protected 的「读数失败后是否继续」暴露给用例（纯换算，不需要造出真错误）
         */
        class UdpServerProbe : public UdpServer
        {
        public:
            using UdpServer::continuesAfterReceiveFailure;
            using UdpServer::UdpServer;
        };

        /**
         * @brief 一条不作答的处理器
         * @return UdpServer::MessageHandler 处理器
         */
        [[nodiscard]] UdpServer::MessageHandler makeSilentHandler()
        {
            return [](const Core::InetAddress, const std::span<const std::uint8_t>) -> Core::Task<std::vector<std::uint8_t>> { co_return std::vector<std::uint8_t>{}; };
        }

        /**
         * @brief 只填了「不作答处理器」的配置，其余项取默认（用来验构造期与未监听时的行为）
         * @return UdpServer::Configuration 配置
         */
        [[nodiscard]] UdpServer::Configuration makeSilentConfiguration()
        {
            UdpServer::Configuration configuration;
            configuration.onMessage = makeSilentHandler();
            return configuration;
        }

        /**
         * @brief 一条把收到的字节原样交回去的处理器，顺带记下来源端口与长度
         * @param seenSourcePort 出参：处理器看到的来源端口
         * @param seenPayloadLength 出参：处理器看到的报文长度
         * @return UdpServer::MessageHandler 处理器
         */
        [[nodiscard]] UdpServer::MessageHandler makeEchoHandler(std::atomic<std::uint16_t> &seenSourcePort, std::atomic<std::size_t> &seenPayloadLength)
        {
            return [&seenSourcePort, &seenPayloadLength](const Core::InetAddress             sourceAddress,
                                                         const std::span<const std::uint8_t> payload) -> Core::Task<std::vector<std::uint8_t>>
            {
                seenPayloadLength.store(payload.size(), std::memory_order_release);
                seenSourcePort.store(sourceAddress.port(), std::memory_order_release);
                co_return std::vector<std::uint8_t>(payload.begin(), payload.end());
            };
        }
    } // namespace

    TEST(UdpServer, EchoesBackToTheSourceAndPinsTheSourceAddress)
    {
        std::atomic<std::uint16_t> seenSourcePort{0};
        std::atomic<std::size_t>   seenPayloadLength{0};
        UdpServerFixture           fixture(makeEchoHandler(seenSourcePort, seenPayloadLength));

        ASSERT_TRUE(fixture.sendToServer(toBytes("ping")));

        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "回环上没有收到应答：收包、交付或回包任一路没通";
        EXPECT_EQ(toText(*reply), "ping") << "应答内容与请求不一致（交付的长度或缓冲的起点错了）";

        ASSERT_TRUE(fixture.waitUntil([&seenSourcePort] { return seenSourcePort.load(std::memory_order_acquire) != 0U; }));
        // 处理器看到的来源端口必须就是对端的端口：回包正是按它发的。两者不是一个，说明交付出去的
        // 是本端地址或被覆写过的地址
        EXPECT_EQ(seenSourcePort.load(std::memory_order_acquire), fixture.peerAddress().port());
        EXPECT_EQ(seenPayloadLength.load(std::memory_order_acquire), 4U);

        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().sentDatagramCount == 1U; }));
        const UdpServer::Stats stats = fixture.server().stats();
        EXPECT_EQ(stats.receivedDatagramCount, 1U);
        EXPECT_EQ(stats.unsentDatagramCount, 0U);
        EXPECT_EQ(stats.failedHandlerCount, 0U);
    }

    TEST(UdpServer, DeliversZeroLengthDatagramToTheHandler)
    {
        // 零长报文是一条合法报文：唤醒信号、探测这类协议把「有没有发」本身当全部内容，
        // 把它当成一次失败的读数（传输层那种直接跳过）就等于让这类协议永远得不到应答
        UdpServerFixture fixture(
                [](const Core::InetAddress, const std::span<const std::uint8_t> payload) -> Core::Task<std::vector<std::uint8_t>>
                {
                    if (payload.empty())
                    {
                        co_return toBytes("ack");
                    }
                    co_return std::vector<std::uint8_t>(payload.begin(), payload.end());
                });

        ASSERT_TRUE(fixture.sendToServer({}));

        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "零长报文没被交付给处理器（或应答没发出去）";
        EXPECT_EQ(toText(*reply), "ack");
        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().receivedDatagramCount == 1U; }));
    }

    TEST(UdpServer, CapsTheDeliveredPayloadAtTheConfiguredBuffer)
    {
        // 钉住「缓冲容量这项配置真的接进了收包」：内核放不下整条报文时丢弃多余字节（UDP 语义），
        // 而这一层分辨不出「恰好这么长」与「被截断」，所以调小容量就是把截断交给调用方——
        // 这条用例把那个后果写成一个看得见的断言
        std::atomic<std::uint16_t> seenSourcePort{0};
        std::atomic<std::size_t>   seenPayloadLength{0};
        UdpServerFixture           fixture(makeEchoHandler(seenSourcePort, seenPayloadLength), 4);

        ASSERT_TRUE(fixture.sendToServer(toBytes("0123456789")));

        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "小缓冲下这一条报文没有任何应答";
        EXPECT_EQ(toText(*reply), "0123") << "交出去的不是缓冲放得下的那一段";

        ASSERT_TRUE(fixture.waitUntil([&seenPayloadLength] { return seenPayloadLength.load(std::memory_order_acquire) != 0U; }));
        EXPECT_EQ(seenPayloadLength.load(std::memory_order_acquire), 4U);
    }

    TEST(UdpServer, RejectsAnOversizedReplyWholeInsteadOfKillingTheListener)
    {
        // 超限应答整条拒发：数据报不会被内核切开，交出一半比不交更坏。同时钉住「拒发不带走收循环」
        // ——摘掉发送出口那侧的接异常分支，平台层抛出的 InvalidArgumentException 会穿出收循环，
        // 第二次往返就再也没有应答
        UdpServerFixture fixture(
                [](const Core::InetAddress, const std::span<const std::uint8_t> payload) -> Core::Task<std::vector<std::uint8_t>>
                {
                    if (!payload.empty() && payload[0] == static_cast<std::uint8_t>('b'))
                    {
                        co_return std::vector<std::uint8_t>(Platform::DatagramSocket::kMaximumDatagramBytes + 1, static_cast<std::uint8_t>('x'));
                    }
                    co_return std::vector<std::uint8_t>(payload.begin(), payload.end());
                });

        ASSERT_TRUE(fixture.sendToServer(toBytes("big")));
        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().unsentDatagramCount == 1U; })) << "超限应答没被整条拒发并计数";
        EXPECT_EQ(fixture.server().stats().sentDatagramCount, 0U) << "超限的应答被当成发出去了";

        ASSERT_TRUE(fixture.sendToServer(toBytes("next")));
        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "一条超限应答把整台服务带走了：第二次往返没有应答";
        EXPECT_EQ(toText(*reply), "next");
        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().sentDatagramCount == 1U; }));
        EXPECT_EQ(fixture.server().stats().receivedDatagramCount, 2U);
    }

    TEST(UdpServer, KeepsServingWhenTheHandlerThrows)
    {
        // 处理器抛异常只丢这一条报文：一条报文里的业务故障不该让整台服务不再接受任何来源，
        // 这与「读数报错不退出循环」是同一条判据的两侧
        std::atomic<std::size_t> handledCallCount{0};
        UdpServerFixture         fixture(
                [&handledCallCount](const Core::InetAddress, const std::span<const std::uint8_t> payload) -> Core::Task<std::vector<std::uint8_t>>
                {
                    const std::size_t callIndex = handledCallCount.fetch_add(1, std::memory_order_relaxed);
                    if (callIndex == 0)
                    {
                        throw std::runtime_error("用例内的处理器故障");
                    }
                    co_return std::vector<std::uint8_t>(payload.begin(), payload.end());
                });

        ASSERT_TRUE(fixture.sendToServer(toBytes("first")));
        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().failedHandlerCount == 1U; })) << "处理器抛出的异常没被接住（或没计数）";
        EXPECT_EQ(fixture.server().stats().sentDatagramCount, 0U) << "抛异常的这一条不该有应答";

        ASSERT_TRUE(fixture.sendToServer(toBytes("second")));
        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "处理器的异常穿出了收循环：第二次往返没有应答";
        EXPECT_EQ(toText(*reply), "second");
        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().receivedDatagramCount == 2U; }));
    }

    TEST(UdpServer, PushesUnsolicitedDatagramsIncludingAnEmptyOne)
    {
        // 服务端主动下发：无连接协议里「不等对方先来」是常态（公告、通知）。第二条是零长报文——
        // 平台层的发送入口即便长度为 0 也拒绝空指针缓冲，不补一字节占位就会被记成发送失败
        std::vector<std::uint8_t> noticeBytes = toBytes("notice");
        UdpServerFixture          fixture(makeSilentHandler());

        const auto notice = fixture.loopThread().runToCompletion(fixture.server().sendTo(fixture.peerAddress(), std::span<const std::uint8_t>(noticeBytes)));
        ASSERT_TRUE(notice.finished);
        ASSERT_TRUE(notice.value.has_value());
        EXPECT_TRUE(notice.value.value()) << "主动下发被判成失败";

        const std::optional<std::vector<std::uint8_t>> pushed = fixture.receiveFromServer();
        ASSERT_TRUE(pushed.has_value()) << "服务端主动发的报文没到对端";
        EXPECT_EQ(toText(*pushed), "notice");

        const auto emptyPush = fixture.loopThread().runToCompletion(fixture.server().sendTo(fixture.peerAddress(), std::span<const std::uint8_t>{}));
        ASSERT_TRUE(emptyPush.finished);
        EXPECT_TRUE(emptyPush.value.value_or(false)) << "零长报文被当成发送失败：本该交出去的空报文被判给了空指针";

        const std::optional<std::vector<std::uint8_t>> pushedEmpty = fixture.receiveFromServer();
        ASSERT_TRUE(pushedEmpty.has_value()) << "零长报文没到对端";
        EXPECT_EQ(pushedEmpty->size(), 0U);

        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().sentDatagramCount == 2U; }));
        EXPECT_EQ(fixture.server().stats().unsentDatagramCount, 0U) << "两条主动下发都不该记成没发出去";
        EXPECT_EQ(fixture.server().stats().receivedDatagramCount, 0U) << "对端一条也没发，服务端不该记收到";
    }

    TEST(UdpServer, ReleasesTheReceiveWaitWhenStopped)
    {
        std::atomic<std::uint16_t> seenSourcePort{0};
        std::atomic<std::size_t>   seenPayloadLength{0};
        UdpServerFixture           fixture(makeEchoHandler(seenSourcePort, seenPayloadLength));

        // 此刻收循环正挂在「可读」上，而没人再发报文：只有 stop() 关掉套接字才叫得醒它。
        // 把 stop() 改成只翻标记，这条会在超时上红（关掉描述符本身不唤醒等待者）
        fixture.stopOnLoopThread();
        EXPECT_TRUE(fixture.waitUntil([&fixture] { return fixture.isListenFinished(); })) << "stop() 之后收循环仍挂在读数上：等待者没被叫醒";
    }

    TEST(UdpServer, RefusesToSendBeforeItIsListening)
    {
        // 没监听就发：交回 false 并记进「该发没发」，而不是悄悄当成已发出
        EventLoopThread                 loopThread;
        UdpServer                       server(loopThread.loop(), makeSilentConfiguration());
        const std::vector<std::uint8_t> earlyBytes = toBytes("too-early");

        // 载荷先落成具名对象：sendTo 是惰性协程，直接把临时量的视图交给它，等首次恢复时那份字节
        // 已经出过作用域
        const auto sendOutcome = loopThread.runToCompletion(server.sendTo(Core::InetAddress("127.0.0.1", 9), std::span<const std::uint8_t>(earlyBytes)));
        ASSERT_TRUE(sendOutcome.finished);
        ASSERT_TRUE(sendOutcome.value.has_value());
        EXPECT_FALSE(sendOutcome.value.value()) << "未监听时的发送被报成成功";
        EXPECT_EQ(server.stats().unsentDatagramCount, 1U);
        EXPECT_EQ(server.stats().sentDatagramCount, 0U);
    }

    TEST(UdpServer, RejectsAnUnusableReceiveBufferCapacity)
    {
        EventLoopThread loopThread;

        UdpServer::Configuration tooSmall = makeSilentConfiguration();
        tooSmall.maximumDatagramByteCount = 0;
        EXPECT_THROW(UdpServer server(loopThread.loop(), tooSmall), Base::InvalidArgumentException) << "容量 0 连一条空报文都放不下，必须拒";

        UdpServer::Configuration tooLarge = makeSilentConfiguration();
        tooLarge.maximumDatagramByteCount = Platform::DatagramSocket::kMaximumDatagramBytes + 1;
        EXPECT_THROW(UdpServer server(loopThread.loop(), tooLarge), Base::InvalidArgumentException) << "缓冲超过单条报文上限：收得到却答不出去";
    }

    TEST(UdpServer, ContinuesAfterReceiveFailureOnlyForPeerGoneErrors)
    {
        // 这条分支走错的代价是「一个消失的对端让整台服务不再接受任何来源」，而真错误要靠对端
        // 消失的时刻与读数竞态，用例造不出确定重叠，因此判据抽成纯函数直测
        EventLoopThread loopThread;
        UdpServerProbe  probe(loopThread.loop(), makeSilentConfiguration());

        EXPECT_TRUE(UdpServerProbe::continuesAfterReceiveFailure(kPeerGoneErrorCode, true, false)) << "ICMP 回声类的读数错误让循环收了手";
        EXPECT_FALSE(UdpServerProbe::continuesAfterReceiveFailure(0, true, false)) << "套接字已不可用（无码收场）时还在继续读：那是 stop() 之后的空转";
        EXPECT_FALSE(UdpServerProbe::continuesAfterReceiveFailure(kPeerGoneErrorCode, false, false)) << "套接字已失效还在继续读";
        EXPECT_FALSE(UdpServerProbe::continuesAfterReceiveFailure(kPeerGoneErrorCode, true, true)) << "已经请求停止还在继续读";
    }
    TEST(UdpServer, ServesThePortOfASocketSomeoneElseBound)
    {
        // 端口由别人 bind、服务端只接手：跨进程共享一条 UDP 端口在 worker 侧就是这一形状（Windows 上
        // 多个进程各自 bind 同一端口不分摊，只能这么交）。判据落在「发往那个端口的报文有回话」上，
        // 端口对不对则由 listeningPort 与交来的那份套接字互相指认
        std::atomic<std::uint16_t> seenSourcePort{0};
        std::atomic<std::size_t>   seenPayloadLength{0};

        // 夹具之外自己 bind 的套接字要自己负责网络库初始化：Windows 上没有 WSAStartup 之前 bind 直接
        // 报 WSAEINVAL，而本条单独跑（ctest 一用例一进程）时没有别的用例替它初始化过
        const Platform::Socket::Initialization network;

        Platform::DatagramSocket boundSocket = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(boundSocket.isValid()) << "夹具绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Platform::SocketAddress boundAddress = boundSocket.localAddress();
        const std::uint16_t           boundPort    = Core::InetAddress(boundAddress.storage, boundAddress.length).port();
        ASSERT_NE(boundPort, 0U);

        UdpServerFixture fixture(makeEchoHandler(seenSourcePort, seenPayloadLength), Platform::DatagramSocket::kMaximumDatagramBytes, std::move(boundSocket));

        EXPECT_EQ(fixture.serverAddress().port(), boundPort) << "接手模式下端口该来自交来的那份套接字，而不是自己再绑一个";
        ASSERT_TRUE(fixture.sendToServer(toBytes("taken")));

        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "在接手来的套接字上没有应答";
        EXPECT_EQ(toText(*reply), "taken");
        ASSERT_TRUE(fixture.waitUntil([&fixture] { return fixture.server().stats().receivedDatagramCount == 1U; }));
    }

    TEST(UdpServer, RejectsMixingTheAdoptedSocketWithAnAddress)
    {
        // 两种启动顺序各自只认一种来源。混用的后果是静默的：接手来的套接字被闲置，对端往那个端口发的
        // 报文一条也到不了，症状与「移交没做成」一模一样，因此这里必须抛出而不是挑一种继续
        //
        // 声明顺序就是销毁顺序的反面：循环最先声明（最后销毁），服务端在它之后、驱动帧再后，
        // 后台循环包装器最后声明（最先销毁、析构里 join）——服务端持有套接字，必须晚于循环停止
        // 才销毁，否则就是在本线程上销毁仍归循环线程的那份注册对象
        Core::EventLoop                        loop;
        const Platform::Socket::Initialization network;
        UdpServer::Configuration               configuration = makeSilentConfiguration();
        UdpServer                              adopting(loop, configuration, Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress()));
        UdpServer                              binding(loop, makeSilentConfiguration());
        std::optional<Core::Task<void>>        adoptingWithAddress;
        std::optional<Core::Task<void>>        bindingWithoutAddress;
        EventLoopThread                        loopThread{loop};

        std::string       adoptingReason;
        std::string       bindingReason;
        std::atomic<bool> isAdoptingRejected{false};
        std::atomic<bool> isBindingRejected{false};

        adoptingWithAddress.emplace(captureCoroutineFailure(adopting.listen(Core::InetAddress("127.0.0.1", 0)), adoptingReason, isAdoptingRejected));
        bindingWithoutAddress.emplace(captureCoroutineFailure(binding.listen(), bindingReason, isBindingRejected));
        loopThread.loop().scheduler().scheduleRemote(adoptingWithAddress->handle());
        loopThread.loop().scheduler().scheduleRemote(bindingWithoutAddress->handle());

        EXPECT_TRUE(waitForCondition([&] { return isAdoptingRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout))
                << "接手来的服务端调带地址的 listen() 没被拒：那份套接字会被静默闲置";
        EXPECT_NE(adoptingReason.find("接手"), std::string::npos) << adoptingReason;
        EXPECT_TRUE(waitForCondition([&] { return isBindingRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout))
                << "按地址构造的服务端调不带地址的 listen() 没被拒：没有可服务的套接字";
        EXPECT_NE(bindingReason.find("接手"), std::string::npos) << bindingReason;
    }

    TEST(UdpServer, RefusesToOpenThePortWithoutAHandler)
    {
        // 「收了报文没人处理」等于把每一条都丢掉，所以端口压根不该开起来。listen() 是惰性协程，
        // 抛出点在首次恢复时——直接 EXPECT_THROW(server.listen(...)) 只构造了协程帧，测不到任何东西。
        // 声明顺序同上：循环最先、后台驱动最后，被拒的服务端要在循环停止之后才销毁
        Core::EventLoop          loop;
        UdpServer::Configuration configuration;
        configuration.maximumDatagramByteCount = Platform::DatagramSocket::kMaximumDatagramBytes;
        UdpServer                       server(loop, configuration);
        std::optional<Core::Task<void>> driver;
        EventLoopThread                 loopThread{loop};

        std::string       reason;
        std::atomic<bool> isRejected{false};

        driver.emplace(captureCoroutineFailure(server.listen(Core::InetAddress("127.0.0.1", 0)), reason, isRejected));
        loopThread.loop().scheduler().scheduleRemote(driver->handle());

        ASSERT_TRUE(waitForCondition([&] { return isRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout));
        EXPECT_NE(reason.find("onMessage"), std::string::npos) << "文案没点名缺的是哪个字段：" << reason;
        EXPECT_EQ(server.listeningPort(), 0U) << "被拒之后端口不该已经开着";
    }

    /**
     * @brief 已在监听的服务端不许再 listen() 一次，而且被拒的那一次不许动到正在跑的服务
     * @details 第二次启动的真实代价不是「多一条错误」：那条路会把 m_socket 换成新的一份，而收循环还挂在
     *          旧的那份上（接收缓冲与事件循环里的注册对象都归它），旧对象一被销毁，原端口就没人读了。
     *          两侧实测的形态都是「端口换了、原服务不再应答」，Windows 与容器 ASan 下都没报出
     *          use-after-free——坏的是这台监听器看起来还在服务。
     * @note listen() 是惰性协程，判据排在首次恢复时，因此这里驱动帧而不是 EXPECT_THROW
     * @note 证伪（两侧实测）：摘掉两条 listen 入口上的 requireFreshStart()，本条红在三处——「没被拒」、
     *       端口从 56714 变成新绑的那一个、随后的往返拿不到应答
     */
    TEST(UdpServer, RejectsASecondListenWhileAlreadyServing)
    {
        std::atomic<std::uint16_t> seenSourcePort{0};
        std::atomic<std::size_t>   seenPayloadLength{0};

        // 驱动帧先声明、因而在夹具（含后台循环）之后销毁：帧要在循环停止之后才回收
        std::optional<Core::Task<void>> secondListenDriver;
        UdpServerFixture                fixture(makeEchoHandler(seenSourcePort, seenPayloadLength));

        const std::uint16_t servingPort = fixture.serverAddress().port();
        ASSERT_NE(servingPort, 0U) << "夹具没把服务起起来";
        ASSERT_TRUE(fixture.sendToServer(toBytes("first")));
        ASSERT_TRUE(fixture.receiveFromServer().has_value()) << "夹具的服务没有应答，后面的断言无从判起";

        std::string       reason;
        std::atomic<bool> isRejected{false};
        secondListenDriver.emplace(captureCoroutineFailure(fixture.server().listen(Core::InetAddress("127.0.0.1", 0)), reason, isRejected));
        fixture.loopThread().loop().scheduler().scheduleRemote(secondListenDriver->handle());

        EXPECT_TRUE(waitForCondition([&] { return isRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout))
                << "第二次 listen() 没被拒：它会换掉正在服务的那份套接字，收循环就挂在已销毁的对象上";
        EXPECT_NE(reason.find("已经在 UDP 端口"), std::string::npos) << "文案没点名「重复启动」这个成因：" << reason;

        // 被拒的那一次不该留下任何痕迹：端口还是那一个，服务照常应答
        EXPECT_EQ(fixture.serverAddress().port(), servingPort) << "被拒的第二次启动换了端口";
        ASSERT_TRUE(fixture.sendToServer(toBytes("second")));
        const std::optional<std::vector<std::uint8_t>> reply = fixture.receiveFromServer();
        ASSERT_TRUE(reply.has_value()) << "第二次 listen() 被拒之后原来的服务不再应答：那次启动并非无害";
        EXPECT_EQ(toText(*reply), "second");
    }
} // namespace AsynGyanis::Net
