// HTTP/3 会话层的用例：本端单向流的绑定、SETTINGS 的产出，以及请求到 Router 的映射 前三条只驱动会话本身——单向流的开流口与流数据出口都是测试给的假实现，因此不涉及 ngtcp2 与真实
// UDP，考的是会话把请求映射到 Router 与响应写回这套接线是否合规矩。最后几条走**真字节**：测试侧自建一条客户端 nghttp3 连接当对端，
// 请求的头块由它真编成 QPACK、响应也由它真解回来，中间不经 UDP——服务端这一侧已经是自研实现，这份对拍正是留着当裁判用的。
#include "Net/Http3/Http3Session.h"

#include "Core/Coroutine/Task.h"
#include "Net/Http/Router.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http3/Qpack.h"
#include "Net/Http3/Http3Frame.h"

#include <gtest/gtest.h>

#include <nghttp3/nghttp3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

        /// 请求的 :scheme 取值：h3 只跑在 TLS 上（放在静态存储期，供伪头构造引用）
        constexpr const char *kRequestScheme = "https";

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
                int                                status{0};     ///< :status（最后一条，即最终响应）
                /// 按到达顺序记下的全部 :status：信息性响应（100/103）会排在最终响应之前
                std::vector<int>                   statuses;
                std::map<std::string, std::string> headers;      ///< 其余头部（同名只留最后一条）
                /// 全部响应字段按到达顺序逐条记下：可重复头（Set-Cookie）只有这里能数出条数
                std::vector<std::pair<std::string, std::string>> headerFields;
                std::string                        body;         ///< 正文
                bool                               isComplete{false}; ///< 是否收到了收尾

                /// 数某个头名出现了几次
                [[nodiscard]] std::size_t countOf(const std::string &name) const
                {
                    return static_cast<std::size_t>(std::ranges::count(headerFields, name, &std::pair<std::string, std::string>::first));
                }
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
             * @param requestStreamId 请求所在的双向流号：一条请求一条流是 HTTP/3 的规矩，
             *        同一对象上再发一条就要换号（客户端发起的双向流是 0、4、8…）
             * @param extraHeaders 伪头之后追加的普通头（本文件用来带 x-request-id）
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节（含控制流与请求流）
             */
            std::vector<CapturedStreamData> submitRequest(const std::string &method, const std::string &path, const std::string &authority,
                                                        const std::int64_t requestStreamId = kFirstRequestStreamId,
                                                        const std::vector<std::pair<std::string, std::string>> &extraHeaders = {})
            {
                std::vector<nghttp3_nv> headerFields = makePseudoHeaders(method, path, authority);
                for (const auto &[name, value]: extraHeaders)
                {
                    headerFields.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t *>(name.data()),
                                                     reinterpret_cast<const std::uint8_t *>(value.data()), name.size(), value.size(),
                                                     NGHTTP3_NV_FLAG_NONE});
                }

                if (nghttp3_conn_submit_request(m_connection, requestStreamId, headerFields.data(), headerFields.size(), nullptr, nullptr) != 0)
                {
                    return {};
                }
                return takeOutgoingBytes();
            }

            /**
             * @brief 提交一条带正文的请求：正文由数据读取回调按批给出
             * @param method 方法原文
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param body 正文
             * @param chunkByteCount 每次回调给出的字节数（分批到达就是这样造出来的）
             * @param extraHeaders 伪头之后追加的普通头（如 expect、content-length）：
             *        nghttp3 只按显式给出的字段编头块，不会自己补这些
             * @return true 请求已提交（字节要靠 takeNextWriteStep() 逐步取出）
             */
            bool submitRequestWithBody(const std::string &method, const std::string &path, const std::string &authority, std::string body,
                                       const std::size_t chunkByteCount,
                                       const std::vector<std::pair<std::string, std::string>> &extraHeaders = {})
            {
                m_requestBody           = std::move(body);
                m_requestBodyOffset     = 0;
                m_requestChunkByteCount = chunkByteCount;

                std::vector<nghttp3_nv> headerFields = makePseudoHeaders(method, path, authority);
                for (const auto &[name, value]: extraHeaders)
                {
                    // nghttp3_nv 只存指针：参数是调用方持有的引用，寿命覆盖到本次提交
                    headerFields.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t *>(name.data()),
                                                     reinterpret_cast<const std::uint8_t *>(value.data()), name.size(), value.size(),
                                                     NGHTTP3_NV_FLAG_NONE});
                }
                nghttp3_data_reader           dataReader{};
                dataReader.read_data = readRequestBody;
                return nghttp3_conn_submit_request(m_connection, kFirstRequestStreamId, headerFields.data(), headerFields.size(), &dataReader,
                                                   this) == 0;
            }

            /**
             * @brief 只取一次写出的字节：把请求分步送到服务端，模拟正文随时间到达
             * @return CapturedStreamData 本次写出的片段；没有待发字节时 streamId 为 -1
             */
            CapturedStreamData takeNextWriteStep()
            {
                CapturedStreamData step;
                step.streamId = -1;

                nghttp3_vec vectors[8]{};
                std::int64_t      streamId = -1;
                int               isFinal  = 0;
                const nghttp3_ssize vectorCount = nghttp3_conn_writev_stream(m_connection, &streamId, &isFinal, vectors, 8);
                if (vectorCount <= 0 || streamId == -1)
                {
                    return step;
                }

                std::size_t totalLength = 0;
                for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
                {
                    step.bytes.insert(step.bytes.end(), vectors[vectorIndex].base, vectors[vectorIndex].base + vectors[vectorIndex].len);
                    totalLength += vectors[vectorIndex].len;
                }
                step.streamId    = streamId;
                step.isEndStream = isFinal != 0;
                nghttp3_conn_add_write_offset(m_connection, streamId, totalLength);
                return step;
            }

            /**
             * @brief 提交一条扩展 CONNECT（RFC 9220）请求，并把 WebSocket 帧当作请求数据随后发出
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param firstWebSocketFrame 隧道建立后要发的第一条 WebSocket 帧字节
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节（含请求头与随后的帧）
             * @note 必须带数据读取回调而不是 nullptr：后者意味着「请求到此结束」，而对端之后还要在
             *       同一条流上发 WebSocket 帧，那时服务端会把 DATA 判成 H3_FRAME_UNEXPECTED。
             *       回调给出最后一条帧之后报「暂时没有」而不是 EOF——隧道到对端关流为止都开着
             */
            /**
             * @brief 提交一条「不带任何请求正文」的扩展 CONNECT：递交即 END_STREAM
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param extraHeaders 伪头之后追加的普通头（本用例用来带 sec-websocket-extensions）
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节
             * @note 与 submitWebSocketTunnel 的区别只有一处：不挂数据读取回调，因此请求在这批字节
             *       之后就收尾——「头与 END_STREAM 同一趟到达」正是这条路径
             */
            std::vector<CapturedStreamData> submitEndedWebSocketTunnel(const std::string &path, const std::string &authority,
                                                                       const std::vector<std::pair<std::string, std::string>> &extraHeaders = {})
            {
                std::vector<nghttp3_nv> headerFields = makePseudoHeaders("CONNECT", path, authority, "websocket");
                for (const auto &[name, value]: extraHeaders)
                {
                    // nghttp3_nv 只存指针：参数由调用方持有，寿命覆盖到本次提交
                    headerFields.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t *>(name.data()),
                                                     reinterpret_cast<const std::uint8_t *>(value.data()), name.size(), value.size(),
                                                     NGHTTP3_NV_FLAG_NONE});
                }
                if (nghttp3_conn_submit_request(m_connection, kFirstRequestStreamId, headerFields.data(), headerFields.size(), nullptr,
                                                nullptr) != 0)
                {
                    return {};
                }
                return takeOutgoingBytes();
            }

            std::vector<CapturedStreamData> submitWebSocketTunnel(const std::string &path, const std::string &authority,
                                                                 std::string firstWebSocketFrame)
            {
                m_tunnelFrames.push_back(std::move(firstWebSocketFrame));

                const std::vector<nghttp3_nv> headerFields = makePseudoHeaders("CONNECT", path, authority, "websocket");
                nghttp3_data_reader           dataReader{};
                dataReader.read_data = readTunnelBody;
                if (nghttp3_conn_submit_request(m_connection, kFirstRequestStreamId, headerFields.data(), headerFields.size(), &dataReader,
                                                this) != 0)
                {
                    return {};
                }
                return takeOutgoingBytes();
            }

            /**
             * @brief 在已建立的隧道上再发一条 WebSocket 帧（同一条流上的后续 DATA）
             * @param webSocketFrame 帧字节
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节
             */
            std::vector<CapturedStreamData> sendWebSocketFrame(std::string webSocketFrame)
            {
                m_tunnelFrames.push_back(std::move(webSocketFrame));
                // 上一批交完之后读回调报的是「暂时没有」，库里正等着这一声才会再来取
                static_cast<void>(nghttp3_conn_resume_stream(m_connection, kFirstRequestStreamId));
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
            static nghttp3_ssize readRequestBody(nghttp3_conn *, std::int64_t, nghttp3_vec *vectors, std::size_t vectorCount, std::uint32_t *flags,
                                                void *connectionUserData, void *streamUserData);
            static nghttp3_ssize readTunnelBody(nghttp3_conn *, std::int64_t, nghttp3_vec *vectors, std::size_t vectorCount, std::uint32_t *flags,
                                               void *connectionUserData, void *streamUserData);

            /**
             * @brief 造出请求的四个伪头
             * @param method 方法原文
             * @param path 路径
             * @param authority 权威主机
             * @return std::vector<nghttp3_nv> 伪头数组（名字是常量、取值来自参数，逐条给出 namelen）
             */
            static std::vector<nghttp3_nv> makePseudoHeaders(std::string_view method, std::string_view path, std::string_view authority,
                                                            std::string_view protocol = {})
            {
                const auto makeHeaderField = [](const char *const name, const std::string_view value)
                {
                    return nghttp3_nv{reinterpret_cast<const std::uint8_t *>(name), reinterpret_cast<const std::uint8_t *>(value.data()),
                                      std::strlen(name), value.size(), NGHTTP3_NV_FLAG_NONE};
                };
                // 取值一律以视图给出：nghttp3_nv 只存指针，而 :scheme 用常量、其余指向调用方的实参，
                // 三者的寿命都覆盖到提交那一刻（曾经把它做成函数内的局部 std::string，返回即悬空）
                std::vector<nghttp3_nv> headerFields{makeHeaderField(":method", method), makeHeaderField(":scheme", kRequestScheme),
                                                     makeHeaderField(":authority", authority), makeHeaderField(":path", path)};
                if (!protocol.empty())
                {
                    // 扩展 CONNECT 用（RFC 9220）：伪头必须排在普通头之前，追加在末尾即可
                    headerFields.push_back(makeHeaderField(":protocol", protocol));
                }
                return headerFields;
            }

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
            std::string      m_requestBody;           ///< 待发的请求正文
            std::size_t      m_requestBodyOffset{0};  ///< 正文已交给 nghttp3 的字节数
            std::size_t      m_requestChunkByteCount{0}; ///< 每次回调给出的正文批大小

            /// 隧道待发的 WebSocket 帧：一条一条交出去，隧道流到对端关流为止都不收尾
            std::deque<std::string> m_tunnelFrames;
            /// 已经交给 nghttp3 的那条帧：库里只借指针，交出去的这条得活到下一次回调
            std::string m_inFlightTunnelFrame;
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
                peer->m_response.statuses.push_back(peer->m_response.status);
            } else
            {
                peer->m_response.headerFields.emplace_back(headerName, headerValue);
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

        nghttp3_ssize Http3ClientPeer::readRequestBody(nghttp3_conn *, std::int64_t, nghttp3_vec *vectors, const std::size_t vectorCount,
                                                      std::uint32_t *flags, void *, void *streamUserData)
        {
            auto *peer = static_cast<Http3ClientPeer *>(streamUserData);
            if (peer == nullptr || vectorCount == 0 || peer->m_requestBodyOffset >= peer->m_requestBody.size())
            {
                *flags = NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }

            const std::size_t remainingByteCount = peer->m_requestBody.size() - peer->m_requestBodyOffset;
            const std::size_t chunkByteCount =
                    (peer->m_requestChunkByteCount < remainingByteCount) ? peer->m_requestChunkByteCount : remainingByteCount;
            vectors[0].base = reinterpret_cast<std::uint8_t *>(peer->m_requestBody.data() + peer->m_requestBodyOffset);
            vectors[0].len  = chunkByteCount;
            peer->m_requestBodyOffset += chunkByteCount;
            // 只有这一批就是最后一批时才收尾，否则后面还有正文
            *flags = (peer->m_requestBodyOffset >= peer->m_requestBody.size()) ? NGHTTP3_DATA_FLAG_EOF : NGHTTP3_DATA_FLAG_NONE;
            return 1;
        }

        nghttp3_ssize Http3ClientPeer::readTunnelBody(nghttp3_conn *, std::int64_t, nghttp3_vec *vectors, const std::size_t vectorCount,
                                                     std::uint32_t *flags, void *connectionUserData, void *)
        {
            auto *peer = static_cast<Http3ClientPeer *>(connectionUserData);
            if (peer == nullptr || vectorCount == 0)
            {
                *flags = NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }
            if (peer->m_tunnelFrames.empty())
            {
                // 隧道还开着、这一批没帧可发：报「暂时没有」而不是 EOF——EOF 会把发送侧关掉，
                // 而隧道要一直开着，之后 sendWebSocketFrame() 会唤醒这里再来取
                return NGHTTP3_ERR_WOULDBLOCK;
            }

            // 库里只借走指针，所以这条帧要挪到成员里活到下一次回调（队列里那份随即销毁）
            peer->m_inFlightTunnelFrame = std::move(peer->m_tunnelFrames.front());
            peer->m_tunnelFrames.pop_front();
            vectors[0].base = reinterpret_cast<std::uint8_t *>(peer->m_inFlightTunnelFrame.data());
            vectors[0].len  = peer->m_inFlightTunnelFrame.size();
            *flags          = NGHTTP3_DATA_FLAG_NONE;
            return 1;
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
     * @brief 畸形请求头：回 400 而不是作废整条连接，也不把请求交给业务
     * @details RFC 9114 §4.1.2 允许服务端在重置之前先答一个错。这条把「连接层判定 → 会话作答」
     *          这一段接起来测——连接层已单测过会发通知，此处钉的是通知真的变成了一个能解开的响应。
     *          非法字节由本层自己的 QPACK 编码器造（nghttp3 客户端不会替我们产出畸形字段名），
     *          而响应仍由 nghttp3 真解回来，判据保持跨实现。
     */
    TEST(Http3Session, AnswersMalformedRequestHeadWithFourHundredAndKeepsConnection)
    {
        FakeStreamOpener opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });
        ASSERT_TRUE(session.isUsable());
        session.flushPendingStreamData();

        bool isHandlerReached = false;
        Router router;
        router.get("/hello",
                   [&isHandlerReached](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       isHandlerReached = true;
                       response.setStatus(200);
                       co_return;
                   });
        session.attachRouter(router);

        std::vector<QpackHeaderField> fieldLines = {
                QpackHeaderField{":method", "GET"},
                QpackHeaderField{":scheme", "https"},
                QpackHeaderField{":authority", "example.com"},
                QpackHeaderField{":path", "/hello"},
                QpackHeaderField{"connection", "keep-alive"}, ///< 连接特定字段：RFC 9114 §4.2 明确禁止
        };
        std::string headerBlock;
        std::string encoderStreamBytes;
        QpackEncoder encoder(0, 0, 0);
        ASSERT_TRUE(encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock, encoderStreamBytes).has_value());
        ASSERT_TRUE(encoderStreamBytes.empty()) << "只用静态表就不该产生编码器流指令，否则这条用例的前提变了";

        // 帧由帧层自己编：长度域是变长整数，手写字节会在载荷超过单字节档时写出自相矛盾的帧
        Http3HeadersFrame headersFrame;
        headersFrame.encodedFieldSection =
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(headerBlock.data()), headerBlock.size());
        std::string requestBytes;
        appendHttp3Frame(requestBytes, headersFrame);
        session.onStreamData(0,
                             std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(requestBytes.data()), requestBytes.size()),
                             true);

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        EXPECT_FALSE(session.isBroken()) << "一个畸形请求不该把整条连接判死";
        EXPECT_FALSE(isHandlerReached) << "畸形的请求不能交到业务手里";
        ASSERT_FALSE(sentStreamData.empty()) << "没有作答：对端只能挂到空闲超时";
        const bool isAnswerOnRequestStream =
                std::ranges::any_of(sentStreamData, [](const CapturedStreamData &chunk) { return chunk.streamId == 0; });
        EXPECT_TRUE(isAnswerOnRequestStream) << "作答没出现在流 0 上：一共只回了 " << sentStreamData.size() << " 段，全是别的流";

        // 作答由本层的帧读取器与 QPACK 解码器解回来：这条要钉的是「会话真的回了一份能解开、
        // 且收尾完整的 400」，跨实现的字节对齐由前面几条 nghttp3 真字节用例负责，这里不重复那份判据
        std::string answerBytes;
        bool isAnswerEnded = false;
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            if (chunk.streamId != 0)
            {
                continue;
            }
            answerBytes.append(reinterpret_cast<const char *>(chunk.bytes.data()), chunk.bytes.size());
            isAnswerEnded = isAnswerEnded || chunk.isEndStream;
        }
        ASSERT_FALSE(answerBytes.empty());

        Http3FrameReader frameReader(64U * 1024U);
        ASSERT_TRUE(frameReader.feed(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(answerBytes.data()),
                                                                    answerBytes.size()))
                            .has_value());
        QpackDecoder answerDecoder(QpackDecoderSettings{.maximumTableCapacityByteCount = 4096,
                                                        .maximumBlockedStreamCount = 100,
                                                        .maximumFieldSectionSizeByteCount = 64U * 1024U});
        std::string statusValue;
        std::string answerBody;
        std::string decoderStreamBytes;
        while (true)
        {
            const auto nextFrame = frameReader.nextFrame();
            ASSERT_TRUE(nextFrame.has_value()) << nextFrame.error().message;
            if (!nextFrame->has_value())
            {
                break;
            }
            if (const auto *headers = std::get_if<Http3HeadersFrame>(&**nextFrame); headers != nullptr)
            {
                std::vector<QpackHeaderField> answerFields;
                const auto decoded = answerDecoder.decodeFieldSection(0, headers->encodedFieldSection, answerFields, decoderStreamBytes);
                ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
                EXPECT_TRUE(*decoded == QpackFieldSectionDecodeStatus::Decoded) << "作答不该依赖动态表";
                for (const auto &field: answerFields)
                {
                    if (field.name == ":status")
                    {
                        statusValue = field.value;
                    }
                }
            } else if (const auto *data = std::get_if<Http3DataFrame>(&**nextFrame); data != nullptr)
            {
                answerBody.append(reinterpret_cast<const char *>(data->payload.data()), data->payload.size());
            }
        }

        EXPECT_EQ(statusValue, "400") << "作答的状态码不是 400";
        EXPECT_FALSE(answerBody.empty()) << "400 应当带上命中的规则，方便对端与运维定位";
        EXPECT_TRUE(isAnswerEnded) << "作答要把这条流收尾，不能让对端等正文";
    }

    /**
     * @brief HEAD 只收头部：正文一个字节都不发，content-length 仍按完整正文给出
     * @details h1/h2 都在发送那一刻把正文换成空（Router 明确把这件事留给会话），h3 此前照发正文，
     *          严格的对端会把它判成畸形响应（RFC 9110 §9.3.2）
     */
    TEST(Http3Session, SuppressesBodyForHeadRequests)
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
        router.get("/bench",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setHeader("content-type", "text/plain");
                       response.setBody("OK");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("HEAD", "/bench", "example.com");
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

        EXPECT_EQ(peer.response().status, 200) << "客户端没解出 200";
        EXPECT_TRUE(peer.response().body.empty()) << "HEAD 响应不该带正文（RFC 9110 §9.3.2）";
        EXPECT_TRUE(peer.response().isComplete) << "HEAD 响应没有收尾";
        const auto contentLengthHeader = peer.response().headers.find("content-length");
        ASSERT_NE(contentLengthHeader, peer.response().headers.end()) << "HEAD 响应仍要给出 content-length";
        EXPECT_EQ(contentLengthHeader->second, "2") << "content-length 必须等于 GET 会发出的那份正文长度";
    }

    /**
     * @brief 走一条完整的 GET 往返，把服务端交出的字节喂回客户端
     * @details 下面几条「响应形状」用例只差业务往响应里写了什么，走完的步子完全一样，
     *          因此把提交请求、喂会话、pump、回喂客户端这四步收在这里
     * @param session 被测会话（由调用方持有，会话不可搬运）
     * @param peer 测试侧的客户端连接
     * @param sentStreamData 会话出口的字节收集容器
     * @param path 请求路径
     * @return 客户端解出来的响应
     */
    Http3ClientPeer::DecodedResponse answerOneGet(Http3Session &session, Http3ClientPeer &peer,
                                                 std::vector<CapturedStreamData> &sentStreamData, const std::string &path)
    {
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", path, "example.com");
        EXPECT_FALSE(requestChunks.empty()) << "客户端没能产出任何字节（请求根本没编出来）";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        EXPECT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        return peer.response();
    }

    /// 一条被本端收口的流：流号与写进 RESET_STREAM / STOP_SENDING 的应用错误码
    struct AbortedStream
    {
        std::int64_t  streamId{0};             ///< 被收口的流
        std::uint64_t applicationErrorCode{0}; ///< RFC 9114 §8.1 那一档的错误码
    };

    /**
     * @brief 造一个只挂了 writer 的会话：出口把字节按流收进 sentStreamData，开流口按本端单向流递增
     * @param opener 假开流口
     * @param sentStreamData 会话出口的字节收集容器
     * @param requestIdGenerator request-id 生成器（可空）
     * @param aborter 流收口出口（可空）：给了就能断言「本端有没有把这条流交代给传输层」
     * @return Http3Session 可按值搬走的会话
     */
    Http3Session makeSession(FakeStreamOpener &opener, std::vector<CapturedStreamData> &sentStreamData,
                             std::shared_ptr<AsynGyanis::Net::HttpRequestIdGenerator> requestIdGenerator = nullptr,
                             Http3Session::StreamAborter aborter = {})
    {
        return Http3Session(std::ref(opener),
                            [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                            {
                                sentStreamData.push_back(
                                        CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                            },
                            {}, nullptr, nullptr, std::move(requestIdGenerator), std::move(aborter));
    }

    /**
     * @brief 造一条带掩码的 WebSocket 文本帧
     * @details 客户端→服务端的帧必须带掩码（RFC 6455 §5.3），测试侧自己拼一份比借用本端的编码器更独立
     * @param textPayload 负载
     * @param maskBytes 4 字节掩码
     * @return std::vector<std::uint8_t> 帧字节（含首字节、长度、掩码与掩蔽后的负载）
     */
    std::vector<std::uint8_t> makeMaskedTextFrame(const std::string_view textPayload, const std::array<std::uint8_t, 4> &maskBytes)
    {
        std::vector<std::uint8_t> frame{0x81U, static_cast<std::uint8_t>(0x80U | textPayload.size())};
        frame.insert(frame.end(), maskBytes.begin(), maskBytes.end());
        for (std::size_t payloadIndex = 0; payloadIndex < textPayload.size(); ++payloadIndex)
        {
            frame.push_back(static_cast<std::uint8_t>(textPayload[payloadIndex]) ^ maskBytes[payloadIndex % maskBytes.size()]);
        }
        return frame;
    }

    /**
     * @brief 可重复头逐条上线，且整段字段行的次序就是业务的设置顺序
     * @details 单值视图是「一名一值」，可重复头只在 headerValues() 里逐条给出：采集时直接用视图，
     *          业务设的第二条 Cookie 会静默消失（h1/h2 都会发全）。按名回查虽然能把值取全，
     *          次序却由那张视图的哈希顺序决定，同名多条会被归到一起——与 h1 的 appendHead 不一致。
     */
    TEST(Http3Session, SendsEveryValueOfRepeatableResponseHeaders)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/cookies",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       // 交错设置：中间夹一条别的头，才能把「同名归组」与「按设置顺序发出」区分开
                       response.setHeader("set-cookie", "first=1");
                       response.setHeader("x-trace", "abc");
                       response.setHeader("set-cookie", "second=2");
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/cookies");
        EXPECT_EQ(response.countOf("set-cookie"), 2U) << "可重复响应头只剩一条，第二条被单值视图吃掉了";
        const std::vector<std::pair<std::string, std::string>> trackedFields = [&response]
        {
            std::vector<std::pair<std::string, std::string>> values;
            for (const auto &[name, value]: response.headerFields)
            {
                if (name == "set-cookie" || name == "x-trace")
                {
                    values.emplace_back(name, value);
                }
            }
            return values;
        }();
        // 逐条比对而不是只数条数：三条的相对次序正是「按设置顺序上线」这条契约
        EXPECT_EQ(trackedFields,
                  (std::vector<std::pair<std::string, std::string>>{{"set-cookie", "first=1"},
                                                                    {"x-trace", "abc"},
                                                                    {"set-cookie", "second=2"}}))
                << "多条同名头的先后顺序要跟着业务的设置顺序，中间夹的头不能被归到后面";
    }

    /**
     * @brief 业务没写 date 时按 h1/h2 同一口径补上（RFC 9110 §6.1 要求源服务器给出）
     */
    TEST(Http3Session, AddsDateHeaderWhenHandlerOmitsIt)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/dated",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setHeader("content-type", "text/plain");
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/dated");
        const auto                             dateHeader = response.headers.find("date");
        ASSERT_NE(dateHeader, response.headers.end()) << "h1/h2 都会自动补 date，h3 漏给会让客户端自己做缓存判定";
        EXPECT_TRUE(dateHeader->second.ends_with("GMT")) << "date 必须是 IMF-fixdate 形态：" << dateHeader->second;
    }

    /**
     * @brief 有正文却没设媒体类型时按纯文本下发（与 HttpResponse::appendHead 的缺省一致）
     */
    TEST(Http3Session, DefaultsContentTypeWhenBodyIsPresent)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/plain",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/plain");
        const auto contentTypeHeader = response.headers.find("content-type");
        ASSERT_NE(contentTypeHeader, response.headers.end()) << "有正文却没设类型，h1/h2 会补 text/plain";
        EXPECT_EQ(contentTypeHeader->second, "text/plain");
    }

    /**
     * @brief 越界的状态码改回 500，而不是把整条流废掉
     * @details setStatus 不校验取值范围，而 :status 必须是三位十进制（RFC 9114 §4.3.2）：
     *          原样交出去会被自己的连接层拒收，这条响应一个字节都发不出去，对端只能干等
     */
    TEST(Http3Session, MapsOutOfRangeStatusCodeBackToFiveHundred)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/bogus",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(1000);
                       response.setBody("boom");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/bogus");
        EXPECT_EQ(response.status, 500) << "越界状态码没有改回 500，而是把这条流的响应废掉了";
        EXPECT_EQ(response.body, "boom");
        EXPECT_TRUE(response.isComplete) << "改回 500 之后这条流仍要正常收尾";
    }

    /**
     * @brief 带 Expect: 100-continue 且声明了正文长度的请求，先收到 100 再收到最终响应
     * @details h1 与 h2 都会先回一个 100 催对端把正文发完（RFC 9110 §10.1.1）；h3 此前不接这个头，
     *          严格等 100 的对端只能靠自己的 expect 超时兜底。信息性响应是一条不带 END_STREAM
     *          的 HEADERS（RFC 9114 §5.3.2），随后才是最终响应
     */
    TEST(Http3Session, AnswersContinueInformationallyBeforeTheFinalResponse)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.post("/upload",
                    [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                    {
                        response.setStatus(201);
                        response.setBody(std::string(request.body()));
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", "abcd", 4,
                                               {{"expect", "100-continue"}, {"content-length", "4"}}))
                << "客户端没能提交这条带正文的请求";
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        const Http3ClientPeer::DecodedResponse response = peer.response();
        ASSERT_EQ(response.statuses.size(), 2U) << "应当有一条信息性响应加一条最终响应，实际收到的状态码序列不符";
        EXPECT_EQ(response.statuses[0], 100) << "第一条应是 100 Continue：对端在等它才敢发正文";
        EXPECT_EQ(response.statuses[1], 201) << "最终响应要照旧给出";
        EXPECT_EQ(response.body, "abcd") << "正文没有完整交给业务";
        EXPECT_TRUE(response.isComplete) << "这条流没有收尾";
    }

    /**
     * @brief 没有 Expect 的请求不会平白收到一个 100
     * @details 拒绝面：100 是给「等着被催」的对端的，给别的请求塞一条会让它多解一段头块
     */
    TEST(Http3Session, DoesNotSendContinueWhenExpectHeaderIsAbsent)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.post("/upload",
                    [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        response.setStatus(201);
                        response.setBody("done");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", "abcd", 4, {{"content-length", "4"}}))
                << "客户端没能提交这条带正文的请求";
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        const Http3ClientPeer::DecodedResponse response = peer.response();
        ASSERT_EQ(response.statuses.size(), 1U) << "没带 Expect 的请求不该收到信息性响应";
        EXPECT_EQ(response.statuses[0], 201);
    }

    /**
     * @brief 业务处理器抛异常时回 500，且会话与后续请求都不受影响
     * @details 异常此前没人接，会穿出 pump()、打断 QuicServer 的收报文循环——整台服务此后
     *          不再处理任何报文。这条用例同时钉住「回 500」与「下一条请求照样正常」两件事
     */
    TEST(Http3Session, Answers500WhenHandlerThrowsAndStaysUsable)
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
        router.get("/boom",
                   [](HttpRequest &, HttpResponse &) -> Core::Task<>
                   {
                       throw std::runtime_error("intentional handler failure");
                   });
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        // 第一条：处理器抛异常，必须是 500（而不是没有响应、也不是异常穿出去）
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/boom", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        // 响应必须真的排出去：异常若穿出 pump()，这里一个字节都不会有（status 会是 0）
        ASSERT_FALSE(sentStreamData.empty()) << "处理器抛异常后一条响应都没发：异常把 pump() 带走了";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 500) << "处理器抛异常没有回 500";
        EXPECT_FALSE(peer.response().body.empty()) << "500 应当带一条可读的正文";

        // 第二条：会话仍然可用，正常请求照常 200（换一条请求流：0 号那条已经用过了）
        // 测试侧的 peer 只记一份响应、正文会跨请求累加，因此按「新增的那一段」核对
        const std::size_t bodyLengthBeforeSecondRequest = peer.response().body.size();
        sentStreamData.clear();
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/hello", "example.com", kFirstRequestStreamId + 4))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> secondPumpTask = session.pump();
        resumeUntilReady(secondPumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "抛过异常之后会话不再服务后续请求";
        ASSERT_GE(peer.response().body.size(), bodyLengthBeforeSecondRequest);
        EXPECT_EQ(peer.response().body.substr(bodyLengthBeforeSecondRequest), "hi") << "第二条响应的正文";
    }

    /**
     * @brief 接上采集端后，h3 的请求数与状态码类如实落账
     * @details 与 h1/h2 同一套口径：收齐的请求计一条、响应按状态码类归档。耗时直方图**不参与**——
     *          h3 各流由传输层驱动，会话没有「收到完整请求」那一刻的戳，宁可不记也不用 0 秒糊弄
     */
    TEST(Http3Session, ReportsRequestsAndStatusClassesToMetricsCollector)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             },
                             Http3Session::StreamCrediter{}, metrics);

        Router router;
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/hello", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        // 服务端的字节要真的喂回客户端，才谈得上「这条请求被正常应答」
        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        ASSERT_EQ(peer.response().status, 200) << "用例前提：这条请求应当被正常应答";

        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.totalRequestCount, 1U) << "h3 的请求没有计入请求数";
        EXPECT_EQ(snapshot.status2xxCount, 1U) << "h3 的 200 响应没有计入 2xx";
        EXPECT_EQ(snapshot.badRequestCount, 0U);
        // 延迟刻意不记：两条计数都不该被写脏
        EXPECT_EQ(snapshot.totalLatencyMicroseconds, 0U) << "h3 不该往耗时直方图里记样本";
        EXPECT_TRUE(std::ranges::all_of(snapshot.latencyBucketCounts, [](const std::uint64_t count) { return count == 0U; }));
    }

    /**
     * @brief 正文越界的 h3 请求：既计请求数，也计一条「坏请求」，响应状态码记 4xx
     */
    TEST(Http3Session, ReportsOversizeRequestAsBadRequestToMetricsCollector)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             },
                             Http3Session::StreamCrediter{}, metrics);

        HttpParserLimits limits;
        limits.maximumBodySize = 8;
        session.setParserLimits(limits);

        Router router;
        router.post("/upload",
                    [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const std::string oversizeBody = "0123456789abcdef"; // 16 字节，超过上限 8
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, oversizeBody.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        ASSERT_EQ(peer.response().status, 413) << "用例前提：这条请求应当被 413 拒绝";

        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.totalRequestCount, 1U) << "被 413 拒掉的请求同样是收齐的 h3 请求，要计入请求数";
        EXPECT_EQ(snapshot.badRequestCount, 1U) << "正文越界应当计一条坏请求（与 h2 同一口径）";
        EXPECT_EQ(snapshot.status4xxCount, 1U) << "413 属于 4xx 类";
    }

    /**
     * @brief 钉住：h3 的请求正文受 HttpParserLimits::maximumBodySize 约束，越界回 413 且不交给业务
     * @details h3 此前完全没有正文上限（h1 有在途预算、h2 有 413），一条 POST 就能把内存吃光。
     *          上限调到 8 字节触发，避免用例为了越界真去分配默认上限那么大的缓冲。
     */
    TEST(Http3Session, RejectsOversizeRequestBodyWith413)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        HttpParserLimits limits;
        limits.maximumBodySize = 8;
        session.setParserLimits(limits);

        bool   isHandlerEntered = false;
        Router router;
        router.post("/upload",
                    [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        isHandlerEntered = true;
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const std::string oversizeBody = "0123456789abcdef"; // 16 字节，超过上限 8
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, oversizeBody.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_FALSE(isHandlerEntered) << "正文越界的请求不该交给业务";
        EXPECT_EQ(peer.response().status, 413) << "正文越界必须回 413（与 h1/h2 同一口径）";
        EXPECT_EQ(peer.response().body, "Payload Too Large");
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
     * @brief h3 上的流式响应（startChunkedResponse + writeChunk）逐块送到客户端
     * @details 顺带钉住两条：流式响应不带 content-length（长度此刻还不知道），以及各块都真的到了
     *          客户端手里
     */
    TEST(Http3Session, StreamsChunkedResponseToTheClient)
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
                       response.startChunkedResponse(200);
                       response.setHeader("content-type", "text/event-stream");
                       if (!co_await response.writeChunk("data: one\n\n"))
                       {
                           co_return;
                       }
                       static_cast<void>(co_await response.writeChunk("data: two\n\n"));
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
        EXPECT_EQ(peer.response().status, 200) << "流式响应的状态码没有送到";
        EXPECT_EQ(peer.response().body, "data: one\n\ndata: two\n\n") << "流式响应的各块没有完整到达客户端";
        EXPECT_TRUE(peer.response().isComplete) << "流式响应没有收尾";
        const auto contentTypeHeader = peer.response().headers.find("content-type");
        ASSERT_NE(contentTypeHeader, peer.response().headers.end());
        EXPECT_EQ(contentTypeHeader->second, "text/event-stream");
        EXPECT_EQ(peer.response().headers.count("content-length"), 0U) << "流式响应的长度此刻还不知道，不该带 content-length";
    }

    /**
     * @brief 流式正文路由在 h3 上真的边收边读：处理器在正文收齐之前就拿到了前几批
     * @details 断言分两步下：先把请求分步送到服务端，只送到「头部 + 第一批」时就要求处理器已经
     *          进去且读到了第一批（这是「不等整份正文」的直接证据）；再把余下的送完，要求它读到
     *          全部字节、按批交付，并且响应是 200。
     */
    TEST(Http3Session, StreamsRequestBodyToTheHandlerBeforeTheBodyIsComplete)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        constexpr std::size_t    kChunkByteCount = 4;
        const std::string        body            = "abcdefghijkl"; // 共 3 批
        std::vector<std::size_t> observedChunkByteCounts;
        bool                     isHandlerEntered = false;

        Router router;
        router.postStreaming("/upload",
                             [&observedChunkByteCounts, &isHandlerEntered](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 isHandlerEntered = true;
                                 while (co_await request.bodyStream()->readNext())
                                 {
                                     observedChunkByteCounts.push_back(request.bodyStream()->chunk().size());
                                 }
                                 response.setStatus(200);
                                 response.setBody("uploaded");
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable());
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", body, kChunkByteCount));

        // 只送到「处理器拿到第一批」为止：此刻正文还远没收齐，处理器却已经进去了
        bool isFirstChunkObserved = false;
        for (std::size_t stepIndex = 0; stepIndex < 16 && !isFirstChunkObserved; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
            isFirstChunkObserved = isHandlerEntered && !observedChunkByteCounts.empty();
        }
        EXPECT_TRUE(isFirstChunkObserved) << "正文收齐之前处理器没拿到第一批：这条路径不是边收边读";
        ASSERT_FALSE(observedChunkByteCounts.empty());
        EXPECT_LT(observedChunkByteCounts.front(), body.size()) << "第一批就是整份正文：说明还是整段收全之后才派的发";

        // 把余下的送完（含收尾），处理器应当读到全部正文
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        std::size_t totalObservedByteCount = 0;
        for (const std::size_t chunkByteCount: observedChunkByteCounts)
        {
            totalObservedByteCount += chunkByteCount;
        }
        EXPECT_EQ(totalObservedByteCount, body.size()) << "处理器读到的正文总量与发出去的不一致";
        EXPECT_GE(observedChunkByteCounts.size(), 2U) << "正文没有按批交付：边收边读没有真正成立";

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "流式正文路由应当正常服务，而不是回错";
        EXPECT_EQ(peer.response().body, "uploaded") << "处理器的响应没有回到客户端";
    }
    /**
     * @brief 对端取消（RESET_STREAM）一条正在收正文的流：会话在下一个安全点把它整条回收
     * @details nghttp3 看不到 QUIC 层的重置信号。少了承载层这一路通知，被取消的请求会连同已攒下的
     *          正文一直留在会话里（对端还能靠归还的 STREAMS 额度反复重来），等正文的处理器更是永远
     *          等不到唤醒。这条用例钉住三件事：处理器被唤醒收尾、请求不再被应答、计入单流取消
     */
    TEST(Http3Session, ReclaimsStreamCancelledByPeer)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             },
                             Http3Session::StreamCrediter{}, metrics);

        constexpr std::size_t    kChunkByteCount = 4;
        const std::string        body            = "abcdefghijkl"; // 共 3 批
        std::vector<std::size_t> observedChunkByteCounts;
        bool                     isHandlerEntered  = false;
        bool                     isHandlerFinished = false;

        Router router;
        router.postStreaming("/upload",
                             [&observedChunkByteCounts, &isHandlerEntered, &isHandlerFinished](HttpRequest &request,
                                                                                              HttpResponse &) -> Core::Task<>
                             {
                                 isHandlerEntered = true;
                                 while (co_await request.bodyStream()->readNext())
                                 {
                                     observedChunkByteCounts.push_back(request.bodyStream()->chunk().size());
                                 }
                                 // 流被取消后 readNext() 必须终止循环；挂在这里就是「等待者永不唤醒」那个缺陷
                                 isHandlerFinished = true;
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", body, kChunkByteCount));

        // 只送到「处理器进入且读到第一批」为止：此刻正文还没收完，这条流是活的
        bool isFirstChunkObserved = false;
        for (std::size_t stepIndex = 0; stepIndex < 16 && !isFirstChunkObserved; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
            isFirstChunkObserved = isHandlerEntered && !observedChunkByteCounts.empty();
        }
        ASSERT_TRUE(isFirstChunkObserved) << "用例前提：处理器应当在正文收齐之前就拿到第一批";

        // 对端放弃这条请求：承载层把 RESET_STREAM 转成这一声通知，回收发生在下一个安全点（pump）
        session.cancelStreamByPeer(kFirstRequestStreamId);
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        EXPECT_TRUE(isHandlerFinished) << "流被取消后等正文的处理器没有醒过来：等待者被留在了已摘掉的记录上";
        const bool hasRequestStreamBytes =
                std::ranges::any_of(sentStreamData, [](const CapturedStreamData &sent) { return sent.streamId == kFirstRequestStreamId; });
        EXPECT_FALSE(hasRequestStreamBytes) << "被取消的流不该再发出任何响应字节";
        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.streamCancelledCount, 1U) << "被对端取消的流没有计入单流取消";
    }


    /**
     * @brief 扩展 CONNECT 与 END_STREAM 同一趟到达时，隧道建立即收尾、响应正常收完
     * @details 头收齐那一刻流号只进了「待建隧道」集合，随后的 END_STREAM 此前被这条分支直接丢掉：
     *          隧道建成后业务永远挂在 receive() 上、对端也拿不到响应收尾。这里钉住「头与 END_STREAM
     *          同趟到达」这条路径——客户端的响应必须收尾（isComplete）
     */
    TEST(Http3Session, ClosesTunnelWhenConnectAndEndStreamArriveTogether)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        bool             isBusinessFinished = false;
        Router           router;
        router.get("/chat",
                   [&isBusinessFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [&isBusinessFinished](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   // 对端一上来就收尾：receive() 必须立刻返回「没有更多消息」，
                                   // 而不是永远挂着
                                   while (const auto message = co_await peer.receive())
                                   {
                                       static_cast<void>(message);
                                   }
                                   isBusinessFinished = true;
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const std::vector<CapturedStreamData> requestChunks = peer.submitEndedWebSocketTunnel("/chat", "example.com");
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
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

        EXPECT_EQ(peer.response().status, 200) << "隧道没有以 2xx 应答";
        EXPECT_TRUE(peer.response().isComplete)
                << "对端已收尾的隧道没有跟着收口：响应永远收不完（业务也醒不过来）";
        EXPECT_TRUE(isBusinessFinished) << "隧道收口后业务没有醒来收尾";
    }

    /**
     * @brief 承载连接没了：还挂在 receive() 上的隧道业务要醒来收尾，会话随后才算空闲
     * @details 传输层收口不会逐条流发信号，这条流上没有「对端取消」那一路通知；少了 abandon 这一步，
     *          会话被摘掉时连着业务协程帧一起销毁，等待之后的收尾永不执行。第二次调用把「业务已跑完
     *          且流已关闭」的记录摘走，承载层据此才敢收连接——两条断言分别钉住唤醒与收敛
     */
    TEST(Http3Session, WakesTunnelBusinessWhenTheCarryingConnectionIsGone)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        bool   isBusinessFinished = false;
        Router router;
        router.get("/chat",
                   [&isBusinessFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [&isBusinessFinished](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   while (const auto message = co_await peer.receive())
                                   {
                                       static_cast<void>(message);
                                   }
                                   isBusinessFinished = true;
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        // 带一条帧而不带 END_STREAM：隧道建起来、业务读完这一条，然后挂在 receive() 上等下一条
        const std::array<std::uint8_t, 4> mask{0x11U, 0x22U, 0x33U, 0x44U};
        const std::vector<std::uint8_t>   firstFrameBytes = makeMaskedTextFrame("hello", mask);
        const std::vector<CapturedStreamData> requestChunks =
                peer.submitWebSocketTunnel("/chat", "example.com", std::string(firstFrameBytes.begin(), firstFrameBytes.end()));
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        ASSERT_TRUE(session.hasOutstandingWork()) << "隧道没建起来，这条用例就没东西可唤醒";

        session.abandonPendingStreams();
        EXPECT_TRUE(isBusinessFinished) << "承载连接收口时没唤醒挂在 receive() 上的隧道业务";

        session.abandonPendingStreams();
        EXPECT_FALSE(session.hasOutstandingWork()) << "业务跑完后会话仍报「有在途工作」：承载层会一直不敢收这条连接";
    }

    /**
     * @brief 正文超出全局在途预算时回 503，且不交给业务（与 h1/h2 同一口径）
     */
    TEST(Http3Session, Answers503WhenInflightBodyBudgetIsExhausted)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      budget = std::make_shared<HttpMemoryBudget>(8); // 只够 8 字节正文

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             },
                             Http3Session::StreamCrediter{}, nullptr, budget);

        bool   isHandlerEntered = false;
        Router router;
        router.post("/upload",
                    [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        isHandlerEntered = true;
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const std::string oversizeBody = "0123456789abcdef"; // 16 字节，超过预算 8
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, oversizeBody.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_FALSE(isHandlerEntered) << "超出全局预算的请求不该交给业务";
        EXPECT_EQ(peer.response().status, 503) << "全局在途预算不足必须回 503（与 h1/h2 同一口径）";
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "被拒的请求不该占着额度（记录析构即归还）";
    }

    /**
     * @brief 排队中与服务中的正文都要占着全局额度，直到这一条服务完才归还
     * @details 额度若在「请求被排进待派发队列」时就归还，排队的正文与正在跑处理器的正文都不再被记账，
     *          多条流各自压一份正文就能把实际占用推过上限——这道限额要挡的正是这个。
     *          判据取两处：请求收齐、还没 pump 时的占用，以及处理器进门时看到的占用
     */
    TEST(Http3Session, KeepsInflightBudgetHeldWhileRequestIsQueuedAndServed)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      budget = std::make_shared<HttpMemoryBudget>(16); // 够一条 10 字节正文，再只剩 6

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             },
                             Http3Session::StreamCrediter{}, nullptr, budget);

        std::size_t reservedAtHandlerEntry = 0;
        Router      router;
        router.post("/upload",
                    [&reservedAtHandlerEntry, budget](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        // 处理器跑起来时，它自己那份正文必须还记在账上
                        reservedAtHandlerEntry = budget->reservedByteCount();
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const std::string body = "0123456789"; // 10 字节，在 16 的预算之内
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", body, body.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        // 请求已收齐、还排在待派发队列里：正文仍在内存里，额度不能先还
        EXPECT_EQ(budget->reservedByteCount(), body.size()) << "请求还在排队，额度就已经归还了";

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(peer.response().status, 200);
        EXPECT_EQ(reservedAtHandlerEntry, body.size()) << "处理器跑起来时没看到自己那份正文占着额度";
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "这一条服务完之后额度应当整份归还";
    }

    /**
     * @brief h3 上跑 WebSocket：扩展 CONNECT（RFC 9220）建隧道，帧在流上原样收发
     * @details 隧道建立之后这条流上跑的就是 WebSocket 帧本身（不是 h3 正文），因此这里手工造一个带
     *          掩码的文本帧喂进去，断言业务把同样的负载回显回来——回显帧由服务端发出，不带掩码，
     *          负载逐字节可比
     */
    TEST(Http3Session, TunnelsWebSocketOverExtendedConnect)
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
        router.get("/chat",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   // 隧道不做任何 h3 编解码：收到一条就原样回一条
                                   while (const auto message = co_await peer.receive())
                                   {
                                       if (!co_await peer.sendText(message->payload))
                                       {
                                           co_return;
                                       }
                                   }
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable());
        // 隧道建立后要发的帧：带掩码的文本帧（构造器见 makeMaskedTextFrame）
        const std::string                 payload = "hello";
        const std::array<std::uint8_t, 4> mask{0x11U, 0x22U, 0x33U, 0x44U};
        const std::vector<std::uint8_t> firstFrameBytes = makeMaskedTextFrame(payload, mask);
        const std::string               webSocketFrameText(firstFrameBytes.begin(), firstFrameBytes.end());

        const std::vector<CapturedStreamData> requestChunks = peer.submitWebSocketTunnel("/chat", "example.com", webSocketFrameText);
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        std::size_t fedServerChunkCount = 0;
        for (; fedServerChunkCount < sentStreamData.size(); ++fedServerChunkCount)
        {
            const CapturedStreamData &written = sentStreamData[fedServerChunkCount];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }
        ASSERT_EQ(peer.response().status, 200) << "隧道没有以 2xx 应答（RFC 9220 里没有 101）";

        // 回显：服务端写出的字节喂给客户端，它解出的 DATA 负载就是回显的 WebSocket 帧
        for (; fedServerChunkCount < sentStreamData.size(); ++fedServerChunkCount)
        {
            const CapturedStreamData &written = sentStreamData[fedServerChunkCount];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }

        const std::string_view echoedFrame = peer.response().body;
        ASSERT_GE(echoedFrame.size(), 2U + payload.size()) << "隧道里没有回显帧：业务没收到帧，或出向帧没发出去";
        EXPECT_EQ(static_cast<std::uint8_t>(echoedFrame[0]), 0x81U) << "回显的不是文本帧";
        EXPECT_EQ(static_cast<std::size_t>(static_cast<std::uint8_t>(echoedFrame[1])), payload.size()) << "回显帧的长度不对";
        EXPECT_EQ(echoedFrame.substr(2, payload.size()), payload) << "回显的负载与发出去的不一致";

        // 第二条帧：首条交付完时出向队列已空、库正等着新数据，这一条能不能出去取决于新数据到达时
        // 有没有把库叫回来（读回调报过「暂时没有」之后，只有 resume_stream 才会再叫它来取）
        const std::string               secondPayload = "world!";
        const std::size_t               sentCountBeforeSecond = sentStreamData.size();
        const std::vector<std::uint8_t> secondFrameBytes = makeMaskedTextFrame(secondPayload, mask);
        for (const CapturedStreamData &chunk: peer.sendWebSocketFrame(std::string(secondFrameBytes.begin(), secondFrameBytes.end())))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        for (std::size_t writtenIndex = sentCountBeforeSecond; writtenIndex < sentStreamData.size(); ++writtenIndex)
        {
            const CapturedStreamData &written = sentStreamData[writtenIndex];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }

        const std::string_view bothEchoes = peer.response().body;
        ASSERT_EQ(bothEchoes.size(), (2U + payload.size()) + (2U + secondPayload.size()))
                << "第二条帧没有回显：首条交付完之后的出向帧没发出去";
        EXPECT_EQ(bothEchoes.substr(2U + payload.size() + 2U, secondPayload.size()), secondPayload)
                << "第二帧的回显负载与发出去的不一致";
    }

    /**
     * @brief 隧道建立时把 permessage-deflate 的协商结论回给对端（README 声称三条通道共用这套协商）
     * @details 只看应答头里那一行：压缩本身由 WebSocketPeer 负责，它的压缩收发已有
     *          TestWebSocketSession 的用例钉住，这里重复一遍不会多出信息
     */
    TEST(Http3Session, EchoesNegotiatedPerMessageDeflateOnTheTunnel)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/chat",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket([](WebSocketPeer &) -> Core::Task<> { co_return; });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const std::vector<CapturedStreamData> requestChunks =
                peer.submitEndedWebSocketTunnel("/chat", "example.com", {{"sec-websocket-extensions", "permessage-deflate; client_max_window_bits"}});
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

        ASSERT_EQ(peer.response().status, 200) << "扩展 CONNECT 应当以 200 应答";
        const auto extensionsHeader = peer.response().headers.find("sec-websocket-extensions");
        ASSERT_NE(extensionsHeader, peer.response().headers.end()) << "对端提了 permessage-deflate，应答里却没回协商结论";
        EXPECT_TRUE(extensionsHeader->second.starts_with("permessage-deflate")) << extensionsHeader->second;
    }

    /**
     * @brief 拒绝面：对端没提扩展时，应答头里不该凭空出现 sec-websocket-extensions
     */
    TEST(Http3Session, OmitsExtensionHeaderWhenPeerOffersNothing)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/chat",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket([](WebSocketPeer &) -> Core::Task<> { co_return; });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        for (const CapturedStreamData &chunk: peer.submitEndedWebSocketTunnel("/chat", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        ASSERT_EQ(peer.response().status, 200) << "扩展 CONNECT 应当以 200 应答";
        EXPECT_EQ(peer.response().headers.count("sec-websocket-extensions"), 0U) << "没协商扩展却回一行，对端会以为要按压缩帧收";
    }

    /**
     * @brief 有生成器时业务与响应头读到的是同一个 request-id，且流式响应的头部也带得上
     * @details 流式响应的头部在处理器第一次写块时就上线了，等处理器返回再设已经来不及——
     *          因此回显必须发生在派发之前，这条用例钉的正是这个时机
     */
    TEST(Http3Session, EchoesRequestIdOnStreamingResponseHead)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session session = makeSession(opener, sentStreamData, std::make_shared<HttpRequestIdGenerator>());

        std::string observedRequestId;
        Router router;
        router.get("/stream",
                   [&observedRequestId](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                   {
                       observedRequestId = std::string(request.requestId());
                       response.startChunkedResponse(200);
                       static_cast<void>(co_await response.writeChunk("data: one\n\n"));
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/stream");

        EXPECT_FALSE(observedRequestId.empty()) << "生成器在场时业务必须读到落定好的 request-id";
        const auto requestIdHeader = response.headers.find("x-request-id");
        ASSERT_NE(requestIdHeader, response.headers.end()) << "响应头里没有 x-request-id";
        EXPECT_EQ(requestIdHeader->second, observedRequestId) << "响应回显的必须正是业务读到的那一份，不能各生成一个";
    }

    /**
     * @brief 客户端带来的链路 id 原样沿用（与 h1/h2 同口径），换掉就断了关联
     */
    TEST(Http3Session, AdoptsClientSuppliedRequestId)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session session = makeSession(opener, sentStreamData, std::make_shared<HttpRequestIdGenerator>());

        Router router;
        router.get("/whoami",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const std::vector<CapturedStreamData> requestChunks =
                peer.submitRequest("GET", "/whoami", "example.com", kFirstRequestStreamId, {{"x-request-id", "trace-me"}});
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

        const auto requestIdHeader = peer.response().headers.find("x-request-id");
        ASSERT_NE(requestIdHeader, peer.response().headers.end());
        EXPECT_EQ(requestIdHeader->second, "trace-me") << "客户端给的合法 id 被换掉了，上游的链路关联就此断掉";
    }

    /**
     * @brief 拒绝面：没接生成器时不该凭空造一个 x-request-id
     */
    TEST(Http3Session, OmitsRequestIdWhenNoGeneratorIsShared)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/plain",
                   [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                   {
                       EXPECT_TRUE(request.requestId().empty()) << "用例前提：没生成器时 id 保持空";
                       response.setStatus(200);
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/plain");
        EXPECT_EQ(response.headers.count("x-request-id"), 0U) << "空 id 也要回显，等于给对端一个空头";
    }

    /**
     * @brief 单连接请求条数到量后 h3 会排空：答完在途的，就不再受理新流
     * @details h1/h2 早就有 maximumRequestsPerConnection（回完当前响应即收口），h3 此前无上限：
     *          一条长连接可以被无限期复用。这里把上限调到 2，避免用例真去刷满默认值
     */
    TEST(Http3Session, DrainsConnectionAfterThePerRequestLimit)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        const auto limits = std::make_shared<HttpServerLimits>();
        limits->maximumRequestsPerConnection = 2;
        session.setServerLimits(limits);

        Router router;
        router.get("/limit",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        // 一条请求一轮：提交、喂会话、派发、把服务端的字节交回客户端
        const auto serveOne = [&](const std::int64_t requestStreamId)
        {
            sentStreamData.clear();
            const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/limit", "example.com", requestStreamId);
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
        };

        serveOne(kFirstRequestStreamId);
        EXPECT_EQ(peer.response().status, 200);
        EXPECT_FALSE(session.isDrainedAndFinished()) << "只答了一条，远没到上限";

        serveOne(4);
        EXPECT_EQ(peer.response().status, 200) << "上限这条响应本身必须照常答完";
        EXPECT_TRUE(session.isDrainedAndFinished()) << "到量之后应当已通告排空且手上没活";

        // 第三条走新流号：排空之后不该再有它的任何字节
        serveOne(8);
        EXPECT_FALSE(std::ranges::any_of(sentStreamData, [](const CapturedStreamData &chunk) { return chunk.streamId == 8; }))
                << "GOAWAY 之后的新流不该被处理，更不该往回写东西";
    }

    /**
     * @brief 排空通告之后的新流要显式取消，错误码是 H3_REQUEST_REJECTED（RFC 9114 §5.2 的 SHOULD）
     * @details 只「不处理」的话对端看不出这条流被判死了，只能等连接收尾；显式取消让客户端立刻
     *          知道该换一条连接重试。上一条用例只管服务端不回字节，这里核对的是它有没有交代给传输层
     */
    TEST(Http3Session, CancelsStreamsArrivingAfterTheDrainAnnouncement)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session                    session = makeSession(
                opener, sentStreamData, nullptr,
                [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        const auto limits = std::make_shared<HttpServerLimits>();
        limits->maximumRequestsPerConnection = 1;
        session.setServerLimits(limits);

        Router router;
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";

        // 第一条：答完之后连接就该通告排空
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/hello", "example.com", kFirstRequestStreamId))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> firstPumpTask = session.pump();
        resumeUntilReady(firstPumpTask);
        ASSERT_TRUE(session.isDrainedAndFinished()) << "用例前提：这一条答完就该通告排空";
        ASSERT_TRUE(abortedStreams.empty()) << "正常答完的流不该被收口";

        // 第二条：通告之后的新流，只喂进来、不该有响应，但要被显式取消
        sentStreamData.clear();
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/late", "example.com", kFirstRequestStreamId + 4))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> latePumpTask = session.pump();
        resumeUntilReady(latePumpTask);

        EXPECT_FALSE(std::ranges::any_of(sentStreamData, [](const CapturedStreamData &chunk) { return chunk.streamId == kFirstRequestStreamId + 4; }))
                << "通告之后的新流不该回任何字节";
        ASSERT_EQ(abortedStreams.size(), 1U) << "这条流没有被显式取消：对端只能等到连接收尾才知道结果";
        EXPECT_EQ(abortedStreams.front().streamId, kFirstRequestStreamId + 4);
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, 0x010bU) << "拒绝一条排空后的请求该用 H3_REQUEST_REJECTED";
    }

    /**
     * @brief 正文越界时立刻回 413，不等对端收尾；回完还请对端别再发正文
     * @details 此前 h3 的 413 排在「请求收齐」之后：客户端一边分批 dribble 一边等，响应永远不来，
     *          这条流就这么挂着。h2 的判据是 isReadyToServe 里带上 isBodyTooLarge，这里对齐它，
     *          并在响应完整交给传输层之后请对端停发（h2 那边同样是发完才 abortStream）
     */
    TEST(Http3Session, AnswersPayloadTooLargeBeforeTheBodyEndsAndAsksPeerToStop)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session                    session = makeSession(
                opener, sentStreamData, nullptr,
                [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 8;
        session.setParserLimits(parserLimits);

        bool isHandlerEntered = false;
        Router router;
        router.post("/upload",
                    [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        isHandlerEntered = true;
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const std::string oversizeBody(32, 'x');
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, 4));

        // 只喂到「还没收尾」为止：最后那片带着 END_STREAM，喂进去就测不出「不等收尾」这件事
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1 || step.isEndStream)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：413 没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(peer.response().status, 413) << "正文越界没能在请求收尾之前就回掉";
        EXPECT_TRUE(peer.response().isComplete) << "413 的响应要收尾";
        EXPECT_FALSE(isHandlerEntered) << "越界的请求不该交给业务";
        ASSERT_EQ(abortedStreams.size(), 1U) << "响应已发出，却没请对端停发剩余正文";
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, 0x0100U) << "这是无错的收尾请求（H3_NO_ERROR），不是错误";
    }

    /**
     * @brief 请求一直收不齐：过 readTimeout 就收口这条流，不连坐同连接的其它请求
     * @details h1/h2 撞到这个时限是掐掉整条连接，h3 多路复用是常态，只处置那一条流。时限取 1 毫秒，
     *          再往里注入一个未来的「本拍时刻」，用例因此是确定的，不靠睡眠等超时
     */
    TEST(Http3Session, AbortsRequestThatStopsArrivingWithinTheReadDeadline)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session                    session = makeSession(
                opener, sentStreamData, nullptr,
                [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        const auto limits = std::make_shared<HttpServerLimits>();
        limits->readTimeout = std::chrono::milliseconds{1};
        session.setServerLimits(limits);

        bool isHandlerEntered = false;
        Router router;
        router.get("/hello",
                   [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       isHandlerEntered = true;
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/hello", "example.com");
        ASSERT_FALSE(requestChunks.empty());
        // 请求流号取常量：peer 交出的第一段往往是本端单向流（控制流 2、编码器流 6）的字节
        const std::int64_t requestStreamId = kFirstRequestStreamId;
        // 头交进去、但强行不收尾：这条请求永远差一个 END_STREAM
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, false);
        }

        session.expireStaleRequests(std::chrono::steady_clock::now() + std::chrono::seconds{5});

        ASSERT_EQ(abortedStreams.size(), 1U) << "过点的请求没有被收口：它会一直占着这条流";
        EXPECT_EQ(abortedStreams.front().streamId, requestStreamId);
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, 0x010cU) << "本端放弃一条请求该用 H3_REQUEST_CANCELLED";
        EXPECT_TRUE(std::ranges::none_of(sentStreamData, [requestStreamId](const CapturedStreamData &chunk)
                                         { return chunk.streamId == requestStreamId; }))
                << "没收齐的请求不该回任何字节（没有 :method/:path 可派发的半成品响应）";
        EXPECT_FALSE(isHandlerEntered) << "过点的请求不该交给业务";
        EXPECT_FALSE(session.hasOutstandingWork()) << "过点的流要连同记账一起摘掉，否则排空永远等不完";
    }

    /**
     * @brief 还在 readTimeout 之内的请求照常能答：时限判定不能把活着的请求误杀
     */
    TEST(Http3Session, KeepsRequestThatIsStillWithinTheReadDeadline)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session                    session = makeSession(
                opener, sentStreamData, nullptr,
                [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        const auto limits = std::make_shared<HttpServerLimits>();
        limits->readTimeout = std::chrono::seconds{60};
        session.setServerLimits(limits);

        Router router;
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.isUsable()) << "测试侧的客户端 h3 连接没建起来";
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/hello", "example.com");
        ASSERT_FALSE(requestChunks.empty());
        // 请求流号取常量：peer 交出的第一段往往是本端单向流（控制流 2、编码器流 6）的字节
        const std::int64_t requestStreamId = kFirstRequestStreamId;
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, false);
        }

        // 本拍时刻就取「现在」：一分钟的预算不该被判定过点
        session.expireStaleRequests(std::chrono::steady_clock::now());
        EXPECT_TRUE(abortedStreams.empty()) << "没到时限的请求被误杀了";

        // 再把这条流收尾：请求照常走完整轮，拿到 200
        const std::vector<std::uint8_t> noBytes;
        session.onStreamData(requestStreamId, std::span<const std::uint8_t>(noBytes), true);
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "差一个收尾的请求，补上收尾之后就该正常应答";
    }
} // namespace AsynGyanis::Net
