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

        /// 取该流上 HEADERS + 后续 CONTINUATION 拼出的响应头块（响应头很小，通常一帧到底）
        std::string responseHeaderBlock(const std::vector<Http2Frame> &frames, const std::uint32_t streamId)
        {
            std::string headerBlock;
            bool isCollecting = false;
            for (const Http2Frame &frame: frames)
            {
                if (frame.header.streamId != streamId)
                {
                    continue;
                }
                if (frame.header.type == Http2FrameType::Headers)
                {
                    isCollecting = true;
                }
                if (!isCollecting)
                {
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

        /// 取该流上响应头里某个头的值；没有该头时返回空串
        std::string findResponseHeaderValue(const std::vector<Http2Frame> &frames, const std::uint32_t streamId, const std::string_view name)
        {
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            std::string errorText;
            if (!decoder.decode(responseHeaderBlock(frames, streamId), headerFields, &errorText))
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

        EXPECT_EQ(findResponseHeaderValue(frames, 1U, ":status"), "200");
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
} // namespace AsynGyanis::Net
