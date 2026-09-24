// TestHttp2CleartextSession.cpp —— 明文 HTTP/2（h2c，先验知识）的端到端测试
//
// 覆盖四块：
//   1) 打开 h2c 后明文连接直接说 h2：前奏 + SETTINGS 交换 → GET → 200 与完整正文（末片带 END_STREAM）；
//   2) 打开 h2c 的端口协议唯一：HTTP/1.1 报文会被连接层判为前奏非法，回 GOAWAY(PROTOCOL_ERROR) 并收口
//      （RFC 9113 §3.4 的先验知识语义，不做嗅探、不做回退）；
//   3) 默认（未打开 h2c）明文连接仍按 HTTP/1.1 服务：同一个 GET 照常得到 200 与正文；
//   4) 流式请求正文：流式路由在头部收齐即派发（不等 END_STREAM）、正文按到达批次交付、跨多帧逐字节一致、
//      收尾之后同一条连接照旧可用、越过 maximumBodySize 回 413。
// 客户端的帧解码复用生产解码器（Http2FrameDecoder），响应的头块复用生产解码器（HpackDecoder）解回，
// 因此「服务端吐出的字节」始终由第二份实现对照。夹具（RunningHttpServerFixture + LoopbackClient）
// 取自 HttpTestSupport.h，端口由内核分配，用例之间不共用端口。
//
// 本文件不起 TLS：h2c 的全部意义就是不经过 TLS 直接说 h2，用真实明文回环才测得到这条路径。

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/HttpRequestBody.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http2/Http2Connection.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http2/Hpack.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include "HttpTestSupport.h"

#include "Http2TestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
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
         * @brief 编一个字面量字段表示（定义见 Http2TestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::hpackLiteralField;

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

        /**
         * @brief 按帧到达顺序解出每条流的响应状态码
         * @details 服务端的 HPACK 编码上下文是按连接推进的：逐条孤立解码会解不开后到的那条
         *          （它引用的动态表条目是前一条响应建立的）。两条流的响应要一起判定的用例用这个助手
         * @param frames 已解出的帧
         * @return std::map<std::uint32_t, std::string> 流号 → :status 值；没回头块或解不开的流不在表里
         */
        std::map<std::uint32_t, std::string> collectResponseStatuses(const std::vector<Http2Frame> &frames)
        {
            std::map<std::uint32_t, std::string> statusByStream;
            HpackDecoder decoder;
            for (const Http2Frame &frame: frames)
            {
                if (frame.header.type != Http2FrameType::Headers || frame.header.streamId == 0U)
                {
                    continue;
                }
                std::vector<HpackHeaderField> headerFields;
                std::string errorText;
                if (!decoder.decode(frame.payload, headerFields, &errorText))
                {
                    continue;
                }
                for (const HpackHeaderField &field: headerFields)
                {
                    if (field.name == ":status")
                    {
                        statusByStream[frame.header.streamId] = field.value;
                    }
                }
            }
            return statusByStream;
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
         * @brief 拼一条带掩码的客户端帧，负载用 16 位扩展长度（RFC 6455 §5.2）
         * @param opCode 操作码
         * @param payload 负载，长度需落在 (125, 65535] 内
         * @return std::string 完整帧字节。makeMaskedClientFrame() 只支持 7 位长度（≤125 字节），
         *         要往隧道里灌超过一个接收窗口的流量得用这条
         */
        std::string makeExtendedMaskedClientFrame(const std::uint8_t opCode, const std::string_view payload)
        {
            const std::array<std::uint8_t, 4> maskKey{0x12U, 0x34U, 0x56U, 0x78U};
            std::string frameBytes;
            frameBytes.push_back(static_cast<char>(0x80U | opCode));
            frameBytes.push_back(static_cast<char>(0x80U | 126U));
            frameBytes.push_back(static_cast<char>(static_cast<std::uint8_t>(payload.size() >> 8U)));
            frameBytes.push_back(static_cast<char>(static_cast<std::uint8_t>(payload.size() & 0xFFU)));
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

        /**
         * @brief 在时限内轮询等待一个原子标志置位
         * @details 流式请求正文的用例要靠服务端处理器置位来确认「正文没发完，处理器已经跑起来了」，
         *          因此需要一个不依赖网络时序的等待原语
         * @param flag 目标标志
         * @param timeout 等待上限
         * @return true 在时限内置位
         */
        bool waitForFlag(const std::atomic<bool> &flag, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (flag.load(std::memory_order_acquire))
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return flag.load(std::memory_order_acquire);
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
     * @brief 钉住：服务器改了 h2 连接层配置，SETTINGS 通告与各项上限随之改变
     * @details 此前 h2 的限额只能在服务端 SETTINGS 里**观测**、改不动（配置一路按缺省值构造）。
     *          三项取值都故意偏离缺省（100 / 16384 / 16 KiB），因此这条断言不是恒等的：
     *          配置没落到连接层就会退回缺省值而变红
     */
    TEST(Http2CleartextSession, AdvertisesConfiguredSettingsWhenServerOverridesThem)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, {}, [](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
            Http2ConnectionConfiguration configuration;
            configuration.maximumConcurrentStreams = 3;
            configuration.maximumFrameSize         = 32768;
            configuration.maximumHeaderListSize    = 2048;
            server.setHttp2Configuration(configuration);
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";

        CleartextHttp2Client client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() &&
                                                receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        Http2SettingsPayload serverSettings;
        std::string          parseError;
        ASSERT_TRUE(parseHttp2SettingsPayload(frames.front(), serverSettings, &parseError)) << "服务端 SETTINGS 负载解析失败：" << parseError;

        std::uint32_t advertised = 0;
        ASSERT_TRUE(tryGetHttp2Setting(serverSettings, Http2SettingIdentifier::MaxConcurrentStreams, advertised));
        EXPECT_EQ(advertised, 3U) << "最大并发流数没有落到通告里（缺省是 100）";
        ASSERT_TRUE(tryGetHttp2Setting(serverSettings, Http2SettingIdentifier::MaxFrameSize, advertised));
        EXPECT_EQ(advertised, 32768U) << "MAX_FRAME_SIZE 没有落到通告里（缺省是 16384）";
        ASSERT_TRUE(tryGetHttp2Setting(serverSettings, Http2SettingIdentifier::MaxHeaderListSize, advertised));
        EXPECT_EQ(advertised, 2048U) << "MAX_HEADER_LIST_SIZE 没有落到通告里（缺省是 16 KiB）";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
    }

    /**
     * @brief 钉住：非法的 h2 连接层配置在**设置时**就被拒绝
     * @details 若留到第一条连接构造会话时才抛，部署方在启动日志里看不到任何异常，
     *          表现只是「h2 服务时好时坏」——那是最难查的一类失败
     */
    TEST(Http2CleartextSession, RejectsInvalidServerConfigurationAtSetter)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        Http2ConnectionConfiguration tooSmallFrame;
        tooSmallFrame.maximumFrameSize = 8192; // 低于 RFC 7540 §6.5.2 的下界 16384
        EXPECT_THROW(server.setHttp2Configuration(tooSmallFrame), Base::InvalidArgumentException);

        Http2ConnectionConfiguration tooLargeFrame;
        tooLargeFrame.maximumFrameSize = 16777216; // 上界是 16777215
        EXPECT_THROW(server.setHttp2Configuration(tooLargeFrame), Base::InvalidArgumentException);

        Http2ConnectionConfiguration badPush;
        badPush.enablePush = 2;
        EXPECT_THROW(server.setHttp2Configuration(badPush), Base::InvalidArgumentException);

        // 被拒绝的设置不能留下半成品：当前生效的仍是那一份合法配置
        Http2ConnectionConfiguration legal;
        legal.maximumConcurrentStreams = 7;
        server.setHttp2Configuration(legal);
        EXPECT_EQ(server.http2Configuration().maximumConcurrentStreams, 7U);
        EXPECT_EQ(server.http2Configuration().maximumFrameSize, kHttp2DefaultMaximumFrameSize);
    }

    /**
     * @brief 钉住：h2c 也服务静态目录，且首条响应落账后不再攥着那份文件映射
     * @details 静态正文按字节原样上线、验证器与 accept-ranges 齐全，而 h2 侧对「静态目录服务」此前
     *          零直测，先把门面钉住；替换文件的断言再钉住「响应发出后立刻解除映射」这条会话纪律
     *          （同步点用 2xx 落账计数，recordResponse 排在解除之后且中间无挂起点，不靠睡眠赌调度）。
     * @note 自 Windows 侧静态正文改为堆读取之后，两个平台都不会因未解除映射而让替换失败：POSIX 本就
     *       允许替换已映射的文件，Windows 则压根不再为静态正文建映射。这条断言因此只钉得住
     *       「响应能正常发出并收尾」，解除映射那半的证伪能力已随平台改动消失（保留的理由是 POSIX 侧
     *       空闲连接不该长期占着一份 fd 与一段地址空间预留）。
     */
    TEST(Http2CleartextSession, ServesStaticFileAndReleasesMappingAfterResponse)
    {
        const AsynGyanis::TestSupport::TemporaryDirectory directory("H2cStaticFile");
        const std::filesystem::path assetPath = directory.path() / "asset.txt";
        {
            std::ofstream initial(assetPath, std::ios::binary | std::ios::trunc);
            initial << "first-version-body";
        }
        ASSERT_TRUE(std::filesystem::exists(assetPath));

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, {}, [&directory](TestHttpServer &server)
        {
            server.setHttp2CleartextEnabled(true);
            server.staticFileDir(directory.path().string());
        });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";

        CleartextHttp2Client client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";
        ASSERT_TRUE(client.sendBytes(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}), kWaitTimeout));

        // 一条请求一个流号；两次都走同一条连接，第二次只在第一次彻底收尾之后才发出
        const auto requestStaticFile = [&client, &frames](const std::uint32_t streamId, const std::string_view path) -> bool
        {
            if (!client.sendBytes(makeRequestHeadersFrame(streamId, makeGetRequestHeaderBlock(path), true), kWaitTimeout))
            {
                return false;
            }
            return client.pumpUntil(frames,
                                    [streamId](const std::vector<Http2Frame> &receivedFrames)
                                    {
                                        return hasEndStream(receivedFrames, streamId);
                                    },
                                    kWaitTimeout);
        };

        ASSERT_TRUE(requestStaticFile(1U, "/asset.txt")) << "没有在时限内拿到静态文件的首条响应";
        HpackDecoder firstDecoder;
        EXPECT_EQ(findResponseHeaderValue(firstDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "first-version-body") << "静态正文必须按字节原样上线";
        HpackDecoder validatorDecoder;
        EXPECT_FALSE(findResponseHeaderValue(validatorDecoder, frames, 1U, "etag").empty()) << "静态响应要带强验证器";
        EXPECT_FALSE(findResponseHeaderValue(validatorDecoder, frames, 1U, "last-modified").empty());
        EXPECT_EQ(findResponseHeaderValue(validatorDecoder, frames, 1U, "accept-ranges"), "bytes");

        // 等到服务端把这条响应落账：recordResponse 排在会话解除映射之后，且两者之间没有挂起点，
        // 因此计数可见就说明「发送完成后的收尾」已经跑过，替换动作不必赌调度
        ASSERT_TRUE(AsynGyanis::TestSupport::waitForCondition([&fixture]()
        {
            return fixture.server().stats().status2xxCount >= 1U;
        })) << "服务端没有把首条静态响应落账";

        // 发布方的常规做法：写临时文件再 rename 覆盖。响应若还攥着那份映射，Windows 上这一步会被挡下
        const std::filesystem::path replacementPath = directory.path() / "asset.txt.next";
        {
            std::ofstream replacement(replacementPath, std::ios::binary | std::ios::trunc);
            replacement << "second-version-body";
        }
        std::error_code renameError;
        std::filesystem::rename(replacementPath, assetPath, renameError);
        EXPECT_FALSE(static_cast<bool>(renameError)) << "首条响应之后文件仍被占用，替换失败：" << renameError.message();

        ASSERT_TRUE(requestStaticFile(3U, "/asset.txt")) << "替换之后同一条连接的第二次请求没有收尾";
        // 第二条响应刻意不再按名查 :status —— findResponseHeaderValue() 每次只喂被查询那一条流的头块，
        // 而服务端的 HPACK 编码上下文是按连接推进的：首条响应插进动态表的条目在这次孤立解码里并不存在，
        // 索引因此位移、查不到值。这里要钉的是「同一条连接跟上了替换后的内容」，正文字节与
        // 「这条流确实回了头块」两项足够，且都不依赖跨流的 HPACK 状态。
        EXPECT_EQ(responseDataPayload(frames, 3U), "second-version-body") << "同一条连接要能跟上被替换后的内容";
        // 头块按帧类型结构判定，不走 HPACK 取值：上面那条注释说明它拿不到跨流的编码上下文
        bool hasSecondResponseHead = false;
        for (const Http2Frame &frame: frames)
        {
            if (frame.header.streamId == 3U && frame.header.type == Http2FrameType::Headers)
            {
                hasSecondResponseHead = true;
            }
        }
        EXPECT_TRUE(hasSecondResponseHead) << "第二条响应没有回头块";

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
     * @brief 钉住：开关关闭时同一段 h2 前奏得不到 h2 应答——协议由开关决定，而不是「前奏恰好也能被当请求处理」
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
     * @brief 钉住：以尾部头块收尾的请求（trailers 带 END_STREAM）照常被路由——尾部头块也是消息结尾（RFC 9113 §8.1）
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
     * @brief 钉住：优雅关停时 h2 对端收到收尾 GOAWAY（NO_ERROR + 最后流号）而不是裸 TCP 关闭——对端据此知道哪些请求已生效
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
     * @brief 钉住：请求头超出本端上限只作废那一条流并按 431 应答，连接上的后续请求照常
     * @details 依据：RFC 9113 §10.5.1 明确「收不下这一条头块的服务端可以回 431」。旧口径是连接级
     *          ENHANCE_YOUR_CALM，等于任何对端发一条超大头部就能把整条连接上别人的在途请求一起带走。
     */
    TEST(Http2CleartextSession, Answers431ForOversizedRequestHeadersAndKeepsConnection)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, HttpParserLimits{}, [](TestHttpServer &server)
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

        // 一条 9000 字节的头值：远超单条头值的 8 KiB 上限，但整个头块仍在一帧之内（16 KiB）
        std::string requestBytes = makeRequestHeadersFrame(1U,
                                                           makeGetRequestHeaderBlock("/hello") + hpackLiteralField("x-big", std::string(9000U, 'a')),
                                                           true);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !responseHeaderBlock(receivedFrames, 1U, 0).empty();
                                     },
                                     kWaitTimeout)) << "超大头部的请求没有收到应答";
        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "431");
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "一条越限的请求头不该把整条连接判死";
        }

        // 同一条连接随后那条正常请求必须拿到 200：HPACK 上下文没被这次拒绝弄乱
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !responseHeaderBlock(receivedFrames, 3U, 0).empty();
                                     },
                                     kWaitTimeout)) << "越限请求之后的正常请求没有收到应答";
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "200");
    }

    /**
     * @brief 钉住：正文超上限回 413 并请对端中止上传（RST_STREAM(NO_ERROR)），而不是把剩余字节白收一遍；连接照常可用
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
     * @brief 钉住：全局在途正文预算用尽时这条流回 503（而非 413），流结束后额度归还
     * @details 单条报文上限不设，越界只能来自全局预算：这样就把「多流共享一份账」这条口径单独钉住
     */
    TEST(Http2CleartextSession, Answers503WhenInflightBodyBudgetIsExhausted)
    {
        // 预算只够 16 字节，而下面这条 POST 有 64 字节正文
        auto budget = std::make_shared<HttpMemoryBudget>(16);
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, HttpParserLimits{},
                                         [budget](TestHttpServer &server)
                                         {
                                             server.setHttp2CleartextEnabled(true);
                                             server.setMemoryBudget(budget);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";

        CleartextHttp2Client client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 越预算的 POST：头块与正文都不收尾（正文还会继续变多，本端已判定不再需要）
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo"), false);
        requestBytes += encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(64U, 'x')}, 1U);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.type == Http2FrameType::Headers && frame.header.streamId == 1U)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "超出全局预算的请求没有收到应答";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "503")
                << "全局预算用尽应当是 503（本端没余量），不是 413（对端报文越界）";

        // 额度随流的记录一起归还：轮询等一小会儿，因为「客户端读到 503」与「记录被摘掉」之间没有严格顺序
        const auto quotaDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (budget->reservedByteCount() != 0 && std::chrono::steady_clock::now() < quotaDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "流结束后额度仍未归还";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：全局在途正文预算按「多条流之和」判定，而不是每条流各算一份
     * @details 上一条用例只发一条流，分辨不出这两种口径——而「多流共享一份账」正是这笔预算存在的理由。
     *          两条流各上传一段都不收尾，第二条那一段就是越界的那一口
     */
    TEST(Http2CleartextSession, SharesOneBodyBudgetAcrossConcurrentStreams)
    {
        // 预算 100：两条流各 60 字节，单看都合规，加起来就越界
        constexpr std::size_t kFirstStreamBodyBytes = 60;
        constexpr std::size_t kSecondStreamBodyBytes = 60;
        constexpr std::size_t kFirstStreamTailBytes = 10;

        auto budget = std::make_shared<HttpMemoryBudget>(100);

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 1024; // 让全局预算成为唯一的约束，而不是单报文正文上限

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             // 回显路由由用例自己注册：这样「先受理那条的正文被完整收下」
                                             // 才有明确对照，而不是落在未匹配路径的兜底响应上
                                             router.post("/echo", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                             {
                                                 response.setBody(request.body());
                                                 co_return;
                                             });
                                         },
                                         parserLimits,
                                         [budget](TestHttpServer &server)
                                         {
                                             server.setHttp2CleartextEnabled(true);
                                             server.setMemoryBudget(budget);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";

        CleartextHttp2Client client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !receivedFrames.empty() && receivedFrames.front().header.type == Http2FrameType::Settings;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        // 两条流都只发到一半（不带 END_STREAM）：第二条那 60 字节落进来时，第一条的 60 字节还占着额度
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo"), false)
                                 + encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(kFirstStreamBodyBytes, 'x')}, 1U)
                                 + makeRequestHeadersFrame(3U, makePostRequestHeaderBlock("/echo"), false)
                                 + encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(kSecondStreamBodyBytes, 'y')}, 3U);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        // 越界那条不必等对端收尾就当场可判：它拿到 503，而先受理的那条还在等正文收齐
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "超出全局预算的那条流没有收到应答";

        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = true, .data = std::string(kFirstStreamTailBytes, 'x')}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "先受理的那条流补齐正文后没有收到应答";

        const std::map<std::uint32_t, std::string> statusByStream = collectResponseStatuses(frames);
        // 先确认两条响应都解得开：解不开时 .at() 抛的是「查不到键」，那会掩盖真正要判的口径问题
        ASSERT_EQ(statusByStream.count(1U), 1U) << "先受理那条流的响应头块解不开";
        ASSERT_EQ(statusByStream.count(3U), 1U) << "越界那条流的响应头块解不开";
        EXPECT_EQ(statusByStream.at(3U), "503") << "两条流各压 60 字节、预算只有 100，后到的那条没被按 503 收口";
        EXPECT_EQ(statusByStream.at(1U), "200") << "先受理的那条流不该被后到的流量挤掉";
        // 先受理的那条正文完整：70 字节原样回显，说明「停止缓冲」只作用在越界的那条流上
        EXPECT_EQ(responseDataPayload(frames, 1U).size(), kFirstStreamBodyBytes + kFirstStreamTailBytes) << "先受理的那条流的回显正文长度不对";

        const auto quotaDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (budget->reservedByteCount() != 0 && std::chrono::steady_clock::now() < quotaDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "两条流都收口后额度仍未归还";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
    }

    /**
     * @brief 钉住：未注册路径在明文 h2 上同样按 404 收口，状态码与正文同源
     * @details h2 与 h1 共用同一张路由表与同一个兜底处理器，但响应是另一条组头块的路径：
     *          兜底处理器改的是响应对象的状态码，:status 要从它那里取，否则会出现
     *          「正文是 Not Found 而状态行说成功」这种自相矛盾的响应
     */
    TEST(Http2CleartextSession, Answers404WithMatchingStatusForUnregisteredPath)
    {
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, {}, HttpParserLimits{},
                                         [](TestHttpServer &server)
                                         {
                                             server.setHttp2CleartextEnabled(true);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";

        CleartextHttp2Client client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "明文回环连接失败";

        std::vector<Http2Frame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/no-such-path"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "未注册路径的请求没有收到应答";

        const std::map<std::uint32_t, std::string> statusByStream = collectResponseStatuses(frames);
        ASSERT_EQ(statusByStream.count(1U), 1U) << "响应头块解不开";
        EXPECT_EQ(statusByStream.at(1U), "404") << "未注册路径的状态码不是 404，实际为 " << statusByStream.at(1U);
        EXPECT_EQ(responseDataPayload(frames, 1U), "Not Found") << "状态码与正文不同源";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
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
     * @brief 钉住：RFC 8441 扩展 CONNECT 把 h2c 流变成 WebSocket 隧道（200 + 规范 accept 值、帧原样回显、Close 后 END_STREAM 收尾）
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
     * @brief 钉住：隧道提前结束（对端直接断开）时业务处理器要被等完，而不是随隧道帧一起销毁
     * @details 判据与 h1 的收口同一条（HttpSession.h「先唤醒再销毁」那段）：处理器挂在 receive() 上时，
     *          隧道循环因传输层不可用退出后必须唤醒它并等它跑完自己的收尾。直接 co_return 会让业务帧
     *          随本帧一起销毁（Task 的析构无条件 destroy()），等待之后的代码全部丢失，而任何已经投递给
     *          事件循环的恢复动作会指向已释放的帧。
     * @note 时序不靠运气：升级应答在启动业务协程之前就已 flush，服务端在处理对端 FIN 时必然已经走到
     *       receive() 的挂起点，因此这条用例要么钉住「等完」，要么钉住「丢弃」，不会两头跑
     */
    TEST(Http2CleartextSession, AwaitsBusinessHandlerWhenTunnelEndsAbruptly)
    {
        std::atomic<bool> isHandlerResumed{false};
        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {},
                                         [&isHandlerResumed](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/chat", [&isHandlerResumed](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                             {
                                                 response.upgradeToWebSocket([&isHandlerResumed](WebSocketPeer &peer) -> Core::Task<>
                                                 {
                                                     // 不等对端发帧：让隧道在业务正挂着的时候被拆掉
                                                     static_cast<void>(co_await peer.receive());
                                                     isHandlerResumed.store(true);
                                                     co_return;
                                                 });
                                                 co_return;
                                             });
                                         },
                                         HttpParserLimits{}, [](TestHttpServer &server)
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

        // 不发任何隧道帧、也不走关闭握手：直接收掉客户端连接，让隧道的读循环拿到「传输层已不可用」
        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_TRUE(isHandlerResumed.load()) << "业务处理器挂在 receive() 上就被连帧一起销毁了：隧道收尾要先唤醒再等它跑完";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：扩展 CONNECT 用了本端未实现的 :protocol 时回 501（而不是当未知方法回 404/405），且连接照旧可用
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
     * @brief 钉住：隧道期间同连接其它流上的普通请求照常服务（200 + 正文），隧道本身不受影响
     * @details 隧道内联驱动这条连接时，驱动者职责由隧道协程承担：它自己读字节、喂正文，
     *          并把其它流上已收齐的请求就地服务掉——对端因此能在隧道存续期间复用同一条连接
     */
    TEST(Http2CleartextSession, ServesConcurrentStreamsWhileTunnelIsOpen)
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
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "200")
                << "隧道期间其它流应当照常服务，而不是被拒绝";
        EXPECT_EQ(responseDataPayload(frames, 3U), "served-hello") << "其它流的正文串了";

        // 隧道本身不受影响：仍能收发帧，并在 Close 之后正常收尾
        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = makeMaskedClientFrame(0x1U, "still-alive")}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return !responseDataPayload(receivedFrames, 1U).empty();
                                     },
                                     kWaitTimeout)) << "隧道在服务其它流之后失效了";
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
     * @brief 钉住：隧道期间本端照常归还接收窗口——累计流量超过初始窗口 65535 也不会被自己的流控账卡住
     * @details 隧道由会话协程自己驱动读循环，「消费即还窗口」这条连接层契约在那条路径上同样成立。
     *          漏掉它时对端发到第 65535 字节之后就会撞上本端通告的窗口，连接层按 RFC 7540 §6.9.1
     *          以 FLOW_CONTROL_ERROR 收口——隧道看着能用，一上量就断。
     *          客户端逐片等回显再发下一片，因此本用例不会把「对端超发」当成失败原因。
     */
    TEST(Http2CleartextSession, CreditsReceiveWindowWhileTunnelIsOpen)
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

        // 每片 16000 字节（一条 DATA 帧装一条完整的文本帧），共 80000 字节：明显超过初始窗口 65535。
        // 逐片等回显再发下一片，客户端因此始终按本端通告的窗口行事
        constexpr std::size_t kChunkPayloadByteCount = 16000;
        constexpr std::size_t kChunkCount = 5;
        for (std::size_t chunkIndex = 0; chunkIndex < kChunkCount; ++chunkIndex)
        {
            const std::string payload(kChunkPayloadByteCount, static_cast<char>('a' + static_cast<int>(chunkIndex)));
            ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = false,
                                                                              .data = makeExtendedMaskedClientFrame(0x1U, payload)},
                                                             1U),
                                         kWaitTimeout));
            // 回显帧不带掩码、负载超过 125 字节时用 16 位扩展长度：帧头 4 字节 + 负载
            constexpr std::size_t kEchoFrameByteCount = kChunkPayloadByteCount + 4U;
            const std::size_t expectedEchoByteCount = (chunkIndex + 1U) * kEchoFrameByteCount;
            ASSERT_TRUE(client.pumpUntil(frames,
                                         [expectedEchoByteCount](const std::vector<Http2Frame> &receivedFrames)
                                         {
                                             return responseDataPayload(receivedFrames, 1U).size() >= expectedEchoByteCount;
                                         },
                                         kWaitTimeout))
                    << "第 " << chunkIndex + 1U << " 片没有被回显（累计应收 " << expectedEchoByteCount << " 字节）：隧道多半已被流控收口";

            // 客户端也得按收下的字节回窗口：回显占的是服务端的发送窗口（连接级与流级初值都是 65535），
            // 不回的话第 5 片回显根本发不出来，失败原因就与本用例要考的「服务端收」那一侧无关了
            const std::string windowUpdates =
                    encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = static_cast<std::uint32_t>(kEchoFrameByteCount)}, 0U) +
                    encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = static_cast<std::uint32_t>(kEchoFrameByteCount)}, 1U);
            ASSERT_TRUE(client.sendBytes(windowUpdates, kWaitTimeout));
        }

        bool hasGoAway = false;
        bool hasWindowUpdate = false;
        for (const Http2Frame &frame: frames)
        {
            hasGoAway = hasGoAway || frame.header.type == Http2FrameType::GoAway;
            hasWindowUpdate = hasWindowUpdate || frame.header.type == Http2FrameType::WindowUpdate;
        }
        EXPECT_FALSE(hasGoAway) << "按窗口规矩发送的对端不该被收口";
        EXPECT_TRUE(hasWindowUpdate) << "消费了 80000 字节却没有回过一次 WINDOW_UPDATE：隧道路径没有归还接收窗口";

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

    /**
     * @brief 钉住：h2 上流式路由在头部收齐、正文没收完时就已派发，且正文按到达批次交付
     * @details 与 h1 侧 HttpStreamingBody.DispatchesBeforeBodyCompletes 同一条契约，承载换成 DATA 帧：
     *          客户端只发第一段正文（不带 END_STREAM），处理器就必须已经拿到那一段——等 END_STREAM
     *          才整块交给路由的旧行为下，本用例会一直等到超时
     */
    TEST(Http2CleartextSession, StreamsRequestBodyBeforeTheBodyCompletes)
    {
        constexpr std::string_view kFirstPortion  = "first-portion";
        constexpr std::string_view kSecondPortion = "second";

        std::atomic<bool>        hasObservedFirstBatch{false};
        std::atomic<std::size_t> firstBatchLength{0};
        std::atomic<int>         batchCount{0};

        const auto registerRoutes = [&hasObservedFirstBatch, &firstBatchLength, &batchCount](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/stream", [&hasObservedFirstBatch, &firstBatchLength, &batchCount](
                                                        HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                int         batches    = 0;
                while (co_await stream->readNext())
                {
                    if (batches == 0)
                    {
                        // 首段到达即置位：客户端据此确认「正文没发完，处理器已经跑起来了」
                        firstBatchLength.store(stream->chunk().size(), std::memory_order_release);
                        hasObservedFirstBatch.store(true, std::memory_order_release);
                    }
                    totalBytes += stream->chunk().size();
                    ++batches;
                }
                batchCount.store(batches, std::memory_order_release);
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, registerRoutes, HttpParserLimits{},
                                        [](TestHttpServer &server)
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

        // 请求头 + 第一段正文，两帧都不收尾
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/stream"), false);
        requestBytes += encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(kFirstPortion)}, 1U);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        EXPECT_TRUE(waitForFlag(hasObservedFirstBatch, kWaitTimeout))
                << "正文未收完时处理器没有拿到首段：h2 上的流式派发没有发生";
        EXPECT_LE(firstBatchLength.load(std::memory_order_acquire), kFirstPortion.size())
                << "首段里出现了客户端尚未发送的字节";

        // 补上第二段并收尾
        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = true, .data = std::string(kSecondPortion)}, 1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "补正文之后没有拿到最终响应";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "bytes=" + std::to_string(kFirstPortion.size() + kSecondPortion.size()));
        EXPECT_EQ(batchCount.load(std::memory_order_acquire), 2) << "两段正文应当按两个批次交付：拉一次读一次没有生效";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：正文跨多个 DATA 帧时逐字节一致，且流式收尾之后同一条连接还能服务下一条请求
     * @details 第二段请求是本用例的另一半：流式收尾要把未交付的正文按已消费归还窗口，并把不再需要的
     *          流按 RST_STREAM 停掉——这两步写错会留下半死的连接状态，下一条请求就拿不到响应
     */
    TEST(Http2CleartextSession, StreamsMultiFrameBodyAndKeepsConnectionUsable)
    {
        constexpr std::size_t kFramePayloadBytes = 16 * 1024;
        constexpr std::size_t kFrameCount        = 3;
        constexpr std::size_t kTailBytes         = 4;

        std::atomic<std::size_t> observedTotalBytes{0};

        const auto registerRoutes = [&observedTotalBytes](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/stream", [&observedTotalBytes](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }
                std::size_t totalBytes = 0;
                while (co_await stream->readNext())
                {
                    totalBytes += stream->chunk().size();
                }
                observedTotalBytes.store(totalBytes, std::memory_order_release);
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, registerRoutes, HttpParserLimits{},
                                        [](TestHttpServer &server)
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

        // 三段 DATA 拼成一份 48 KiB 的正文（每段都是合法帧长），末段收尾
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/stream"), false);
        for (std::size_t frameIndex = 0; frameIndex < kFrameCount; ++frameIndex)
        {
            const bool isLastFrame = frameIndex + 1 == kFrameCount;
            requestBytes += encodeHttp2DataFrame(
                    Http2DataPayload{.endStream = isLastFrame, .data = std::string(kFramePayloadBytes, static_cast<char>('a' + frameIndex))}, 1U);
        }
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "多帧正文没有被服务完";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "bytes=" + std::to_string(kFramePayloadBytes * kFrameCount));
        EXPECT_EQ(observedTotalBytes.load(std::memory_order_acquire), kFramePayloadBytes * kFrameCount);

        // 第二条请求（流号 3）：流式收尾之后连接必须照旧可用
        std::string secondRequestBytes = makeRequestHeadersFrame(3U, makePostRequestHeaderBlock("/stream"), false);
        secondRequestBytes += encodeHttp2DataFrame(Http2DataPayload{.endStream = true, .data = std::string(kTailBytes, 'z')}, 3U);
        ASSERT_TRUE(client.sendBytes(secondRequestBytes, kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "流式收尾之后同一条连接不再服务后续请求";
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 3U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 3U), "bytes=" + std::to_string(kTailBytes));
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "合规的流式请求不该让连接进入收尾";
        }

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：流式正文越过 maximumBodySize 时回 413，而不是把业务写出的 200 发出去
     */
    TEST(Http2CleartextSession, Answers413WhenStreamedBodyExceedsLimit)
    {
        constexpr std::size_t kLimitBytes   = 8;
        constexpr std::size_t kPayloadBytes = 64;

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = kLimitBytes;

        // 处理器是否跑起来过：越界时状态码要改成 413，但「头部收齐即派发」这条语义不变——
        // 少了这个断言，本用例在「等收齐再交给路由、由收齐路径判超限回 413」的旧行为下同样通过
        std::atomic<bool> hasHandledRequest{false};

        const auto registerRoutes = [&hasHandledRequest](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/stream", [&hasHandledRequest](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                hasHandledRequest.store(true, std::memory_order_release);
                HttpRequestBody *stream = request.bodyStream();
                std::size_t      totalBytes = 0;
                if (stream != nullptr)
                {
                    while (co_await stream->readNext())
                    {
                        totalBytes += stream->chunk().size();
                    }
                }
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, registerRoutes, parserLimits,
                                        [](TestHttpServer &server)
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

        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/stream"), false);
        requestBytes += encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = std::string(kPayloadBytes, 'x')}, 1U);
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "正文越界后没有拿到响应";

        HpackDecoder responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "413")
                << "正文越过上限时应当回 413，而不是把业务写出的 200 发出去";
        EXPECT_EQ(responseDataPayload(frames, 1U), "Payload Too Large");
        EXPECT_TRUE(hasHandledRequest.load(std::memory_order_acquire))
                << "越界也应当先按流式派发把请求交给路由（头部收齐即派发），而不是绕过路由直接回 413";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：接收窗口只在「业务消费」之后归还——慢消费者期间不归还，放行后凭归还才发得出窗口之外的字節
     * @details 客户端在这里是**遵守窗口的**：先只发用一个初始窗口的量，处理器握着首段不放时窗口不该变大；
     *          放行后服务端随消费归还窗口，客户端收到 WINDOW_UPDATE 才发最后一段。
     * @note 前半段（握着不放时没有 WINDOW_UPDATE）单独看不是强断言：吸收本身也受处理器拉取驱动，
     *       「到达即归还」的退化实现同样可能凑不满窗口更新的阈值而通过。真正有判别力的是后半段——
     *       若归还根本不存在，「没有归还就发不出去」会让步进卡在超时上；窗口记账本身则由
     *       HttpStreamBody 的单元用例逐条钉住（见 TestHttpStreamBody）
     */
    TEST(Http2CleartextSession, CreditsReceiveWindowOnlyAfterTheHandlerConsumes)
    {
        // 正文总量取「一个初始窗口 + 一段」：超出的那一段只有在服务端归还窗口之后才发得出去
        constexpr std::size_t kWindowBytes      = kHttp2InitialWindowSizeByteCount;
        constexpr std::size_t kWireFrameBytes   = 16383;
        constexpr std::size_t kWireFrameCount   = kWindowBytes / kWireFrameBytes;
        constexpr std::size_t kBeyondWindowBytes = 4096;
        constexpr auto        kAbsenceCheckWindow = std::chrono::milliseconds{300};

        std::atomic<bool>        hasHeldFirstBatch{false};
        std::atomic<bool>        isHandlerReleased{false};
        std::atomic<std::size_t> observedTotalBytes{0};

        const auto registerRoutes = [&hasHeldFirstBatch, &isHandlerReleased, &observedTotalBytes](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/stream", [&hasHeldFirstBatch, &isHandlerReleased, &observedTotalBytes](
                                                        HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                bool        isFirst    = true;
                while (co_await stream->readNext())
                {
                    if (isFirst)
                    {
                        // 慢消费者：上一段还没被消费（读取器要到下一次 readNext 才丢弃它并归还窗口），
                        // 处理器就在这里停住——服务端此刻不该归还任何接收窗口
                        hasHeldFirstBatch.store(true, std::memory_order_release);
                        isFirst = false;
                        while (!isHandlerReleased.load(std::memory_order_acquire))
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        }
                    }
                    totalBytes += stream->chunk().size();
                }
                observedTotalBytes.store(totalBytes, std::memory_order_release);
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, registerRoutes, HttpParserLimits{},
                                        [](TestHttpServer &server)
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

        // 先发满初始窗口（连接级与流级初值都是 65535，因此这些字节一个都不越界），超出窗口的那段留到放行之后
        std::string requestBytes = makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/stream"), false);
        for (std::size_t frameIndex = 0; frameIndex < kWireFrameCount; ++frameIndex)
        {
            requestBytes += encodeHttp2DataFrame(
                    Http2DataPayload{.endStream = false, .data = std::string(kWireFrameBytes, static_cast<char>('a' + frameIndex))}, 1U);
        }
        ASSERT_TRUE(client.sendBytes(requestBytes, kWaitTimeout));

        ASSERT_TRUE(waitForFlag(hasHeldFirstBatch, kWaitTimeout)) << "处理器没有拿到首段就停下了：流式派发没有发生";

        // 反面：处理器停着没消费，服务端就不该归还窗口
        const auto absenceDeadline = std::chrono::steady_clock::now() + kAbsenceCheckWindow;
        while (std::chrono::steady_clock::now() < absenceDeadline)
        {
            static_cast<void>(client.pumpUntil(frames,
                                               [](const std::vector<Http2Frame> &)
                                               {
                                                   return false;
                                               },
                                               std::chrono::milliseconds{50}));
        }
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::WindowUpdate)
                    << "业务还没消费，服务端就归还了接收窗口：背压没有落在消费上";
        }

        // 正面：放行后随消费归还窗口；客户端等到归还才发超出窗口的那一段
        isHandlerReleased.store(true, std::memory_order_release);
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         for (const Http2Frame &frame: receivedFrames)
                                         {
                                             if (frame.header.type == Http2FrameType::WindowUpdate)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     },
                                     kWaitTimeout)) << "消费之后也没有归还接收窗口：对端的窗口会被一路耗尽，超出窗口的正文永远发不出来";

        ASSERT_TRUE(client.sendBytes(encodeHttp2DataFrame(Http2DataPayload{.endStream = true,
                                                                          .data = std::string(kBeyondWindowBytes, 'z')},
                                                         1U),
                                     kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "放行之后没有拿到最终响应";

        const std::size_t expectedBodyBytes = kWireFrameCount * kWireFrameBytes + kBeyondWindowBytes;
        HpackDecoder      responseDecoder;
        EXPECT_EQ(findResponseHeaderValue(responseDecoder, frames, 1U, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "bytes=" + std::to_string(expectedBodyBytes));
        EXPECT_EQ(observedTotalBytes.load(std::memory_order_acquire), expectedBodyBytes);
        for (const Http2Frame &frame: frames)
        {
            EXPECT_NE(frame.header.type, Http2FrameType::GoAway) << "按窗口规矩发送的对端不该被收口";
        }

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在客户端断开后没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：响应写到一半被对端抽走的连接记一条 writeAbortedConnectionCount，写满收口的不误计
     *
     * @details h2 的落账点是 `flushOutgoingBytes` 里「本侧把连接判死」那一处，与 h1 的
     *          `recordSendFailure` 不是同一段代码，所以两侧各要一条直测。正文用流式一段段的写，
     *          与 h1 那条流式失败用例同形状：客户端只取走第一段就带着未读数据关闭，内核回 RST，
     *          处理器随后某一次挂起的写必然把失败交回来。
     * @warning 不采用「一次给出 4 MiB 整块正文」的写法：Windows 的套接字缓冲能把整块收下，写侧
     *          一次都不报错，读数停在 0（实测）——那种判据实际量的是各平台的缓冲大小。
     */
    TEST(Http2CleartextSession, CountsConnectionAbortedMidResponseBody)
    {
        const auto registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.get("/abort-mid-body", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                // 一段段写到传输层拒绝为止：这样失败一定落在某次写出上，不依赖内核能缓冲多少字节
                response.startChunkedResponse(200);
                const std::string payload(64U * 1024U, 'x');
                for (int round = 0; round < 64; ++round)
                {
                    if (!co_await response.writeChunk(payload))
                    {
                        co_return;
                    }
                }
                co_return;
            });
        };

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, registerRoutes, {},
                                         [](TestHttpServer &server)
                                         {
                                             server.setHttp2CleartextEnabled(true);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";
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
        ASSERT_TRUE(client.sendBytes(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}), kWaitTimeout));

        // 窗口先给足再发请求：不给窗口的话服务端会停在流控上，永远不碰套接字，
        // 这条用例要钉的「写出失败」就根本不会发生（那一路另有对应用例）。
        // 顺序有讲究（RFC 7540 §6.9）：流级 WINDOW_UPDATE 必须排在该流的 HEADERS 之后，
        // 给一条还不存在的流还窗口是连接级错误，服务端会直接回 GOAWAY 把连接收掉
        std::string requestWithCredits;
        requestWithCredits += encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = 4U * 1024U * 1024U}, 0U);
        requestWithCredits += makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/abort-mid-body"), true);
        requestWithCredits += encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = 4U * 1024U * 1024U}, 1U);
        ASSERT_TRUE(client.sendBytes(requestWithCredits, kWaitTimeout)) << "额度与请求未能写入";

        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return std::any_of(receivedFrames.begin(),
                                                           receivedFrames.end(),
                                                           [](const Http2Frame &frame)
                                                           {
                                                               return frame.header.type == Http2FrameType::Data && frame.header.streamId == 1U;
                                                           });
                                     },
                                     kWaitTimeout)) << "一段正文都没拿到：服务端可能压根没开始写";

        // 此刻内核里还压着大量未读字节，直接关闭回的是 RST 而不是优雅 EOF
        client.closeNow();
        ASSERT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在对端断开后没有收口";
        EXPECT_EQ(fixture.server().stats().writeAbortedConnectionCount, 1u)
                << "写出被中途抽走的连接没有记上（或记了多次）：这条连接只该有一份「未发完」的记录";

        // 反向下界：一条完整送到的连接不得被算成「写出被抽走」。同一条用例里读同一个计数，
        // 因此它钉的是「不误计」，而不是另一个装配下的另一份读数
        CleartextHttp2Client healthyClient(listeningPort);
        ASSERT_TRUE(healthyClient.isValid()) << "对照连接失败";
        std::vector<Http2Frame> healthyFrames;
        ASSERT_TRUE(healthyClient.sendBytes(std::string(kHttp2ConnectionPreface) + encodeHttp2SettingsFrame(Http2SettingsPayload{}), kWaitTimeout));
        ASSERT_TRUE(healthyClient.sendBytes(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}), kWaitTimeout));
        ASSERT_TRUE(healthyClient.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(healthyClient.pumpUntil(healthyFrames,
                                            [](const std::vector<Http2Frame> &receivedFrames)
                                            {
                                                return hasEndStream(receivedFrames, 1U);
                                            },
                                            kWaitTimeout)) << "对照请求没有拿到完整响应";
        EXPECT_EQ(fixture.server().stats().writeAbortedConnectionCount, 1u) << "正常写满收口的连接被误计成写出失败";

        healthyClient.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "对照连接没有收口";
        EXPECT_FALSE(fixture.startThrew());
    }

    /**
     * @brief 钉住：响应卡在流控队列里没送完就被对端抽走的连接，也要记一条 writeAbortedConnectionCount
     *
     * @details 这一类比「写出侧失败」更常见，也更难发现：对端不读也不还窗口，正文就停在流的待发
     *          队列里，**一次都没碰过套接字**，因此写侧永远不会报错、也不会留下一行错误日志。
     *          探针那批「读到一半放弃」的连接走的就是这条路，读数若只挂在写失败上就一直是 0。
     */
    TEST(Http2CleartextSession, CountsConnectionAbortedWhileResponseBodyStillBlockedOnFlowControl)
    {
        const auto registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.get("/huge", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.setBody(std::string(4U * 1024U * 1024U, 'x'));
                co_return;
            });
        };

        RunningHttpServerFixture fixture(makeCleartextLimits(), std::chrono::milliseconds{30}, {}, registerRoutes, {},
                                         [](TestHttpServer &server)
                                         {
                                             server.setHttp2CleartextEnabled(true);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";
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
        ASSERT_TRUE(client.sendBytes(encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true}), kWaitTimeout));

        // 刻意只给连接级窗口、不给流级窗口，也不补 WINDOW_UPDATE：服务端送完初始窗口的量之后
        // 就把剩下的正文留在流的队列里，写侧一次都不失败
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/huge"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<Http2Frame> &receivedFrames)
                                     {
                                         return std::any_of(receivedFrames.begin(),
                                                           receivedFrames.end(),
                                                           [](const Http2Frame &frame)
                                                           {
                                                               return frame.header.type == Http2FrameType::Data && frame.header.streamId == 1U;
                                                           });
                                     },
                                     kWaitTimeout)) << "初始窗口内的正文都没送到：这条用例没测到流控停住的那条路";

        client.closeNow();
        ASSERT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话在对端断开后没有收口";
        EXPECT_EQ(fixture.server().stats().writeAbortedConnectionCount, 1u)
                << "响应停在流控队列里就被抽走的连接没记上：这类连接写侧不报错，只有收口时看得见";

        EXPECT_FALSE(fixture.startThrew());
    }
} // namespace AsynGyanis::Net
