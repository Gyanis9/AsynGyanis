// TestHttp2CleartextSession.cpp —— 明文 HTTP/2（h2c，先验知识）的端到端测试
//
// 覆盖三块：
//   1) 打开 h2c 后明文连接直接说 h2：前奏 + SETTINGS 交换 → GET → 200 与完整正文（末片带 END_STREAM）；
//   2) 打开 h2c 的端口协议唯一：HTTP/1.1 报文会被连接层判为前奏非法，回 GOAWAY(PROTOCOL_ERROR) 并收口
//      （RFC 9113 §3.4 的先验知识语义，不做嗅探、不做回退）；
//   3) 默认（未打开 h2c）明文连接仍按 HTTP/1.1 服务：同一个 GET 照常得到 200 与正文。
// 客户端的帧解码复用生产解码器（Http2FrameDecoder），响应的头块复用生产解码器（HpackDecoder）解回，
// 因此「服务端吐出的字节」始终由第二份实现对照。夹具（RunningHttpServerFixture + LoopbackClient）
// 取自 HttpTestSupport.h，端口由内核分配，用例之间不共用端口。
//
// 本文件不起 TLS：h2c 的全部意义就是不经过 TLS 直接说 h2，用真实明文回环才测得到这条路径。

#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http2/Http2Connection.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http2/Hpack.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 明文连接的限额：超时四项都放宽，用例只关心协议行为
        HttpServerLimits makeCleartextLimits()
        {
            HttpServerLimits limits;
            limits.idleTimeout    = std::chrono::seconds{10};
            limits.readTimeout    = std::chrono::seconds{10};
            limits.writeTimeout   = std::chrono::seconds{10};
            limits.settingsAcknowledgementTimeout = std::chrono::seconds{10};
            return limits;
        }

        /**
         * @brief 按 RFC 7541 拼一个「带增量索引的字面量」，名字走静态表索引
         * @param staticNameIndex 静态表里的名字索引
         * @param value 头值
         * @return std::string 编码结果
         */
        std::string hpackLiteralField(const std::size_t staticNameIndex, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(staticNameIndex, 6, 0x40);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 拼一个「带增量索引的字面量」，名字与值都是字面量（RFC 7541 §6.2.1 的名字索引 0）
         * @param name 头名
         * @param value 头值
         * @return std::string 编码结果
         */
        std::string hpackLiteralField(const std::string_view name, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(0, 6, 0x40);
            appendHpackString(bytes, name);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 拼一个 GET 请求头块（:method GET、:scheme http、:path、:authority）
         * @param path 请求路径
         * @return std::string 头块字节。索引取自 RFC 7541 Appendix A：2 是 :method: GET、
         *         6 是 :scheme: http、4 是 :path: /
         */
        std::string makeGetRequestHeaderBlock(const std::string_view path)
        {
            std::string headerBlock;
            headerBlock += encodeHpackInteger(2, 7, 0x80);
            headerBlock += encodeHpackInteger(6, 7, 0x80);
            headerBlock += path == "/" ? encodeHpackInteger(4, 7, 0x80) : hpackLiteralField(4, path);
            headerBlock += hpackLiteralField(1, "localhost");
            return headerBlock;
        }

        /**
         * @brief 拼一个 HEADERS 帧（头块在一帧内结束，不带优先级字段）
         * @param streamId 流号
         * @param headerBlock 头块字节
         * @param endStream 是否同时带上 END_STREAM
         * @return std::string 完整帧字节
         */
        std::string makeRequestHeadersFrame(const std::uint32_t streamId, const std::string &headerBlock, const bool endStream)
        {
            return encodeHttp2HeadersFrame(Http2HeadersPayload{.endStream = endStream,
                                                                .endHeaders = true,
                                                                .headerBlockFragment = headerBlock},
                                           streamId);
        }

        /**
         * @brief 拼一个 POST 请求头块（:method POST、:scheme http、:path、:authority）
         * @param path 请求路径
         * @return std::string 头块字节。索引取自 RFC 7541 Appendix A：3 是 :method: POST、6 是 :scheme: http
         */
        std::string makePostRequestHeaderBlock(const std::string_view path)
        {
            std::string headerBlock;
            headerBlock += encodeHpackInteger(3, 7, 0x80);
            headerBlock += encodeHpackInteger(6, 7, 0x80);
            headerBlock += hpackLiteralField(4, path);
            headerBlock += hpackLiteralField(1, "localhost");
            return headerBlock;
        }

        /**
         * @brief 拼一个只带普通头、并以上面收尾的尾部头块帧（RFC 9113 §8.1）
         * @param streamId 流号
         * @return std::string 完整帧字节
         */
        std::string makeTrailersFrame(const std::uint32_t streamId)
        {
            // 尾部头块里不得出现伪头（:method 之类），因此用一个普通头；它同时是 END_STREAM 的载体
            return encodeHttp2HeadersFrame(
                    Http2HeadersPayload{.endStream = true, .endHeaders = true, .headerBlockFragment = hpackLiteralField("x-trailer", "done")},
                    streamId);
        }

        /// 一条明文 h2 客户端：只负责「发字节、把收到的字节解成帧」
        class CleartextHttp2Client
        {
        public:
            /**
             * @brief 连接服务端端口
             * @param port 服务端监听端口
             */
            explicit CleartextHttp2Client(const std::uint16_t port) :
                m_socket(port)
            {
            }

            [[nodiscard]] bool isValid() const noexcept
            {
                return m_socket.isValid();
            }

            /**
             * @brief 把整段字节写出去
             * @param bytes 待发字节
             * @param timeout 写入等待上限
             * @return true 全部字节已被内核接收
             */
            bool sendBytes(const std::string_view bytes, const std::chrono::milliseconds timeout) const
            {
                return m_socket.sendText(bytes, timeout);
            }

            /**
             * @brief 轮询读并解码，直到谓词满足、对端关闭或超时
             * @param frames 输入输出：已解出的帧（按到达顺序累积）
             * @param isDone 判定谓词
             * @param timeout 等待上限
             * @return true 谓词在时限内满足
             */
            bool pumpUntil(std::vector<Http2Frame> &frames, const std::function<bool(const std::vector<Http2Frame> &)> &isDone,
                           const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    drainDecodedFrames(frames);
                    if (isDone(frames))
                    {
                        return true;
                    }
                    if (m_hasDecodeError || std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }

                    // 读到的字节攒进待解缓冲：解码器自己缓冲半帧，所以这里不做帧边界判断
                    const ReadOutcome outcome = m_socket.readOnce(m_pendingFrameBytes);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        drainDecodedFrames(frames);
                        return isDone(frames);
                    }
                    if (outcome == ReadOutcome::Idle)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                }
            }

            /**
             * @brief 轮询读直到观察到对端关闭或连接出错
             * @param frames 输入输出：已解出的帧
             * @param timeout 等待上限
             * @return true 在时限内观察到连接已断
             */
            bool waitForClosure(std::vector<Http2Frame> &frames, const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    drainDecodedFrames(frames);
                    const ReadOutcome outcome = m_socket.readOnce(m_pendingFrameBytes);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        return true;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            void closeNow()
            {
                m_socket.closeNow();
            }

        private:
            /// 把待解缓冲里的字节喂给解码器，产出的帧追加进 frames
            void drainDecodedFrames(std::vector<Http2Frame> &frames)
            {
                std::size_t consumedByteCount = 0;
                while (consumedByteCount < m_pendingFrameBytes.size())
                {
                    const Http2FrameDecodeStatus status =
                            m_frameDecoder.parse(m_pendingFrameBytes.data() + consumedByteCount,
                                                 m_pendingFrameBytes.size() - consumedByteCount);
                    if (status != Http2FrameDecodeStatus::Frame)
                    {
                        if (status == Http2FrameDecodeStatus::Error)
                        {
                            m_hasDecodeError = true;
                        }
                        consumedByteCount = m_pendingFrameBytes.size();
                        break;
                    }
                    consumedByteCount += m_frameDecoder.consumedByteCount();
                    frames.push_back(m_frameDecoder.takeFrame());
                }
                m_pendingFrameBytes.clear();
            }

            LoopbackClient m_socket;             ///< 明文回环连接（发字节、收字节）
            Http2FrameDecoder m_frameDecoder;    ///< 生产解码器：服务端吐出的字节由它解回
            std::string m_pendingFrameBytes;     ///< 已收到、还没喂给解码器的字节
            bool m_hasDecodeError{false};        ///< 是否已解出非法帧
        };

        /// 该流上是否出现过 END_STREAM（消息边界：响应正文到此为止）
        bool hasEndStream(const std::vector<Http2Frame> &frames, const std::uint32_t streamId)
        {
            for (const Http2Frame &frame: frames)
            {
                if (frame.header.streamId == streamId && (frame.header.flags & kHttp2FlagEndStream) != 0)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 取该流上第 blockIndex 个头块（HEADERS + 其后续 CONTINUATION 拼起来）
         * @param frames 已解出的帧
         * @param streamId 目标流号
         * @param blockIndex 第几个头块，从 0 起；一条流上可能有多个（例如先 100 再 200）
         * @return std::string 头块字节；不足那么多个时返回空串
         */
        std::string responseHeaderBlock(const std::vector<Http2Frame> &frames, const std::uint32_t streamId,
                                        const std::size_t blockIndex = 0)
        {
            std::string headerBlock;
            std::size_t blockCount = 0;
            bool isCollecting = false;
            for (const Http2Frame &frame: frames)
            {
                if (frame.header.streamId != streamId)
                {
                    continue;
                }
                if (frame.header.type == Http2FrameType::Headers)
                {
                    isCollecting = blockCount == blockIndex;
                }
                if (!isCollecting)
                {
                    // 还没轮到目标头块：数到它为止（每个头块以 END_HEADERS 收尾）
                    if (frame.header.type == Http2FrameType::Headers && (frame.header.flags & kHttp2FlagEndHeaders) != 0)
                    {
                        ++blockCount;
                    }
                    continue;
                }
                if (frame.header.type == Http2FrameType::Headers || frame.header.type == Http2FrameType::Continuation)
                {
                    headerBlock += frame.payload;
                }
                if ((frame.header.flags & kHttp2FlagEndHeaders) != 0)
                {
                    break;
                }
            }
            return headerBlock;
        }

        /// 取该流上全部 DATA 帧的正文（按到达顺序拼接）
        std::string responseDataPayload(const std::vector<Http2Frame> &frames, const std::uint32_t streamId)
        {
            std::string body;
            for (const Http2Frame &frame: frames)
            {
                if (frame.header.streamId == streamId && frame.header.type == Http2FrameType::Data)
                {
                    body += frame.payload;
                }
            }
            return body;
        }

        /**
         * @brief 取该流上响应头里某个头的值；没有该头、或头块解不开时返回空串
         * @param decoder 逐条响应按到达顺序复用的解码器：服务端的 HPACK 编码器会引用前一条响应
         *        建立起来的动态表，换一个全新的解码器就解不开第二条响应（索引无人认得）
         * @param frames 已解出的帧
         * @param streamId 目标流号
         * @param name 头名
         * @return std::string 头值；取不到时为空串
         */
        std::string findResponseHeaderValue(HpackDecoder &decoder, const std::vector<Http2Frame> &frames, const std::uint32_t streamId,
                                           const std::string_view name, const std::size_t blockIndex = 0)
        {
            std::vector<HpackHeaderField> headerFields;
            std::string errorText;
            if (!decoder.decode(responseHeaderBlock(frames, streamId, blockIndex), headerFields, &errorText))
            {
                return {};
            }
            for (const HpackHeaderField &field: headerFields)
            {
                if (field.name == name)
                {
                    return field.value;
                }
            }
            return {};
        }

        /// GOAWAY 帧里带的错误码：负载是「4 字节最后流号 + 4 字节错误码」（RFC 9113 §6.8）
        Http2ErrorCode readGoAwayErrorCode(const std::string &payload)
        {
            if (payload.size() < 8U)
            {
                return Http2ErrorCode::NoError;
            }
            const auto rawValue = static_cast<std::uint32_t>(
                    (static_cast<std::uint8_t>(payload[4]) << 24) | (static_cast<std::uint8_t>(payload[5]) << 16) |
                    (static_cast<std::uint8_t>(payload[6]) << 8) | static_cast<std::uint8_t>(payload[7]));
            return static_cast<Http2ErrorCode>(rawValue);
        }

        /// GOAWAY 帧里带的最后流号：负载前 4 字节，最高位保留（RFC 9113 §6.8）
        std::uint32_t readGoAwayLastStreamId(const std::string &payload)
        {
            if (payload.size() < 4U)
            {
                return 0U;
            }
            const auto rawValue = static_cast<std::uint32_t>(
                    (static_cast<std::uint8_t>(payload[0]) << 24) | (static_cast<std::uint8_t>(payload[1]) << 16) |
                    (static_cast<std::uint8_t>(payload[2]) << 8) | static_cast<std::uint8_t>(payload[3]));
            return rawValue & 0x7fffffffU;
        }

        /// RFC 6455 §1.3 的示例 key 与它对应的 Sec-WebSocket-Accept（规范原文给出的黄金值）
        constexpr std::string_view kRfc6455SampleKey = "dGhlIHNhbXBsZSBub25jZQ==";
        constexpr std::string_view kRfc6455SampleAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

        /**
         * @brief 拼一条扩展 CONNECT 的请求头块（RFC 8441 §4）：五个伪头齐全，WebSocket 握手头随后
         * @param path 请求路径（隧道的目标资源）
         * @return std::string 头块字节
         */
        std::string makeWebSocketTunnelHeaderBlock(const std::string_view path)
        {
            std::string headerBlock;
            headerBlock += hpackLiteralField(2, "CONNECT");
            headerBlock += encodeHpackInteger(6, 7, 0x80); // :scheme: http
            headerBlock += path == "/" ? encodeHpackInteger(4, 7, 0x80) : hpackLiteralField(4, path);
            headerBlock += hpackLiteralField(1, "localhost");
            headerBlock += hpackLiteralField(":protocol", "websocket");
            // 普通头（必须排在伪头之后）：版本与 key 两项与 h1 的 101 握手完全一致
            headerBlock += hpackLiteralField("sec-websocket-version", "13");
            headerBlock += hpackLiteralField("sec-websocket-key", kRfc6455SampleKey);
            return headerBlock;
        }

        /**
         * @brief 拼一条客户端 WebSocket 帧（必须带掩码，RFC 6455 §5.3）：负载不超过 125 字节
         * @param opCode 操作码（1 = 文本、8 = 关闭）
         * @param payload 负载
         * @return std::string 完整帧字节
         */
        std::string makeMaskedClientFrame(const std::uint8_t opCode, const std::string_view payload)
        {
            const std::array<std::uint8_t, 4> maskKey{0x12U, 0x34U, 0x56U, 0x78U};
            std::string frameBytes;
            frameBytes.push_back(static_cast<char>(0x80U | opCode));
            frameBytes.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload.size())));
            for (const std::uint8_t maskByte: maskKey)
            {
                frameBytes.push_back(static_cast<char>(maskByte));
            }
            for (std::size_t index = 0; index < payload.size(); ++index)
            {
                frameBytes.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ maskKey[index % 4U]));
            }
            return frameBytes;
        }

        /**
         * @brief 解一条服务端 WebSocket 帧（服务端帧不带掩码）：返回操作码与负载
         * @param frameBytes 帧字节
         * @return std::pair<int, std::string> 操作码与负载；字节不足或长度字段用了扩展长度时返回 {-1, ""}
         */
        std::pair<int, std::string> parseServerFrame(const std::string_view frameBytes)
        {
            if (frameBytes.size() < 2U)
            {
                return {-1, {}};
            }
            const auto firstByte = static_cast<std::uint8_t>(frameBytes[0]);
            const auto secondByte = static_cast<std::uint8_t>(frameBytes[1]);
            if ((secondByte & 0x80U) != 0U)
            {
                return {-1, {}}; // 服务端不该掩码
            }
            const std::size_t payloadLength = secondByte & 0x7FU;
            if (payloadLength > 125U || frameBytes.size() < 2U + payloadLength)
            {
                return {-1, {}}; // 本用例的负载都很短，扩展长度不该出现
            }
            return {static_cast<int>(firstByte & 0x0FU), std::string(frameBytes.substr(2U, payloadLength))};
        }
    } // namespace

    /**
     * @brief 钉住：打开 h2c 后明文连接按先验知识直接说 h2——前奏 + SETTINGS 交换后 GET 得到 200 与完整正文
     */
    TEST(Http2CleartextSession, ServesRequestWhenCleartextHttp2IsEnabled)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, {}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        EXPECT_TRUE(fixture.server().isHttp2CleartextEnabled()) << "开关没有落到服务器上";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        // 前奏与客户端自己的 SETTINGS 一起发：这是 RFC 9113 §3.4 规定的开端
        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));

        // 服务端必须先用自己的 SETTINGS 起头（§3.4：服务端前奏即一个 SETTINGS 帧）
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() &&
                                                receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";
        EXPECT_EQ(frames.front().header.streamId, 0U) << "SETTINGS 必须是连接级帧（流号 0）";
        ASSERT_TRUE(client.sendBytes(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}), kWaitTimeout));

        // 一条完整请求：GET /hello，头块与 END_STREAM 一并发（无正文）
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "没有在时限内拿到流 1 的完整响应";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "served-hello");
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "正常请求不该被收口";
        }

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：打开 h2c 的端口只说 h2——HTTP/1.1 报文会被判为前奏非法，回 GOAWAY(PROTOCOL_ERROR) 并收口
     */
    TEST(Http2CleartextSession, RejectsHttp11RequestWithGoAwayWhenCleartextHttp2IsEnabled)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, {}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        // 对端不懂先验知识，发的是 HTTP/1.1 报文：连接层按前奏逐字节校验，必然在第一个字节就判错
        ASSERT_TRUE(client.sendBytes("GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n", kWaitTimeout));

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.type == Http2FrameType::GoAway)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "HTTP/1.1 报文没有被判为非法前奏，也没收到 GOAWAY";

        const Http2Frame *goAwayFrame = nullptr;
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.type == Http2FrameType::GoAway)
            {
                goAwayFrame = &frame;
                break;
            }
        }
        ASSERT_NE(goAwayFrame, nullptr);
        EXPECT_EQ(goAwayFrame->header.streamId, 0U) << "GOAWAY 必须是连接级帧（流号 0）";
        EXPECT_EQ(readGoAwayErrorCode(goAwayFrame->payload), Http2ErrorCode::ProtocolError)
                << "非法前奏应按 PROTOCOL_ERROR 收口（RFC 9113 §3.4）";
        EXPECT_TRUE(client.waitForClosure(frames, kWaitTimeout)) << "GOAWAY 之后连接没有关闭";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：默认（未打开 h2c）明文连接仍按 HTTP/1.1 服务——开关的默认值不改变既有行为
     */
    TEST(Http2CleartextSession, KeepsHttp11ServingWhenCleartextHttp2IsDisabled)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        EXPECT_FALSE(fixture.server().isHttp2CleartextEnabled()) << "h2c 的默认值必须是关闭";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";
        ASSERT_TRUE(client.sendText("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", kWaitTimeout));

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "200 OK", kWaitTimeout)) << "HTTP/1.1 请求没有拿到状态行：已收到 " << responseText;
        ASSERT_TRUE(client.waitForText(responseText, "served-hello", kWaitTimeout)) << "HTTP/1.1 响应正文不完整：已收到 " << responseText;
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：开关关闭时同一段 h2 前奏得不到 h2 应答——端口上的协议确实由该开关决定，
     *        而不是「前奏恰好也能被当成请求处理」
     * @details 这是上一条用例的反面：若 createConnection() 忽略开关一律建 HTTP/2 会话，
     *          本用例会因为「应答是 SETTINGS 帧、不是 HTTP/1.」而失败
     */
    TEST(Http2CleartextSession, AnswersHttp11WhenCleartextHttp2IsDisabled)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";
        ASSERT_TRUE(client.sendText(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));

        // h2 服务端在这里会先吐一个二进制 SETTINGS 帧（首字节 0x00），HTTP/1.1 服务端只可能回文本状态行
        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "HTTP/1.", kWaitTimeout)) << "关闭 h2c 时没有按 HTTP/1.1 应答：已收到 "
                                                                              << responseText.size() << " 字节";
        EXPECT_FALSE(responseText.empty());
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：正文以**尾部头块**收尾的请求（不带 END_STREAM 的 DATA + 带 END_STREAM 的 trailers）
     *        会被正常路由并回响应——尾部头块也是消息结尾（RFC 9113 §8.1）
     * @details 会话只按 `Http2ReceivedData::endStream` 判定「正文收齐」，不读流状态；连接层若在
     *          尾部头块分支只把流置成 half-closed (remote) 而不产出收尾片段，这条请求就永远等不到
     *          收齐、不会进路由（客户端只能等到超时）。本用例的响应必须在时限内出现。
     */
    TEST(Http2CleartextSession, ServesRequestWhoseBodyEndsWithTrailers)
    {
        const std::string_view requestBody = "trailed-body";
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, [requestBody](Router &router, Core::EventLoop &)
        {
            router.post("/echo", [requestBody](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                // 回显正文：只有正文真被收齐了，回显才等于原样
                static_cast<void>(requestBody);
                response.setBody(request.body());
                co_return;
            });
        }, HttpParserLimits{}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 三段一气的请求：请求头（不收尾）→ 正文（不收尾）→ 尾部头块（收尾）
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo"), false);
        requestBytes += encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(requestBody)}, 1U);
        requestBytes += makeTrailersFrame(1U);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "以尾部头块收尾的请求没有被路由（正文收齐没有被识别）";
        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), requestBody) << "回显的正文与原请求不一致：正文没有被完整收齐";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：优雅关停时 h2 对端收到的是收尾 GOAWAY（NO_ERROR + 已处理的最后流号），
     *        而不是一个裸的 TCP 关闭——对端据此知道哪些请求已经生效、新流没有生效
     * @details 服务器在「关停没有在途工作的连接」那一步先给协议层一次写字节的机会
     *          （`Core::Connection::onGracefulShutdownRequested()`），HTTP/2 会话借此发出 GOAWAY。
     *          本端已经服务过流 1，因此 last-stream-id 应当是 1。
     */
    TEST(Http2CleartextSession, SendsGoAwayBeforeGracefulShutdown)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, {}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 先服务一条完整请求：关停通告里的 last-stream-id 才有可断言的取值（应当是 1）
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "请求没有在时限内被服务";

        // 关停：连接此时没有在途工作，属于「优雅收口」那一路
        ASSERT_TRUE(fixture.drainServer(std::chrono::milliseconds{1000}, kWaitTimeout)) << "drain 没有在时限内完成";

        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.type == Http2FrameType::GoAway)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "关停时没有收到收尾 GOAWAY";

        const Http2Frame *goAwayFrame = nullptr;
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.type == Http2FrameType::GoAway)
            {
                goAwayFrame = &frame;
                break;
            }
        }
        ASSERT_NE(goAwayFrame, nullptr);
        EXPECT_EQ(readGoAwayErrorCode(goAwayFrame->payload), Http2ErrorCode::NoError) << "优雅关停的收尾通告带 NO_ERROR";
        EXPECT_EQ(readGoAwayLastStreamId(goAwayFrame->payload), 1U) << "已处理的最后流号是 1：对端据此知道它不必重试这条请求";
        EXPECT_TRUE(client.waitForClosure(frames, kWaitTimeout)) << "GOAWAY 之后连接没有关闭";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：正文超过上限时回 413 并**请对端中止上传**（RST_STREAM(NO_ERROR)），
     *        而不是把剩余字节白收一遍；连接与后续请求照常工作
     */
    TEST(Http2CleartextSession, AbortsOversizedUploadAfterAnswering413)
    {
        // 正文上限设得很小：一条 64 字节的 POST 必然越界
        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 16;
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, parserLimits, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 超限的 POST：请求头不收尾、正文不收尾（对端还有更多要传）
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo"), false);
        requestBytes += encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(64U, 'x')}, 1U);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        // 413 与 RST_STREAM(NO_ERROR) 都要出现：前者是应答，后者是「别再传了」（RFC 9113 §8.1）
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         bool hasTooLarge = false;
                                         bool hasAbort = false;
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             hasTooLarge = hasTooLarge || frame.header.type == Http2FrameType::Headers;
                                             hasAbort = hasAbort || frame.header.type == Http2FrameType::RstStream;
                                         }
                                         return hasTooLarge && hasAbort;
                                     },
                                     kWaitTimeout)) << "超限上传没有收到 413 与中止请求的 RST_STREAM";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "413");
        Http2ErrorCode abortErrorCode = Http2ErrorCode::ProtocolError;
        bool hasAbortFrame = false;
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.type == Http2FrameType::RstStream && frame.header.streamId == 1U)
            {
                Http2RstStreamPayload payload;
                std::string parseErrorText;
                ASSERT_TRUE(parseHttp2RstStreamPayload(frame, payload, &parseErrorText)) << parseErrorText;
                abortErrorCode = payload.errorCode;
                hasAbortFrame = true;
            }
        }
        ASSERT_TRUE(hasAbortFrame) << "流 1 上没有中止请求的 RST_STREAM";
        EXPECT_EQ(abortErrorCode, Http2ErrorCode::NoError)
                << "请对端中止发送用的是 NO_ERROR（§8.1），不是把这条流判成出错";
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "中止单流不该把连接收掉";
        }

        // 连接照常工作：越界请求之后的另一条请求仍得到 200
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "越界上传之后连接不再可用";
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 3U), "served-hello");

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：一条明文连接上并发两条流各自拿到属于自己的响应——多路复用不因传输是明文而失效
     * @details 两条请求同批写出（对端流号 1 与 3），响应允许乱序到达，但**正文与请求必须一一对应**：
     *          串流（把 A 的正文发给 B）是这类实现最容易犯又最难察觉的错误
     */
    TEST(Http2CleartextSession, ServesTwoConcurrentStreamsOverOneConnection)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, [](Router &router, Core::EventLoop &)
        {
            router.get("/world", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.setBody("served-world");
                co_return;
            });
        }, HttpParserLimits{}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 两条并发请求一次写出：流 1 要 /hello，流 3 要 /world
        std::string requestBytes = makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello"), true);
        requestBytes += makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/world"), true);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U) && hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "两条并发流没有都在时限内收完";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "served-hello") << "流 1 的正文串了";
        EXPECT_EQ(responseDataPayload(frames, 3U), "served-world") << "流 3 的正文串了";
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "正常并发不该触发收口";
        }

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：RFC 8441 的扩展 CONNECT 把一条 h2c 流变成 WebSocket 隧道——200 + 规范黄金 accept 值、
     *        文本帧原样回显（隧道内是 DATA 帧）、Close 之后本侧方向以 END_STREAM 收尾
     * @details 客户端与 h1 的差别只有握手承载方式：没有 Upgrade/Connection 头，改用 :protocol=websocket；
     *          服务端的应答是 200 而不是 101。accept 值用 RFC 6455 §1.3 的示例 key 对照规范原文的黄金值。
     */
    TEST(Http2CleartextSession, ServesWebSocketTunnelOverExtendedConnect)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, [](Router &router, Core::EventLoop &)
        {
            // 同一个处理器同时服务 h1 的 101 升级与 h2 的扩展 CONNECT：后者按 GET 参与路由（见 mapToHttpRequest）
            router.get("/chat", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.upgradeToWebSocket([](WebSocketPeer &peer) -> Core::Task<>
                {
                    while (true)
                    {
                        const std::optional<WebSocketMessage> message = co_await peer.receive();
                        if (!message.has_value())
                        {
                            co_return;
                        }
                        if (!co_await peer.sendText(message->payload))
                        {
                            co_return;
                        }
                    }
                });
                co_return;
            });
        }, HttpParserLimits{}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 扩展 CONNECT：伪头齐全（:protocol=websocket），并要求本侧不要在 200 之后收尾
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeWebSocketTunnelHeaderBlock("/chat"), false), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.streamId == 1U && frame.header.type == Http2FrameType::Headers)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "扩展 CONNECT 没有得到应答";
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.streamId == 1U && frame.header.type == Http2FrameType::Headers)
            {
                EXPECT_EQ(frame.header.flags & kHttp2FlagEndStream, 0) << "升级应答不能带 END_STREAM：这条流接下来要承载帧";
            }
        }

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, "sec-websocket-accept"), kRfc6455SampleAccept)
                << "accept 值必须是 RFC 6455 §1.3 的规范黄金值";

        // 隧道里的文本帧：客户端发带掩码的帧（装在 DATA 帧里），服务端回不带掩码的同内容帧
        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = makeMaskedClientFrame(0x1U, "hi-tunnel")}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !responseDataPayload(receivedFrames, 1U).empty();
                                     },
                                     kWaitTimeout)) << "隧道里没有回显";
        const std::pair<int, std::string> echoedFrame = parseServerFrame(responseDataPayload(frames, 1U));
        EXPECT_EQ(echoedFrame.first, 1) << "回显的应当是文本帧";
        EXPECT_EQ(echoedFrame.second, "hi-tunnel");

        // 关闭握手：客户端发 Close，服务端回 Close 并把本侧方向以 END_STREAM 收尾
        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = makeMaskedClientFrame(0x8U, "")}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "Close 之后隧道没有收尾";
        const std::string tunnelPayload = responseDataPayload(frames, 1U);
        EXPECT_NE(tunnelPayload.find(static_cast<char>(0x88U)), std::string::npos)
                << "服务端应当回一条 Close 帧（FIN + 操作码 8）";
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "隧道正常收尾不该把连接收掉";
        }

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：扩展 CONNECT 用了本端未实现的 :protocol 时回 501（而不是当未知方法回 404/405），
     *        且连接照旧可用
     */
    TEST(Http2CleartextSession, Answers501ForUnsupportedConnectProtocol)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, [](Router &router, Core::EventLoop &)
        {
            router.get("/chat", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                // 故意注册一个会登记升级的处理器：:protocol 不是 websocket 时不该走到这里
                response.upgradeToWebSocket([](WebSocketPeer &) -> Core::Task<>
                {
                    co_return;
                });
                co_return;
            });
        }, HttpParserLimits{}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // :protocol=https：本端只实现 websocket，应按「协议未实现」回 501
        std::string headerBlock;
        headerBlock += hpackLiteralField(2, "CONNECT");
        headerBlock += encodeHpackInteger(6, 7, 0x80);
        headerBlock += hpackLiteralField(4, "/chat");
        headerBlock += hpackLiteralField(1, "localhost");
        headerBlock += hpackLiteralField(":protocol", "https");
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, headerBlock, false), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "未实现的 :protocol 没有得到应答";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "501");

        // 连接照旧可用：随后一条普通请求正常服务
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "未实现的 :protocol 之后连接不再可用";
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "200");

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：隧道期间同连接其它流的请求回 503（本类的单驱动循环无法并发服务两条流），
     *        且隧道本身照旧收尾
     * @details 这是扩容隧道那条已知取舍的可观测契约：宁可明确拒绝，也不把请求晾到隧道结束
     */
    TEST(Http2CleartextSession, RefusesConcurrentRequestsWhileTunnelIsOpen)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, [](Router &router, Core::EventLoop &)
        {
            router.get("/chat", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.upgradeToWebSocket([](WebSocketPeer &peer) -> Core::Task<>
                {
                    while (true)
                    {
                        const std::optional<WebSocketMessage> message = co_await peer.receive();
                        if (!message.has_value())
                        {
                            co_return;
                        }
                        if (!co_await peer.sendText(message->payload))
                        {
                            co_return;
                        }
                    }
                });
                co_return;
            });
        }, HttpParserLimits{}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 先立起隧道（流 1），再用流 3 发一条普通请求
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeWebSocketTunnelHeaderBlock("/chat"), false), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.streamId == 1U && frame.header.type == Http2FrameType::Headers)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "隧道没有建立";
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "隧道期间的另一条流没有得到应答";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "503")
                << "隧道期间其它流应当被明确拒绝，而不是晾着";

        // 隧道本身不受影响：仍能收发帧，并在 Close 之后正常收尾
        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = makeMaskedClientFrame(0x1U, "still-alive")}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !responseDataPayload(receivedFrames, 1U).empty();
                                     },
                                     kWaitTimeout)) << "隧道在拒绝其它流之后失效了";
        const std::pair<int, std::string> echoedFrame = parseServerFrame(responseDataPayload(frames, 1U));
        EXPECT_EQ(echoedFrame.second, "still-alive");

        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = makeMaskedClientFrame(0x8U, "")}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "隧道没有按 Close 收尾";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：h2 上对端声明 Expect: 100-continue 时，本端在收到正文之前先发一个 100 的 HEADERS
     * @details 与 h1 同一条规范（RFC 9110 §10.1.1），只是承载换成 HEADERS（:status 100）且不带 END_STREAM；
     *          客户端据此才肯发正文，否则要等自己的超时
     */
    TEST(Http2CleartextSession, AnswersContinueBeforeTheBodyArrives)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, [](Router &router, Core::EventLoop &)
        {
            router.post("/upload", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                response.setBody("received-" + std::to_string(request.body().size()));
                co_return;
            });
        }, HttpParserLimits{}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        CleartextHttp2Client client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 请求头带 expect，正文（DATA）留到看到 100 之后再发
        std::string headerBlock = makePostRequestHeaderBlock("/upload");
        headerBlock += hpackLiteralField("content-length", "5");
        headerBlock += hpackLiteralField("expect", "100-continue");
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, headerBlock, false), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.streamId == 1U && frame.header.type == Http2FrameType::Headers)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到 100（响应头）";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status", 0), "100")
                << "先到的应当是 100，而不是最终状态码";
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.streamId == 1U && frame.header.type == Http2FrameType::Headers)
            {
                EXPECT_EQ(frame.header.flags & kHttp2FlagEndStream, 0) << "100 不能带 END_STREAM：正文还没到";
                break;
            }
        }

        // 补上正文：本端照常路由并以 200 + 回显正文收尾
        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = true, .data = "12345"}, 1U), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "补正文之后没有拿到最终响应";
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status", 1), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "received-5");

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }
} // namespace AsynGyanis::Net
