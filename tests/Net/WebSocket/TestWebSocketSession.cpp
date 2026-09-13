// TestWebSocketSession.cpp —— WebSocket 升级与会话循环（RFC 6455 §4/§5）的真实回环端到端测试
//
// 客户端一律手写字节：升级请求、掩码帧与期望的服务端帧都在本文件里按协议拼出，
// 不引入任何 ws 客户端库，因此断言不会被被测实现「自证」。
// 凡是要断言「解到连接关闭」的用例，都先等客户端读到 EOF，再做完整比对；渐进性断言只用子串。

#include "HttpTestSupport.h"

#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Net/WebSocket/WebSocketFrame.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using HttpTestSupport::LoopbackClient;
        using HttpTestSupport::RunningHttpServerFixture;

        /// 一般等待上限：回环上的握手与一次收发都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{3000};

        /// 空闲清扫节拍：与 HTTP 用例同档，保证超时收口类断言不会久等
        constexpr std::chrono::milliseconds kSweepInterval{50};

        /// 升级路由的路径
        constexpr std::string_view kHandshakePath = "/ws";

        /// RFC 6455 §1.3 给出的黄金样本：客户端 Sec-WebSocket-Key
        constexpr std::string_view kRfcClientKey = "dGhlIHNhbXBsZSBub25jZQ==";

        /// 与上面那枚 key 配对的黄金 Accept 值（RFC 6455 §1.3 原文给出，独立于被测实现）
        constexpr std::string_view kRfcAcceptValue = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

        /// 客户端掩码键：用例固定用它算黄金字节
        constexpr std::array<std::uint8_t, 4> kClientMaskKey{0x37, 0xFA, 0x21, 0x3D};

        /// 手写黄金字节：带掩码的 Text 帧 "hello"
        /// 布局 0x81(FIN=1,Text) 0x85(MASK=1,长度 5) 掩码键 0x37FA213D + 逐字节异或后的负载
        constexpr std::string_view kMaskedHelloFrame = "\x81\x85\x37\xFA\x21\x3D\x5F\x9F\x4D\x51\x58";

        /**
         * @brief 组装一条标准的升级请求（RFC 6455 §4.1 六条要求全部满足）
         * @return std::string 完整请求报文
         */
        std::string upgradeRequestText()
        {
            std::string request;
            request.append("GET /ws HTTP/1.1\r\n");
            request.append("host: test\r\n");
            request.append("upgrade: websocket\r\n");
            request.append("connection: Upgrade\r\n");
            request.append("sec-websocket-key: ");
            request.append(kRfcClientKey);
            request.append("\r\n");
            request.append("sec-websocket-version: 13\r\n\r\n");
            return request;
        }

        /**
         * @brief 期望的 101 报文：Accept 值取自 RFC 6455 §1.3 原文
         * @return std::string 逐字节期望的应答
         */
        std::string expectedHandshakeResponseText()
        {
            std::string response;
            response.append("HTTP/1.1 101 Switching Protocols\r\n");
            response.append("Upgrade: websocket\r\n");
            response.append("Connection: Upgrade\r\n");
            response.append("Sec-WebSocket-Accept: ");
            response.append(kRfcAcceptValue);
            response.append("\r\n\r\n");
            return response;
        }

        /**
         * @brief 手写一个客户端帧：置 MASK 位并按 4 字节掩码键逐字节异或（RFC 6455 §5.3）
         * @param opCodeValue 操作码原始取值（0x1 Text、0x0 Continuation、0x8 Close、0x9 Ping）
         * @param payload 负载
         * @param isFinal 是否消息末帧
         * @return std::string 线上字节
         * @note 长度一律用 7 位档：用例负载都短于 126 字节。本函数有意不复用被测编码器，
         *       否则编码器出错时服务端与客户端会一起错，测试就失去判据
         */
        std::string maskedClientFrame(const std::uint8_t opCodeValue, const std::string_view payload, const bool isFinal = true)
        {
            std::string frame;
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(opCodeValue | (isFinal ? 0x80U : 0x00U))));
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(0x80U | payload.size())));
            for (const std::uint8_t maskByte: kClientMaskKey)
            {
                frame.push_back(static_cast<char>(maskByte));
            }
            for (std::size_t index = 0; index < payload.size(); ++index)
            {
                const auto payloadByte = static_cast<std::uint8_t>(static_cast<unsigned char>(payload[index]));
                frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payloadByte ^ kClientMaskKey[index % kClientMaskKey.size()])));
            }
            return frame;
        }

        /**
         * @brief 拼一个服务端方向的帧（不带掩码）
         * @param opCodeValue 操作码原始取值
         * @param payload 负载
         * @return std::string 期望字节
         * @note 服务端发出的帧一律不置掩码位（RFC 6455 §5.1），长度同样只用 7 位档
         */
        std::string serverFrameBytes(const std::uint8_t opCodeValue, const std::string_view payload)
        {
            std::string frame;
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(opCodeValue | 0x80U)));
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload.size())));
            frame.append(payload);
            return frame;
        }

        /**
         * @brief 轮询读到累计字节数达到期望长度
         * @param client 客户端
         * @param accumulated 输入输出：累计读到的字节
         * @param expectedLength 期望的累计长度
         * @param timeout 等待上限
         * @return true 在时限内凑齐（对端提前关闭时按已读到的字节数判定）
         */
        bool readUntilLength(const LoopbackClient &client, std::string &accumulated, const std::size_t expectedLength,
                             const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (accumulated.size() < expectedLength)
            {
                const HttpTestSupport::ReadOutcome outcome = client.readOnce(accumulated);
                if (outcome == HttpTestSupport::ReadOutcome::PeerClosed || outcome == HttpTestSupport::ReadOutcome::Broken)
                {
                    return accumulated.size() >= expectedLength;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /**
         * @brief 业务处理器收到的消息记录槽
         * @details 处理器跑在事件循环线程上，用例在测试线程上核对，因此用互斥锁保护。
         */
        struct MessageRecord
        {
            std::mutex mutex;                        ///< 保护 messages
            std::vector<WebSocketMessage> messages;  ///< 按到达顺序记录的数据消息

            /**
             * @brief 记录一条消息
             * @param message 收到的消息
             */
            void append(const WebSocketMessage &message)
            {
                const std::lock_guard<std::mutex> guard(mutex);
                messages.push_back(message);
            }

            /// 已记录的消息条数
            [[nodiscard]] std::size_t count()
            {
                const std::lock_guard<std::mutex> guard(mutex);
                return messages.size();
            }

            /// 记录的消息副本
            [[nodiscard]] std::vector<WebSocketMessage> snapshot()
            {
                const std::lock_guard<std::mutex> guard(mutex);
                return messages;
            }
        };

        /**
         * @brief 回显业务：每条数据消息原样发回，并把收到的内容记进记录槽
         * @param record 记录槽
         * @param peer 对端对象
         * @return Core::Task<> 收到空结果（连接收口）时返回
         */
        Core::Task<> echoHandler(const std::shared_ptr<MessageRecord> record, WebSocketPeer &peer)
        {
            while (const std::optional<WebSocketMessage> message = co_await peer.receive())
            {
                record->append(*message);
                if (message->opCode == WebSocketOpCode::Text)
                {
                    [[maybe_unused]] const bool isEchoSent = co_await peer.sendText(message->payload);
                } else
                {
                    [[maybe_unused]] const bool isEchoSent = co_await peer.sendBinary(message->payload);
                }
            }
            co_return;
        }

        /**
         * @brief 起一台注册了 /ws 升级路由的服务器
         * @param record 回显业务的消息记录槽
         * @return std::unique_ptr<RunningHttpServerFixture> 已投递 start() 的服务器夹具
         * @note 路由用 any() 注册：拒绝面用例（非 GET、缺 Upgrade 等）也必须能走到这条路由上，
         *       否则测到的是「路由未命中」而不是「升级校验拒绝了这条请求」
         */
        std::unique_ptr<RunningHttpServerFixture> makeWebSocketServer(const std::shared_ptr<MessageRecord> &record)
        {
            const HttpTestSupport::RouteRegistrar registrar = [record](Router &router, Core::EventLoop &)
            {
                router.any(std::string(kHandshakePath), [record](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.upgradeToWebSocket([record](WebSocketPeer &peer) -> Core::Task<>
                    {
                        co_await echoHandler(record, peer);
                    });
                    co_return;
                });
            };

            return std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, kSweepInterval, HttpTestSupport::SlowRouteOptions{},
                                                              registrar);
        }

        /**
         * @brief 断言升级请求被拒：回 400、响应里带中文原因、并且按 close 收口
         * @param port 服务端端口
         * @param requestText 待发出的请求报文
         * @param expectedReasonFragment 期望出现在响应正文里的原因片段
         * @param record 消息记录槽（拒绝面不应交付任何消息）
         */
        void expectUpgradeRejected(const std::uint16_t port, const std::string &requestText, const std::string_view expectedReasonFragment,
                                   const std::shared_ptr<MessageRecord> &record)
        {
            LoopbackClient client(port);
            ASSERT_TRUE(client.isValid());
            ASSERT_TRUE(client.sendText(requestText, kWaitTimeout));

            std::string accumulated;
            ASSERT_TRUE(client.waitForText(accumulated, "HTTP/1.1 400 ", kWaitTimeout)) << "响应：" << accumulated;

            // 渐进性断言只用子串：状态行与头部行可能分属两个 TCP 段，逐字节比对要等连接关闭
            EXPECT_NE(accumulated.find(expectedReasonFragment), std::string::npos) << "响应：" << accumulated;
            EXPECT_EQ(accumulated.find("Sec-WebSocket-Accept"), std::string::npos) << "被拒的请求不应升级";

            // 拒绝之后按 close 收口：先等对端读到 EOF，再断定这次连接确实结束了
            ASSERT_TRUE(client.waitForClosure(accumulated, kWaitTimeout)) << "被拒的升级请求应随后断开连接";
            EXPECT_EQ(record->count(), 0U) << "升级失败不应把连接交给业务处理器";
        }
    } // namespace

    // ============================================================================
    // 握手：101 与黄金 Accept 值
    // ============================================================================

    /**
     * @brief 钉住完整握手：标准升级请求收到 101，Sec-WebSocket-Accept 等于 RFC 6455 §1.3 的黄金值
     * @details 期望报文用的是规范正文里印出来的那组 key/accept，且 101 必须逐字节等于握手模块的产物——
     *          会话不得给它补 date 或 content-length。
     */
    TEST(WebSocketSession, CompletesHandshakeWithRfcGoldenAcceptValue)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText(), kWaitTimeout));

        const std::string expected = expectedHandshakeResponseText();
        std::string accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, expected.size(), kWaitTimeout)) << "只收到 " << accumulated.size() << " 字节";

        EXPECT_EQ(accumulated.substr(0, expected.size()), expected);
        EXPECT_EQ(accumulated.find("date:"), std::string::npos) << "101 是切换协议的应答，不该被补 date 头";
        EXPECT_EQ(accumulated.find("content-length"), std::string::npos) << "101 没有正文，不该被补 content-length";
    }

    // ============================================================================
    // 收发：回显、分片重组、心跳与关闭握手
    // ============================================================================

    /**
     * @brief 钉住回显：客户端发带掩码的 Text 帧（黄金字节），服务端回同内容且不带掩码的 Text 帧
     */
    TEST(WebSocketSession, EchoesMaskedTextFrameAsUnmaskedText)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        // 先自检手写字节：黄金常量与手写编码函数必须给出同一串（两者独立算出来，任一处算错都会当场暴露）
        ASSERT_EQ(std::string(kMaskedHelloFrame), maskedClientFrame(0x1, "hello"));

        const std::string request = upgradeRequestText();
        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x1, "hello");

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(request + std::string(kMaskedHelloFrame), kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, handshake.size() + expectedEcho.size(), kWaitTimeout));

        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedEcho.size()), expectedEcho);
        EXPECT_EQ(accumulated.size(), handshake.size() + expectedEcho.size()) << "除 101 与一条回显外不应有别的字节";

        const std::vector<WebSocketMessage> messages = record->snapshot();
        ASSERT_EQ(messages.size(), 1U);
        EXPECT_EQ(messages.front().opCode, WebSocketOpCode::Text);
        EXPECT_EQ(messages.front().payload, "hello");
    }

    /**
     * @brief 钉住合法多字节文本不被 UTF-8 校验误伤：三字节与四字节字符原样回显
     * @details 校验必须放行所有合法序列，而不是只放行 ASCII；这里用一个 3 字节字符加一个 4 字节
     *          字符（后者超出 BMP）作为负载，回显逐字节相同才算通过。
     */
    TEST(WebSocketSession, EchoesValidMultibyteUtf8Text)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        // 负载 = 「汉字」U+6C49 U+5B57（各 3 字节）+ U+1F600（4 字节）
        const std::string text = std::string("\xE6\xB1\x89\xE5\xAD\x97", 6) + std::string("\xF0\x9F\x98\x80", 4);
        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x1, text);

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x1, text), kWaitTimeout));

        std::string accumulated;
        const std::size_t expectedLength = handshake.size() + expectedEcho.size();
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedLength, kWaitTimeout)) << "只收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedEcho.size()), expectedEcho);

        const std::vector<WebSocketMessage> messages = record->snapshot();
        ASSERT_EQ(messages.size(), 1U) << "合法多字节文本必须交付业务，而不是被判成非法";
        EXPECT_EQ(messages.front().opCode, WebSocketOpCode::Text);
        EXPECT_EQ(messages.front().payload, text);
    }

    /**
     * @brief 钉住校验只落在文本帧上：二进制帧携带非法 UTF-8 字节也必须原样交付
     * @details RFC 6455 §5.6 的 UTF-8 约束只针对文本消息；把校验张冠李戴到 Binary 上，二进制
     *          协议（图片、protobuf 这类负载）就会因为某个字节像非法序列而被无辜断连。
     */
    TEST(WebSocketSession, DoesNotValidateBinaryPayloadAsUtf8)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        // 0xFF 0xFE 0x80 里 0x80 是孤立续字节，作为文本必然非法，作为二进制负载则完全正常
        const std::string payload = std::string("\xFF\xFE\x80", 3);
        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x2, payload);

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x2, payload), kWaitTimeout));

        std::string accumulated;
        const std::size_t expectedLength = handshake.size() + expectedEcho.size();
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedLength, kWaitTimeout)) << "只收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedEcho.size()), expectedEcho);

        const std::vector<WebSocketMessage> messages = record->snapshot();
        ASSERT_EQ(messages.size(), 1U) << "二进制负载不参与 UTF-8 校验，必须交付业务";
        EXPECT_EQ(messages.front().opCode, WebSocketOpCode::Binary);
        EXPECT_EQ(messages.front().payload, payload);
    }

    /**
     * @brief 钉住分片重组：客户端发两个分片，服务端只交付一条完整消息（重组在解码层完成）
     */
    TEST(WebSocketSession, ReassemblesFragmentedMessageIntoOneDelivery)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        // 分片一：FIN=0 的 Text "Hel"；分片二：FIN=1 的 Continuation "lo"
        const std::string firstFragment = maskedClientFrame(0x1, "Hel", false);
        const std::string secondFragment = maskedClientFrame(0x0, "lo", true);

        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x1, "Hello");

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + firstFragment + secondFragment, kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, handshake.size() + expectedEcho.size(), kWaitTimeout));
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedEcho.size()), expectedEcho);

        // 回显说明业务已经拿到消息：此刻记录槽里必须恰好有一条「Hello」，而不是两条分片
        const std::vector<WebSocketMessage> messages = record->snapshot();
        ASSERT_EQ(messages.size(), 1U) << "分片应重组成一条消息，而不是逐片交给业务";
        EXPECT_EQ(messages.front().opCode, WebSocketOpCode::Text) << "操作码取消息首帧，而不是继续帧";
        EXPECT_EQ(messages.front().payload, "Hello");
    }

    /**
     * @brief 钉住 UTF-8 校验按重组后的整条消息做：被分片切开的字符不得被判成截断序列
     * @details 三字节字符 U+4E16 被切成「E4 B8」+「96」两片，前一片单独看就是截断的序列。
     *          逐分片校验会把它判非法并回 1007，只有按重组后的整条消息（解码层已重组完整、只交付
     *          一条消息）校验才放行——本用例正是这条契约的判据。
     */
    TEST(WebSocketSession, AcceptsUtf8CharacterSplitAcrossFragments)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string firstFragment = maskedClientFrame(0x1, std::string("\xE4\xB8", 2), false);
        const std::string secondFragment = maskedClientFrame(0x0, std::string("\x96", 1), true);

        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x1, std::string("\xE4\xB8\x96", 3));

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + firstFragment + secondFragment, kWaitTimeout));

        std::string accumulated;
        const std::size_t expectedLength = handshake.size() + expectedEcho.size();
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedLength, kWaitTimeout)) << "只收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake) << "被切开的字符不得被判成非法负载而回 1007";
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedEcho.size()), expectedEcho);

        const std::vector<WebSocketMessage> messages = record->snapshot();
        ASSERT_EQ(messages.size(), 1U);
        EXPECT_EQ(messages.front().payload, std::string("\xE4\xB8\x96", 3));
    }

    /**
     * @brief 钉住心跳：收到 Ping 自动回一条负载原样的 Pong，且不把控制帧交给业务
     */
    TEST(WebSocketSession, AnswersPingWithPongEchoingPayload)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedPong = serverFrameBytes(0xA, "ping");

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x9, "ping"), kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, handshake.size() + expectedPong.size(), kWaitTimeout));
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedPong.size()), expectedPong);
        EXPECT_EQ(accumulated.size(), handshake.size() + expectedPong.size()) << "Pong 之后不应有别的字节";
        EXPECT_EQ(record->count(), 0U) << "Ping 是控制帧，不应交付业务";
    }

    /**
     * @brief 钉住关闭握手（RFC 6455 §5.5.1）：收到 Close 回一条同状态码的 Close，随后断开 TCP
     * @details 断言分两步：先等客户端读到 EOF（时序纪律：连接类断言必须等对端真的关闭），
     *          再对累计字节做完整比对——echo 之后服务端不得再补第二条 Close。
     */
    TEST(WebSocketSession, EchoesCloseCodeAndClosesConnection)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        // 关闭帧负载 = 2 字节大端状态码 1000 = 0x03E8
        const std::string expectedClose = serverFrameBytes(0x8, "\x03\xE8");

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x8, "\x03\xE8"), kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(client.waitForClosure(accumulated, kWaitTimeout)) << "关闭握手之后服务端应断开连接";
        ASSERT_GE(accumulated.size(), handshake.size() + expectedClose.size()) << "累计收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedClose.size()), expectedClose);
        EXPECT_EQ(accumulated.size(), handshake.size() + expectedClose.size()) << "状态码 echo 之后不应再补第二条 Close";
        EXPECT_EQ(record->count(), 0U) << "Close 是控制帧，不应交付业务";
    }

    // ============================================================================
    // 读窗口交接与任意字节切分
    // ============================================================================

    /**
     * @brief 钉住升级请求之后那部分字节的交接：请求与两帧写在同一个 TCP 段里也不能丢
     * @details 会话解析完升级请求时，读窗口里可能已经躺着客户端发来的帧。这些字节属于解码器，
     *          漏掉它们就会出现「握手成功但第一条消息永远收不到」——本用例专门钉住这一步。
     */
    TEST(WebSocketSession, KeepsFramesArrivingInTheSameSegmentAsHandshake)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedFirstEcho = serverFrameBytes(0x1, "one");
        const std::string expectedSecondEcho = serverFrameBytes(0x1, "two");

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());

        // 一次 sendall：升级请求 + 两帧，全部挤在同一个 TCP 段里
        const std::string sameSegmentBytes = upgradeRequestText() + maskedClientFrame(0x1, "one") + maskedClientFrame(0x1, "two");
        ASSERT_TRUE(client.sendText(sameSegmentBytes, kWaitTimeout));

        std::string accumulated;
        const std::size_t expectedLength = handshake.size() + expectedFirstEcho.size() + expectedSecondEcho.size();
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedLength, kWaitTimeout)) << "只收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedFirstEcho.size()), expectedFirstEcho);
        EXPECT_EQ(accumulated.substr(handshake.size() + expectedFirstEcho.size(), expectedSecondEcho.size()), expectedSecondEcho);
    }

    /**
     * @brief 钉住任意字节切分：升级请求与若干帧逐字节发送，行为与一次性发送一致
     */
    TEST(WebSocketSession, HandlesHandshakeAndFramesFedByteByByte)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x1, "drip");
        const std::string expectedPong = serverFrameBytes(0xA, "hb");

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());

        // 逐字节喂：解析器与解码器都必须在任意字节边界上续得上
        const std::string bytes = upgradeRequestText() + maskedClientFrame(0x1, "drip") + maskedClientFrame(0x9, "hb");
        for (const char byte: bytes)
        {
            ASSERT_TRUE(client.sendText(std::string_view(&byte, 1), kWaitTimeout));
        }

        std::string accumulated;
        const std::size_t expectedLength = handshake.size() + expectedEcho.size() + expectedPong.size();
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedLength, kWaitTimeout)) << "只收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedEcho.size()), expectedEcho);
        EXPECT_EQ(accumulated.substr(handshake.size() + expectedEcho.size(), expectedPong.size()), expectedPong);
    }

    // ============================================================================
    // 拒绝面与解码错误
    // ============================================================================

    /**
     * @brief 钉住拒绝面：登记了升级但请求不构成合法握手时回 400、给中文原因、不升级也不交付业务
     * @details 四种情况各改一处：非 GET、缺 Upgrade、版本 12、key 解码后不是 16 字节。
     */
    TEST(WebSocketSession, RejectsInvalidUpgradeRequestsWith400)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));
        const std::uint16_t port = server->listeningPort();

        // 非 GET：握手要求 GET（RFC 6455 §4.1 第 1 条）
        const std::string nonGetRequest = "POST /ws HTTP/1.1\r\nhost: test\r\nupgrade: websocket\r\nconnection: Upgrade\r\n"
                                          "sec-websocket-key: " +
                                          std::string(kRfcClientKey) + "\r\nsec-websocket-version: 13\r\n\r\n";
        SCOPED_TRACE("非 GET 方法");
        expectUpgradeRejected(port, nonGetRequest, "GET", record);

        // 缺 Upgrade 头
        const std::string missingUpgradeRequest = "GET /ws HTTP/1.1\r\nhost: test\r\nconnection: Upgrade\r\n"
                                                  "sec-websocket-key: " +
                                                  std::string(kRfcClientKey) + "\r\nsec-websocket-version: 13\r\n\r\n";
        SCOPED_TRACE("缺 Upgrade 头");
        expectUpgradeRejected(port, missingUpgradeRequest, "Upgrade", record);

        // 协议版本 12：本实现只认 13
        const std::string wrongVersionRequest = "GET /ws HTTP/1.1\r\nhost: test\r\nupgrade: websocket\r\nconnection: Upgrade\r\n"
                                                "sec-websocket-key: " +
                                                std::string(kRfcClientKey) + "\r\nsec-websocket-version: 12\r\n\r\n";
        SCOPED_TRACE("协议版本 12");
        expectUpgradeRejected(port, wrongVersionRequest, "13", record);

        // key 是合法 base64 但解码后不是 16 字节（这里 15 字节）
        const std::string shortKeyRequest = "GET /ws HTTP/1.1\r\nhost: test\r\nupgrade: websocket\r\nconnection: Upgrade\r\n"
                                            "sec-websocket-key: MDEyMzQ1Njc4OWFiY2Rl\r\nsec-websocket-version: 13\r\n\r\n";
        SCOPED_TRACE("key 解码后 15 字节");
        expectUpgradeRejected(port, shortKeyRequest, "16", record);
    }

    /**
     * @brief 钉住解码错误的收口：客户端发未掩码帧（RFC 6455 §5.1 要求必须掩码）时按 1002 关闭并断开
     * @details 先等连接关闭再比对：服务端只应发出 101 与一条 Close(1002)，且非法帧不得交给业务。
     */
    TEST(WebSocketSession, ClosesWithProtocolErrorWhenClientFrameIsUnmasked)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        // 关闭帧负载 = 2 字节大端状态码 1002 = 0x03EA
        const std::string expectedClose = serverFrameBytes(0x8, "\x03\xEA");

        // 未掩码的 Text 帧 "hello"：第二个字节最高位为 0
        const std::string unmasked = std::string("\x81\x05", 2) + "hello";

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + unmasked, kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(client.waitForClosure(accumulated, kWaitTimeout)) << "协议错误后服务端应断开连接";
        ASSERT_GE(accumulated.size(), handshake.size() + expectedClose.size()) << "累计收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedClose.size()), expectedClose);
        EXPECT_EQ(record->count(), 0U) << "非法帧不应交付业务";
    }

    /**
     * @brief 钉住非法文本负载的收口（RFC 6455 §5.6）：负载不是合法 UTF-8 时按 1007 关闭并断开
     * @details 负载取「合法前缀 "ok" + 截断的 3 字节序列」，违规字节不在开头，因此日志里的位置必须
     *          指向它而不是笼统说「不合法」。先等客户端读到 EOF 再完整比对：服务端只应发出 101 与
     *          一条 Close(1007)，非法文本不得交付业务。
     */
    TEST(WebSocketSession, ClosesWithInvalidPayloadDataWhenTextPayloadIsNotUtf8)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        // 关闭帧负载 = 2 字节大端状态码 1007 = 0x03EF
        const std::string expectedClose = serverFrameBytes(0x8, "\x03\xEF");

        // 合法前缀 "ok" 之后是截断的 3 字节序列 E4 B8（三字节字符少最后一个字节）
        const std::string invalidPayload = std::string("ok", 2) + std::string("\xE4\xB8", 2);

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x1, invalidPayload), kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(client.waitForClosure(accumulated, kWaitTimeout)) << "非法 UTF-8 文本帧应导致服务端关闭连接";
        ASSERT_GE(accumulated.size(), handshake.size() + expectedClose.size()) << "累计收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake);
        EXPECT_EQ(accumulated.substr(handshake.size(), expectedClose.size()), expectedClose);
        EXPECT_EQ(accumulated.size(), handshake.size() + expectedClose.size()) << "1007 之后不应再补第二条 Close";
        EXPECT_EQ(record->count(), 0U) << "非法文本帧不应交付业务";
    }
} // namespace AsynGyanis::Net
