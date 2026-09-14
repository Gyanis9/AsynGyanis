/**
 * @file TestHttp3Session.cpp
 * @brief HTTP/3 会话层的用例：本端单向流的绑定、SETTINGS 的产出，以及请求到 Router 的映射
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 前三条只驱动会话本身——单向流的开流口与流数据出口都是测试给的假实现，因此不涉及 ngtcp2
 *          与真实 UDP，考的是会话对 nghttp3 的绑定是否合规矩。最后两条走**真字节**：测试侧自建一条
 *          客户端 nghttp3 连接当对端，请求的头块由它真编成 QPACK、响应也由它真解回来，中间不经 UDP。
 */

#include "Net/Http3/Http3Session.h"

#include "Core/Coroutine/Task.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <nghttp3/nghttp3.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端（服务端）发起的单向流号序列：RFC 9000 §2.1 规定服务端发起的单向流号 ≡ 3 (mod 4)
        constexpr std::int64_t kFirstServerUnidirectionalStreamId = 3;
        constexpr std::int64_t kUnidirectionalStreamIdStep       = 4;

        /// 对端（客户端）发起的单向流号序列：≡ 2 (mod 4)，依次是控制流 2、QPACK 编码流 6、解码流 10
        constexpr std::int64_t kClientControlStreamId      = 2;
        constexpr std::int64_t kClientQpackEncoderStreamId = 6;
        constexpr std::int64_t kClientQpackDecoderStreamId = 10;

        /// 客户端发起的双向流号序列：≡ 0 (mod 4)，第一条就是承载首个请求的那条
        constexpr std::int64_t kFirstRequestStreamId = 0;

        /// HTTP/3 的单向流类型：控制流是 0（RFC 9114 §6.2.1）
        constexpr std::uint8_t kControlStreamType = 0x00;

        /// SETTINGS 帧的帧类型（RFC 9114 §7.2.4）
        constexpr std::uint8_t kSettingsFrameType = 0x04;

        /// 记下会话交给出口的一段流数据
        struct CapturedStreamData
        {
            std::int64_t              streamId{0};        ///< 流号
            std::vector<std::uint8_t> bytes;              ///< 字节
            bool                      isEndStream{false}; ///< 是否收尾
        };

        /**
         * @brief 按「服务端单向流号依次递增」给出流号的假开流口
         */
        class FakeStreamOpener
        {
        public:
            FakeStreamOpener() = default;

            /// 给出一条新的本端单向流号
            [[nodiscard]] std::int64_t operator()()
            {
                const std::int64_t streamId = m_nextStreamId;
                m_nextStreamId += kUnidirectionalStreamIdStep;
                m_openedStreamIds.push_back(streamId);
                return streamId;
            }

            /// 已经被开出来的流号（按开流顺序）
            [[nodiscard]] const std::vector<std::int64_t> &openedStreamIds() const noexcept
            {
                return m_openedStreamIds;
            }

        private:
            std::int64_t              m_nextStreamId{kFirstServerUnidirectionalStreamId}; ///< 下一条流号
            std::vector<std::int64_t> m_openedStreamIds;                                 ///< 已开出的流号
        };

        /**
         * @brief 测试侧的 HTTP/3 客户端：一条客户端 nghttp3 连接
         *
         * @details 与 `TestQuicServer` 里用 ngtcp2 搭客户端同一个道理：被测的是服务端这一侧，
         *          对端用一个独立实现（这里是 nghttp3 的客户端侧）来当。它不碰 UDP 与 QUIC，
         *          字节直接在内存里递来递去，因此考的是 h3 层的编解码与映射。
         */
        class Http3ClientPeer
        {
        public:
            /// 解出来的响应
            struct DecodedResponse
            {
                int                                status{0};     ///< :status
                std::map<std::string, std::string> headers;      ///< 其余头部
                std::string                        body;         ///< 正文
                bool                               isComplete{false}; ///< 是否收到了收尾
            };

            Http3ClientPeer()
            {
                nghttp3_settings settings;
                nghttp3_settings_default(&settings);

                const nghttp3_callbacks callbacks = makeCallbacks();
                if (nghttp3_conn_client_new(&m_connection, &callbacks, &settings, nullptr, this) != 0)
                {
                    m_connection = nullptr;
                    return;
                }

                // 客户端也要有自己的三条单向流：控制流 2、QPACK 编码流 6、解码流 10
                // （少了它们 nghttp3 连自己的 SETTINGS 与 QPACK 指令都发不出去）
                if (nghttp3_conn_bind_control_stream(m_connection, kClientControlStreamId) != 0 ||
                    nghttp3_conn_bind_qpack_streams(m_connection, kClientQpackEncoderStreamId, kClientQpackDecoderStreamId) != 0)
                {
                    nghttp3_conn_del(m_connection);
                    m_connection = nullptr;
                }
            }

            ~Http3ClientPeer()
            {
                if (m_connection != nullptr)
                {
                    nghttp3_conn_del(m_connection);
                    m_connection = nullptr;
                }
            }

            Http3ClientPeer(const Http3ClientPeer &) = delete;

            Http3ClientPeer &operator=(const Http3ClientPeer &) = delete;

            /// 会话是否可用
            [[nodiscard]] bool isUsable() const noexcept
            {
                return m_connection != nullptr;
            }

            /**
             * @brief 提交一条请求，并把由此产生的全部待发字节取出来
             * @param method 方法原文
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节（含控制流与请求流）
             */
            std::vector<CapturedStreamData> submitRequest(const std::string &method, const std::string &path, const std::string &authority)
            {
                // 伪头的名字是编译期常量、取值来自参数，逐条按名字长度给出 namelen
                const auto makeHeaderField = [](const char *const name, const std::string &value)
                {
                    return nghttp3_nv{reinterpret_cast<const std::uint8_t *>(name), reinterpret_cast<const std::uint8_t *>(value.data()),
                                      std::strlen(name), value.size(), NGHTTP3_NV_FLAG_NONE};
                };
                const std::string scheme = "https";
                const std::vector<nghttp3_nv> headerFields{makeHeaderField(":method", method), makeHeaderField(":scheme", scheme),
                                                           makeHeaderField(":authority", authority), makeHeaderField(":path", path)};

                if (nghttp3_conn_submit_request(m_connection, kFirstRequestStreamId, headerFields.data(), headerFields.size(), nullptr, nullptr) !=
                    0)
                {
                    return {};
                }
                return takeOutgoingBytes();
            }

            /// 把服务端回的字节喂进来解出响应
            void receive(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
            {
                if (m_connection == nullptr)
                {
                    return;
                }
                static_cast<void>(
                        nghttp3_conn_read_stream2(m_connection, streamId, data.data(), data.size(), isEndStream ? 1 : 0, m_timestamp++));
            }

            /// 解出来的响应
            [[nodiscard]] const DecodedResponse &response() const noexcept
            {
                return m_response;
            }

        private:
            /**
             * @brief 反复取待发字节直到没有
             * @return std::vector<CapturedStreamData> 按流号分好的片段
             */
            std::vector<CapturedStreamData> takeOutgoingBytes()
            {
                std::vector<CapturedStreamData> chunks;
                std::map<std::int64_t, std::size_t> chunkIndexByStreamId;

                for (std::size_t writeIndex = 0; writeIndex < 64; ++writeIndex)
                {
                    nghttp3_vec vectors[8]{};
                    std::int64_t      streamId = -1;
                    int               isFinal  = 0;
                    const nghttp3_ssize vectorCount = nghttp3_conn_writev_stream(m_connection, &streamId, &isFinal, vectors, 8);
                    if (vectorCount < 0 || (vectorCount == 0 && streamId == -1))
                    {
                        break;
                    }

                    std::size_t totalLength = 0;
                    for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
                    {
                        totalLength += vectors[vectorIndex].len;
                    }

                    // 同一条流可能分几次产出（头块一段、正文一段），这里按流号合并成一条，
                    // 免得下游把「同一流的第二次产出」当成收尾之后的意外字节
                    if (const auto existing = chunkIndexByStreamId.find(streamId); existing != chunkIndexByStreamId.end())
                    {
                        CapturedStreamData &chunk = chunks[existing->second];
                        for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
                        {
                            chunk.bytes.insert(chunk.bytes.end(), vectors[vectorIndex].base, vectors[vectorIndex].base + vectors[vectorIndex].len);
                        }
                        chunk.isEndStream = chunk.isEndStream || isFinal != 0;
                    } else
                    {
                        CapturedStreamData chunk;
                        chunk.streamId = streamId;
                        chunk.isEndStream = isFinal != 0;
                        for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
                        {
                            chunk.bytes.insert(chunk.bytes.end(), vectors[vectorIndex].base, vectors[vectorIndex].base + vectors[vectorIndex].len);
                        }
                        chunkIndexByStreamId.emplace(streamId, chunks.size());
                        chunks.push_back(std::move(chunk));
                    }

                    nghttp3_conn_add_write_offset(m_connection, streamId, totalLength);
                }
                return chunks;
            }

            /// 客户端侧的响应回调：:status 与正文都由这里记下
            static int onReceiveHeader(nghttp3_conn *, std::int64_t, std::int32_t, nghttp3_rcbuf *name, nghttp3_rcbuf *value, std::uint8_t, void *userData,
                                       void *);
            static int onReceiveData(nghttp3_conn *, std::int64_t, const std::uint8_t *data, std::size_t dataLength, void *userData, void *);
            static int onEndStream(nghttp3_conn *, std::int64_t, void *userData, void *);
            static int onStreamClose(nghttp3_conn *, std::int64_t, std::uint64_t, void *userData, void *);
            static void onRandom(std::uint8_t *destination, std::size_t destinationLength);

            static nghttp3_callbacks makeCallbacks() noexcept
            {
                nghttp3_callbacks callbacks{};
                callbacks.recv_header  = onReceiveHeader;
                callbacks.recv_data    = onReceiveData;
                callbacks.end_stream   = onEndStream;
                callbacks.stream_close = onStreamClose;
                callbacks.rand         = onRandom;
                return callbacks;
            }

            nghttp3_conn    *m_connection{nullptr};   ///< 客户端连接
            DecodedResponse  m_response;              ///< 解出来的响应
            nghttp3_tstamp   m_timestamp{1};          ///< read_stream2 的时间戳（单调递增即可）
        };

        int Http3ClientPeer::onReceiveHeader(nghttp3_conn *, std::int64_t, std::int32_t, nghttp3_rcbuf *name, nghttp3_rcbuf *value, std::uint8_t,
                                            void *userData, void *)
        {
            auto *peer = static_cast<Http3ClientPeer *>(userData);
            if (peer == nullptr)
            {
                return 0;
            }
            const nghttp3_vec nameBuffer  = nghttp3_rcbuf_get_buf(name);
            const nghttp3_vec valueBuffer = nghttp3_rcbuf_get_buf(value);
            const std::string headerName(reinterpret_cast<const char *>(nameBuffer.base), nameBuffer.len);
            const std::string headerValue(reinterpret_cast<const char *>(valueBuffer.base), valueBuffer.len);

            if (headerName == ":status")
            {
                peer->m_response.status = std::atoi(headerValue.c_str());
            } else
            {
                peer->m_response.headers[headerName] = headerValue;
            }
            return 0;
        }

        int Http3ClientPeer::onReceiveData(nghttp3_conn *, std::int64_t, const std::uint8_t *data, const std::size_t dataLength, void *userData, void *)
        {
            auto *peer = static_cast<Http3ClientPeer *>(userData);
            if (peer != nullptr)
            {
                peer->m_response.body.append(reinterpret_cast<const char *>(data), dataLength);
            }
            return 0;
        }

        int Http3ClientPeer::onEndStream(nghttp3_conn *, std::int64_t, void *userData, void *)
        {
            auto *peer = static_cast<Http3ClientPeer *>(userData);
            if (peer != nullptr)
            {
                peer->m_response.isComplete = true;
            }
            return 0;
        }

        int Http3ClientPeer::onStreamClose(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *)
        {
            return 0;
        }

        void Http3ClientPeer::onRandom(std::uint8_t *destination, const std::size_t destinationLength)
        {
            // 本用例不考流量分析的抗性，固定字节即可（nghttp3 只要求可预期地给出一些字节）
            for (std::size_t index = 0; index < destinationLength; ++index)
            {
                destination[index] = static_cast<std::uint8_t>(index * 31U + 7U);
            }
        }

        /**
         * @brief 把一条「即时完成」的协程推到结束
         * @details 与 tests/Net/Http 里的既有口径一致：用例中的处理器都不等 I/O，因此首次 resume()
         *          就该跑完；这里多推进几次是为了容下 await 链上的多次让出
         * @param task 待推进的协程
         */
        void resumeUntilReady(Core::Task<> &task)
        {
            for (int resumeIndex = 0; resumeIndex < 16 && !task.isReady(); ++resumeIndex)
            {
                task.handle().resume();
            }
        }
    } // namespace

    /**
     * @brief 会话建立时开三条本端单向流并在控制流上产出 SETTINGS
     */
    TEST(Http3Session, BindsControlAndQpackStreamsThenEmitsSettings)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        // 会话按值存开流口，要观察它到底开了哪些流号就得把本对象按引用交进去（否则填的是副本）
        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        ASSERT_TRUE(session.isUsable()) << "三条本端单向流都开得出来，会话却不是可用状态";
        ASSERT_EQ(opener.openedStreamIds().size(), 3U)
                << "会话应当正好开三条本端单向流（控制流 + QPACK 编码流 + QPACK 解码流）";

        session.flushPendingStreamData();

        ASSERT_FALSE(sentStreamData.empty()) << "会话建好后一段字节都没产出：SETTINGS 没有发出去";
        const CapturedStreamData &settingsFrame = sentStreamData.front();
        EXPECT_EQ(settingsFrame.streamId, opener.openedStreamIds().front())
                << "SETTINGS 应当产在控制流上（也就是第一条开出来的单向流）";
        ASSERT_GE(settingsFrame.bytes.size(), 2U) << "控制流上第一段字节太短，装不下流类型与帧类型";
        EXPECT_EQ(settingsFrame.bytes[0], kControlStreamType) << "单向流的第一字节应当是流类型，控制流为 0";
        EXPECT_EQ(settingsFrame.bytes[1], kSettingsFrameType) << "控制流上的第一个帧应当是 SETTINGS";
    }

    /**
     * @brief 吃下对端控制流上的 SETTINGS 之后会话仍可用
     */
    TEST(Http3Session, ConsumesPeerSettingsAndStaysUsable)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        // 会话按值存开流口，要观察它到底开了哪些流号就得把本对象按引用交进去（否则填的是副本）
        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });
        ASSERT_TRUE(session.isUsable());
        session.flushPendingStreamData();

        // 客户端发起的单向流号 ≡ 2 (mod 4)，第一条就是控制流 2：内容是流类型 0x00 + 长度为 0 的 SETTINGS
        const std::vector<std::uint8_t> peerControlStreamBytes{kControlStreamType, kSettingsFrameType, 0x00};
        session.onStreamData(kClientControlStreamId, peerControlStreamBytes, false);

        EXPECT_FALSE(session.isBroken()) << "吃下对端合法的 SETTINGS 不该把会话弄坏";
        EXPECT_TRUE(session.isUsable());
    }

    /**
     * @brief 开不出本端单向流时会话如实不可用，且不会往出口写任何字节
     */
    TEST(Http3Session, IsUnusableAndSilentWhenNoUnidirectionalStreamCanBeOpened)
    {
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session([] { return std::int64_t{-1}; },
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        EXPECT_FALSE(session.isUsable()) << "单向流都开不出来，会话不该报可用";
        session.flushPendingStreamData();
        EXPECT_TRUE(sentStreamData.empty()) << "会话不可用时不该往出口写任何字节";
    }

    /**
     * @brief 真字节往返：客户端提的请求经 QPACK 编出来，服务端解出来交给 Router，响应再解回客户端
     * @details 头块由客户端 nghttp3 真编、响应由它真解，因此这条用例同时钉住「映射对不对」与
     *          「两端字节能不能互通」——比只调 add* 接口的单元断言强一层
     */
    TEST(Http3Session, DispatchesRequestThroughRouterAndAnswersWithRealBytes)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        // 处理器收到的请求：断言它的字段就是「h1/h2 上同一份业务代码会看到的那一份」
        HttpMethod  observedMethod{HttpMethod::UNKNOWN};
        std::string observedUri;
        std::string observedHost;
        std::string observedVersion;
        Router      router;
        router.get("/hello",
                   [&observedMethod, &observedUri, &observedHost, &observedVersion](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                   {
                       observedMethod  = request.method();
                       observedUri     = std::string(request.uri());
                       observedHost    = request.getHeader("host").value_or("");
                       observedVersion = request.httpVersion();
                       response.setStatus(200);
                       response.setHeader("content-type", "text/plain");
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/hello", "example.com");
        ASSERT_FALSE(requestChunks.empty()) << "客户端没能产出任何字节（请求根本没编出来）";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(observedMethod, HttpMethod::GET) << "方法没有从 :method 映射过来";
        EXPECT_EQ(observedUri, "/hello") << ":path 没有映射成请求 URI";
        EXPECT_EQ(observedHost, "example.com") << ":authority 没有补齐成 host 头";
        EXPECT_EQ(observedVersion, "HTTP/3") << "请求版本号不是 HTTP/3";
        EXPECT_EQ(peer.response().status, 200) << "客户端没解出 200";
        EXPECT_EQ(peer.response().body, "hi") << "正文没有回到客户端";
        EXPECT_TRUE(peer.response().isComplete) << "响应没有收尾";
        const auto contentTypeHeader = peer.response().headers.find("content-type");
        ASSERT_NE(contentTypeHeader, peer.response().headers.end()) << "业务设的响应头没送到";
        EXPECT_EQ(contentTypeHeader->second, "text/plain");
        const auto contentLengthHeader = peer.response().headers.find("content-length");
        ASSERT_NE(contentLengthHeader, peer.response().headers.end()) << "缺正文长度时应当按实际长度补上 content-length";
        EXPECT_EQ(contentLengthHeader->second, "2");
    }

    /**
     * @brief 把请求正文的字节按 DATA 帧归还给 QUIC 的接收窗口
     * @details read_stream2 的消费计数不含 DATA 负载，正文那部分必须单独归还；漏了这条，
     *          正文一大就会把接收窗口用光（对端随后被流控卡住，而本端并不知道为什么）
     */
    TEST(Http3Session, CreditsRequestBodyBytesBackToTheTransport)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::size_t                     creditedByteCount{0};

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             },
                             [&creditedByteCount](const std::int64_t, const std::size_t consumedByteCount) { creditedByteCount += consumedByteCount; });

        const std::vector<std::uint8_t> payload{'a', 's', 'y', 'n'};
        session.addRequestHeader(kFirstRequestStreamId, ":method", "POST");
        session.addRequestHeader(kFirstRequestStreamId, ":path", "/upload");
        session.addRequestBody(kFirstRequestStreamId, payload);

        EXPECT_EQ(creditedByteCount, payload.size()) << "请求正文的字节没有被归还给接收窗口";
    }

    /**
     * @brief 处理器要求流式响应时，h3 侧必须明确失败，而不是把一份错的响应发出去
     */
    TEST(Http3Session, Answers500WhenHandlerAsksForAStreamingResponse)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        Router router;
        router.get("/stream",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       // 分块响应（流式）在 h3 上尚未实现
                       response.startChunkedResponse(200);
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable());
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/stream", "example.com");
        ASSERT_FALSE(requestChunks.empty());
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 500) << "流式响应在 h3 上没实现，应当明确回 500，而不是发出错的响应";
    }
} // namespace AsynGyanis::Net
