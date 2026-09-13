// TestWebSocketSession.cpp —— WebSocket 升级与会话循环（RFC 6455 §4/§5）的真实回环端到端测试
//
// 客户端一律手写字节：升级请求、掩码帧与期望的服务端帧都在本文件里按协议拼出，
// 不引入任何 ws 客户端库，因此断言不会被被测实现「自证」。
// 凡是要断言「解到连接关闭」的用例，都先等客户端读到 EOF，再做完整比对；渐进性断言只用子串。
// 末节的统计用例同一条连接串起升级、消息与协议错误收口，并核对普通 HTTP 请求不污染 WebSocket 计数。
// 发送失败类用例另在根日志器上挂记录型 Sink（HttpTestSupport::LogCapture），
// 断言「一次失败只留一条日志」与「本侧收口的短路返回不记日志」两条口径。

#include "HttpTestSupport.h"

#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/WebSocket/PerMessageDeflate.h"
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

        /// 对端复位后的等待上限：写侧要等内核回 RST 或等写超时清扫，比一般等待更宽松
        constexpr std::chrono::milliseconds kResetWaitTimeout{6000};

        /// 帧写出失败日志的识别片段：只有失败路径会输出它，用它数「同一次失败记了几条」
        constexpr std::string_view kFrameWriteFailureFragment = "帧写出失败";

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
         * @param isCompressed 是否置 RSV1（permessage-deflate 的压缩标记）；用例用它构造压缩消息
         * @return std::string 线上字节
         * @note 长度一律用 7 位档：用例负载都短于 126 字节。本函数有意不复用被测编码器，
         *       否则编码器出错时服务端与客户端会一起错，测试就失去判据
         */
        std::string maskedClientFrame(const std::uint8_t opCodeValue, const std::string_view payload, const bool isFinal = true,
                                      const bool isCompressed = false)
        {
            std::string frame;
            frame.push_back(static_cast<char>(
                    static_cast<std::uint8_t>(opCodeValue | (isFinal ? 0x80U : 0x00U) | (isCompressed ? 0x40U : 0x00U))));
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

        // 关闭的两侧各计各的：对端发起的这次关闭记在对端一侧，本侧那条回帧不得再记成本侧发起
        ASSERT_TRUE(HttpTestSupport::waitForCondition(
                [&server]
                {
                    return server->server().stats().webSocketPeerCloseCount >= 1;
                },
                kWaitTimeout)) << "对端发起的关闭握手没有计数";
        const HttpServerStats closeStats = server->server().stats();
        EXPECT_EQ(closeStats.webSocketPeerCloseCount, 1u) << "一次对端 Close 只该计一次";
        EXPECT_EQ(closeStats.webSocketServerCloseCount, 0u) << "回应对端 Close 的回帧被重复记成了本侧发起关闭";
        EXPECT_EQ(closeStats.webSocketProtocolErrorCloseCount, 0u) << "正常关闭握手不是协议错误收口";
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

    /**
     * @brief 钉住发送失败契约：对端带未读数据关闭（内核回 RST）后，业务的 sendText 必须返回 false
     * @details 契约与 HTTP 流式侧一致：传输层失败（对端关闭/RST/描述符被关）折成布尔结果并记日志，
     *          只有控制帧超长之类的用法错误才抛异常。业务在帧发送失败时置位标记并收手，主线程据此
     *          断言它确实拿到了 false——异常若打穿业务，这个标记永远不会被置位。
     */
    TEST(WebSocketSession, ReturnsFalseWhenPeerResetsDuringSend)
    {
        std::atomic<bool> didBusinessObserveSendFailure{false};
        std::atomic<int>  sentFrameCount{0};

        const HttpTestSupport::RouteRegistrar registrar = [&didBusinessObserveSendFailure, &sentFrameCount](Router &router, Core::EventLoop &)
        {
            router.any(std::string(kHandshakePath),
                       [&didBusinessObserveSendFailure, &sentFrameCount](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.upgradeToWebSocket([&didBusinessObserveSendFailure, &sentFrameCount](WebSocketPeer &peer) -> Core::Task<>
                {
                    // 一直写到对端不再可用为止：对端停读会让发送缓冲填满，业务因此停在等可写上，
                    // RST 到达时正是这次挂起的写把失败交回来
                    const std::string payload(64 * 1024, 'w');
                    for (int round = 0; round < 64; ++round)
                    {
                        if (!co_await peer.sendText(payload))
                        {
                            // 传输层失败以 false 抵达：业务据此收手，并留下可核对的证据
                            didBusinessObserveSendFailure.store(true, std::memory_order_release);
                            co_return;
                        }
                        sentFrameCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    co_return;
                });
                co_return;
            });
        };

        const std::unique_ptr<RunningHttpServerFixture> server = std::make_unique<RunningHttpServerFixture>(
                HttpServerLimits{}, kSweepInterval, HttpTestSupport::SlowRouteOptions{}, registrar);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());

        // 握手与首批帧数据都先读到客户端：接收缓冲里留着一大截未读字节，随后的 close 才会让
        // 内核回 RST（只收到 EOF 的话写侧拿不到错误事件）
        std::string accumulated;
        ASSERT_TRUE(client.sendText(upgradeRequestText(), kWaitTimeout));
        ASSERT_TRUE(client.waitForText(accumulated, "Sec-WebSocket-Accept", kWaitTimeout)) << "握手没有完成：" << accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedHandshakeResponseText().size() + 1024, kWaitTimeout))
                << "握手之后没有读到帧数据，累计 " << accumulated.size() << " 字节";
        client.closeNow();

        EXPECT_TRUE(server->awaitConnectionsDrained(kResetWaitTimeout)) << "对端复位后 WebSocket 会话没有收口";
        EXPECT_TRUE(didBusinessObserveSendFailure.load(std::memory_order_acquire))
                << "对端已复位，业务的 sendText 没有返回 false：传输层失败以异常打穿了业务";
        EXPECT_LT(sentFrameCount.load(std::memory_order_relaxed), 64) << "对端已经复位，写侧却宣称 64 帧全部成功";
        EXPECT_FALSE(server->startThrew()) << "对端复位把服务器主协程带崩了";
    }

    /**
     * @brief 钉住 WebSocket 侧的失败日志口径：一次传输失败只留一条日志，收口之后的发送不再新增日志
     * @details 与 HTTP 流式侧同一条契约（见 HttpResponse::writeChunk）：false = 本帧未发出、连接不可
     *          再用、调用方应停止发送，两种来源——本侧已收口（不新增日志）与传输失败（本次记一条）。
     *          对端带未读数据关闭让内核回 RST，构造真实的传输失败；失败后再发数据帧与 Close 都被
     *          短路返回 false，这两次都不得再记日志，因此整条断开的连接只应留下一条日志。
     */
    TEST(WebSocketSession, LogsExactlyOneEntryPerFailedFrameWriteAndStaysSilentAfterwards)
    {
        const HttpTestSupport::LogCapture logCapture;

        std::atomic<bool> didBusinessObserveSendFailure{false};
        std::atomic<bool> didRetryReturnFalse{false};
        std::atomic<bool> isPeerClosedAfterFailure{false};
        std::atomic<int>  logCountAtFailure{-1};
        std::atomic<int>  logCountAfterRetry{-1};

        const HttpTestSupport::RouteRegistrar registrar =
                [&logCapture, &didBusinessObserveSendFailure, &didRetryReturnFalse, &isPeerClosedAfterFailure, &logCountAtFailure,
                 &logCountAfterRetry](Router &router, Core::EventLoop &)
        {
            router.any(std::string(kHandshakePath),
                       [&logCapture, &didBusinessObserveSendFailure, &didRetryReturnFalse, &isPeerClosedAfterFailure, &logCountAtFailure,
                        &logCountAfterRetry](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.upgradeToWebSocket(
                        [&logCapture, &didBusinessObserveSendFailure, &didRetryReturnFalse, &isPeerClosedAfterFailure, &logCountAtFailure,
                         &logCountAfterRetry](WebSocketPeer &peer) -> Core::Task<>
                {
                    // 一直写到对端不再可用为止：对端停读让发送缓冲填满，业务因此停在等可写上，
                    // RST 到达时正是这次挂起的写把失败交回来
                    const std::string payload(64 * 1024, 'w');
                    for (int round = 0; round < 64; ++round)
                    {
                        if (!co_await peer.sendText(payload))
                        {
                            // 失败那一刻：同一次失败只应留下一条日志（异常一条 + 非正值一条就该是 2）
                            logCountAtFailure.store(static_cast<int>(logCapture.countContaining(kFrameWriteFailureFragment)),
                                                    std::memory_order_release);
                            isPeerClosedAfterFailure.store(!peer.isOpen(), std::memory_order_release);
                            didBusinessObserveSendFailure.store(true, std::memory_order_release);

                            // 本侧已收口：数据帧与第二条 Close 都短路返回 false，且都不新增日志
                            const bool isRetryFrameSent = co_await peer.sendText("retry-after-failure");
                            const bool isSecondCloseSent = co_await peer.close();
                            logCountAfterRetry.store(static_cast<int>(logCapture.countContaining(kFrameWriteFailureFragment)),
                                                     std::memory_order_release);
                            didRetryReturnFalse.store(!isRetryFrameSent && !isSecondCloseSent, std::memory_order_release);
                            co_return;
                        }
                    }
                    co_return;
                });
                co_return;
            });
        };

        const std::unique_ptr<RunningHttpServerFixture> server = std::make_unique<RunningHttpServerFixture>(
                HttpServerLimits{}, kSweepInterval, HttpTestSupport::SlowRouteOptions{}, registrar);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());

        // 握手与首批帧数据都先读到客户端：接收缓冲里留着一大截未读字节，随后的 close 才会让
        // 内核回 RST（只收到 EOF 的话写侧拿不到错误事件）
        std::string accumulated;
        ASSERT_TRUE(client.sendText(upgradeRequestText(), kWaitTimeout));
        ASSERT_TRUE(client.waitForText(accumulated, "Sec-WebSocket-Accept", kWaitTimeout)) << "握手没有完成：" << accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, expectedHandshakeResponseText().size() + 1024, kWaitTimeout))
                << "握手之后没有读到帧数据，累计 " << accumulated.size() << " 字节";
        client.closeNow();

        ASSERT_TRUE(server->awaitConnectionsDrained(kResetWaitTimeout)) << "对端复位后 WebSocket 会话没有收口";
        ASSERT_TRUE(didBusinessObserveSendFailure.load(std::memory_order_acquire)) << "业务没有观察到 sendText 的 false";
        EXPECT_TRUE(isPeerClosedAfterFailure.load(std::memory_order_acquire))
                << "传输失败之后本侧应被标记为不可用（此后 send*() 一律短路）";
        EXPECT_EQ(logCountAtFailure.load(std::memory_order_acquire), 1)
                << "同一次传输失败只应记一条日志，实际条数见上：异常路径与非正返回值路径各记一条就会是 2";
        EXPECT_TRUE(didRetryReturnFalse.load(std::memory_order_acquire))
                << "本侧收口之后的数据帧与 Close 都应短路返回 false，而不是抛异常或宣称成功";
        EXPECT_EQ(logCountAfterRetry.load(std::memory_order_acquire), logCountAtFailure.load(std::memory_order_acquire))
                << "本侧收口之后的短路返回不得新增日志";
        // 会话收尾不会再补 Close（本侧已收口），因此整条断开的连接只留一条日志
        EXPECT_EQ(logCapture.countContaining(kFrameWriteFailureFragment), 1u) << "会话收尾阶段把同一件事又记了一遍";
        EXPECT_EQ(logCapture.countContaining("请停止继续发送"), 1u) << "唯一那条日志必须是可操作的中文文案";
        EXPECT_FALSE(server->startThrew()) << "对端复位把服务器主协程带崩了";
    }

    // ============================================================================
    // WebSocketPeer 契约：不经会话直接驱动，钉住「本侧已收口」的短路口径
    // ============================================================================

    /**
     * @brief 钉住本侧收口后的短路口径：发送一律返回 false，且一条新日志都不产生
     * @details 与「传输失败」相对的一侧：本侧已收口时不记日志（收口原因在那一刻已交代过），
     *          因此调用方看到 false 且日志没有新增，就应当理解为「本侧已经收了」，而不是「又失败了一次」。
     *          用假发送回调同步驱动，不依赖网络，断言的是纯契约而不是时序
     */
    TEST(WebSocketPeerContract, ReturnsFalseWithoutNewLogAfterLocalClose)
    {
        const HttpTestSupport::LogCapture logCapture;

        int sentFrameCount = 0;
        WebSocketPeer peer([&sentFrameCount](const std::string_view) -> Core::Task<bool>
        {
            ++sentFrameCount;
            co_return true;
        });

        // 首次 close() 正常写出一条 Close 帧：本侧随即收口。这是预期路径，不是失败，不该记日志
        Core::Task<bool> closeTask = peer.close();
        closeTask.handle().resume();
        EXPECT_TRUE(closeTask.await_resume());
        EXPECT_FALSE(peer.isOpen());
        EXPECT_EQ(sentFrameCount, 1);

        // 本侧已收口：数据帧、Ping 与第二条 Close 都短路返回 false，且都不新增日志
        Core::Task<bool> textTask = peer.sendText("after-local-close");
        textTask.handle().resume();
        EXPECT_FALSE(textTask.await_resume());
        Core::Task<bool> pingTask = peer.sendPing();
        pingTask.handle().resume();
        EXPECT_FALSE(pingTask.await_resume());
        Core::Task<bool> secondCloseTask = peer.close();
        secondCloseTask.handle().resume();
        EXPECT_FALSE(secondCloseTask.await_resume());

        EXPECT_EQ(sentFrameCount, 1) << "本侧收口之后不得再写出任何帧";
        EXPECT_EQ(logCapture.countContaining(kFrameWriteFailureFragment), 0u)
                << "本侧已收口的短路返回不记日志：否则调用方会把同一件事看成两次失败";
    }

    // ============================================================================
    // 服务器统计：升级、消息与两侧关闭各计各的
    // ============================================================================

    /**
     * @brief 钉住 WebSocket 侧各项计数：一次升级、两条文本消息、一条非法帧各落在哪个字段上
     * @details 计数点分布在会话阶段与对端对象上（见 HttpServerStats 各字段的说明），这里用一条真实
     *          连接把它们串起来核对。末尾另起一条普通 HTTP 连接做对照：它只动请求与状态码计数，
     *          WebSocket 各项必须纹丝不动——否则「观测升级比例」这类结论会被普通流量污染。
     */
    TEST(WebSocketSession, ReportsUpgradeMessagesAndClosuresInServerStats)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        const std::uint16_t port = server->listeningPort();
        ASSERT_NE(port, 0);

        const std::size_t handshakeLength = expectedHandshakeResponseText().size();
        const std::size_t firstEchoLength = serverFrameBytes(0x1, "one").size();

        LoopbackClient client(port);
        ASSERT_TRUE(client.isValid());

        // 升级请求与第一条文本帧挤在同一段里发出：升级与首条消息的计数都要落账
        std::string accumulated;
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x1, "one"), kWaitTimeout));
        ASSERT_TRUE(readUntilLength(client, accumulated, handshakeLength + firstEchoLength, kWaitTimeout))
                << "握手或第一条回显没有到达，累计 " << accumulated.size() << " 字节";

        // 第二条文本消息：消息计数应随之到 2，且两条都已交付业务
        ASSERT_TRUE(client.sendText(maskedClientFrame(0x1, "two"), kWaitTimeout));
        ASSERT_TRUE(readUntilLength(client, accumulated, handshakeLength + firstEchoLength + serverFrameBytes(0x1, "two").size(),
                                    kWaitTimeout))
                << "第二条回显没有到达，累计 " << accumulated.size() << " 字节";
        ASSERT_EQ(record->count(), 2U) << "两条文本消息都应交付业务";

        // 未掩码帧（RFC 6455 §5.1 要求客户端必须掩码）：服务端按 1002 收口并断开
        ASSERT_TRUE(client.sendText(std::string("\x81\x05", 2) + "hello", kWaitTimeout));
        ASSERT_TRUE(client.waitForClosure(accumulated, kWaitTimeout)) << "协议错误后服务端应断开连接";

        // 收口计数在 101 与回显之后才落账，按条件轮询而不是立刻断言
        ASSERT_TRUE(HttpTestSupport::waitForCondition(
                [&server]
                {
                    return server->server().stats().webSocketProtocolErrorCloseCount >= 1;
                },
                kWaitTimeout)) << "协议错误收口未在时限内计数";

        const HttpServerStats stats = server->server().stats();
        EXPECT_EQ(stats.webSocketUpgradeCount, 1u) << "101 已发出却不算升级成功";
        EXPECT_EQ(stats.webSocketMessageCount, 2u) << "只该记两条数据消息：分片重组后的一条算一次，控制帧不计";
        EXPECT_EQ(stats.webSocketProtocolErrorCloseCount, 1u) << "未掩码帧没有被记成协议错误收口";
        EXPECT_EQ(stats.webSocketPeerCloseCount, 0u) << "对端没有发 Close 帧，不该记成对端发起关闭";
        EXPECT_EQ(stats.webSocketServerCloseCount, 1u) << "本侧发出 1002 收口，应记一次本侧发起关闭";
        EXPECT_EQ(stats.badRequestCount, 0u) << "WS 帧解码失败回的是 Close 帧而不是 4xx，不该并入报文解析失败";
        EXPECT_EQ(stats.totalRequestCount, 1u) << "这条连接上只有升级请求被解析收齐";

        // 对照：普通 HTTP 请求只增加请求与状态码计数，WebSocket 各项保持不变
        LoopbackClient httpClient(port);
        ASSERT_TRUE(httpClient.isValid());
        ASSERT_TRUE(httpClient.sendText(HttpTestSupport::helloRequestText(), kWaitTimeout));
        std::string httpResponseText;
        ASSERT_TRUE(httpClient.waitForText(httpResponseText, "served-hello", kWaitTimeout)) << "普通请求未被正常服务";
        ASSERT_TRUE(HttpTestSupport::waitForCondition(
                [&server]
                {
                    return server->server().stats().totalRequestCount >= 2;
                },
                kWaitTimeout)) << "普通请求未计入请求条数";

        const HttpServerStats statsAfterHttpRequest = server->server().stats();
        EXPECT_EQ(statsAfterHttpRequest.totalRequestCount, 2u) << "普通请求没有计入请求条数";
        EXPECT_EQ(statsAfterHttpRequest.webSocketUpgradeCount, stats.webSocketUpgradeCount)
                << "普通 HTTP 请求被记成了 WebSocket 升级";
        EXPECT_EQ(statsAfterHttpRequest.webSocketMessageCount, stats.webSocketMessageCount)
                << "普通 HTTP 请求被记成了 WebSocket 消息";
        EXPECT_EQ(statsAfterHttpRequest.webSocketProtocolErrorCloseCount, stats.webSocketProtocolErrorCloseCount)
                << "普通 HTTP 请求被记成了 WebSocket 协议错误收口";
        EXPECT_EQ(statsAfterHttpRequest.webSocketPeerCloseCount, stats.webSocketPeerCloseCount)
                << "普通 HTTP 请求被记成了对端发起关闭";
        EXPECT_EQ(statsAfterHttpRequest.webSocketServerCloseCount, stats.webSocketServerCloseCount)
                << "普通 HTTP 请求被记成了本侧发起关闭";
    }
    /// 客户端提供 permessage-deflate 的请求头（写法照常见客户端的提供方式，带一个本端会忽略的参数）
    constexpr std::string_view kPerMessageDeflateOfferHeader =
            "sec-websocket-extensions: permessage-deflate; client_max_window_bits\r\n";

    /// 由 Python zlib 独立算出的 "hello" 压缩负载（裸 deflate + Z_SYNC_FLUSH，去掉尾部 00 00 FF FF）
    constexpr std::string_view kCompressedHelloPayload = "\xCA\x48\xCD\xC9\xC9\x07\x00";

    /// 服务端在接受协商后必须回的扩展取值（本端选定的两条 no_context_takeover）
    constexpr std::string_view kPerMessageDeflateResponseLine =
            "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n";

    /**
     * @brief 钉住 permessage-deflate 端到端：协商成功 → 服务端解对端的压缩帧、自己也按压缩回帧
     * @details 对端发的是 Python zlib 独立算出的压缩负载（不是本端编码器的产物），因此「服务端解压正确」
     *          这条结论不依赖被测实现；服务端的回帧则钉住 RSV1 置位、线上不留四字节空块尾、且能解回原文。
     */
    TEST(WebSocketSession, CompressesMessagesAfterNegotiatingPerMessageDeflate)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        // 扩展提供头插在结束空行之前：升级请求与压缩的首帧一次写出，走「101 之前就到达的字节」这条路径
        std::string request = upgradeRequestText();
        request.insert(request.size() - 2, std::string(kPerMessageDeflateOfferHeader));

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(request + maskedClientFrame(0x1, kCompressedHelloPayload, true, true), kWaitTimeout));

        std::string accumulated;
        ASSERT_TRUE(client.waitForText(accumulated, kPerMessageDeflateResponseLine, kWaitTimeout))
                << "101 里没有回扩展协商结论：" << accumulated;

        // 业务必须收到解压后的原文：压缩负载由 Python zlib 独立算出，解错就说明解压链路是坏的
        const auto recordDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (record->count() == 0 && std::chrono::steady_clock::now() < recordDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::vector<WebSocketMessage> messages = record->snapshot();
        ASSERT_EQ(messages.size(), 1U) << "压缩消息没有解压后交付业务";
        EXPECT_EQ(messages.front().opCode, WebSocketOpCode::Text);
        EXPECT_EQ(messages.front().payload, "hello") << "解压结果不是原文";

        // 回帧：握手之后紧跟一个服务端帧（无掩码）。用例负载很短，长度只可能落在 7 位档
        const std::size_t frameOffset = accumulated.find("\r\n\r\n") + 4;
        ASSERT_LT(frameOffset, accumulated.size()) << "没有收到回帧：" << accumulated;
        const std::string_view frameBytes(accumulated.data() + frameOffset, accumulated.size() - frameOffset);
        ASSERT_GE(frameBytes.size(), 2U) << "回帧不完整：" << accumulated;

        const auto firstByte = static_cast<std::uint8_t>(frameBytes[0]);
        EXPECT_EQ(firstByte & 0x80U, 0x80U) << "数据帧的 FIN 位必须为 1";
        EXPECT_EQ(firstByte & 0x40U, 0x40U) << "协商之后服务端回帧必须置 RSV1（RFC 7692 §6）";
        EXPECT_EQ(firstByte & 0x0FU, 0x01U) << "操作码应当是 Text";

        const auto payloadLength = static_cast<std::size_t>(static_cast<std::uint8_t>(frameBytes[1]));
        ASSERT_EQ(frameBytes.size(), 2U + payloadLength) << "回帧长度与长度域不一致：" << accumulated;
        const std::string_view wirePayload = frameBytes.substr(2);
        EXPECT_FALSE(wirePayload.ends_with(std::string("\x00\x00\xFF\xFF", 4)))
                << "四字节空块尾不该出现在线上负载里（RFC 7692 §7.2.1）";

        const std::optional<std::string> inflated = inflateWebSocketMessage(wirePayload, WebSocketFrameDecoder::kMaximumMessagePayloadLength);
        ASSERT_TRUE(inflated.has_value()) << "服务端的回帧解不开";
        EXPECT_EQ(*inflated, "hello");

        client.closeNow();
        EXPECT_TRUE(server->awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住对端不提供扩展时服务端不声明扩展、也不压缩回帧（回归：协商开关不能被默认打开）
     */
    TEST(WebSocketSession, KeepsFramesPlainWhenClientDoesNotOfferDeflate)
    {
        const auto record = std::make_shared<MessageRecord>();
        const std::unique_ptr<RunningHttpServerFixture> server = makeWebSocketServer(record);
        ASSERT_TRUE(server->awaitRunning(kWaitTimeout));

        LoopbackClient client(server->listeningPort());
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendText(upgradeRequestText() + maskedClientFrame(0x1, "hello"), kWaitTimeout));

        const std::string handshake = expectedHandshakeResponseText();
        const std::string expectedEcho = serverFrameBytes(0x1, "hello");
        std::string accumulated;
        ASSERT_TRUE(readUntilLength(client, accumulated, handshake.size() + expectedEcho.size(), kWaitTimeout));
        EXPECT_EQ(accumulated.substr(0, handshake.size()), handshake) << "未提供扩展时 101 不得多出扩展头";
        EXPECT_EQ(accumulated.substr(handshake.size()), expectedEcho) << "未协商就不该压缩回帧";
    }

} // namespace AsynGyanis::Net
