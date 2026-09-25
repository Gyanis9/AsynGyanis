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

        /// 「一条连接只许开一条流」那趟的结论
        struct StreamBudgetRunOutcome
        {
            bool isStarted{false};          ///< 前奏是否走完
            int firstStatusCode{0};         ///< 第一条请求的状态码
            std::string firstErrorMessage;  ///< 第一条失败时的中文原因
            /// 第一条收齐之后连接是否还算健康：额度见顶且没有在途的流时，本端应当自己退场
            bool isHealthyAfterFirst{true};
            int secondStatusCode{0};        ///< 第二条请求的状态码（提不出流就该是 0）
            std::string secondErrorMessage; ///< 第二条被拒时带回来的中文原因
        };

        /**
         * @brief 用「最多开一条流」的配置走完「一条成功 → 连接自己退场 → 第二条被拒」
         * @details 那条额度平时是 2^30（RFC 7540 §5.1.1 给客户端流号的上界：奇数、严格递增、不过
         *          2^31-1），压到 1 才测得到见顶之后的行为。真实场景并不遥远：一条待命连接按一万请求
         *          每秒约 30 小时就用完了流号。
         * @param loop 客户端事件循环
         * @param clientSide 本端一侧的通路
         * @param outcome 就地收集结论
         */
        Core::Task<void> runStreamBudgetClient(Core::EventLoop &loop, TcpStream clientSide, StreamBudgetRunOutcome &outcome)
        {
            Http2ClientConnection::Config config;
            config.maximumOpenedStreamCount = 1U;
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)),
                    config);
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);

            const Http2ClientResponse first = co_await connection->request(
                    "http", "peer", "GET", "/tick", {}, {}, kClientWaitTimeout);
            outcome.firstStatusCode = first.statusCode;
            outcome.firstErrorMessage = first.errorMessage;
            outcome.isHealthyAfterFirst = connection->isHealthy();

            const Http2ClientResponse second = co_await connection->request(
                    "http", "peer", "GET", "/tick-again", {}, {}, kClientWaitTimeout);
            outcome.secondStatusCode = second.statusCode;
            outcome.secondErrorMessage = second.errorMessage;
            co_return;
        }

        /// 「一条在途、额度刚见顶」时提第二条的结论
        struct BusyBudgetRunOutcome
        {
            bool isStarted{false};              ///< 前奏是否走完
            std::size_t inFlightWhenRefused{0}; ///< 提第二条那一刻本端在途几条流
            int secondStatusCode{0};            ///< 第二条的状态码（被拒就该是 0）
            std::string secondErrorMessage;     ///< 第二条被拒的中文原因
            bool isFirstStillPending{false};    ///< 第二条被拒之后，在途的第一条是否没被连坐
        };

        /**
         * @brief 提起一条请求并把结论写回调用方给的格子
         * @param connection 被测连接
         * @param path 请求路径
         * @param waitTimeout 这条请求的等待上限
         * @param statusCode 输出：状态码
         * @param errorMessage 输出：失败原因
         */
        Core::Task<void> runOneBudgetRequest(Http2ClientConnection &connection, const std::string_view path,
                                             const std::chrono::milliseconds waitTimeout, int &statusCode,
                                             std::string &errorMessage)
        {
            const Http2ClientResponse response = co_await connection.request("http", "peer", "GET", path, {}, {}, waitTimeout);
            statusCode = response.statusCode;
            errorMessage = response.errorMessage;
            co_return;
        }

        /**
         * @brief 额度只有 1 的连接上：第一条挂在半路（对端只收不答），再提第二条
         * @details 这一条测的是**闸本身**：上面那条顺序用例里第一条已收齐，连接随即自己退场，第二条
         *          是被「连接已不可用」挡下的，闸拆掉也照样绿——所以要制造「到界且仍有流在途」这个
         *          中间态。在途数读的是本端自己的账，不靠睡；第二条刻意给一个短时限，让「闸没拦住」
         *          表现为「等到超时、原因不点名额度」而不是把用例挂死。
         * @param loop 客户端事件循环
         * @param clientSide 本端一侧的通路
         * @param outcome 就地收集结论
         */
        Core::Task<void> runBusyBudgetClient(Core::EventLoop &loop, TcpStream clientSide, BusyBudgetRunOutcome &outcome)
        {
            Http2ClientConnection::Config config;
            config.maximumOpenedStreamCount = 1U;
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)),
                    config);
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);

            int firstStatus = 0;
            std::string firstError;
            Core::Task<void> firstWork = runOneBudgetRequest(*connection, "/tick", kClientWaitTimeout, firstStatus, firstError);
            if (!firstWork.isReady())
            {
                loop.scheduler().schedule(firstWork.handle());
            }

            const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (connection->inFlightStreamCount() == 0U && std::chrono::steady_clock::now() < settleDeadline)
            {
                Core::Timer settleTimer(loop);
                co_await settleTimer.waitFor(std::chrono::milliseconds{1});
            }
            outcome.inFlightWhenRefused = connection->inFlightStreamCount();

            const Http2ClientResponse second = co_await connection->request(
                    "http", "peer", "GET", "/tick-again", {}, {}, std::chrono::milliseconds{150});
            outcome.secondStatusCode = second.statusCode;
            outcome.secondErrorMessage = second.errorMessage;
            outcome.isFirstStillPending = connection->inFlightStreamCount() == 1U;

            // 收尾：本端关掉通路，对端的读取循环与本端那条在途的请求都随之收场
            connection->close();
            const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (connection->inFlightStreamCount() > 0U && std::chrono::steady_clock::now() < drainDeadline)
            {
                Core::Timer drainTimer(loop);
                co_await drainTimer.waitFor(std::chrono::milliseconds{1});
            }
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

        /// 「请求发出过、对端没答」这一位的结论
        struct SentFlagRunOutcome
        {
            bool isStarted{false};
            bool isAnyByteSent{false};
            bool isAnyByteReceived{false};
            int statusCode{0};
            bool isOk{false};
        };

        /**
         * @brief 把本端发来的请求整段读掉、却始终不作答，然后收口的对端
         * @details 先读后关是关键：不读就关会把已发出去的字节连着 RST 一起丢掉，那一支应当判成
         *          「没发出去、可以重来」，就测不到「发出去了但没人答」这一位了。
         */
        Core::Task<void> runSilentClosingPeer(Core::EventLoop &loop, TcpStream peer)
        {
            Http2FrameDecoder decoder;
            std::array<char, 24> preface{};
            static_cast<void>(co_await peer.readExact(preface.data(), preface.size()));
            static_cast<void>(co_await readPeerFrames(peer, decoder, 1)); // 本端的 SETTINGS
            const std::string greeting = makeFrame(Http2FrameType::Settings, 0U, 0U, {});
            co_await peer.writeAll(greeting.data(), greeting.size());
            // 把 HEADERS 与 DATA 读干净（正文只有一条帧），然后一个字都不答就退栈收口
            static_cast<void>(co_await readPeerFrames(peer, decoder, 2));
            co_return;
        }

        /// 提一条带正文的 POST，把两位「发出过 / 收到过」带回来
        Core::Task<void> runSentFlagClient(Core::EventLoop &loop, TcpStream clientSide, SentFlagRunOutcome &outcome)
        {
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)));
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);
            const Http2ClientResponse response = co_await connection->request(
                    "http", "peer", "POST", "/upload", {}, std::string(4U * 1024U, 'y'), kClientWaitTimeout);
            outcome.isAnyByteSent = response.isAnyByteSent;
            outcome.isAnyByteReceived = response.isAnyByteReceived;
            outcome.statusCode = response.statusCode;
            outcome.isOk = response.isOk();
            loop.stop();
            co_return;
        }

        /// 「对端只给了半截响应就收口」的结论
        struct TruncatedRunOutcome
        {
            bool isStarted{false};
            int statusCode{0};        ///< 解出来的状态码：它确实是 200，判据不在这里
            std::string body;         ///< 已收到的那半截正文
            std::string errorMessage; ///< 失败原因；修之前这里是空的
            bool isOk{false};         ///< isOk() 的返回值——这条用例的主判据
        };

        /**
         * @brief 回一句 200 与一段 DATA 就把通路收口的对端（两边都不给 END_STREAM）
         * @details 收口发生在协程 co_return 时：通路是本协程按值持有的，帧栈一退套接字就关掉
         */
        Core::Task<void> runTruncatingPeer(Core::EventLoop &loop, TcpStream peer)
        {
            Http2FrameDecoder decoder;
            std::array<char, 24> preface{};
            static_cast<void>(co_await peer.readExact(preface.data(), preface.size()));
            static_cast<void>(co_await readPeerFrames(peer, decoder, 1)); // 本端的 SETTINGS
            const std::string greeting = makeFrame(Http2FrameType::Settings, 0U, 0U, {});
            co_await peer.writeAll(greeting.data(), greeting.size());
            static_cast<void>(co_await readPeerFrames(peer, decoder, 1)); // 本端的 HEADERS

            HpackEncoder peerEncoder;
            const std::string headerBlock = peerEncoder.encode({HpackHeaderField{":status", "200"}});
            const std::string bytes = makeFrame(Http2FrameType::Headers, kHttp2FlagEndHeaders, 1U, headerBlock)
                                      + makeFrame(Http2FrameType::Data, 0U, 1U, "half-body");
            co_await peer.writeAll(bytes.data(), bytes.size());
            // 之后什么都不发：本端要等的 END_STREAM 永远不会来。按住通路不收也不断，让本端自己的
            // 请求时限去掐——比「关掉套接字制造 EOF」稳定：回路上带未读数据收口会变成 RST，
            // 那时连已经送出去的那句 200 都可能一起丢掉，判据就测不到「截断」这件事了
            Core::Timer holdTimer(loop);
            co_await holdTimer.waitFor(std::chrono::milliseconds{600});
            co_return;
        }

        /// 走一趟「半截响应」：本端必须把它判成失败，而不是「200 加一段短正文」
        Core::Task<void> runTruncatedClient(Core::EventLoop &loop, TcpStream clientSide, TruncatedRunOutcome &outcome)
        {
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)));
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);
            const Http2ClientResponse response = co_await connection->request(
                    "http", "peer", "GET", "/tick", {}, {}, std::chrono::milliseconds{200});
            outcome.statusCode = response.statusCode;
            outcome.body = response.body;
            outcome.errorMessage = response.errorMessage;
            outcome.isOk = response.isOk();
            loop.stop();
            co_return;
        }

        /// 「两条大正文同时堵在写上」的结论
        struct ContendedWriteRunOutcome
        {
            bool isStarted{false};
            bool isHealthyWhileStalled{false};   ///< 两条都堵着、通路还活着时连接是否仍算可用
            std::size_t inFlightWhileStalled{0}; ///< 同一刻在途的流数
            std::size_t finishedRequestCount{0}; ///< 两条是否都收了口
            std::string peerDecodeErrorText;     ///< 对端解帧的报错：被撕开的字节会在这里露出来
        };

        /**
         * @brief 先通告一个大接收窗口、按住一段时间不收字节、之后才开始解帧的对端
         * @details 窗口必须大：协议默认的 65535 发送窗口会让第二条流卡在流控上，压根到不了套接字，
         *          而本条要量的正是「两个协程同时堵在同一条通路的写上」。按住不收，512 KiB 的正文
         *          一定能把套接字缓冲塞满，两边都在等可写。
         */
        Core::Task<void> runSlowDrainingPeer(Core::EventLoop &loop, TcpStream peer, PeerFrames &received,
                                             const std::chrono::milliseconds holdTime)
        {
            Http2SettingsPayload window;
            window.parameters.push_back(Http2Setting{
                    .identifier = static_cast<std::uint16_t>(Http2SettingIdentifier::InitialWindowSize),
                    .value = 8U * 1024U * 1024U});
            const std::string greeting = encodeHttp2SettingsFrame(window);
            co_await peer.writeAll(greeting.data(), greeting.size());

            Core::Timer holdTimer(loop);
            co_await holdTimer.waitFor(holdTime);

            Http2FrameDecoder decoder;
            // 解帧之前先把 24 字节前奏吃掉：它不是帧，直接喂给解码器只会报「帧头不合规」
            std::array<char, 24> preface{};
            static_cast<void>(co_await peer.readExact(preface.data(), preface.size()));
            try
            {
                while (true)
                {
                    const PeerFrames batch = co_await readPeerFrames(peer, decoder, 1);
                    for (const Http2Frame &frame: batch.frames)
                    {
                        received.frames.push_back(frame);
                    }
                    if (!batch.errorText.empty())
                    {
                        received.errorText = batch.errorText;
                        break;
                    }
                    if (batch.frames.empty())
                    {
                        break; // 通路收口
                    }
                }
            } catch (const std::exception &)
            {
                // 本端按时限掐掉通路：解到哪儿算哪儿
            }
            co_return;
        }

        /// 提起一条大正文的 POST：正文注定发不完，要看的只有它有没有把连接弄坏
        Core::Task<void> runStalledPost(Http2ClientConnection &connection, const std::string &body,
                                       std::size_t &finishedRequestCount)
        {
            static_cast<void>(co_await connection.request("http", "peer", "POST", "/upload", {}, body,
                                                         std::chrono::milliseconds{400}));
            ++finishedRequestCount;
            co_return;
        }

        /**
         * @brief 给足窗口、之后**一个字节都不读**的对端
         * @details 上面那条慢读对端要的是「解帧无错」，这一条要的是「写一定堵住在套接字上」：连接窗口
         *          先按 8 MiB 续上（不然两条流各发 64 KiB 就停在流控上，压根到不了套接字缓冲），
         *          之后本端不 read——套接字发送缓冲一满，两边就同时堵在写上。
         */
        Core::Task<void> runNeverReadingPeer(Core::EventLoop &loop, TcpStream peer, const std::chrono::milliseconds holdTime)
        {
            Http2SettingsPayload window;
            window.parameters.push_back(Http2Setting{
                    .identifier = static_cast<std::uint16_t>(Http2SettingIdentifier::InitialWindowSize),
                    .value = 8U * 1024U * 1024U});
            // 连接级 WINDOW_UPDATE 的增量按 32 位大端排：这里是 8 MiB。0 是非法增量（§6.9.1），不能写
            const std::string connectionCredit{static_cast<char>(0x00), static_cast<char>(0x80),
                                               static_cast<char>(0x00), static_cast<char>(0x00)};
            const std::string greeting = encodeHttp2SettingsFrame(window)
                                         + rawFrameBytes(static_cast<std::uint8_t>(Http2FrameType::WindowUpdate), 0U, 0U,
                                                         connectionCredit);
            co_await peer.writeAll(greeting.data(), greeting.size());
            Core::Timer holdTimer(loop);
            co_await holdTimer.waitFor(holdTime);
            co_return; // 就此退栈：本端看到的是一句 200 都没有、通路还在但字节发不出去
        }

        /**
         * @brief 两条 512 KiB 正文并发提交，在它们都堵在写上时采样连接状态
         */
        Core::Task<void> runContendedWriteClient(Core::EventLoop &loop, TcpStream clientSide,
                                                ContendedWriteRunOutcome &outcome, const std::size_t bodyByteCount)
        {
            auto connection = std::make_unique<Http2ClientConnection>(
                    loop, HttpOutboundConnection::forPlain(HttpOutboundEndpointKey{"peer", 0U, false}, std::move(clientSide)));
            outcome.isStarted = co_await connection->start(kClientWaitTimeout);
            if (outcome.isStarted)
            {
                const std::string body(bodyByteCount, 'x');
                std::size_t finishedRequestCount = 0;
                Core::Task<void> first = runStalledPost(*connection, body, finishedRequestCount);
                Core::Task<void> second = runStalledPost(*connection, body, finishedRequestCount);
                if (!first.isReady())
                {
                    loop.scheduler().schedule(first.handle());
                }
                if (!second.isReady())
                {
                    loop.scheduler().schedule(second.handle());
                }

                // 等两条都真的挂上去了再采样：这是在等本端自己的计数，不是在睡
                const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                while (connection->inFlightStreamCount() < 2U && std::chrono::steady_clock::now() < settleDeadline)
                {
                    Core::Timer settleTimer(loop);
                    co_await settleTimer.waitFor(std::chrono::milliseconds{1});
                }
                outcome.inFlightWhileStalled = connection->inFlightStreamCount();
                outcome.isHealthyWhileStalled = connection->isHealthy();

                const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                while (finishedRequestCount < 2U && std::chrono::steady_clock::now() < drainDeadline)
                {
                    Core::Timer drainTimer(loop);
                    co_await drainTimer.waitFor(std::chrono::milliseconds{1});
                }
                outcome.finishedRequestCount = finishedRequestCount;
            }
            loop.stop();
            co_return;
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
     * @brief 钉住：开流额度见顶之后，本端交代一条 NO_ERROR 的 GOAWAY 退场，并不再提出新流
     * @details 客户端流号是「奇数、严格递增、不超过 2^31-1」（§5.1.1），一条连接最多提 2^30 条流；
     *          不缺这条闸的话，`m_nextStreamId` 到顶会回绕成小号甚至偶数，之后每一条请求都被对端按
     *          PROTOCOL_ERROR 判死——而一条待命连接按一万请求每秒约 30 小时就到界，不是理论问题。
     *          判据四条一起看：第一条照常 200、它收齐之后连接已不算健康、第二条提不出来且原因点名
     *          「额度」、对端那侧确实收到一条 last-stream-id 为 1 且不带错误码的 GOAWAY（只掐通路不
     *          吭声，对端就只能看到一个断掉的连接）。
     */
    TEST(Http2ClientConnection, RetiresConnectionWhenTheStreamIdBudgetIsSpent)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        PeerFrames received;
        std::string prefaceText;
        StreamBudgetRunOutcome outcome;
        auto peerWork = runScriptedPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)), received, prefaceText);
        auto clientWork = runStreamBudgetClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(received.errorText.empty()) << "对端解帧就失败了：" << received.errorText;
        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，后面的额度判定无从谈起";
        EXPECT_EQ(outcome.firstStatusCode, 200) << "第一条本该照常走完，失败原因：" << outcome.firstErrorMessage;
        EXPECT_FALSE(outcome.isHealthyAfterFirst) << "额度见顶且最后一条流已收齐，本端应当自己退场而不是留着待用";
        EXPECT_EQ(outcome.secondStatusCode, 0) << "额度见顶之后还提出了第二条流：流号就要回绕了";
        EXPECT_NE(outcome.secondErrorMessage.find("额度"), std::string::npos)
                << "第二条被拒的原因没点名额度，排查时会以为是网络问题：「" << outcome.secondErrorMessage << "」";

        const Http2Frame *goAway = findFrame(received.frames, Http2FrameType::GoAway, false);
        ASSERT_NE(goAway, nullptr) << "退场要按 §6.8 交代一句 GOAWAY，不能直接掐通路";
        EXPECT_EQ(goAway->header.streamId, 0U) << "GOAWAY 是连接级帧";
        Http2GoAwayPayload goAwayPayload;
        std::string goAwayErrorText;
        ASSERT_TRUE(parseHttp2GoAwayPayload(*goAway, goAwayPayload, &goAwayErrorText)) << goAwayErrorText;
        EXPECT_EQ(goAwayPayload.lastStreamId, 1U) << "已受理的最后一条流是 1：更后面的流对端可以整个不当回事";
        EXPECT_EQ(static_cast<std::uint16_t>(goAwayPayload.errorCode), static_cast<std::uint16_t>(Http2ErrorCode::NoError))
                << "配额用完不是谁的违规，GOAWAY 不该带错误码";
    }

    /**
     * @brief 钉住：额度见顶而最后一条流还在途时，闸要当场拒掉新请求，而不是再占一条流
     * @details 上面那条顺序用例测的是「到点退场」，这一条测的是闸本身：那条用例里第一条已经收齐、
     *          连接随即自己退场，第二条其实是被「连接已不可用」挡下的——把额度判断整个拆掉它照样绿。
     *          这里让对端只收不答，第一条就一直挂在途上，于是「到界 + 有流在途」这个中间态真的被走到。
     *          最硬的一条判据在对端那侧：只应看到一条 HEADERS。闸失效时第二条会以流号 3 上过通路，
     *          用例这边则表现为「原因不点名额度」（它等满了自己那个 150 毫秒时限）。
     */
    TEST(Http2ClientConnection, RefusesANewStreamWhenTheBudgetIsSpentWhileOneIsInFlight)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        PeerFrames received;
        BusyBudgetRunOutcome outcome;
        const std::string greeting = makeFrame(Http2FrameType::Settings, 0U, 0U, {});
        auto peerWork = runOpeningPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)), received, greeting);
        auto clientWork = runBusyBudgetClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(received.errorText.empty()) << "对端解帧就失败了：" << received.errorText;
        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，后面的额度判定无从谈起";
        EXPECT_EQ(outcome.inFlightWhenRefused, 1U) << "第一条没挂在途上：这条用例没测到「到界且有流在途」";
        EXPECT_EQ(outcome.secondStatusCode, 0) << "额度见顶且第一条还在途，第二条不该再占一条流";
        EXPECT_NE(outcome.secondErrorMessage.find("额度"), std::string::npos)
                << "第二条没被额度闸挡下（它等满了自己的时限），原因：「" << outcome.secondErrorMessage << "」";
        EXPECT_TRUE(outcome.isFirstStillPending) << "第二条被拒时把在途的第一条连坐了：拒绝新流不该动已有的流";

        std::size_t peerHeadersFrameCount = 0;
        for (const Http2Frame &frame: received.frames)
        {
            if (frame.header.type == Http2FrameType::Headers)
            {
                ++peerHeadersFrameCount;
            }
        }
        EXPECT_EQ(peerHeadersFrameCount, 1U) << "对端看到了第二条 HEADERS：额度闸没拦住，流号会一路涨到回绕";
    }

    /**
     * @brief 钉住「发出过字节」这一位：请求整个交上通路、对端一个字节没答就收口
     * @details 出站侧「能不能重来一次」就靠这两位（发出过 / 收到过）分开判：只看收到过没有，会把
     *          「POST 已整个发出、对端只是没答完」当成空闲期被对端收掉的连接，于是重发一遍。
     *          这条用例钉的是判据的输入：isAnyByteSent 为真、isAnyByteReceived 为假、整体算失败。
     *          置位刻意放在写成功之后——通路本来就死着时那次写会失败，那一支仍该是「可以重来」。
     */
    TEST(Http2ClientConnection, MarksARequestAsSentWhenThePeerAnsweredNothing)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        SentFlagRunOutcome outcome;
        auto peerWork = runSilentClosingPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)));
        auto clientWork = runSentFlagClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，请求根本没上路";
        EXPECT_TRUE(outcome.isAnyByteSent) << "正文已整个交上通路，这一位却为假：复用侧会把它当成可以重来一次";
        EXPECT_FALSE(outcome.isAnyByteReceived) << "对端一个字都没答，这一位不该为真";
        EXPECT_EQ(outcome.statusCode, 0);
        EXPECT_FALSE(outcome.isOk) << "发出过却什么也没收到，必须算失败（重发与否由调用方按这两位决定）";
    }

    /**
     * @brief 钉住：没等到 END_STREAM 的响应不算收齐，即便状态码已经解出来
     * @details 对端先回一句 200 和一段 DATA 就再没下文（这里由请求时限把通路掐断来制造），本端手里
     *          确实有「200」——但把它连同半截正文当成一次成功响应交出去，是最坏的结局：调用方看不出
     *          自己拿到的是残缺正文，正文短了、校验失败了都只能事后猜。判据三条一起看：状态码保留
     *          （排查时要看得出是对端半路没写完）、isOk() 为假、失败原因非空。
     */
    TEST(Http2ClientConnection, RejectsAResponseThatNeverGotEndStream)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        TruncatedRunOutcome outcome;
        auto peerWork = runTruncatingPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)));
        auto clientWork = runTruncatedClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，后面的判据无从谈起";
        EXPECT_EQ(outcome.statusCode, 200) << "已解出的状态码要留着：它说明这是「截断」而不是「什么都没收到」";
        EXPECT_EQ(outcome.body, "half-body") << "半截正文本身要原样交回来，正文长度：" << outcome.body.size();
        EXPECT_FALSE(outcome.isOk) << "没有 END_STREAM 就不是一个收齐的响应，200 也不能算成功";
        EXPECT_FALSE(outcome.errorMessage.empty()) << "判成失败就得给一句原因，否则调用方不知道断在哪";
    }

    /**
     * @brief 钉住：两条大正文同时堵在写上时，后到的那个排队等写权，不去撞开第二条写
     * @details 通路读只能有一个等待者，这一点由驱动租约管住了；写没有这个限制，但「一次 send 只送一段
     *          字节」意味着两个协程同时在写会把同一帧的字节撕成两段交错送出去，而传输层的等待器一个
     *          方向只登记一个等待者——第二个会当场撞出用法错误，本层把它折成「通路不可用」，一条好好的
     *          连接就因为没人读套接字而死掉了。判据两条各拦一种失效：连接仍然可用（没被第二次写撞死）、
     *          对端解帧无错（字节没被撕开）。
     */
    TEST(Http2ClientConnection, KeepsTheConnectionUsableWhileTwoWritersAreStalled)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        PeerFrames received;
        ContendedWriteRunOutcome outcome;
        auto peerWork = runSlowDrainingPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)), received,
                                           std::chrono::milliseconds{200});
        auto clientWork = runContendedWriteClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome,
                                                 512U * 1024U);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，两条请求根本没机会上路";
        ASSERT_EQ(outcome.inFlightWhileStalled, 2U) << "两条大正文没能同时在途：这条用例没测到并发写";
        EXPECT_TRUE(outcome.isHealthyWhileStalled) << "第二个写协程撞开了同一条通路的写：连接被自己人判死了";
        EXPECT_EQ(outcome.finishedRequestCount, 2U) << "两条请求都该在自己的时限到点后收口，不能被写权挂住";
        EXPECT_TRUE(received.errorText.empty()) << "对端解帧报错，说明写出去的字节被撕开了：" << received.errorText;

        // 同一趟顺带钉住连接级窗口的账：本端通告的是流级大窗口，连接窗口仍是协议默认的 65535，而对端
        // 一次 WINDOW_UPDATE 都没发 —— 两条流合起来发的 DATA 就不能越过那 65535 字节。窗口是在写之前
        // 扣还是之后扣，差别正在这里：之后扣的话第二条看到的还是满窗，两条各发一轮就越了界
        std::size_t dataPayloadByteCount = 0;
        for (const Http2Frame &frame: received.frames)
        {
            if (frame.header.type == Http2FrameType::Data)
            {
                dataPayloadByteCount += frame.payload.size();
            }
        }
        EXPECT_LE(dataPayloadByteCount, 65535U) << "越过对端连接发送窗口：" << dataPayloadByteCount << " 字节";
    }

    /**
     * @brief 钉住写权：两条流真的同时堵在套接字写上时，连接不能被自己人判死
     * @details 上一条靠「对端晚点才开始读」限制总量，顺带钉住连接窗口；这一条把连接窗口也续到 8 MiB
     *          并且**一个字节都不读**，让两条 8 MiB 的正文确实堆在套接字发送缓冲上——只有堵在那里，
     *          「第二个写协程自己去碰通路」这件事才会发生。传输层的等待器一个方向只登记一个等待者，
     *          第二个会撞出用法错误，本层把它折成「通路不可用」：一条只是没人读的连接就此死掉。
     *          判据取堵在那儿那一刻的采样：连接仍算可用、两条流都还在途。
     */
    TEST(Http2ClientConnection, QueuesSecondWriterWhileTheSocketBufferIsFull)
    {
        Core::EventLoop loop;
        int clientDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(clientDescriptor, peerDescriptor));

        ContendedWriteRunOutcome outcome;
        auto peerWork = runNeverReadingPeer(loop, TcpStream(Core::AsyncSocket(loop, peerDescriptor)),
                                            std::chrono::milliseconds{600});
        auto clientWork = runContendedWriteClient(loop, TcpStream(Core::AsyncSocket(loop, clientDescriptor)), outcome,
                                                8U * 1024U * 1024U);
        static_cast<void>(peerWork.handle().resume());
        static_cast<void>(clientWork.handle().resume());
        loop.run();

        EXPECT_TRUE(outcome.isStarted) << "前奏没走完，两条请求根本没机会上路";
        ASSERT_EQ(outcome.inFlightWhileStalled, 2U) << "两条大正文没能同时在途：这条用例没测到并发写";
        EXPECT_TRUE(outcome.isHealthyWhileStalled) << "第二个写协程自己去碰了通路，把一条只是没人读的连接判死了";
        EXPECT_EQ(outcome.finishedRequestCount, 2U) << "两条请求都该在自己的时限到点后收口";
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
