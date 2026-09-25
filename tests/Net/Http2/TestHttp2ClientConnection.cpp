// HTTP/2 客户端连接层的端到端用例：对端是自家服务端（已过 h2spec 146/146 那一侧）
#include "Http2TestSupport.h"
#include "HttpTestSupport.h"
#include "Core/Socket/AsyncSocket.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http2/Http2ClientConnection.h"
#include "Net/Tcp/TcpClient.h"
#include "Net/Tcp/TcpStream.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <coroutine>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        using namespace TestSupport; // 帧包装与头值查找小工具（Http2TestSupport.h）
        using namespace std::chrono_literals;
        constexpr auto kClientWaitTimeout = std::chrono::seconds{10};

        /// 一轮「建连 → 发 N 次同样的请求 → 收尾」的结论
        struct ClientRunOutcome
        {
            bool isStarted{false};          ///< 前奏与本端 SETTINGS 是否走完（含收到对端 SETTINGS）
            std::vector<int> statusCodes;   ///< 每次请求的状态码，0 表示没拿到响应
            std::vector<std::string> bodies; ///< 每次请求的正文
            std::vector<std::string> errors; ///< 每次请求的中文失败原因（成功时为空串）
        };

        /**
         * @brief 连上一台 h2c 服务端并走完连接前奏
         * @param loop 客户端事件循环
         * @param port 服务端端口
         * @return std::unique_ptr<Http2ClientConnection> 可用的连接；失败为空
         */
        Core::Task<std::unique_ptr<Http2ClientConnection>> openConnection(Core::EventLoop &loop, const std::uint16_t port)
        {
            auto stream = co_await TcpClient::connect(loop, "127.0.0.1", port);
            if (!stream)
            {
                co_return nullptr;
            }
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"127.0.0.1", port, false},
                                                           std::move(*stream)));
            if (!co_await connection->start(kClientWaitTimeout))
            {
                co_return nullptr;
            }
            co_return connection;
        }

        Core::Task<void> runClientTask(Core::EventLoop &loop, const std::uint16_t port, const std::string &method,
                                       const std::string &path, const std::string &body, const std::size_t requestCount,
                                       const std::chrono::milliseconds requestTimeout, ClientRunOutcome &outcome)
        {
            auto connection = co_await openConnection(loop, port);
            outcome.isStarted = connection != nullptr;
            if (connection != nullptr)
            {
                for (std::size_t index = 0; index < requestCount; ++index)
                {
                    Http2ClientResponse response = co_await connection->request(
                            "http", "127.0.0.1:" + std::to_string(port), method, path, {}, body, requestTimeout);
                    outcome.statusCodes.push_back(response.statusCode);
                    outcome.bodies.push_back(std::move(response.body));
                    outcome.errors.push_back(std::move(response.errorMessage));
                }
                co_await connection->shutdown();
            }
            loop.stop();
            co_return;
        }

        ClientRunOutcome runClientRequests(const std::uint16_t port, const std::string &method, const std::string &path,
                                           const std::string &body = {}, const std::size_t requestCount = 1,
                                           const std::chrono::milliseconds requestTimeout = kClientWaitTimeout)
        {
            Core::EventLoop loop;
            ClientRunOutcome outcome;
            auto work = runClientTask(loop, port, method, path, body, requestCount, requestTimeout, outcome);
            if (!work.isReady())
            {
                loop.scheduler().schedule(work.handle());
            }
            loop.run();
            return outcome;
        }

        /// 带 /echo 路由（把请求正文原样回出）的夹具参数
        void registerEchoRoute(Router &router, Core::EventLoop &)
        {
            router.post("/echo", [](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
            {
                response.setBody(request.body());
                co_return;
            });
        }

        /// 脚本化对端读到的一串帧，以及读过程中出的错
        struct PeerFrames
        {
            std::vector<Http2Frame> frames; ///< 解出来的帧，按到达顺序
            std::string errorText;          ///< 帧层解码失败的原因；空表示一路正常
        };

        /**
         * @brief 从通路上读字节并解帧，攒到 expectedFrameCount 条为止
         * @param stream 对端一侧的通路
         * @param decoder 跨次调用留存的解码器（半帧要靠它拼完）
         * @param expectedFrameCount 至少解出几条帧
         * @return Core::Task<PeerFrames> 攒到的帧；通路收口时可能少于请求条数
         */
        Core::Task<PeerFrames> readPeerFrames(TcpStream &stream, Http2FrameDecoder &decoder, const std::size_t expectedFrameCount)
        {
            PeerFrames batch;
            while (batch.frames.size() < expectedFrameCount && batch.errorText.empty())
            {
                std::array<char, 4096> buffer{};
                const ssize_t receivedByteCount = co_await stream.read(buffer.data(), buffer.size());
                if (receivedByteCount <= 0)
                {
                    break; // 对端收口或读错：把手上已有的帧交回去，由用例断言
                }
                std::size_t offset = 0;
                while (offset < static_cast<std::size_t>(receivedByteCount))
                {
                    const Http2FrameDecodeStatus status = decoder.parse(buffer.data() + offset,
                                                                        static_cast<std::size_t>(receivedByteCount) - offset);
                    if (status == Http2FrameDecodeStatus::NeedMore)
                    {
                        break;
                    }
                    if (status == Http2FrameDecodeStatus::Error)
                    {
                        batch.errorText = decoder.errorMessage();
                        break;
                    }
                    offset += decoder.consumedByteCount();
                    batch.frames.push_back(decoder.takeFrame());
                }
            }
            co_return batch;
        }

        /// 在收到的帧里按类型找一条（requiresAck 同时判 ACK 位的有无）
        const Http2Frame *findFrame(const std::vector<Http2Frame> &frames, const Http2FrameType type, const bool requiresAck)
        {
            for (const Http2Frame &frame: frames)
            {
                if (frame.header.type == type
                    && ((frame.header.flags & kHttp2FlagAcknowledge) != 0U) == requiresAck)
                {
                    return &frame;
                }
            }
            return nullptr;
        }

        /**
         * @brief 手拼一帧：绕过帧编码器对「未定义类型」的拒绝
         * @details 自家的 encodeHttp2Frame 不接受 §6 之外的类型值（那是写出侧的守门，合理），而对端可以
         *          照 §4.1 送来任意类型字节——要验本端「忽略未知帧」这条 MUST，只能按线上格式自己排字节。
         * @param type 线上类型字节
         * @param flags 标志位
         * @param streamId 流号（R 位须为 0）
         * @param payload 负载
         * @return std::string 9 字节帧头加负载
         */
        std::string rawFrameBytes(const std::uint8_t type, const std::uint8_t flags, const std::uint32_t streamId,
                                  const std::string_view payload)
        {
            const std::size_t length = payload.size();
            std::string bytes;
            bytes.reserve(kHttp2FrameHeaderByteCount + length);
            bytes.push_back(static_cast<char>((length >> 16) & 0xFFU));
            bytes.push_back(static_cast<char>((length >> 8) & 0xFFU));
            bytes.push_back(static_cast<char>(length & 0xFFU));
            bytes.push_back(static_cast<char>(type));
            bytes.push_back(static_cast<char>(flags));
            bytes.push_back(static_cast<char>((streamId >> 24) & 0xFFU));
            bytes.push_back(static_cast<char>((streamId >> 16) & 0xFFU));
            bytes.push_back(static_cast<char>((streamId >> 8) & 0xFFU));
            bytes.push_back(static_cast<char>(streamId & 0xFFU));
            bytes.append(payload);
            return bytes;
        }

        /**
         * @brief 一条按脚本行事的对端：验前奏、发自己的 SETTINGS 与 PING、回一条 200
         * @details 脚本不做任何宽松处理：本端少发一条 ACK、把空正文请求的 END_STREAM 漏掉，这里都会
         *          直接体现在「攒到的帧」上，由用例逐条断言。通路被本端关掉时不报错，只把已攒到的交出。
         */
        Core::Task<void> runScriptedPeer(Core::EventLoop &loop, TcpStream peer, PeerFrames &received, std::string &prefaceText)
        {
            Http2FrameDecoder decoder;
            try
            {
                std::array<char, 24> preface{};
                co_await peer.readExact(preface.data(), preface.size());
                prefaceText.assign(preface.data(), preface.size());

                bool isGreetingSent = false;
                bool isAnswered = false;
                while (true)
                {
                    PeerFrames batch = co_await readPeerFrames(peer, decoder, 1);
                    for (const Http2Frame &frame: batch.frames)
                    {
                        received.frames.push_back(frame);
                        const bool isSettings = frame.header.type == Http2FrameType::Settings;
                        if (isSettings && (frame.header.flags & kHttp2FlagAcknowledge) == 0U && !isGreetingSent)
                        {
                            // 自己的 SETTINGS 空负载（全按协议默认），外加一条 PING 探本端会不会原样 ACK
                            isGreetingSent = true;
                            const std::string greeting = makeFrame(Http2FrameType::Settings, 0U, 0U, {})
                                                       + makeFrame(Http2FrameType::Ping, 0U, 0U, "12345678")
                                                       // 夹一条本端不认识的帧类型：§4.1/§5.5 要求「忽略」而不是
                                                       // 判死，判死的话这条请求就拿不到 200 了。未定义类型只能
                                                       // 手拼——自家的帧编码器会拒绝产出它
                                                       + rawFrameBytes(100U, 0U, 1U, "noise");
                            co_await peer.writeAll(greeting.data(), greeting.size());
                        }
                        if (frame.header.type == Http2FrameType::Headers && !isAnswered)
                        {
                            isAnswered = true;
                            HpackEncoder peerEncoder;
                            const std::string headerBlock = peerEncoder.encode({HpackHeaderField{":status", "200"}});
                            const std::string responseFrame = makeFrame(Http2FrameType::Headers,
                                                                        static_cast<std::uint8_t>(kHttp2FlagEndHeaders
                                                                                                 | kHttp2FlagEndStream),
                                                                        frame.header.streamId, headerBlock);
                            co_await peer.writeAll(responseFrame.data(), responseFrame.size());
                        }
                    }
                    if (!batch.errorText.empty())
                    {
                        received.errorText = std::move(batch.errorText);
                        break;
                    }
                    if (batch.frames.empty())
                    {
                        break; // 通路收口
                    }
                }
            } catch (const std::exception &)
            {
                // 本端中途关掉了通路：脚本就此为止，已攒到的帧照样交给用例
            }
            loop.stop();
            co_return;
        }

        /// 本端走完全程的结论（走「描述符对 + 脚本对端」这一趟）
        struct RawPairRunOutcome
        {
            bool isStarted{false};      ///< 前奏是否走完
            int statusCode{0};          ///< 响应状态码
            std::string errorMessage;   ///< 失败原因
        };

        /// 在已连好的通路上走完「前奏 → 一条 GET → 礼貌收尾」
        Core::Task<void> runRawPairClient(Core::EventLoop &loop, TcpStream clientSide, RawPairRunOutcome &outcome)
        {
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)));
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);

            // 前奏没成也要把 request 问一句：它会把连接判死的原话带回来，否则用例只剩「没走通」一个字
            Http2ClientResponse response = co_await connection->request(
                    "http", "peer", "GET", "/tick", {}, {}, kClientWaitTimeout);
            outcome.statusCode = response.statusCode;
            outcome.errorMessage = std::move(response.errorMessage);
            co_await connection->shutdown();
            co_return;
        }

        /// 一段只带单个参数的 SETTINGS 原文（对端照它通告一个越界值）
        std::string singleSettingFrameBytes(const Http2SettingIdentifier identifier, const std::uint32_t value)
        {
            Http2SettingsPayload payload;
            payload.parameters.push_back(
                    Http2Setting{.identifier = static_cast<std::uint16_t>(identifier), .value = value});
            return encodeHttp2SettingsFrame(payload);
        }

        /**
         * @brief 开局按脚本回一段字节、之后只等本端收场的对端
         * @details 本端遇到连接级违规时应当按 §6.8 交代一条带错误码的 GOAWAY，而不是无声把通路关掉——
         *          现场只剩「连接没了」对排查没有价值，对端也无从知道自己哪一步踩了线。收场之后本端
         *          可能还会先发自己的请求帧（正文洪泛那条就是这种顺序），所以要一直读到看见 GOAWAY。
         */
        Core::Task<void> runOpeningPeer(Core::EventLoop &loop, TcpStream peer, PeerFrames &received,
                                        const std::string openingBytes)
        {
            Http2FrameDecoder decoder;
            try
            {
                std::array<char, 24> preface{};
                co_await peer.readExact(preface.data(), preface.size());
                static_cast<void>(co_await readPeerFrames(peer, decoder, 1)); // 本端的 SETTINGS
                co_await peer.writeAll(openingBytes.data(), openingBytes.size());
                while (findFrame(received.frames, Http2FrameType::GoAway, false) == nullptr)
                {
                    PeerFrames batch = co_await readPeerFrames(peer, decoder, 1);
                    for (const Http2Frame &frame: batch.frames)
                    {
                        received.frames.push_back(frame);
                    }
                    if (!batch.errorText.empty())
                    {
                        received.errorText = std::move(batch.errorText);
                        break;
                    }
                    if (batch.frames.empty())
                    {
                        break; // 通路收口而没等到 GOAWAY：让用例去判这条失败
                    }
                }
            } catch (const std::exception &)
            {
                // 本端中途关掉了通路：已攒到的帧照样交给用例
            }
            loop.stop();
            co_return;
        }

        /// 本端一侧只走到前奏：start() 的结论就是判据
        Core::Task<void> runBadSettingsClient(Core::EventLoop &loop, TcpStream clientSide, bool &isStarted)
        {
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)));
            isStarted = co_await connection->start(kClientWaitTimeout);
            co_return;
        }
        /// 两条请求同时在一条连接上跑完之后留下的结论
        struct MultiplexRunOutcome
        {
            bool isStarted{false};
            std::vector<std::string> bodies;   ///< 每条请求的正文，按提交顺序
            std::vector<std::string> errors;   ///< 每条请求的失败原因（成功时为空串）
        };

        /**
         * @brief 会合点：等 N 个子协程全部回来，再叫起驱动者
         * @details 最后一个完成的人负责唤醒，且走循环的调度器而不是在子协程的栈上直接恢复——
         *          驱动者一收尾就会销毁存着子协程的容器，那正好是正在跑 arrive() 的那个自己。
         */
        class JoinGate
        {
        public:
            /**
             * @brief 构造会合点
             * @param loop 唤醒投递到哪条循环
             * @param expectedCount 要等几个子协程
             */
            JoinGate(Core::EventLoop &loop, const std::size_t expectedCount) noexcept
                : m_loop(&loop), m_remaining(expectedCount)
            {
            }

            /// 一个子协程回来了；最后一个负责把驱动者排上调度器
            void arrive() noexcept
            {
                if (--m_remaining != 0U || m_waiter == nullptr)
                {
                    return;
                }
                const std::coroutine_handle<> waiter = std::exchange(m_waiter, nullptr);
                m_loop->scheduler().schedule(waiter);
            }

            /// 已经到齐就不挂
            [[nodiscard]] bool await_ready() const noexcept { return m_remaining == 0U; }

            /**
             * @brief 登记驱动协程，等最后一个子协程来叫醒
             * @param waiter 当前协程句柄
             * @return true 恒挂起（构造时 expectedCount>=1，不会到齐后才进来）
             */
            bool await_suspend(const std::coroutine_handle<> waiter) noexcept
            {
                m_waiter = waiter;
                return true;
            }

            void await_resume() const noexcept {}

        private:
            Core::EventLoop *m_loop;            ///< 唤醒投递的目标循环
            std::size_t m_remaining;            ///< 还差几个子协程
            std::coroutine_handle<> m_waiter{}; ///< 挂着等他们的驱动协程
        };

        /**
         * @brief 一条「先把两条请求都收下、再倒序作答」的对端脚本
         * @details 判并发不能靠计时：慢机器上「排队也来得及」的读法会假绿。这里让对端收齐两条
         *          HEADERS 才开始回答，并且按**倒序**作答（后到的那条先回），每条响应的正文写成
         *          「peer-body-<那条请求的 :path>」：正文按请求各自的身份回，判据就不依赖子协程被
         *          调度的先后，只看每条请求拿回的是不是自己那份。
         * @param peer 对端一侧的通路
         * @param openedRequests 输出：开始作答之前收到的「流号 + :path」，按到达顺序
         */
        Core::Task<void> runMultiplexPeer(TcpStream peer,
                                          std::vector<std::pair<std::uint32_t, std::string>> &openedRequests)
        {
            Http2FrameDecoder decoder;
            HpackDecoder pathDecoder;
            try
            {
                std::array<char, 24> preface{};
                co_await peer.readExact(preface.data(), preface.size());
                static_cast<void>(co_await readPeerFrames(peer, decoder, 1)); // 本端的 SETTINGS
                const std::string greeting = makeFrame(Http2FrameType::Settings, 0U, 0U, {});
                co_await peer.writeAll(greeting.data(), greeting.size());

                while (openedRequests.size() < 2U)
                {
                    PeerFrames batch = co_await readPeerFrames(peer, decoder, 1);
                    for (const Http2Frame &frame: batch.frames)
                    {
                        if (frame.header.type == Http2FrameType::Headers)
                        {
                            std::vector<HpackHeaderField> headerFields;
                            std::string decodeErrorText;
                            // 头块照样得解完：本端的编码器在动态表里留着前面几条，跳过就不动了
                            if (pathDecoder.decode(frame.payload, headerFields, &decodeErrorText))
                            {
                                openedRequests.emplace_back(frame.header.streamId,
                                                            findHeaderValue(headerFields, ":path"));
                            }
                        }
                    }
                    if (!batch.errorText.empty() || batch.frames.empty())
                    {
                        co_return; // 解帧出错或通路收口：把已收到的部分交给用例
                    }
                }

                for (auto iterator = openedRequests.rbegin(); iterator != openedRequests.rend(); ++iterator)
                {
                    const std::uint32_t streamId = iterator->first;
                    HpackEncoder peerEncoder;
                    const std::string headerBlock = peerEncoder.encode({HpackHeaderField{":status", "200"}});
                    std::string answer = makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, streamId, headerBlock);
                    const std::string bodyText = "peer-body-" + iterator->second;
                    answer += makeFrame(Http2FrameType::Data, kHttp2FlagEndStream, streamId, bodyText);
                    co_await peer.writeAll(answer.data(), answer.size());
                }

                // 本端收尾会发一条 GOAWAY；收到或通路断开就结束
                while (true)
                {
                    PeerFrames batch = co_await readPeerFrames(peer, decoder, 1);
                    for (const Http2Frame &frame: batch.frames)
                    {
                        if (frame.header.type == Http2FrameType::GoAway)
                        {
                            co_return;
                        }
                    }
                    if (batch.frames.empty())
                    {
                        co_return;
                    }
                }
            } catch (const std::exception &)
            {
                // 本端收口了通路：脚本就此为止，已收到的部分交给用例
            }
            co_return;
        }

        /// 一条并发请求：结果写进 outcome 的第 index 格，回来就在 gate 上记一笔
        Core::Task<void> runOneMultiplexRequest(Http2ClientConnection &connection, const std::size_t index,
                                                MultiplexRunOutcome &outcome, JoinGate &gate)
        {
            Http2ClientResponse response = co_await connection.request("http", "peer", "GET",
                                                                      index == 0U ? "/tick-a" : "/tick-b",
                                                                      {}, {}, kClientWaitTimeout);
            outcome.bodies[index] = std::move(response.body);
            outcome.errors[index] = std::move(response.errorMessage);
            gate.arrive();
            co_return;
        }

        /// 走「描述符对 + 多路复用对端」这一趟：两条请求同时在一条连接上
        Core::Task<void> runMultiplexTask(Core::EventLoop &loop, TcpStream clientSide, MultiplexRunOutcome &outcome)
        {
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)));
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);
            if (outcome.isStarted)
            {
                constexpr std::size_t kRequestCount = 2U;
                outcome.bodies.resize(kRequestCount);
                outcome.errors.resize(kRequestCount);
                JoinGate gate(loop, kRequestCount);
                std::vector<Core::Task<void> > children;
                children.reserve(kRequestCount);
                for (std::size_t index = 0; index < kRequestCount; ++index)
                {
                    children.push_back(runOneMultiplexRequest(*connection, index, outcome, gate));
                }
                for (Core::Task<void> &child: children)
                {
                    if (!child.isReady())
                    {
                        loop.scheduler().schedule(child.handle());
                    }
                }
                co_await gate;
                co_await connection->shutdown();
            }
            loop.stop();
            co_return;
        }

    } // namespace

    /**
     * @brief 钉住：客户端连接层能走完 h2c 前奏并拿到一条完整的 200 响应
     * @details 对端是自家服务端。这一条同时验了两件事：本端发出的前奏与 SETTINGS 被对端接受（对端
     *          若判成协议错误就会回 GOAWAY/RST，这里拿不到响应），以及本端能把 HEADERS 里的 :status
     *          与普通字段解出来。
     */
    TEST(Http2ClientConnection, GetsResponseOverCleartextHttp2)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         RouteRegistrar{}, HttpParserLimits{},
                                         [](TestHttpServer &server)
                                         {
                                             static_cast<void>(server.setHttp2CleartextEnabled(true));
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kClientWaitTimeout)) << "服务端未在时限内进入接受循环";

        const ClientRunOutcome outcome = runClientRequests(fixture.listeningPort(), "GET", "/hello");
        EXPECT_TRUE(outcome.isStarted) << "连接前奏没走完";
        ASSERT_EQ(outcome.statusCodes.size(), 1U);
        EXPECT_TRUE(outcome.errors[0].empty()) << "失败原因：" << outcome.errors.at(0);
        EXPECT_EQ(outcome.statusCodes[0], 200);
        EXPECT_NE(outcome.bodies[0].find("served-hello"), std::string::npos) << "正文：" << outcome.bodies[0];
    }

    /**
     * @brief 钉住：同一条连接上的第二个头块仍与对端解码器同步（HPACK 动态表）
     * @details 第一个头块把伪头与普通头带增量索引塞进了两边的表，第二个头块因此可以按索引引用。
     *          本端编码器与解码器的表只要有一侧修剪不一致，对端就解不开、这条请求拿不到 200。
     */
    TEST(Http2ClientConnection, KeepsHeaderCompressionInSyncAcrossRequests)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         RouteRegistrar{}, HttpParserLimits{},
                                         [](TestHttpServer &server)
                                         {
                                             static_cast<void>(server.setHttp2CleartextEnabled(true));
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kClientWaitTimeout)) << "服务端未在时限内进入接受循环";

        const ClientRunOutcome outcome = runClientRequests(fixture.listeningPort(), "GET", "/hello", {}, 3);
        EXPECT_TRUE(outcome.isStarted);
        ASSERT_EQ(outcome.statusCodes.size(), 3U);
        for (std::size_t index = 0; index < outcome.statusCodes.size(); ++index)
        {
            EXPECT_EQ(outcome.statusCodes[index], 200) << "第 " << index + 1 << " 条失败：" << outcome.errors[index];
            EXPECT_NE(outcome.bodies[index].find("served-hello"), std::string::npos) << "第 " << index + 1 << " 条正文";
        }
    }

    /**
     * @brief 钉住流控：正文越过对端窗口时必须等 WINDOW_UPDATE 续发，收的一方要按时归还窗口
     * @details 200 KiB 远大于协议默认的 65535 窗口，两头都要动起来才走得完：本端发正文时按窗口切片
     *          并等增量，收响应时按已消费字节回 WINDOW_UPDATE。少任何一边都是死等，用例表现为超时。
     */
    TEST(Http2ClientConnection, ExchangesBodyLargerThanTheFlowControlWindow)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &loop)
                                         {
                                             registerEchoRoute(router, loop);
                                         },
                                         HttpParserLimits{},
                                         [](TestHttpServer &server)
                                         {
                                             static_cast<void>(server.setHttp2CleartextEnabled(true));
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kClientWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string payload(200U * 1024U, 'x');
        const ClientRunOutcome outcome = runClientRequests(fixture.listeningPort(), "POST", "/echo", payload);
        EXPECT_TRUE(outcome.isStarted);
        ASSERT_EQ(outcome.statusCodes.size(), 1U);
        EXPECT_EQ(outcome.statusCodes[0], 200) << "失败原因：" << outcome.errors[0];
        EXPECT_EQ(outcome.bodies[0].size(), payload.size()) << "回显正文长度不符（流控或分帧漏了字节）";
        EXPECT_EQ(outcome.bodies[0], payload);
    }

    /**
     * @brief 钉住：路由没命中的 404 也要被解成状态码，而不是当成失败
     * @details 状态码来自 :status 伪头；把它丢掉或与普通字段混在一起，调用方就分不清「拿到了响应」与
     *          「连接坏了」这两件事。
     */
    TEST(Http2ClientConnection, ReportsStatusForRouteMisses)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         RouteRegistrar{}, HttpParserLimits{},
                                         [](TestHttpServer &server)
                                         {
                                             static_cast<void>(server.setHttp2CleartextEnabled(true));
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kClientWaitTimeout)) << "服务端未在时限内进入接受循环";

        const ClientRunOutcome outcome = runClientRequests(fixture.listeningPort(), "GET", "/no-such-route");
        EXPECT_TRUE(outcome.isStarted);
        ASSERT_EQ(outcome.statusCodes.size(), 1U);
        EXPECT_EQ(outcome.statusCodes[0], 404) << "失败原因：" << outcome.errors[0];
        EXPECT_TRUE(outcome.errors[0].empty()) << "拿到响应就不该再报失败原因：" << outcome.errors[0];
    }

    /**
     * @brief 钉住请求级时限：对端迟迟不响应时到点就掐断，调用方不会被永远挂住
     * @details 收发协程挂在套接字的等待器上，而等待器没有取消接口——时限只能靠「关掉通路」让它醒来。
     *          这条通路一旦抛异常，异常必须在本层折成失败：穿过协程帧会让调用方的 co_await 再也回不来，
     *          用例表现就不是失败而是挂死（本条正是那处的回归判据）。
     */
    TEST(Http2ClientConnection, CutOffByRequestDeadlineAgainstSlowServer)
    {
        constexpr auto kSlowRouteTime = std::chrono::milliseconds{900};
        constexpr auto kRequestTimeout = std::chrono::milliseconds{150};

        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute,
                                         RouteRegistrar{}, HttpParserLimits{},
                                         [](TestHttpServer &server)
                                         {
                                             static_cast<void>(server.setHttp2CleartextEnabled(true));
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kClientWaitTimeout)) << "服务端未在时限内进入接受循环";

        const auto startTime = std::chrono::steady_clock::now();
        const ClientRunOutcome outcome =
                runClientRequests(fixture.listeningPort(), "GET", "/slow", {}, 1, kRequestTimeout);
        const auto elapsedMillis =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);

        EXPECT_TRUE(outcome.isStarted) << "前奏都没走完就测不出请求级时限";
        ASSERT_EQ(outcome.statusCodes.size(), 1U);
        EXPECT_EQ(outcome.statusCodes[0], 0) << "超时不该给出一个看起来像成功的状态码";
        EXPECT_FALSE(outcome.errors[0].empty()) << "超时必须把原因带回给调用方";
        EXPECT_LT(elapsedMillis, kSlowRouteTime)
                << "一直等到了服务端写完响应（" << elapsedMillis.count() << " 毫秒）：时限没有生效";

        // 收尾前让那条慢路由自己跑完：夹具销毁会先让循环停手，不该把一条要靠定时器才结束的在途请求留到那之后
        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 钉住本端**发出去**的字节符合协议：对着一个脚本化对端逐帧验
     * @details 上面几条都拿自家服务端当对端，而它是宽松的一侧（h2spec 只裁服务端方向，反方向没有现成
     *          oracle）：漏回 SETTINGS ACK、漏 ACK PING、空正文请求不带 END_STREAM、收尾不发 GOAWAY，
     *          服务端都不会立刻把这几件事变成可见的失败。这一条把判据从「服务端接了」换成「对端逐帧看见」。
     */
    TEST(Http2ClientConnection, SendsSpecLegalFramesToRawPeer)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        PeerFrames received;
        std::string prefaceText;
        RawPairRunOutcome outcome;
        auto peerWork = runScriptedPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)), received, prefaceText);
        auto clientWork = runRawPairClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(received.errorText.empty()) << "对端解帧就失败了：" << received.errorText;
        ASSERT_EQ(prefaceText, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") << "前奏要逐字节是这 24 个字符（RFC 7540 §3.4）";
        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，后面的帧自然无从谈起";
        EXPECT_EQ(outcome.statusCode, 200) << "失败原因：" << outcome.errorMessage;

        const Http2Frame *ownSettings = findFrame(received.frames, Http2FrameType::Settings, false);
        const Http2Frame *settingsAck = findFrame(received.frames, Http2FrameType::Settings, true);
        const Http2Frame *pingAck = findFrame(received.frames, Http2FrameType::Ping, true);
        const Http2Frame *requestHeaders = nullptr;
        for (const Http2Frame &frame: received.frames)
        {
            if (frame.header.type == Http2FrameType::Headers)
            {
                requestHeaders = &frame;
                break;
            }
        }
        const Http2Frame *goAway = findFrame(received.frames, Http2FrameType::GoAway, false);

        ASSERT_NE(ownSettings, nullptr) << "客户端 SETTINGS 都没发出来";
        EXPECT_EQ(ownSettings->header.streamId, 0U) << "SETTINGS 只能是连接级帧（§6.5）";
        ASSERT_NE(settingsAck, nullptr) << "收到对端 SETTINGS 必须回 ACK，否则对端会一直等在确认上（§6.5.3）";
        EXPECT_EQ(settingsAck->header.streamId, 0U);
        EXPECT_EQ(settingsAck->payload.size(), 0U) << "ACK 的 SETTINGS 负载必须为空（§6.5）";
        ASSERT_NE(pingAck, nullptr) << "未 ACK 的 PING 必须原样回（§6.7）";
        EXPECT_EQ(pingAck->payload, "12345678") << "PING ACK 要带回同样的 8 字节不透明数据";

        ASSERT_NE(requestHeaders, nullptr) << "没有 HEADERS，请求根本没提出来";
        EXPECT_EQ(requestHeaders->header.streamId, 1U) << "客户端首条流必须是 1，且只能是奇数（§5.1.1）";
        EXPECT_EQ(requestHeaders->header.flags & kHttp2FlagEndHeaders, kHttp2FlagEndHeaders) << "单帧头块要置 END_HEADERS";
        EXPECT_EQ(requestHeaders->header.flags & kHttp2FlagEndStream, kHttp2FlagEndStream)
                << "没有正文的请求必须用 END_HEADERS 就把流收口（§8.1）";

        HpackDecoder peerSideDecoder;
        std::vector<HpackHeaderField> headerFields;
        std::string decodeErrorText;
        ASSERT_TRUE(peerSideDecoder.decode(requestHeaders->payload, headerFields, &decodeErrorText)) << decodeErrorText;
        EXPECT_EQ(findHeaderValue(headerFields, ":method"), "GET");
        EXPECT_EQ(findHeaderValue(headerFields, ":path"), "/tick");
        EXPECT_EQ(findHeaderValue(headerFields, ":scheme"), "http");
        EXPECT_EQ(findHeaderValue(headerFields, ":authority"), "peer") << "四个伪头缺一不可，且必须排在普通头之前（§8.1.2.1）";

        ASSERT_NE(goAway, nullptr) << "礼貌收尾要发 GOAWAY，对端才知道这条连接不再有新流（§6.8）";
        EXPECT_EQ(goAway->header.streamId, 0U);
        Http2GoAwayPayload goAwayPayload;
        std::string goAwayErrorText;
        ASSERT_TRUE(parseHttp2GoAwayPayload(*goAway, goAwayPayload, &goAwayErrorText)) << goAwayErrorText;
        EXPECT_EQ(goAwayPayload.lastStreamId, 1U) << "已受理的最后一条流之外，对端可以把更后面的流整个不当回事";
        EXPECT_EQ(static_cast<std::uint16_t>(goAwayPayload.errorCode), static_cast<std::uint16_t>(Http2ErrorCode::NoError))
                << "正常收尾不该带错误码";
    }

    /**
     * @brief 钉住：对端 SETTINGS 的越界取值一条都不能落进账本，且收场要带 GOAWAY 说明原因
     * @details 三条各踩中 §6.5.2 的一档：MAX_FRAME_SIZE 低于下界（本端按它切正文，收到 0 就是每帧 0
     *          字节的死循环）、INITIAL_WINDOW_SIZE 越过 2^31-1、ENABLE_PUSH 不是布尔。判据两条一起看：
     *          start() 必须失败，且本端要说出一条带对应错误码的 GOAWAY——只把连接判死而不吭声，对端
     *          只看到一个断掉的通路，排查时什么也问不出来。
     */
    TEST(Http2ClientConnection, RejectsIllegalPeerSettingsWithGoAway)
    {
        /// 一条越界的 SETTINGS 参数，连同本端应当回的错误码
        struct IllegalSetting
        {
            const char *label;                      ///< 失败信息里说清踩了哪一档
            Http2SettingIdentifier identifier;      ///< 参数标识
            std::uint32_t value;                    ///< 越界取值
            Http2ErrorCode expectedCode;            ///< 本端该在 GOAWAY 里带的错误码
        };
        const std::vector<IllegalSetting> illegalSettings = {
            IllegalSetting{"MAX_FRAME_SIZE 低于下界", Http2SettingIdentifier::MaxFrameSize, 1U, Http2ErrorCode::ProtocolError},
            IllegalSetting{"INITIAL_WINDOW_SIZE 越过 2^31-1", Http2SettingIdentifier::InitialWindowSize, 0x80000000U,
                           Http2ErrorCode::FlowControlError},
            IllegalSetting{"ENABLE_PUSH 不是布尔", Http2SettingIdentifier::EnablePush, 2U, Http2ErrorCode::ProtocolError},
        };

        for (const IllegalSetting &entry: illegalSettings)
        {
            int clientDescriptor = -1;
            int peerDescriptor = -1;
            ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor)) << entry.label;

            Core::EventLoop loop;
            PeerFrames received;
            bool isStarted = true;
            auto peerWork = runOpeningPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)), received,
                                           singleSettingFrameBytes(entry.identifier, entry.value));
            auto clientWork = runBadSettingsClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), isStarted);
            static_cast<void>(peerWork.handle().resume());
            static_cast<void>(clientWork.handle().resume());
            loop.run();

            EXPECT_FALSE(isStarted) << entry.label << "：越界的通告被当合法参数收下了";
            const Http2Frame *goAway = findFrame(received.frames, Http2FrameType::GoAway, false);
            ASSERT_NE(goAway, nullptr) << entry.label << "：本端判死却没按 §6.8 发 GOAWAY，对端只看到一个断掉的连接";
            Http2GoAwayPayload payload;
            std::string errorText;
            ASSERT_TRUE(parseHttp2GoAwayPayload(*goAway, payload, &errorText)) << errorText;
            EXPECT_EQ(static_cast<std::uint16_t>(payload.errorCode), static_cast<std::uint16_t>(entry.expectedCode))
                    << entry.label << "：GOAWAY 里的错误码不对，现场会顺着错方向查";
            EXPECT_EQ(payload.lastStreamId, 0U) << entry.label << "：还没提过任何流，last-stream-id 该是 0";
        }
    }

    /**
     * @brief 钉住：对端用一串 CONTINUATION 洪泛头块时，本端在撑爆内存之前收口
     * @details 帧上限只钳得住单帧，攒起来的头块可以一段一段无限续——不设闸门就是一个远端可打的内存
     *          DoS。两条判据：这次请求必须以失败收场（半个头块不能当响应交回调用方），且要按 §6.8 回
     *          一条 ENHANCE_YOUR_CALM 的 GOAWAY。只结这条流不行：本端不肯存下的片段交不给 HPACK
     *          解码器，两边的动态表就此错位，留着连接只会让后面每条响应都解歪。
     */
    TEST(Http2ClientConnection, RefusesToBufferAnOverSizedHeaderBlock)
    {
        const std::string flood = makeFrame(Http2FrameType::Settings, 0U, 0U, {})
                                  + makeFrame(Http2FrameType::Headers, 0U, 1U, std::string(8192U, 'x'))
                                  + makeFrame(Http2FrameType::Continuation, 0U, 1U, std::string(8193U, 'x'));

        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        PeerFrames received;
        RawPairRunOutcome outcome;
        auto peerWork = runOpeningPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)), received, flood);
        auto clientWork = runRawPairClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_EQ(outcome.statusCode, 0) << "越界的头块被当响应收了：" << outcome.errorMessage;
        EXPECT_FALSE(outcome.errorMessage.empty()) << "收场原因必须带回给调用方";
        const Http2Frame *goAway = findFrame(received.frames, Http2FrameType::GoAway, false);
        ASSERT_NE(goAway, nullptr) << "本端静默关掉通路，对端不知道为什么";
        Http2GoAwayPayload payload;
        std::string errorText;
        ASSERT_TRUE(parseHttp2GoAwayPayload(*goAway, payload, &errorText)) << errorText;
        EXPECT_EQ(static_cast<std::uint16_t>(payload.errorCode), static_cast<std::uint16_t>(Http2ErrorCode::EnhanceYourCalm))
                << "内存闸门触发的收口要报 ENHANCE_YOUR_CALM，报成协议错误会把排查带去别处";
    }

    /**
     * @brief 钉住：一条连接上同时跑两条请求，且响应按流号各归各的
     * @details 对端收齐两条 HEADERS 才开始回答，并按**倒序**作答——本端要是把两条排成队，第一条永远
     *          等不到「两条都到」；本端要是按到达顺序而不是按流号分发响应，正文就会串到另一条请求上。
     *          两条判据都是结构性的，不靠计时，慢机器上不会假绿也不会假红。
     */
    TEST(Http2ClientConnection, ServesTwoConcurrentRequestsOnOneConnection)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        std::vector<std::pair<std::uint32_t, std::string>> openedRequests;
        MultiplexRunOutcome outcome;
        auto peerWork = runMultiplexPeer(TcpStream(Core::AsyncSocket(loop, peerDescriptor)), openedRequests);
        auto clientWork = runMultiplexTask(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        ASSERT_TRUE(outcome.isStarted) << "连接前奏没走完";
        ASSERT_EQ(outcome.bodies.size(), 2U);
        EXPECT_TRUE(outcome.errors[0].empty()) << "第一条失败：" << outcome.errors[0];
        EXPECT_TRUE(outcome.errors[1].empty()) << "第二条失败：" << outcome.errors[1];
        ASSERT_EQ(openedRequests.size(), 2U) << "对端没能同时收到两条请求：本端还在排队发";
        // 流号按提交顺序可能是 1、3 也可能是 3、1（调度先后不由本层规定），但两条合起来必须是 1 与 3：
        // 客户端流号必须奇数且严格递增（§5.1.1）
        std::vector<std::uint32_t> streamIds{openedRequests[0].first, openedRequests[1].first};
        std::sort(streamIds.begin(), streamIds.end());
        EXPECT_EQ(streamIds[0], 1U) << "客户端首条流必须是 1（§5.1.1）";
        EXPECT_EQ(streamIds[1], 3U) << "第二条该是 3：流号奇数且严格递增";
        EXPECT_EQ(outcome.bodies[0], "peer-body-/tick-a") << "第一条拿到了别人的响应：" << outcome.bodies[0];
        EXPECT_EQ(outcome.bodies[1], "peer-body-/tick-b") << "第二条拿到了别人的响应：" << outcome.bodies[1];
    }
} // namespace AsynGyanis::Net
