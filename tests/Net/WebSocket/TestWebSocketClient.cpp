// TestWebSocketClient.cpp —— 出站 WebSocket 会话的真实回环测试（RFC 6455 §4.1/§4.2.2/§5.1/§5.3/§5.5/§7.4）
//
// 对端由本文件按规范手写字节：101 应答、帧头、掩码异或、状态码都自己拼、自己解，不借被测实现的
// 编码器与解码器，因此客户端不可能「自证」。唯一借用的是 accept 值的计算——那条推导已由
// TestWebSocketHandshake.cpp 里 RFC 6455 §4.2.2 的金标准对照钉住，本文件把它当已知量。
//
// 判据分两面：发出去的帧按线上字节判（MASK 位、键的位置、逐帧换键、异或结果、操作码），
// 收进来的按交付给业务的东西判（消息完整、控制帧不外露、Close 的码与原因）。
// 时限那一条断的是「有出口」而不是「够快」：对端一言不发时 connect() 必须带着原因回来。
#include "Net/WebSocket/WebSocketClient.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/WebSocket/WebSocketHandshake.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
namespace AsynGyanis::Net
{
    namespace
    {
        /// 一般等待上限：回环上的握手与一次收发都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{3000};
        /// 本文件里的对端一律在这个路径上应答
        constexpr std::string_view kRequestTarget = "/ws";
        // 帧首字节 = FIN 置位 + 操作码；MASK 位在第二个字节上，与方向有关，因此这里只列首字节
        constexpr std::uint8_t kTextFrameFirstByte   = 0x81;
        constexpr std::uint8_t kBinaryFrameFirstByte = 0x82;
        constexpr std::uint8_t kCloseFrameFirstByte  = 0x88;
        constexpr std::uint8_t kPingFrameFirstByte   = 0x89;
        constexpr std::uint8_t kPongFrameFirstByte   = 0x8A;
        /// 用例里当「全零键」对照用的那把键：EXPECT 一个 4 字节数组不是零
        constexpr std::array<std::uint8_t, 4> kZeroMaskKey{0, 0, 0, 0};
        /**
         * @brief 客户端发来的一帧解好掩码之后的样子
         */
        struct WireFrame
        {
            std::uint8_t                firstByte{0};    ///< 首字节原值（FIN、RSV 与操作码都在里面）
            bool                        isMasked{false}; ///< 第二字节的 MASK 位
            std::array<std::uint8_t, 4> maskKey{};       ///< 掩码键原值（未掩码时全 0）
            std::string                 payload;         ///< 已解掩码的负载
        };
        /// 一次用例运行的结论：握手成不成、交付了什么、发出去几帧、以及两份线上读数
        struct ClientRunOutcome
        {
            bool                        is_connected{false};          ///< connect() 是否成功
            std::string                 failure_reason;               ///< 失败时点明断在哪一段
            std::vector<WebSocketFrame> delivered;                    ///< 会话交给业务的消息
            std::vector<bool>           send_results;                 ///< 用例里几次 send*/close 的结论
            std::string                 receive_error;                ///< receive() 交回的拒因
            std::string                 accepted_subprotocol;         ///< 握手谈定的子协议
            bool                        is_open_after_actions{false}; ///< 动作跑完时会话是否仍对开
        };
        using Action     = std::function<Core::Task<void>(WebSocketClient &, ClientRunOutcome &)>;
        using PeerScript = std::function<void(class PeerWire &)>;
        /**
         * @brief 脚本用的字接口：在已接受的那条连接上读与写
         * @details 升级请求原文与之后的帧字节分开累计：断言「线上第几帧」时不必再去切请求头。
         *          每一次读都带时限，并在对端收口时立刻结束——脚本卡在「等客户端说话」上时
         *          用例要能红，不能让测试进程挂住。
         */
        class PeerWire
        {
        public:
            PeerWire(const int clientSocket, std::string &headSink, std::string &frameSink, std::mutex &sinkMutex, const std::atomic<bool> &isStopping) :
                m_socket(clientSocket), m_headSink(headSink), m_frameSink(frameSink), m_sinkMutex(sinkMutex), m_isStopping(isStopping),
                m_deadline(std::chrono::steady_clock::now() + kWaitTimeout)
            {
            }
            /// 读到头部块的 \r\n\r\n 为止，结果落在请求那份读数里
            [[nodiscard]] std::string readUpgradeRequest()
            {
                std::string accumulated;
                while (!m_isPeerClosed && accumulated.find("\r\n\r\n") == std::string::npos && stillWaiting())
                {
                    accumulated.append(readSome());
                }
                const std::lock_guard guard(m_sinkMutex);
                m_headSink = accumulated;
                return accumulated;
            }
            /// 读完整一整个客户端帧（按 MASK 位与长度域自己算总长），原始字节落进帧那份读数
            void readOneClientFrame()
            {
                std::string bytes = readExactly(2);
                if (bytes.size() < 2)
                {
                    return;
                }
                const auto  secondByte = static_cast<std::uint8_t>(bytes[1]);
                std::size_t remainder  = secondByte & 0x7FU;
                if (remainder == 126)
                {
                    bytes.append(readExactly(2));
                    if (bytes.size() < 4)
                    {
                        return;
                    }
                    remainder = (static_cast<std::size_t>(static_cast<unsigned char>(bytes[2])) << 8) | static_cast<std::size_t>(static_cast<unsigned char>(bytes[3]));
                }
                if ((secondByte & 0x80U) != 0U)
                {
                    remainder += 4; // 掩码键跟在长度之后
                }
                bytes.append(readExactly(remainder));
                const std::lock_guard guard(m_sinkMutex);
                m_frameSink.append(bytes);
            }
            bool write(const std::string_view bytes)
            {
                std::size_t sentByteCount = 0;
                while (sentByteCount < bytes.size())
                {
                    const ssize_t sent = ::send(m_socket, bytes.data() + sentByteCount, static_cast<int>(bytes.size() - sentByteCount), 0);
                    if (sent <= 0)
                    {
                        return false;
                    }
                    sentByteCount += static_cast<std::size_t>(sent);
                }
                return true;
            }

        private:
            [[nodiscard]] bool stillWaiting() const
            {
                return !m_isStopping.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < m_deadline;
            }
            [[nodiscard]] std::string readExactly(const std::size_t count)
            {
                std::string accumulated;
                while (accumulated.size() < count && stillWaiting() && !m_isPeerClosed)
                {
                    accumulated.append(readSome());
                }
                return accumulated;
            }
            /// 一次带时限的读取；空串表示这一拍没有字节（超时或对端收口）
            [[nodiscard]] std::string readSome()
            {
                fd_set readSet{};
                FD_ZERO(&readSet);
                FD_SET(m_socket, &readSet);
                timeval pollInterval{0, 50 * 1000};
                if (::select(m_socket + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                {
                    return {};
                }
                std::array<char, 4096> chunk{};
                const ssize_t          received = ::recv(m_socket, chunk.data(), static_cast<int>(chunk.size()), 0);
                if (received == 0)
                {
                    m_isPeerClosed = true; // 对端正常收口：脚本该立刻收摊，而不是把上限等满
                    return {};
                }
                if (received < 0)
                {
                    m_isPeerClosed = true;
                    return {};
                }
                return std::string(chunk.data(), static_cast<std::size_t>(received));
            }
            int                                   m_socket;              ///< 已接受的连接
            std::string                          &m_headSink;            ///< 请求原文的落点
            std::string                          &m_frameSink;           ///< 请求之后的帧字节落点
            std::mutex                           &m_sinkMutex;           ///< 保护两份落点
            const std::atomic<bool>              &m_isStopping;          ///< 收摊标志
            std::chrono::steady_clock::time_point m_deadline;            ///< 本条连接的总上限
            bool                                  m_isPeerClosed{false}; ///< 对端是否已经收口或被复位
        };
        /**
         * @brief 只在 127.0.0.1 随机端口上说话的手写 WebSocket 对端
         * @details 工作线程接受一条连接后跑脚本；析构置收摊标志并 join，所以脚本必须能自己收口
         */
        class ScriptedWsPeer
        {
        public:
            explicit ScriptedWsPeer(PeerScript script) : m_script(std::move(script))
            {
            }
            ScriptedWsPeer(const ScriptedWsPeer &)            = delete;
            ScriptedWsPeer &operator=(const ScriptedWsPeer &) = delete;
            ~ScriptedWsPeer()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }
            /// 起工作线程并等它把端口报回来
            bool start()
            {
                m_worker = std::thread([this] { run(); });
                return waitFor([this] { return m_port.load(std::memory_order_acquire) != 0U; });
            }
            /// 脚本已经跑完了吗（等到上限为止）
            bool finished()
            {
                return waitFor([this] { return m_isFinished.load(std::memory_order_acquire); });
            }
            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }
            [[nodiscard]] std::string headBytes() const
            {
                const std::lock_guard guard(m_sinkMutex);
                return m_headBytes;
            }
            [[nodiscard]] std::string frameBytes() const
            {
                const std::lock_guard guard(m_sinkMutex);
                return m_frameBytes;
            }

        private:
            static bool waitFor(const std::function<bool()> &condition)
            {
                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (!condition() && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return condition();
            }
            void run()
            {
                const Platform::Socket::Initialization network;
                int                                    listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(listener))
                {
                    m_isFinished.store(true, std::memory_order_release);
                    return;
                }
                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = 0;
                if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    m_isFinished.store(true, std::memory_order_release);
                    return;
                }
                sockaddr_in bound{};
                // 长度参数的类型两家不同（Winsock 是 int*，POSIX 是 socklen_t*），按仓里既有写法统一用 socklen_t
                socklen_t boundLength = static_cast<socklen_t>(sizeof(bound));
                if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &boundLength) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    m_isFinished.store(true, std::memory_order_release);
                    return;
                }
                m_port.store(ntohs(bound.sin_port), std::memory_order_release);
                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(listener, &readSet);
                    timeval pollInterval{0, 50 * 1000};
                    if (::select(listener + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }
                    sockaddr_in peer{};
                    socklen_t   peerLength = static_cast<socklen_t>(sizeof(peer));
                    const int   client     = static_cast<int>(::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peerLength));
                    if (!Platform::FileDescriptor::isValid(client))
                    {
                        continue;
                    }
                    PeerWire wire(client, m_headBytes, m_frameBytes, m_sinkMutex, m_isStopping);
                    m_script(wire);
                    Platform::FileDescriptor::close(client);
                    m_isFinished.store(true, std::memory_order_release);
                    return;
                }
                Platform::FileDescriptor::close(listener);
            }
            PeerScript                 m_script;            ///< 这条连接上要演的戏
            std::thread                m_worker;            ///< 接受与脚本线程
            std::atomic<bool>          m_isStopping{false}; ///< 收摊标志
            std::atomic<bool>          m_isFinished{false}; ///< 脚本是否已跑完
            std::atomic<std::uint16_t> m_port{0};           ///< 实际监听端口，0 表示还没起来
            mutable std::mutex         m_sinkMutex;         ///< 保护下面两份线上读数
            std::string                m_headBytes;         ///< 读到的升级请求原文
            std::string                m_frameBytes;        ///< 请求之后读到的帧字节
        };
        // ---------------------------------------------------------------
        // 按 RFC 自己拼与拆线上字节
        // ---------------------------------------------------------------
        /// 服务端 → 客户端的帧：一律不带掩码（RFC 6455 §5.1），长度按 7 / 16 位两档编码
        [[nodiscard]] std::string makeServerFrame(const std::uint8_t firstByte, const std::string_view payload)
        {
            std::string frame;
            frame.push_back(static_cast<char>(firstByte));
            if (payload.size() < 126)
            {
                frame.push_back(static_cast<char>(payload.size()));
            } else
            {
                frame.push_back(static_cast<char>(126));
                frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFFU));
                frame.push_back(static_cast<char>(payload.size() & 0xFFU));
            }
            frame.append(payload);
            return frame;
        }
        /// 一条合法的 101 应答：三条必发头部 + Accept + 结束空行；额外头部由用例追加
        [[nodiscard]] std::string makeUpgradeResponse(const std::string_view clientKey, const std::string_view extraHeaders = {})
        {
            std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n";
            response += "Sec-WebSocket-Accept: " + computeWebSocketAcceptValue(clientKey) + "\r\n";
            response.append(extraHeaders);
            response += "\r\n";
            return response;
        }
        /// 把状态码拼成 Close 负载的两字节大端前缀（本文件自己的实现，不用被测的那份）
        [[nodiscard]] std::string makeClosePayload(const std::uint16_t statusCode, const std::string_view reason)
        {
            std::string payload;
            payload.push_back(static_cast<char>((statusCode >> 8U) & 0xFFU));
            payload.push_back(static_cast<char>(statusCode & 0xFFU));
            payload.append(reason);
            return payload;
        }
        /// 带掩码的一帧（用来造「服务端不该这么发」的线上形状）
        [[nodiscard]] std::string makeMaskedServerFrame(const std::uint8_t firstByte, const std::string_view payload, const std::array<std::uint8_t, 4> &maskKey)
        {
            std::string frame;
            frame.push_back(static_cast<char>(firstByte));
            frame.push_back(static_cast<char>(0x80U | payload.size()));
            for (const std::uint8_t keyByte: maskKey)
            {
                frame.push_back(static_cast<char>(keyByte));
            }
            for (std::size_t byteIndex = 0; byteIndex < payload.size(); ++byteIndex)
            {
                frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[byteIndex]) ^ maskKey[byteIndex % 4]));
            }
            return frame;
        }
        /// 从请求原文里取出 Sec-WebSocket-Key 的取值（按行自己找，不借 HTTP 解析器）
        [[nodiscard]] std::string extractKeyFrom(const std::string_view requestText)
        {
            constexpr std::string_view kKeyName = "Sec-WebSocket-Key: ";
            const std::size_t          start    = requestText.find(kKeyName);
            if (start == std::string_view::npos)
            {
                return {};
            }
            const std::size_t valueStart = start + kKeyName.size();
            const std::size_t valueEnd   = requestText.find("\r\n", valueStart);
            return std::string(requestText.substr(valueStart, valueEnd - valueStart));
        }
        /**
         * @brief 从帧读数里解出第 index 帧（本文件独立实现的最小解析）
         * @param frameBytes 101 之后读到的全部字节
         * @param index 第几帧，从 0 起
         * @return std::optional<WireFrame> 该帧没凑齐或不存在时为空
         */
        [[nodiscard]] std::optional<WireFrame> frameAt(const std::string_view frameBytes, const std::size_t index)
        {
            std::size_t offset = 0;
            for (std::size_t seen = 0; seen <= index; ++seen)
            {
                if (offset + 2 > frameBytes.size())
                {
                    return std::nullopt;
                }
                WireFrame frame;
                frame.firstByte           = static_cast<std::uint8_t>(frameBytes[offset]);
                const auto secondByte     = static_cast<std::uint8_t>(frameBytes[offset + 1]);
                frame.isMasked            = (secondByte & 0x80U) != 0U;
                std::size_t payloadLength = secondByte & 0x7FU;
                offset += 2;
                if (payloadLength == 126)
                {
                    if (offset + 2 > frameBytes.size())
                    {
                        return std::nullopt;
                    }
                    payloadLength = (static_cast<std::size_t>(static_cast<unsigned char>(frameBytes[offset])) << 8) |
                                    static_cast<std::size_t>(static_cast<unsigned char>(frameBytes[offset + 1]));
                    offset += 2;
                }
                if (frame.isMasked)
                {
                    if (offset + 4 > frameBytes.size())
                    {
                        return std::nullopt;
                    }
                    std::copy_n(frameBytes.begin() + static_cast<std::ptrdiff_t>(offset), 4, frame.maskKey.begin());
                    offset += 4;
                }
                if (offset + payloadLength > frameBytes.size())
                {
                    return std::nullopt;
                }
                frame.payload.assign(frameBytes.substr(offset, payloadLength));
                if (frame.isMasked)
                {
                    // 掩码按 4 字节循环异或（§5.3）：负载第 i 字节配键的第 i%4 位
                    for (std::size_t byteIndex = 0; byteIndex < frame.payload.size(); ++byteIndex)
                    {
                        frame.payload[byteIndex] = static_cast<char>(static_cast<unsigned char>(frame.payload[byteIndex]) ^ frame.maskKey[byteIndex % 4]);
                    }
                }
                offset += payloadLength;
                if (seen == index)
                {
                    return frame;
                }
            }
            return std::nullopt;
        }
        /// 从 Close 帧的负载里取状态码（不足 2 字节表示没给码）
        [[nodiscard]] std::optional<std::uint16_t> closeCodeOf(const WireFrame &frame)
        {
            if (frame.payload.size() < 2)
            {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<unsigned char>(frame.payload[0])) << 8U) |
                                              static_cast<std::uint16_t>(static_cast<unsigned char>(frame.payload[1])));
        }
        /// 一条最简的握手参数：只给路径，其余取默认
        [[nodiscard]] WebSocketClient::Configuration baseConfiguration()
        {
            WebSocketClient::Configuration configuration;
            configuration.requestTarget = std::string(kRequestTarget);
            return configuration;
        }
        /// 什么都不做的动作（只用来断握手本身）
        const Action kNoAction = [](WebSocketClient &, ClientRunOutcome &) -> Core::Task<void> { co_return; };
        /**
         * @brief 在循环线程上把 connect → action → 收口串起来跑完
         * @details 协程帧由调用方拿着并活到 loop.run() 返回之后：中途任何异常都先落进结论，
         *          不让它穿到线程入口（那会连着把整个测试进程带走）
         */
        Core::Task<void> driveClient(Core::EventLoop &loop, const WebSocketClient::Configuration &configuration, const Action &action, ClientRunOutcome &outcome)
        {
            auto connected = co_await WebSocketClient::connect(loop, configuration);
            if (!connected.has_value())
            {
                outcome.failure_reason = connected.error();
                loop.stop();
                co_return;
            }
            outcome.is_connected         = true;
            outcome.accepted_subprotocol = connected.value()->agreement().acceptedSubprotocol;
            co_await action(*connected.value(), outcome);
            outcome.is_open_after_actions = connected.value()->isOpen();
            // 会话在这里销毁：析构把通路关掉，对端脚本因此能收口
            connected.value().reset();
            loop.stop();
        }
        /**
         * @brief 在脚本的陪同下跑一次客户端
         * @param peerScript 对端要演的戏（在工作线程上执行）
         * @param action 会话建好后要做的动作（在循环线程上执行）
         * @param configuration 握手参数；主机与端口由本函数覆盖
         * @param wireBytes 出参：对端读到的两份线上字节（请求原文与之后的帧）
         * @return ClientRunOutcome 结论读数
         */
        ClientRunOutcome runClientAgainst(const PeerScript &peerScript, const Action &action, WebSocketClient::Configuration configuration,
                                          std::pair<std::string, std::string> &wireBytes)
        {
            ScriptedWsPeer peer(peerScript);
            if (!peer.start())
            {
                ADD_FAILURE() << "对端没能在回环上监听";
                return {};
            }
            configuration.hostName = "127.0.0.1";
            configuration.port     = peer.port();
            Core::EventLoop  loop;
            ClientRunOutcome outcome;
            auto             work = driveClient(loop, configuration, action, outcome);
            if (!work.isReady())
            {
                loop.scheduler().schedule(work.handle());
            }
            loop.run();
            // 会话已经收手：等脚本跑完，之后那两份线上读数才是完整的
            static_cast<void>(peer.finished());
            wireBytes = {peer.headBytes(), peer.frameBytes()};
            return outcome;
        }
        /// 断一次失败：拒因里必须点名给的那一段
        void expectFailure(const ClientRunOutcome &outcome, std::string_view reasonFragment)
        {
            EXPECT_FALSE(outcome.is_connected);
            EXPECT_NE(outcome.failure_reason.find(reasonFragment), std::string::npos) << "实际拒因：" << outcome.failure_reason;
        }
    } // namespace
    TEST(WebSocketClient, CompletesHandshakeAndDeliversUnmaskedText)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            static_cast<void>(wire.write(makeServerFrame(kTextFrameFirstByte, "hi")));
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (received.has_value())
            {
                outcome.delivered.push_back(*received);
            } else
            {
                outcome.receive_error = received.error();
            }
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        EXPECT_EQ(outcome.receive_error, "");
        ASSERT_EQ(outcome.delivered.size(), 1U);
        EXPECT_EQ(outcome.delivered.front().opCode, WebSocketOpCode::Text);
        EXPECT_EQ(outcome.delivered.front().payload, "hi");
        EXPECT_TRUE(outcome.delivered.front().isFinal);
        EXPECT_TRUE(outcome.is_open_after_actions);
        // 请求原文里的五条必发头部（RFC 6455 §4.1）：本端没提扩展与子协议，就不该有那两条出去
        EXPECT_NE(wireBytes.first.find("GET /ws HTTP/1.1\r\n"), std::string::npos) << wireBytes.first;
        // 端口是非默认的（回环上的随机端口），权威标识必须把它带上（RFC 9110 §4.2.2）
        EXPECT_NE(wireBytes.first.find("Host: 127.0.0.1:"), std::string::npos) << wireBytes.first;
        EXPECT_NE(wireBytes.first.find("Upgrade: websocket\r\n"), std::string::npos);
        EXPECT_NE(wireBytes.first.find("Connection: Upgrade\r\n"), std::string::npos);
        EXPECT_NE(wireBytes.first.find("Sec-WebSocket-Key: "), std::string::npos);
        EXPECT_NE(wireBytes.first.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
        EXPECT_EQ(wireBytes.first.find("Sec-WebSocket-Extensions"), std::string::npos);
        EXPECT_EQ(wireBytes.first.find("Sec-WebSocket-Protocol"), std::string::npos);
    }
    TEST(WebSocketClient, EveryOutboundFrameIsMaskedWithItsOwnKey)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            wire.readOneClientFrame();
            wire.readOneClientFrame();
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            outcome.send_results.push_back(co_await client.sendText("alpha"));
            outcome.send_results.push_back(co_await client.sendBinary(std::string{"bravo", 6}));
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        ASSERT_EQ(outcome.send_results, (std::vector<bool>{true, true}));
        const auto first  = frameAt(wireBytes.second, 0);
        const auto second = frameAt(wireBytes.second, 1);
        ASSERT_TRUE(first.has_value()) << "第一帧没在线上凑齐：" << wireBytes.second.size() << " 字节";
        ASSERT_TRUE(second.has_value()) << "第二帧没在线上凑齐：" << wireBytes.second.size() << " 字节";
        EXPECT_EQ(first->firstByte, kTextFrameFirstByte);
        EXPECT_EQ(second->firstByte, kBinaryFrameFirstByte);
        EXPECT_TRUE(first->isMasked) << "客户端发出的帧没置 MASK 位（RFC 6455 §5.1）";
        EXPECT_TRUE(second->isMasked);
        EXPECT_EQ(first->payload, "alpha");
        EXPECT_EQ(second->payload, (std::string{"bravo", 6})) << "负载里的 NUL 被截断了：本层要按「指针 + 长度」取";
        EXPECT_NE(first->maskKey, second->maskKey) << "两帧用了同一把掩码键：§5.3 的抗重放就没了";
        EXPECT_NE(first->maskKey, kZeroMaskKey) << "全零键等于没掩码";
        EXPECT_NE(second->maskKey, kZeroMaskKey);
    }
    TEST(WebSocketClient, CarriesBytesAfterTheUpgradeHeadIntoFirstMessage)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            // 升级应答与第一条消息一次写出：解析器只该消化到头块结束，剩下的必须交给帧解码器
            const std::string request = wire.readUpgradeRequest();
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(request)) + makeServerFrame(kTextFrameFirstByte, "tail")));
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (received.has_value())
            {
                outcome.delivered.push_back(*received);
            } else
            {
                outcome.receive_error = received.error();
            }
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        EXPECT_EQ(outcome.receive_error, "");
        ASSERT_EQ(outcome.delivered.size(), 1U) << "应答之后的那半截没接上：整条消息被截了";
        EXPECT_EQ(outcome.delivered.front().payload, "tail");
    }
    TEST(WebSocketClient, AnswersPeerPingWithPongOfSamePayload)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            static_cast<void>(wire.write(makeServerFrame(kPingFrameFirstByte, "hb")));
            static_cast<void>(wire.write(makeServerFrame(kTextFrameFirstByte, "after")));
            wire.readOneClientFrame();
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (received.has_value())
            {
                outcome.delivered.push_back(*received);
            }
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        ASSERT_EQ(outcome.delivered.size(), 1U) << "Ping 不该交给业务";
        EXPECT_EQ(outcome.delivered.front().payload, "after");
        const auto pong = frameAt(wireBytes.second, 0);
        ASSERT_TRUE(pong.has_value()) << "没看见本端回出去的 Pong";
        EXPECT_EQ(pong->firstByte, kPongFrameFirstByte);
        EXPECT_EQ(pong->payload, "hb") << "Pong 要把 Ping 的负载原样带回去（RFC 6455 §5.5.3）";
        EXPECT_TRUE(pong->isMasked);
    }
    TEST(WebSocketClient, ConsumesPeerPongWithoutDeliveringIt)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            static_cast<void>(wire.write(makeServerFrame(kPongFrameFirstByte, "stale")));
            static_cast<void>(wire.write(makeServerFrame(kTextFrameFirstByte, "real")));
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (received.has_value())
            {
                outcome.delivered.push_back(*received);
            }
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        ASSERT_EQ(outcome.delivered.size(), 1U);
        EXPECT_EQ(outcome.delivered.front().opCode, WebSocketOpCode::Text);
        EXPECT_EQ(outcome.delivered.front().payload, "real") << "Pong 被当成消息交给了业务";
    }
    TEST(WebSocketClient, EchoesPeerCloseCodeAndMarksSessionClosed)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            static_cast<void>(wire.write(makeServerFrame(kCloseFrameFirstByte, makeClosePayload(1001, "bye"))));
            wire.readOneClientFrame();
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (received.has_value())
            {
                outcome.delivered.push_back(*received);
            } else
            {
                outcome.receive_error = received.error();
            }
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        EXPECT_EQ(outcome.receive_error, "");
        ASSERT_EQ(outcome.delivered.size(), 1U);
        EXPECT_EQ(outcome.delivered.front().opCode, WebSocketOpCode::Close);
        EXPECT_EQ(outcome.delivered.front().payload, makeClosePayload(1001, "bye")) << "对端给的码与原因要原样交给业务";
        EXPECT_FALSE(outcome.is_open_after_actions);
        const auto echo = frameAt(wireBytes.second, 0);
        ASSERT_TRUE(echo.has_value()) << "本端没回 Close";
        EXPECT_EQ(echo->firstByte, kCloseFrameFirstByte);
        ASSERT_TRUE(closeCodeOf(*echo).has_value());
        EXPECT_EQ(*closeCodeOf(*echo), 1001U) << "回帧要把对端那个码原样答回去（RFC 6455 §5.5.1）";
    }
    TEST(WebSocketClient, StopsSendingAfterLocalCloseAndCarriesReason)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            wire.readOneClientFrame();
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            outcome.send_results.push_back(co_await client.close(1000, "done"));
            outcome.send_results.push_back(co_await client.sendText("late"));
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        ASSERT_EQ(outcome.send_results, (std::vector<bool>{true, false})) << "本端发过 Close 之后就该拒绝再发数据帧（RFC 6455 §5.5.1）";
        const auto close = frameAt(wireBytes.second, 0);
        ASSERT_TRUE(close.has_value());
        EXPECT_EQ(close->firstByte, kCloseFrameFirstByte);
        EXPECT_EQ(close->payload, makeClosePayload(1000, "done")) << "关闭原因要跟着上线";
        EXPECT_EQ(wireBytes.second.find("late"), std::string::npos);
    }
    TEST(WebSocketClient, RejectsBadCloseArgumentsBeforeTouchingTheWire)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            wire.readOneClientFrame();
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            // 用法判据都在调用点跑完：Task 是惰性启动的，把判据放进协程体就要等到首次恢复才露出来，
            // 抛出时既不建帧也不留半条帧在线上
            EXPECT_THROW(static_cast<void>(client.close(1005, std::string_view{})), Base::InvalidArgumentException);
            EXPECT_THROW(static_cast<void>(client.close(1000, std::string{"\xFF\xFE", 2})), Base::InvalidArgumentException);
            EXPECT_THROW(static_cast<void>(client.close(1000, std::string(200, 'x'))), Base::InvalidArgumentException);
            EXPECT_THROW(static_cast<void>(client.sendPing(std::string(200, 'p'))), Base::InvalidArgumentException);
            outcome.send_results.push_back(co_await client.close(1000, "ok"));
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        ASSERT_EQ(outcome.send_results, (std::vector<bool>{true}));
        // 线上只该看见最后那一条合法的 Close：被拒的三次一个字节都没出去
        const auto close = frameAt(wireBytes.second, 0);
        ASSERT_TRUE(close.has_value()) << "合法那条 Close 没上线：" << wireBytes.second;
        EXPECT_EQ(close->payload, makeClosePayload(1000, "ok"));
        EXPECT_FALSE(frameAt(wireBytes.second, 1).has_value()) << "被拒的用法错误也发了帧";
        EXPECT_FALSE(outcome.is_open_after_actions) << "合法收口之后会话就不该再对开";
    }
    TEST(WebSocketClient, FailsWhenResponseIsNotSwitchingProtocols)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.readUpgradeRequest());
            static_cast<void>(wire.write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"));
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, kNoAction, baseConfiguration(), wireBytes);
        expectFailure(outcome, "没回 101");
        EXPECT_NE(outcome.failure_reason.find("200"), std::string::npos);
    }
    TEST(WebSocketClient, FailsWhenServerFramesAreMasked)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            // 服务端发来的帧带掩码：这个方向上 §5.1 明写必须不带
            static_cast<void>(wire.write(makeMaskedServerFrame(kTextFrameFirstByte, "abc", std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44})));
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (!received.has_value())
            {
                outcome.receive_error = received.error();
            } else
            {
                outcome.delivered.push_back(*received);
            }
            co_return;
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, baseConfiguration(), wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        EXPECT_EQ(outcome.delivered.size(), 0U);
        EXPECT_NE(outcome.receive_error.find("掩码"), std::string::npos) << outcome.receive_error;
        EXPECT_FALSE(outcome.is_open_after_actions);
    }
    TEST(WebSocketClient, FailsWhenPeerSelectsUnofferedSubprotocol)
    {
        const PeerScript script = [](PeerWire &wire)
        { static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()), "Sec-WebSocket-Protocol: chat\r\n"))); };
        auto configuration         = baseConfiguration();
        configuration.subprotocols = {"wamp"};
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, kNoAction, configuration, wireBytes);
        expectFailure(outcome, "不在本端提议的名单里");
        EXPECT_NE(outcome.failure_reason.find("chat"), std::string::npos);
        // 要约里只有 wamp：线上就该原样带出去
        EXPECT_NE(wireBytes.first.find("Sec-WebSocket-Protocol: wamp\r\n"), std::string::npos) << wireBytes.first;
    }
    TEST(WebSocketClient, AcceptsSubprotocolThePeerChoseFromTheOffer)
    {
        const PeerScript script = [](PeerWire &wire)
        { static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()), "Sec-WebSocket-Protocol: chat\r\n"))); };
        auto configuration         = baseConfiguration();
        configuration.subprotocols = {"chat", "wamp"};
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, kNoAction, configuration, wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        EXPECT_EQ(outcome.accepted_subprotocol, "chat");
        EXPECT_NE(wireBytes.first.find("Sec-WebSocket-Protocol: chat, wamp\r\n"), std::string::npos) << "要约要按给出的顺序上线：" << wireBytes.first;
    }
    TEST(WebSocketClient, RejectsMessageOverConfiguredLimitWith1009)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.write(makeUpgradeResponse(extractKeyFrom(wire.readUpgradeRequest()))));
            static_cast<void>(wire.write(makeServerFrame(kTextFrameFirstByte, std::string(24, 'x'))));
            wire.readOneClientFrame();
        };
        const Action action = [](WebSocketClient &client, ClientRunOutcome &outcome) -> Core::Task<void>
        {
            const auto received = co_await client.receive();
            if (!received.has_value())
            {
                outcome.receive_error = received.error();
            }
            co_return;
        };
        auto configuration               = baseConfiguration();
        configuration.maximumMessageSize = 8;
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, action, configuration, wireBytes);
        ASSERT_TRUE(outcome.is_connected) << outcome.failure_reason;
        EXPECT_NE(outcome.receive_error.find("上限"), std::string::npos) << outcome.receive_error;
        EXPECT_FALSE(outcome.is_open_after_actions);
        const auto close = frameAt(wireBytes.second, 0);
        ASSERT_TRUE(close.has_value()) << "越限之后没按协议出声";
        ASSERT_TRUE(closeCodeOf(*close).has_value());
        EXPECT_EQ(*closeCodeOf(*close), 1009U) << "消息过大要按 1009 收口（RFC 6455 §7.4.1）";
    }
    TEST(WebSocketClient, FailsWhenPeerClosesBeforeTheUpgradeResponse)
    {
        const PeerScript script = [](PeerWire &wire)
        {
            static_cast<void>(wire.readUpgradeRequest());
            // 读完请求就收摊：本端连一个字节都没等到，必须报「对端先关了」而不是永等
        };
        std::pair<std::string, std::string> wireBytes;
        const ClientRunOutcome              outcome = runClientAgainst(script, kNoAction, baseConfiguration(), wireBytes);
        EXPECT_FALSE(outcome.is_connected);
        EXPECT_NE(outcome.failure_reason.find("之前就把连接关了"), std::string::npos) << outcome.failure_reason;
    }
} // namespace AsynGyanis::Net
