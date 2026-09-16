#include "Net/Http3/Http3Session.h"

#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpChunkFrame.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http/Router.h"

#include <chrono>
#include <cstring>

#include <nghttp3/nghttp3.h>

namespace AsynGyanis::Net
{
    namespace
    {
        /// HTTP/3 响应里唯一必须由本端补上的头：状态伪头（RFC 9114 §4.3.2）
        constexpr const char *kStatusHeaderName = ":status";

        /// 当前单调时钟的纳秒读数（nghttp3 的时间戳口径与 ngtcp2 一致：单调、纳秒）
        nghttp3_tstamp currentTimestamp() noexcept
        {
            return static_cast<nghttp3_tstamp>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /// 收到请求正文分片：攒起来并把这段字节的接收额度还掉
        int receiveDataCallback(nghttp3_conn *, std::int64_t streamId, const std::uint8_t *data, std::size_t dataLength, void *connectionUserData,
                                void *) noexcept
        {
            auto *session = static_cast<Http3Session *>(connectionUserData);
            if (session != nullptr)
            {
                session->addRequestBody(streamId, std::span<const std::uint8_t>(data, dataLength));
            }
            return 0;
        }

        /// 收到一个头字段：伪头（:method 等）与普通头都由这里转交
        int receiveHeaderCallback(nghttp3_conn *, std::int64_t streamId, std::int32_t, nghttp3_rcbuf *name, nghttp3_rcbuf *value, std::uint8_t,
                                  void *connectionUserData, void *) noexcept
        {
            auto *session = static_cast<Http3Session *>(connectionUserData);
            if (session == nullptr)
            {
                return 0;
            }
            const nghttp3_vec nameBuffer  = nghttp3_rcbuf_get_buf(name);
            const nghttp3_vec valueBuffer = nghttp3_rcbuf_get_buf(value);
            // nghttp3 的缓冲只在本回调期间有效，所以这里立刻拷成自己的字符串
            session->addRequestHeader(streamId, std::string(reinterpret_cast<const char *>(nameBuffer.base), nameBuffer.len),
                                      std::string(reinterpret_cast<const char *>(valueBuffer.base), valueBuffer.len));
            return 0;
        }

        /// 一个头块开始：本类不需要在头块边界做事
        int beginHeadersCallback(nghttp3_conn *, std::int64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 一个头块结束：头已收齐，方法/路径此刻可判——命中流式正文路由就在这里派发
        int endHeadersCallback(nghttp3_conn *, const std::int64_t streamId, int, void *const connectionUserData, void *) noexcept
        {
            if (auto *session = static_cast<Http3Session *>(connectionUserData); session != nullptr)
            {
                session->beginStreamingRequestIfMatched(streamId);
            }
            return 0;
        }

        /// 一条流的接收侧关闭：对服务端来说就是「请求收全了」，整理成 HttpRequest 排队
        int endStreamCallback(nghttp3_conn *, std::int64_t streamId, void *connectionUserData, void *) noexcept
        {
            auto *session = static_cast<Http3Session *>(connectionUserData);
            if (session != nullptr)
            {
                session->finishRequest(streamId);
            }
            return 0;
        }

        /// 一条流彻底关闭：该流的本地状态（含待发正文）都可以丢了
        int streamCloseCallback(nghttp3_conn *, std::int64_t streamId, std::uint64_t, void *connectionUserData, void *) noexcept
        {
            auto *session = static_cast<Http3Session *>(connectionUserData);
            if (session != nullptr)
            {
                session->dropRequest(streamId);
            }
            return 0;
        }

        /// 被流间同步挡住的字节终于被消费了
        int deferredConsumeCallback(nghttp3_conn *, std::int64_t, std::size_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 本端发出去的流数据被对端确认：本类按流关闭统一清理，不在这里动
        int acknowledgedStreamDataCallback(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 对端要求本端停止发送
        int stopSendingCallback(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// nghttp3 要求本端重置一条流，尚未收全的请求随这次重置作废
        int resetStreamCallback(nghttp3_conn *, std::int64_t streamId, std::uint64_t, void *connectionUserData, void *) noexcept
        {
            auto *session = static_cast<Http3Session *>(connectionUserData);
            if (session != nullptr)
            {
                session->noteStreamResetByPeer(streamId);
                session->dropRequest(streamId);
            }
            return 0;
        }

        /// 对端发起连接级收口
        int shutdownCallback(nghttp3_conn *, std::int64_t, void *) noexcept
        {
            return 0;
        }

        /// 收到对端的 SETTINGS
        int receiveSettingsCallback(nghttp3_conn *, const nghttp3_settings *, void *) noexcept
        {
            return 0;
        }

        /**
         * @brief 把待发响应的正文交给 nghttp3
         * @details nghttp3 只是把 `nghttp3_vec` 收下（不拷贝），因此这里给出的是会话持有的那一段字节，
         *          它会一直活到流关闭（丢包重传时 nghttp3 还会再用一次）。给了 EOF 之后本回调不会再被调。
         */
        nghttp3_ssize readResponseBodyCallback(nghttp3_conn *, std::int64_t, nghttp3_vec *vectors, std::size_t vectorCount, std::uint32_t *flags,
                                              void *, void *streamUserData) noexcept
        {
            if (streamUserData == nullptr || vectorCount == 0)
            {
                *flags = NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }

            auto *body = static_cast<Http3Session::OutgoingBody *>(streamUserData);
            if (body->offset >= body->bytes.size())
            {
                *flags = NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }

            vectors[0].base = reinterpret_cast<std::uint8_t *>(body->bytes.data() + body->offset);
            vectors[0].len  = body->bytes.size() - body->offset;
            body->offset    = body->bytes.size();
            *flags          = NGHTTP3_DATA_FLAG_EOF;
            return 1;
        }

        /**
         * @brief 流式响应的正文读取回调
         * @details 从会话持有的分片队列里取一片交出去；交出去的整片要等传输层接走之后才会丢
         *          （见 noteStreamingResponseDrained），因此缓冲不随响应时长无限增长。
         *          这里只交付队列头一片：nghttp3 把没写完的 vec 留在自己的 outq 里，下一次写会接着取。
         */
        nghttp3_ssize readStreamingResponseBody(nghttp3_conn *, std::int64_t, nghttp3_vec *vectors, std::size_t vectorCount,
                                               std::uint32_t *flags, void *, void *streamUserData) noexcept
        {
            auto *state = static_cast<Http3Session::StreamingResponse *>(streamUserData);
            if (state == nullptr || vectorCount == 0)
            {
                *flags = NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }

            if (state->chunks.empty() || state->headOffset >= state->chunks.front().size())
            {
                if (state->isFinished)
                {
                    // 写完了也没数据了：报 EOF 收尾（不带 NO_END_STREAM 才会真关掉发送侧）
                    *flags = NGHTTP3_DATA_FLAG_EOF;
                    return 0;
                }

                // 还没写完、这一批又没数据可交：报「暂时没有」。必须用 WOULDBLOCK——库里会保住这次
                // 数据请求、等 resume_stream 再叫；改报 EOF 则会被当成「正文已写完」，数据请求也被消费掉，
                // 之后的块再也发不出去（实测：隧道第二条帧、SSE 第二段都会卡死在首段之后）
                return NGHTTP3_ERR_WOULDBLOCK;
            }

            const std::string &headChunk = state->chunks.front();
            vectors[0].base              = reinterpret_cast<std::uint8_t *>(const_cast<char *>(headChunk.data()) + state->headOffset);
            vectors[0].len               = headChunk.size() - state->headOffset;
            state->headOffset            = headChunk.size();
            // 最后一片且已写完：这一批就是正文末尾，报 EOF 收尾
            *flags = state->isFinished && state->chunks.size() == 1 ? NGHTTP3_DATA_FLAG_EOF : NGHTTP3_DATA_FLAG_NONE;
            return 1;
        }

        /**
         * @brief 填好 nghttp3 的回调表
         * @details HTTP/2 侧的帧状态机是手写的，这里不重复那套：h3 的帧与 QPACK 全交给 nghttp3，
         *          本表只把它的通知接住。返回值非 0 会被 nghttp3 当成致命错误，因此除转发外一律返回 0。
         * @return nghttp3_callbacks 回调表
         */
        nghttp3_callbacks makeCallbacks() noexcept
        {
            nghttp3_callbacks callbacks{};
            callbacks.acked_stream_data = acknowledgedStreamDataCallback;
            callbacks.stream_close      = streamCloseCallback;
            callbacks.recv_data         = receiveDataCallback;
            callbacks.deferred_consume  = deferredConsumeCallback;
            callbacks.begin_headers     = beginHeadersCallback;
            callbacks.recv_header       = receiveHeaderCallback;
            callbacks.end_headers       = endHeadersCallback;
            callbacks.end_stream        = endStreamCallback;
            callbacks.stop_sending      = stopSendingCallback;
            callbacks.reset_stream      = resetStreamCallback;
            callbacks.shutdown          = shutdownCallback;
            callbacks.recv_settings     = receiveSettingsCallback;
            return callbacks;
        }
    } // namespace

    Http3Session::Http3Session(StreamOpener opener, StreamWriter writer, StreamCrediter crediter,
                               std::shared_ptr<HttpMetricsCollector> metrics) :
        m_writer(std::move(writer)), m_crediter(std::move(crediter)), m_metrics(std::move(metrics))
    {
        if (!opener || !m_writer)
        {
            LOG_ERROR("Http3Session: 缺少单向流的开流口或流数据出口，HTTP/3 会话不可用");
            return;
        }

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        // RFC 9220 的扩展 CONNECT（WebSocket 隧道）：不开这一项，nghttp3 会把带 :protocol 的
        // CONNECT 请求判成非法，隧道根本建立不起来
        settings.enable_connect_protocol = 1;

        const nghttp3_callbacks callbacks = makeCallbacks();
        if (nghttp3_conn_server_new(&m_connection, &callbacks, &settings, nullptr, this) != 0)
        {
            m_connection = nullptr;
            LOG_ERROR("Http3Session: nghttp3 服务端会话创建失败，HTTP/3 会话不可用");
            return;
        }

        // 三条本端发起的单向流：控制流、QPACK 编码流、QPACK 解码流。流号由传输层开出来
        // （ngtcp2 不认识这三条流的话，往它们上面写数据会被 STREAM_NOT_FOUND 拒掉）
        const std::int64_t controlStreamId      = opener();
        const std::int64_t qpackEncoderStreamId = opener();
        const std::int64_t qpackDecoderStreamId = opener();
        if (controlStreamId < 0 || qpackEncoderStreamId < 0 || qpackDecoderStreamId < 0)
        {
            LOG_WARN("Http3Session: 开本端单向流失败，HTTP/3 会话不可用");
            return;
        }

        // 绑定顺序按 nghttp3 的接口来：控制流单独绑，两条 QPACK 流一次绑（编码流在前）
        if (nghttp3_conn_bind_control_stream(m_connection, controlStreamId) != 0 ||
            nghttp3_conn_bind_qpack_streams(m_connection, qpackEncoderStreamId, qpackDecoderStreamId) != 0)
        {
            LOG_WARN("Http3Session: 控制流或 QPACK 流绑定失败，HTTP/3 会话不可用");
            return;
        }

        m_isUsable = true;
        LOG_DEBUG_FMT("Http3Session: HTTP/3 会话已建立（控制流 {}、QPACK 编码流 {}、解码流 {}）", controlStreamId, qpackEncoderStreamId,
                      qpackDecoderStreamId);
    }

    Http3Session::~Http3Session()
    {
        if (m_connection != nullptr)
        {
            nghttp3_conn_del(m_connection);
            m_connection = nullptr;
        }
    }

    void Http3Session::setParserLimits(const HttpParserLimits limits) noexcept
    {
        m_parserLimits = limits;
    }

    void Http3Session::attachRouter(Router &router) noexcept
    {
        m_router = &router;
    }

    bool Http3Session::isUsable() const noexcept
    {
        return m_isUsable && !m_isBroken;
    }

    bool Http3Session::isBroken() const noexcept
    {
        return m_isBroken;
    }

    void Http3Session::onStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
    {
        if (m_connection == nullptr || m_isBroken)
        {
            return;
        }

        // 返回的「已消费字节数」才是可以还给 QUIC 的流控额度；DATA 帧里的应用数据不算在内，
        // 那部分由 recv_data 回调给出。两处加起来才是这条流真正被消费掉的总量
        const nghttp3_ssize consumedLength =
                nghttp3_conn_read_stream2(m_connection, streamId, data.data(), data.size(), isEndStream ? 1 : 0, currentTimestamp());
        if (consumedLength < 0)
        {
            // 带上流号与本次读入的字节数：没有这两个数，事后只能靠猜是哪条流、字节到齐没有
            LOG_WARN_FMT("Http3Session: 流 {} 的 {} 字节读入被 nghttp3 拒绝（{}）", streamId, data.size(),
                         nghttp3_strerror(static_cast<int>(consumedLength)));
            markBroken(static_cast<int>(consumedLength), "读入流数据");
            return;
        }
        if (consumedLength > 0 && m_crediter)
        {
            m_crediter(streamId, static_cast<std::size_t>(consumedLength));
        }

        // 此刻已经在 nghttp3 的回调之外了：等正文的处理器可以安全唤醒（它们醒来会回头调 nghttp3），
        // 隧道里攒下的帧也在这里交给对端对象
        wakeStreamingRequests();
        wakeWebSocketTunnels();

        // 对端的数据可能解锁了本端待发的东西（比如 QPACK 动态表更新后头块才能编码）
        flushPendingStreamData();
    }

    void Http3Session::flushPendingStreamData()
    {
        if (m_connection == nullptr || m_isBroken)
        {
            return;
        }

        for (std::size_t writeIndex = 0; writeIndex < kMaximumWritesPerFlush; ++writeIndex)
        {
            // nghttp3_vec 与 ngtcp2_vec 布局一致（都是 {指针, 长度}），因此这里顺手就能递给传输层
            nghttp3_vec vectors[kMaximumDataVectors]{};
            std::int64_t      streamId    = -1;
            int               isFinal     = 0;
            const nghttp3_ssize vectorCount =
                    nghttp3_conn_writev_stream(m_connection, &streamId, &isFinal, vectors, kMaximumDataVectors);
            if (vectorCount < 0)
            {
                markBroken(static_cast<int>(vectorCount), "取待发流数据");
                return;
            }
            if (vectorCount == 0 && streamId == -1)
            {
                // 没有待发字节，也没有要收尾的流：本轮搬完了
                return;
            }

            // nghttp3 给的是分片数组，传输层的出口一次只收一段连续字节，先拼起来。
            // 拼好的字节在交出时必须是有效的，所以用成员缓冲而不是临时对象
            std::size_t totalLength = 0;
            for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
            {
                totalLength += vectors[vectorIndex].len;
            }
            m_pendingBytes.resize(totalLength);
            std::size_t offset = 0;
            for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
            {
                if (vectors[vectorIndex].len == 0)
                {
                    continue;
                }
                std::memcpy(m_pendingBytes.data() + offset, vectors[vectorIndex].base, vectors[vectorIndex].len);
                offset += vectors[vectorIndex].len;
            }

            m_writer(streamId, m_pendingBytes, isFinal != 0);
            // 字节已被 QUIC 收下，回告 nghttp3 实际收下的长度（它按这个推进写窗口）。
            // 「只收尾、不带数据」时长度是 0，这一次调用同样不能省
            nghttp3_conn_add_write_offset(m_connection, streamId, totalLength);
            if (totalLength != 0)
            {
                // 流式响应的缓冲腾出一块了：等空间的生产者可以继续写
                noteStreamingResponseDrained(streamId);
            }
        }
    }

    Core::Task<> Http3Session::pump()
    {
        if (m_connection == nullptr || m_isBroken)
        {
            co_return;
        }

        while (!m_readyRequests.empty())
        {
            const std::int64_t streamId              = m_readyRequests.front().streamId;
            const bool         isBodyTooLarge        = m_readyRequests.front().isBodyTooLarge;
            const bool         isHeaderLimitExceeded = m_readyRequests.front().isHeaderLimitExceeded;
            const bool         isUriTooLong          = m_readyRequests.front().isUriTooLong;
            HttpRequest        request        = std::move(m_readyRequests.front().request);
            m_readyRequests.pop_front();

            // 与 h1/h2 同口径：收齐的请求计入请求数（含随后被 413 拒掉的那些，它们同样是有效的 h3 请求）
            // 与 h1/h2 同口径：收齐的请求计入请求数（含随后被 413 拒掉的那些，它们同样是有效的 h3 请求）
            if (m_metrics != nullptr)
            {
                m_metrics->countParsedRequest();
            }

            HttpResponse response;
            // 业务异常在这里就地收口（与 h1/h2 同一处置）：不捕获的话它会穿出 pump()、
            // 打断 QuicServer 的收报文循环，整台服务不再处理任何报文。
            // 声明在分支之外：下面的统计要按「有没有半途抛异常」决定这条流式响应是否落账
            std::exception_ptr handlerException;

            if (isHeaderLimitExceeded || isUriTooLong)
            {
                // 与 h1/h2 同一套状态码：头部越限 431、请求目标越限 414，都不交给业务
                const int   rejectionStatus = isHeaderLimitExceeded ? 431 : 414;
                const char *rejectionBody   = isHeaderLimitExceeded ? "Request Header Fields Too Large" : "URI Too Long";
                if (m_metrics != nullptr)
                {
                    m_metrics->countBadRequest();
                }
                LOG_ERROR_FMT("Http3Session: 流 {} 的请求头部或请求目标超过配置上限，已按 {} 应答且不交给业务", streamId, rejectionStatus);
                response.setStatus(rejectionStatus);
                response.setBody(rejectionBody);
                static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
            } else if (isBodyTooLarge)
            {
                // 正文越界：不派发，直接回 413（与 h1/h2 同一口径与文案）
                if (m_metrics != nullptr)
                {
                    m_metrics->countBadRequest();
                }
                LOG_ERROR_FMT("Http3Session: 流 {} 的请求正文超过上限，已按 413 应答且不交给业务", streamId);
                response.setStatus(413);
                response.setBody("Payload Too Large");
                static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
            } else if (m_router != nullptr)
            {
                // 隧道流上跑的是 WebSocket 帧而不是正文段，因此不装流式发送口
                const bool isTunnelStream = m_pendingTunnelStreams.contains(streamId);
                if (!isTunnelStream)
                {
                    attachChunkSender(streamId, response);
                }
                // HEAD：响应只发头部（正文由发送口抑制），与 h1/h2 同一口径
                if (request.method() == HttpMethod::HEAD)
                {
                    response.suppressStreamingBody();
                }

                try
                {
                    co_await m_router->route(request, response);
                } catch (...)
                {
                    handlerException = std::current_exception();
                }
                if (handlerException != nullptr)
                {
                    const bool isStreamingStarted = response.isChunkedResponse() && response.hasSentChunkedHead();
                    if (isStreamingStarted)
                    {
                        // 头部已随首段正文上线：改不了状态码，只能记日志并补末片收尾
                        LOG_ERROR_FMT("Http3Session: 流 {} 的流式响应中途抛出异常，头部已上线无法改写状态码，"
                                      "已按原状态码收尾",
                                      streamId);
                    } else
                    {
                        response.reset();
                        response.setStatus(500);
                        response.setBody("Internal Server Error");
                        static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
                    }
                }

                if (isTunnelStream)
                {
                    m_pendingTunnelStreams.erase(streamId);
                    co_await serveWebSocketTunnel(streamId, response);
                    continue;
                }
            } else
            {
                LOG_WARN_FMT("Http3Session: 流 {} 上的请求没有接上路由器，回 503", streamId);
                response.setStatus(503);
                response.setBody("HTTP/3 会话尚未接上路由器");
            }

            if (response.isChunkedResponse())
            {
                // 流式响应：响应头与各块在处理器写的过程中已经出去了，这里只做收尾
                finishStreamingResponse(streamId, response);
                // 中途抛异常的流式响应只发了一半，落账等于把半成品记成已应答（与 h2 同一判据）
                if (m_metrics != nullptr && handlerException == nullptr)
                {
                    m_metrics->countResponseStatus(response.status());
                }
            } else
            {
                finalizeResponseForHttp3(streamId, response);
                submitResponse(streamId, response);
                if (m_metrics != nullptr)
                {
                    m_metrics->countResponseStatus(response.status());
                }
            }
        }

        flushPendingStreamData();
        co_return;
    }

    void Http3Session::addRequestHeader(const std::int64_t streamId, std::string name, std::string value)
    {
        IncomingRequest &incoming = m_incomingRequests[streamId];
        // 头部限额与 h1/h2 同口径：条数、单名/单值长度、整块净字节。越限只置位、让请求收完，
        // 服务阶段统一回 431——中途断开的话对端只看到「连接没了」，拿不到「头部太大」这个结论
        ++incoming.headerFieldCount;
        incoming.headerBlockByteCount += name.size() + value.size();
        if ((m_parserLimits.maximumHeaderCount != 0 && incoming.headerFieldCount > m_parserLimits.maximumHeaderCount) ||
            (m_parserLimits.maximumHeaderFieldNameLength != 0 && name.size() > m_parserLimits.maximumHeaderFieldNameLength) ||
            (m_parserLimits.maximumHeaderFieldValueLength != 0 && value.size() > m_parserLimits.maximumHeaderFieldValueLength) ||
            (m_parserLimits.maximumHeaderBlockLength != 0 && incoming.headerBlockByteCount > m_parserLimits.maximumHeaderBlockLength))
        {
            incoming.isHeaderLimitExceeded = true;
        }
        if (name == ":method")
        {
            incoming.method = std::move(value);
            return;
        }
        if (name == ":path")
        {
            if (m_parserLimits.maximumUriLength != 0 && value.size() > m_parserLimits.maximumUriLength)
            {
                incoming.isUriTooLong = true;
            }
            incoming.path = std::move(value);
            return;
        }
        if (name == ":authority")
        {
            incoming.authority = std::move(value);
            return;
        }
        if (name == ":protocol")
        {
            // RFC 9220 的扩展 CONNECT 靠它说明这条流要跑什么协议（websocket）
            incoming.protocol = std::move(value);
            return;
        }
        if (name == ":scheme")
        {
            // 服务端已知自己在 TLS 上，与 h2 侧口径一致：不映射、也不伪造一条头部
            return;
        }
        if (name == "host")
        {
            incoming.hasHostHeader = true;
        }
        incoming.request.addHeader(std::move(name), std::move(value));
    }

    void Http3Session::addRequestBody(const std::int64_t streamId, const std::span<const std::uint8_t> data)
    {
        if (m_pendingTunnelStreams.contains(streamId))
        {
            // 隧道流已派发但还没建起来（建隧道在 pump() 里）：帧先攒着，建好后一次性交给对端对象。
            // 漏了这条，字节会落到下面「非流式」的分支里，凭空又建出一份请求记录来
            m_pendingTunnelBytes[streamId].append(reinterpret_cast<const char *>(data.data()), data.size());
            return;
        }

        if (const auto tunnel = m_webSocketTunnels.find(streamId); tunnel != m_webSocketTunnels.end())
        {
            // CONNECT 隧道：对端发来的是 WebSocket 帧本身（RFC 9220 §4）。**不在这里直接喂**：喂进去
            // 会让业务协程立刻跑起来，它回头就调 nghttp3 发帧，而此刻还在 nghttp3 的回调里
            // （重入是未定义行为）。攒起来，回到安全点由 wakeWebSocketTunnels() 喂
            tunnel->second->pendingIncomingBytes.append(reinterpret_cast<const char *>(data.data()), data.size());
            tunnel->second->hasPendingFeed = true;
            return;
        }

        if (const auto found = m_streamingRequests.find(streamId); found != m_streamingRequests.end())
        {
            HttpStreamBody &streamBody = found->second->body;

            // 体量越界（与 h1/h2 同口径）：此后到达的字节一律丢弃，响应在服务阶段按 413 发出。
            // 判在收的过程中而不是收齐之后——等 END_STREAM 再判，内存已经占住了
            if (m_parserLimits.maximumBodySize != 0
                && streamBody.totalReceivedByteCount() + data.size() > m_parserLimits.maximumBodySize)
            {
                if (!streamBody.isBodyTooLarge())
                {
                    LOG_ERROR_FMT("Http3Session: 流 {} 的请求正文超过上限 {} 字节，已停止流式接收并按 413 应答",
                                  streamId, m_parserLimits.maximumBodySize);
                    streamBody.markBodyTooLarge();
                }
            } else
            {
                // 流式：字节进本流自己的缓冲，**窗口在字节被处理器取走时才还**（消费回调已绑好）。
                // 这里顺手还掉就等于「到达即归还」，背压随之失效——那正是流式路径要保住的东西
                streamBody.append(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), data.size(), false);
                return;
            }

            // 丢弃的字节同样要还窗口：不还的话对端会卡在自己耗尽的接收窗口上
            if (m_crediter && !data.empty())
            {
                m_crediter(streamId, data.size());
            }
            return;
        }

        IncomingRequest &incoming = m_incomingRequests[streamId];

        // 体量越界：只标记与记日志，不再缓冲；此后到达的 DATA 一律丢弃，但窗口照还。
        // 响应在服务阶段统一按 413 发出（与 h1/h2 同一口径）
        if (!incoming.isBodyTooLarge && m_parserLimits.maximumBodySize != 0
            && incoming.body.size() + data.size() > m_parserLimits.maximumBodySize)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的请求正文超过上限 {} 字节，已停止缓冲并按 413 应答",
                          streamId, m_parserLimits.maximumBodySize);
            incoming.isBodyTooLarge = true;
        }
        if (!incoming.isBodyTooLarge)
        {
            incoming.body.append(reinterpret_cast<const char *>(data.data()), data.size());
        }

        // 非流式：正文整段收在请求对象里，本端等于立刻消费掉了，因此到达即归还接收额度
        // （DATA 帧的字节不在 read_stream2 的消费计数里，要在这里单独还）；丢弃的字节同样要还
        if (m_crediter && !data.empty())
        {
            m_crediter(streamId, data.size());
        }
    }

    void Http3Session::finishRequest(const std::int64_t streamId)
    {
        // 隧道流（含待建的）：对端用 END_STREAM 结束隧道（RFC 9220 §5 的关闭方式之一），
        // 该流的发送方向就此关闭。已建成的隧道随之收口并让业务看到终点，而不是永远挂着
        if (m_pendingTunnelStreams.contains(streamId) || m_webSocketTunnels.contains(streamId))
        {
            if (const auto tunnel = m_webSocketTunnels.find(streamId); tunnel != m_webSocketTunnels.end())
            {
                tunnel->second->isStreamClosed = true;
                closeTunnel(streamId);
            }
            return;
        }

        if (const auto found = m_streamingRequests.find(streamId); found != m_streamingRequests.end())
        {
            // 流式：正文到此为止。这次空追加只带「收尾」一个信息，等正文的处理器随后就能看到终点
            found->second->body.append({}, 0, true);
            return;
        }
        enqueueRequest(streamId);
    }

    void Http3Session::noteStreamResetByPeer(const std::int64_t streamId) noexcept
    {
        if (m_metrics == nullptr)
        {
            return;
        }

        // 「还没答完」的三种形态：请求已收齐但还没派发、正在收或正在跑、流式响应还在写
        const bool isStillPending =
                std::ranges::any_of(m_readyRequests, [streamId](const ReadyRequest &entry) { return entry.streamId == streamId; }) ||
                m_incomingRequests.contains(streamId) || m_streamingResponses.contains(streamId) || m_webSocketTunnels.contains(streamId);
        if (isStillPending)
        {
            m_metrics->countStreamCancelled();
        }
    }

    void Http3Session::dropRequest(const std::int64_t streamId)
    {
        m_outgoingBodies.erase(streamId);
        m_incomingRequests.erase(streamId);
        // 还没派发的请求记录一并摘掉：对端已经重置了这条流，再派发就是给一条死流跑业务
        std::erase_if(m_readyRequests, [streamId](const ReadyRequest &entry) { return entry.streamId == streamId; });
        m_pendingTunnelStreams.erase(streamId);
        m_pendingTunnelBytes.erase(streamId);

        if (const auto streaming = m_streamingResponses.find(streamId); streaming != m_streamingResponses.end())
        {
            // 流没了：先叫醒等缓冲排空的生产者，再摘记录。等待器与生产者各持一份共享所有权，
            // 摘表后他们读到的是 isStreamClosed，不会踩空；漏掉这一步生产者会永远等不到唤醒
            const std::shared_ptr<StreamingResponse> state = streaming->second;
            state->isStreamClosed = true;
            if (const std::coroutine_handle<> waiter = std::exchange(state->spaceWaiter, {}); waiter != nullptr)
            {
                waiter.resume();
            }
            m_streamingResponses.erase(streaming);
        }

        if (const auto tunnel = m_webSocketTunnels.find(streamId); tunnel != m_webSocketTunnels.end())
        {
            // 隧道：承载侧的流没了，对端对象随之关闭，挂在 receive() 上的业务也要醒来收尾
            //（记录先留着——业务协程可能还挂着，跑完由 reapFinishedTunnels() 一起摘掉）
            tunnel->second->isStreamClosed = true;
            tunnel->second->peer->markClosed();
            tunnel->second->peer->wakeDeliveryWaiter();
            return;
        }

        const auto found = m_streamingRequests.find(streamId);
        if (found == m_streamingRequests.end())
        {
            return;
        }

        // 流没了：等正文的处理器要立刻看到终止（markBroken 会触发到达通知），而不是永远挂着。
        // 记录先留着——它的派发协程可能还在跑，跑完由 reapFinishedStreamingRequests() 连同记录摘掉
        found->second->isStreamClosed = true;
        found->second->body.markBroken();
    }

    void Http3Session::enqueueRequest(const std::int64_t streamId)
    {
        const auto found = m_incomingRequests.find(streamId);
        if (found == m_incomingRequests.end())
        {
            return;
        }
        // 同一条流只派发一次：头收齐时已经派发过的，end_stream 再来一次就什么都不做
        if (m_pendingTunnelStreams.contains(streamId) || m_webSocketTunnels.contains(streamId) ||
            m_streamingRequests.contains(streamId))
        {
            return;
        }

        IncomingRequest incoming = std::move(found->second);
        m_incomingRequests.erase(found);

        // RFC 9220 的扩展 CONNECT：`:method = CONNECT` 且 `:protocol = websocket`。它与 h1 的 Upgrade
        // 同义，因此按 GET 交给路由——同一个 router.get(路径, 处理器) 既能服务 h1 的 101 升级，
        // 也能服务 h3 上的隧道（与 h2 侧同一口径）
        const bool isWebSocketTunnelRequest = incoming.method == "CONNECT" && incoming.protocol == "websocket";

        HttpRequest request = std::move(incoming.request);
        // 方法原文经 methodFromString 映射：未收录的方法落到 UNKNOWN，路由器按既有规则回 404/405，
        // 绝不静默降级成某条业务路由
        request.setMethod(isWebSocketTunnelRequest ? HttpMethod::GET : HttpRequest::methodFromString(incoming.method));
        request.setUri(incoming.path.empty() ? std::string("/") : incoming.path);
        request.setHttpVersion(std::string(kHttp3RequestVersion));
        request.setBody(std::move(incoming.body));
        // :authority 就是权威主机来源：对端没显式给 host 头时用它补齐，与 h1/h2 读 host 的口径对齐
        if (!incoming.authority.empty() && !incoming.hasHostHeader)
        {
            request.addHeader("host", incoming.authority);
        }

        m_readyRequests.push_back(ReadyRequest{.streamId = streamId, .request = std::move(request),
                                               .isBodyTooLarge = incoming.isBodyTooLarge,
                                               .isHeaderLimitExceeded = incoming.isHeaderLimitExceeded,
                                               .isUriTooLong = incoming.isUriTooLong});
        if (isWebSocketTunnelRequest)
        {
            m_pendingTunnelStreams.insert(streamId);
        }
    }

    void Http3Session::beginStreamingRequestIfMatched(const std::int64_t streamId)
    {
        const auto found = m_incomingRequests.find(streamId);
        // 这条流已经派发过（隧道记录在案）就不再派发：重复派发会让同一条流上出现两份响应，
        // 后一份还会把 nghttp3 的 stream_user_data 从隧道状态改成别的，读回调随即读错对象
        if (found == m_incomingRequests.end() || m_router == nullptr || m_isBroken || m_pendingTunnelStreams.contains(streamId))
        {
            return;
        }

        // 判定要用的东西先取出来：判定通过后这份记录就要从 m_incomingRequests 里搬走
        IncomingRequest   &incoming        = found->second;
        const std::string  methodText      = incoming.method;
        const std::string  pathText        = incoming.path;
        const std::string  authorityText   = incoming.authority;
        const std::string  protocolText    = incoming.protocol;
        const bool         hasHostHeader   = incoming.hasHostHeader;
        const HttpMethod   method          = HttpRequest::methodFromString(methodText);
        const std::string  uri             = pathText.empty() ? std::string("/") : pathText;

        // 扩展 CONNECT（RFC 9220）要在**头收齐时**就派发：隧道建立之后对端才会在同一
        // 条流上发 WebSocket 帧，等 end_stream 就等于永远等不到（对方不会结束这条流）
        if (methodText == "CONNECT" && protocolText == "websocket")
        {
            enqueueRequest(streamId);
            LOG_DEBUG_FMT("Http3Session: 流 {} 是扩展 CONNECT（websocket），已在头部收齐时派发", streamId);
            return;
        }

        // 流式正文路由同理：正文边收边交，业务不必等整份正文；其余路由照旧等 end_stream
        if (!m_router->hasStreamingRoute(method, uri))
        {
            return;
        }

        auto streamingRequest     = std::make_unique<StreamingRequest>();
        streamingRequest->request = std::move(incoming.request);
        streamingRequest->request.setMethod(method);
        streamingRequest->request.setUri(uri);
        streamingRequest->request.setHttpVersion(std::string(kHttp3RequestVersion));
        // :authority 补齐 host 头，与 h1/h2 读 host 的口径一致
        if (!authorityText.empty() && !hasHostHeader)
        {
            streamingRequest->request.addHeader("host", authorityText);
        }
        m_incomingRequests.erase(found);

        StreamingRequest &created = *streamingRequest;
        m_streamingRequests.emplace(streamId, std::move(streamingRequest));

        // 消费即还窗口：由 HttpStreamBody 在字节被处理器取走之后回调进来（到达时还就等于没有背压）
        created.body.reset([this, streamId](const std::size_t consumedFlowControlByteCount)
                           {
                               if (m_crediter && consumedFlowControlByteCount != 0)
                               {
                                   m_crediter(streamId, consumedFlowControlByteCount);
                               }
                           });
        created.body.setBodyArrivedHandler([this, streamId] { noteBodyProgress(streamId); });
        created.reader.attach(created.body, makeBodyPump(streamId));
        created.request.setBodyStream(&created.reader);

        // 协程本身是惰性的，构造它不会执行任何一行；**但不在这里 resume**：此刻还在 nghttp3 的
        // 回调里，处理器一上来就可能提交响应，而回调期间重入库是未定义行为。只置标记，
        // 回到安全点由 wakeStreamingRequests() 起
        created.serveTask.emplace(serveStreamingRequest(streamId, created));
        created.hasPendingWake = true;

        LOG_DEBUG_FMT("Http3Session: 流 {} 命中的是流式正文路由，已在头部收齐时派发", streamId);
    }

    Core::Task<> Http3Session::serveStreamingRequest(const std::int64_t streamId, StreamingRequest &streamingRequest)
    {
        HttpResponse response;
        attachChunkSender(streamId, response);
        if (streamingRequest.request.method() == HttpMethod::HEAD)
        {
            response.suppressStreamingBody();
        }

        // 业务异常必须在这里收口：本协程的 Task 由会话自己驱动，异常若逃出去只会存进 promise
        // 被静默吞掉——对端既拿不到 500、也等不到 END_STREAM，只能挂到 QUIC 空闲超时
        std::exception_ptr handlerException = nullptr;
        try
        {
            co_await m_router->route(streamingRequest.request, response);
        } catch (...)
        {
            handlerException = std::current_exception();
        }
        if (handlerException != nullptr)
        {
            if (response.isChunkedResponse() && response.hasSentChunkedHead())
            {
                LOG_ERROR_FMT("Http3Session: 流 {} 的流式响应中途抛出异常，头部已上线无法改写状态码，已按原状态码收尾", streamId);
            } else
            {
                response.reset();
                response.setStatus(500);
                response.setBody("Internal Server Error");
                static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
            }
        }

        // 正文越界且业务还没开始流式写出：按 413 改判（与 h2 侧同一处置）。
        // 已经开始流式写出时头部已上线，改状态码不可能，只能让它收尾
        if (streamingRequest.body.isBodyTooLarge() && !response.isChunkedResponse())
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的流式请求正文超过上限，已按 413 改判", streamId);
            response.reset();
            response.setStatus(413);
            response.setBody("Payload Too Large");
            static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
        }

        if (response.isChunkedResponse())
        {
            // 流式响应：响应头与各块在处理器写的过程中已经出去了，这里只做收尾
            finishStreamingResponse(streamId, response);
        } else
        {
            finalizeResponseForHttp3(streamId, response);
            submitResponse(streamId, response);
        }
        streamingRequest.isServeFinished = true;
        co_return;
    }

    HttpRequestBody::Pump Http3Session::makeBodyPump(const std::int64_t streamId)
    {
        return [this, streamId]() -> Core::Task<bool>
        {
            const auto found = m_streamingRequests.find(streamId);
            if (found == m_streamingRequests.end())
            {
                // 这条流的状态已经没了：按终止处理，处理器不会拿到半份正文
                co_return false;
            }

            // h3 的正文由承载推来：泵的职责是「等到下一批到达（或收尾、断开）」，而不是像 h1/h2
            // 那样主动去读一批。等待者由到达通知在安全点唤醒
            co_await BodyWaitAwaiter(found->second.get());
            co_return true;
        };
    }

    void Http3Session::noteBodyProgress(const std::int64_t streamId) noexcept
    {
        if (const auto found = m_streamingRequests.find(streamId); found != m_streamingRequests.end())
        {
            found->second->hasPendingWake = true;
        }
    }

    void Http3Session::wakeStreamingRequests()
    {
        // 先按流号收集再逐条唤醒：唤醒之后处理器会继续跑，它可能提交响应甚至收尾，
        // 直接遍历容器会在中途被改动
        std::vector<std::int64_t> pendingStreamIds;
        for (const auto &entry: m_streamingRequests)
        {
            if (entry.second->hasPendingWake)
            {
                pendingStreamIds.push_back(entry.first);
            }
        }

        for (const std::int64_t streamId: pendingStreamIds)
        {
            const auto found = m_streamingRequests.find(streamId);
            if (found == m_streamingRequests.end())
            {
                continue;
            }

            StreamingRequest &request = *found->second;
            request.hasPendingWake    = false;

            if (!request.isServeStarted)
            {
                request.isServeStarted = true;
                request.serveTask->handle().resume();
                continue;
            }

            // 等正文的那个：取出句柄再唤醒。处理器跑完可能把自己从表里摘掉，唤醒之后不再碰 request
            if (const std::coroutine_handle<> waiter = std::exchange(request.bodyWaiter, {}); waiter != nullptr)
            {
                waiter.resume();
            }
        }

        reapFinishedStreamingRequests();
        reapFinishedTunnels();
    }

    void Http3Session::reapFinishedTunnels()
    {
        // 业务跑完且流已关闭才摘：业务协程还挂着时销毁记录会连它的协程帧一起毁掉
        std::erase_if(m_webSocketTunnels,
                      [](const auto &entry) { return entry.second->isBusinessFinished && entry.second->isStreamClosed; });
    }

    void Http3Session::wakeWebSocketTunnels()
    {
        // 先按流号收集再逐条喂：喂进去会让业务协程跑起来，它可能在半路把隧道收口，直接遍历会被改动
        std::vector<std::int64_t> pendingStreamIds;
        for (const auto &entry: m_webSocketTunnels)
        {
            if (entry.second->hasPendingFeed)
            {
                pendingStreamIds.push_back(entry.first);
            }
        }

        for (const std::int64_t streamId: pendingStreamIds)
        {
            const auto found = m_webSocketTunnels.find(streamId);
            if (found == m_webSocketTunnels.end())
            {
                continue;
            }

            WebSocketTunnel &tunnel = *found->second;
            tunnel.hasPendingFeed    = false;
            std::string incomingBytes = std::move(tunnel.pendingIncomingBytes);
            tunnel.pendingIncomingBytes.clear();
            if (incomingBytes.empty())
            {
                continue;
            }

            const WebSocketFeedStatus feedStatus = tunnel.peer->feedBytes(incomingBytes.data(), incomingBytes.size());
            if (m_crediter != nullptr)
            {
                // 字节已交给对端对象（解码器已消费），到达即归还的接收额度在这里补上：
                // 隧道分支在 addRequestBody 里攒字节时没有归还过，不补就会把 QUIC 流窗口用光
                m_crediter(streamId, incomingBytes.size());
            }
            if (feedStatus == WebSocketFeedStatus::DecodeError)
            {
                LOG_WARN_FMT("Http3Session: 流 {} 上的 WebSocket 帧解不开（{}），按 {} 收口隧道", streamId, tunnel.peer->decodeErrorText(),
                             tunnel.peer->decodeErrorCloseCode());
                closeTunnel(streamId);
            }
        }

        reapFinishedTunnels();
    }

    Core::Task<> Http3Session::serveWebSocketTunnel(const std::int64_t streamId, HttpResponse &response)
    {
        if (!response.isWebSocketUpgradeRequested())
        {
            // 业务没登记升级：这条流不是隧道，按普通响应回（RFC 9220 也允许服务端不升级）
            finalizeResponseForHttp3(streamId, response);
            submitResponse(streamId, response);
            co_return;
        }

        // h3 里没有 101：RFC 9220 规定隧道以 2xx 应答，此后这条流上跑的就是 WebSocket 帧本身
        response.setStatus(200);

        // 复用流式响应那套：应答头先出去且**不结束这条流**，出向帧由数据读取回调按需拉走
        const std::shared_ptr<StreamingResponse> state = streamingResponseFor(streamId);
        if (!submitStreamingResponseHead(streamId, *state, response))
        {
            co_return;
        }

        auto tunnel     = std::make_unique<WebSocketTunnel>();
        tunnel->handler = response.webSocketHandler();
        tunnel->peer    = std::make_unique<WebSocketPeer>(
                [this, streamId](const std::string_view frameBytes) -> Core::Task<bool>
                { co_return co_await sendTunnelBytes(streamId, frameBytes); });

        WebSocketTunnel &created = *tunnel;
        m_webSocketTunnels.emplace(streamId, std::move(tunnel));

        // 隧道建立之前就跟到的帧字节（同一批字节里 DATA 紧跟在请求头后面）：交给这条隧道
        if (const auto pending = m_pendingTunnelBytes.find(streamId); pending != m_pendingTunnelBytes.end())
        {
            created.pendingIncomingBytes = std::move(pending->second);
            created.hasPendingFeed      = !created.pendingIncomingBytes.empty();
            m_pendingTunnelBytes.erase(pending);
        }

        // 应答已经排进待发字节，此刻起业务：与 h2 侧同一时机（业务一上来就能收到对端抢发的帧）
        created.businessTask.emplace(runTunnelBusiness(streamId));
        created.businessTask->handle().resume();

        // 攒下的帧此刻才喂（上面那次 resume 让业务挂在 receive() 上，这里喂进去正好唤醒它）
        wakeWebSocketTunnels();
        co_return;
    }

    Core::Task<> Http3Session::runTunnelBusiness(const std::int64_t streamId)
    {
        const auto found = m_webSocketTunnels.find(streamId);
        if (found == m_webSocketTunnels.end())
        {
            co_return;
        }

        WebSocketTunnel &tunnel = *found->second;
        try
        {
            co_await tunnel.handler(*(tunnel.peer));
        } catch (const std::exception &exception)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的 WebSocket 业务处理器抛出异常，已按连接不可用收口。原因：{}", streamId, exception.what());
        } catch (...)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的 WebSocket 业务处理器抛出非标准异常（无 what() 描述）", streamId);
        }

        if (const auto stillThere = m_webSocketTunnels.find(streamId); stillThere != m_webSocketTunnels.end())
        {
            stillThere->second->isBusinessFinished = true;
            closeTunnel(streamId);
        }
        co_return;
    }

    Core::Task<bool> Http3Session::sendTunnelBytes(const std::int64_t streamId, const std::string_view frameBytes)
    {
        const auto found = m_webSocketTunnels.find(streamId);
        if (found == m_webSocketTunnels.end())
        {
            co_return false; // 隧道已经收口，调用方应停止写入
        }

        // 只查表：对端已经重置这条流时它不在表里，重建只会留下永远清理不掉的条目
        const std::shared_ptr<StreamingResponse> state = findStreamingResponse(streamId);
        // 状态不在（流已被重置）或承载侧的流已经关闭：写多少都出不去，直接按失败收手
        if (state == nullptr || state->isStreamClosed)
        {
            co_return false;
        }
        if (!frameBytes.empty())
        {
            state->chunks.emplace_back(frameBytes);
            state->pendingByteCount += state->chunks.back().size();
        }
        // 与流式响应同一道闸：生产者跑得比网络快就挂起等排空；流一关闭立即收手
        while (!state->isStreamClosed && state->pendingByteCount > kStreamingResponseBufferByteCount)
        {
            co_await ResponseSpaceAwaiter(state);
        }
        if (state->isStreamClosed)
        {
            co_return false;
        }

        // 有新正文了：让库里再来取（读回调上次报的是 EOF|NO_END_STREAM，得唤一声它才会重来），
        // 然后立刻把攒下的发出去
        static_cast<void>(nghttp3_conn_resume_stream(m_connection, streamId));
        flushPendingStreamData();
        co_return true;
    }

    void Http3Session::closeTunnel(const std::int64_t streamId)
    {
        const auto found = m_webSocketTunnels.find(streamId);
        if (found == m_webSocketTunnels.end())
        {
            return;
        }

        found->second->peer->markClosed();
        // 挂在 receive() 上的业务要醒来收尾。隧道记录活到业务跑完（reapFinishedTunnels），
        // 因此协程帧在此期间不会被销毁，这次唤醒不存在「恢复已销毁帧」的风险
        found->second->peer->wakeDeliveryWaiter();

        // 出向到此为止：标记写完并唤一次，库会把余下的取走并在最后关掉发送侧
        if (const auto state = m_streamingResponses.find(streamId); state != m_streamingResponses.end())
        {
            state->second->isFinished = true;
            static_cast<void>(nghttp3_conn_resume_stream(m_connection, streamId));
            flushPendingStreamData();
        }
    }

    void Http3Session::reapFinishedStreamingRequests()
    {
        // 两个条件都要满足才摘：派发协程跑完**且**承载侧的流已关闭。handler 先跑完而流还开着时
        // 不能摘——那之后还会有 DATA 到达，记录没了就会被当成非流式请求又建出一份来
        std::erase_if(m_streamingRequests,
                      [](const auto &entry) { return entry.second->isServeFinished && entry.second->isStreamClosed; });
    }

    void Http3Session::finalizeResponseForHttp3(const std::int64_t streamId, HttpResponse &response)
    {
        // 升级只能走 RFC 9220 的扩展 CONNECT（:method=CONNECT + :protocol=websocket）：普通请求上
        // 登记升级会产出一条既不是 101 也不是隧道的响应，对端无从处理，因此明确回 500 并留下日志
        if (response.isWebSocketUpgradeRequested())
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 上的处理器要求 WebSocket 升级，但这条流不是扩展 CONNECT（RFC 9220），已改回 500",
                          streamId);
            response.reset();
            response.setStatus(500);
            response.setBody("HTTP/3 上的 WebSocket 升级需要扩展 CONNECT 请求（RFC 9220）");
        }
    }

    std::vector<nghttp3_nv> makeHeaderFieldViews(const std::vector<std::string> &names, const std::vector<std::string> &values)
    {
        std::vector<nghttp3_nv> headerFields;
        headerFields.reserve(names.size());
        for (std::size_t headerIndex = 0; headerIndex < names.size(); ++headerIndex)
        {
            headerFields.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t *>(names[headerIndex].data()),
                                              reinterpret_cast<const std::uint8_t *>(values[headerIndex].data()), names[headerIndex].size(),
                                              values[headerIndex].size(), NGHTTP3_NV_FLAG_NONE});
        }
        return headerFields;
    }

    void Http3Session::attachChunkSender(const std::int64_t streamId, HttpResponse &response)
    {
        // 捕获 &response：发送口只在处理器运行期间被调用，而处理器就活在这次路由的栈帧里
        response.setChunkSender([this, streamId, &response](const std::string_view chunk) -> Core::Task<bool>
                                {
                                    // HEAD：响应只有头部，正文段一字节都不发（头部由收尾路径与 END_STREAM
                                    // 一起发出）。发出去会被对端当成下一条报文的开头
                                    if (response.isStreamingBodySuppressed())
                                    {
                                        co_return true;
                                    }
                                    // 这块路径保持「按需建表」：首个写入块发生在状态建立之前，
                                    // 只查不建会让第一块直接失败（另一条路径——隧道帧——才必须只查不建）
                                    co_return co_await sendStreamingChunk(streamId, streamingResponseFor(streamId), response, chunk);
                                });
    }

    std::shared_ptr<Http3Session::StreamingResponse> Http3Session::findStreamingResponse(const std::int64_t streamId) const noexcept
    {
        const auto entry = m_streamingResponses.find(streamId);
        return entry == m_streamingResponses.end() ? nullptr : entry->second;
    }

    std::shared_ptr<Http3Session::StreamingResponse> Http3Session::streamingResponseFor(const std::int64_t streamId)
    {
        std::shared_ptr<StreamingResponse> &entry = m_streamingResponses[streamId];
        if (entry == nullptr)
        {
            entry = std::make_shared<StreamingResponse>();
        }
        return entry;
    }

    bool Http3Session::submitStreamingResponseHead(const std::int64_t streamId, StreamingResponse &state, HttpResponse &response)
    {
        std::vector<std::string> names;
        std::vector<std::string> values;
        names.reserve(response.headers().size() + 1);
        values.reserve(response.headers().size() + 1);
        names.emplace_back(kStatusHeaderName);
        values.emplace_back(std::to_string(response.status()));
        for (const auto &headerEntry: response.headers())
        {
            // 流式响应的长度此刻还不知道（这正是分块的意义）：content-length 一律不发，
            // 否则那个数字会跟随后陆续发出的 DATA 对不上
            if (headerEntry.first == "content-length")
            {
                continue;
            }
            // HttpResponse 是按 h1 口径造的：startChunkedResponse() 会往头部里放 transfer-encoding，
            // 而 h3 禁止连接特定字段（RFC 9114 §4.2）。带上它会被对端判成报文格式错误——
            // 实测客户端直接回 MALFORMED_HTTP_HEADER，整条响应连正文一起废掉
            if (isConnectionSpecificHeaderName(headerEntry.first))
            {
                LOG_DEBUG_FMT("Http3Session: 流 {} 的流式响应已丢弃 HTTP/3 禁止的连接特定头「{}」", streamId, headerEntry.first);
                continue;
            }
            names.push_back(headerEntry.first);
            values.push_back(headerEntry.second);
        }

        // 读正文的回调靠 stream_user_data 找回状态，必须先挂上
        if (nghttp3_conn_set_stream_user_data(m_connection, streamId, &state) != 0)
        {
            // nghttp3 找不到这条流：对端多半已经把它 RST 掉了（慢业务上很常见）。
            // 这只是**这一条流**的响应发不出去，连接与其它流都还健康——按流级作废处理，
            // 不能置 m_isBroken（那会连带关掉整条 QUIC 连接；h2 同场景只作废该流）
            LOG_WARN_FMT("Http3Session: 流 {} 已经不在了（对端多半已重置该流），这条流式响应作废；连接与其它流不受影响", streamId);
            m_streamingResponses.erase(streamId);
            return false;
        }

        const std::vector<nghttp3_nv> headerFields = makeHeaderFieldViews(names, values);
        nghttp3_data_reader           dataReader{};
        dataReader.read_data = readStreamingResponseBody;
        if (const int result = nghttp3_conn_submit_response(m_connection, streamId, headerFields.data(), headerFields.size(), &dataReader);
            result != 0)
        {
            markBroken(result, "提交流式响应头");
            return false;
        }

        state.isHeadSent = true;
        return true;
    }

    Core::Task<bool> Http3Session::sendStreamingChunk(const std::int64_t streamId, std::shared_ptr<StreamingResponse> state,
                                                     HttpResponse &response, const std::string_view chunk)
    {
        // 承载侧的流已经关闭：这一段写不出去，按失败收手
        if (state->isStreamClosed)
        {
            co_return false;
        }
        if (!state->isHeadSent)
        {
            // 首个段落是 HttpResponse::writeChunk() 推上来的 HTTP/1.1 头部文本（状态行 + 头部块）：
            // 内容是 h1 线格式，不是 h3 要发的头块，这次调用只当「头部该上线了」的信号，
            // 真正发出的字段按响应对象现取；它自身那些字节不进正文（与 h2 侧同一口径）
            if (!submitStreamingResponseHead(streamId, *state, response))
            {
                co_return false;
            }
            co_return true;
        }

        // 其余段落是 h1 分块帧成帧的正文（RFC 9112 §7.1）：h3 里没有分块帧这一层，只把帧里的负载收下。
        // 一片一块内存，追加不搬动已经交给 nghttp3 的字节（它会把没写完的 vec 留到下一次写再取）
        const std::string_view payload = chunkFramePayload(chunk);
        if (!payload.empty())
        {
            state->chunks.emplace_back(payload);
            state->pendingByteCount += state->chunks.back().size();
        }
        // 有界缓冲：生产者跑得比网络快就挂起等排空，而不是把内存堆到把进程拖垮。
        // 流被关闭（对端重置/连接收口）时无需再等，直接收手
        while (!state->isStreamClosed && state->pendingByteCount > kStreamingResponseBufferByteCount)
        {
            co_await ResponseSpaceAwaiter(state);
        }
        if (state->isStreamClosed)
        {
            co_return false;
        }

        // 有新数据了：唤一声让库里再来取（读回调上次报的是 WOULDBLOCK，库在等这一声），随后立刻发出去
        static_cast<void>(nghttp3_conn_resume_stream(m_connection, streamId));
        flushPendingStreamData();
        co_return true;
    }

    void Http3Session::finishStreamingResponse(const std::int64_t streamId, HttpResponse &response)
    {
        const std::shared_ptr<StreamingResponse> state = streamingResponseFor(streamId);
        if (!state->isHeadSent && !submitStreamingResponseHead(streamId, *state, response))
        {
            return; // 提交失败时已经记过日志
        }

        state->isFinished = true;
        // 收尾：唤一声让库把余下的取走、最后关掉发送侧（读回调此时会报 EOF）
        static_cast<void>(nghttp3_conn_resume_stream(m_connection, streamId));
        flushPendingStreamData();
    }

    void Http3Session::noteStreamingResponseDrained(const std::int64_t streamId)
    {
        const auto found = m_streamingResponses.find(streamId);
        if (found == m_streamingResponses.end())
        {
            return;
        }

        StreamingResponse &state = *found->second;

        // 整片交付完的才可以丢：这次 add_write_offset 说明传输层已经把读回调交出去的那段字节接走
        // （QuicConnection 自己留着重传用的副本），nghttp3 不会再借这块内存。半片的留着，
        // 下一次读回调从 headOffset 往后接着取。在读回调里丢会改掉还没被拷走的 vec，因此只在这里丢
        while (!state.chunks.empty() && state.headOffset >= state.chunks.front().size())
        {
            state.pendingByteCount -= state.chunks.front().size();
            state.chunks.pop_front();
            state.headOffset = 0;
        }

        // 队列里还有分片：库里此刻正等着（读回调报过 WOULDBLOCK），唤一声它才会接着来取
        if (!state.chunks.empty())
        {
            static_cast<void>(nghttp3_conn_resume_stream(m_connection, streamId));
        }

        if (const std::coroutine_handle<> waiter = std::exchange(state.spaceWaiter, {}); waiter != nullptr)
        {
            waiter.resume();
        }
    }

    void Http3Session::submitResponse(const std::int64_t streamId, const HttpResponse &response)
    {
        const std::string_view body = response.body();

        // 正文交给 nghttp3 时它只借走指针（丢包重传还会再用），所以放进按流号索引的表里；
        // std::map 的节点地址稳定，后面的增删不会让已经交出去的指针失效
        OutgoingBody &outgoingBody = m_outgoingBodies[streamId];
        outgoingBody.bytes.assign(body.begin(), body.end());
        outgoingBody.offset = 0;

        // 头的字符串要活过下面那次调用（nghttp3_nv 里存的是指针），因此名字与取值都由本函数持有
        std::vector<std::string> names;
        std::vector<std::string> values;
        names.reserve(response.headers().size() + 2);
        values.reserve(response.headers().size() + 2);
        names.emplace_back(kStatusHeaderName);
        values.emplace_back(std::to_string(response.status()));

        bool hasContentLengthHeader = false;
        for (const auto &headerEntry: response.headers())
        {
            // h3 禁止连接特定字段（RFC 9114 §4.2）：HttpResponse 按 h1 口径可能带上它们，
            // 带上会被对端判成报文格式错误，整条响应作废
            if (isConnectionSpecificHeaderName(headerEntry.first))
            {
                LOG_DEBUG_FMT("Http3Session: 流 {} 的响应已丢弃 HTTP/3 禁止的连接特定头「{}」", streamId, headerEntry.first);
                continue;
            }
            if (headerEntry.first == "content-length")
            {
                hasContentLengthHeader = true;
            }
            names.push_back(headerEntry.first);
            values.push_back(headerEntry.second);
        }

        // 无正文的状态码（204/304）不带 content-length；其余若业务没写就按实际正文长度补上，
        // 否则对端只能靠 END_STREAM 判完，逐字节对不上 h1/h2 给出的那一份头部
        const bool isBodylessStatus = HttpResponse::isBodylessStatusCode(response.status());
        if (!isBodylessStatus && !hasContentLengthHeader)
        {
            names.emplace_back("content-length");
            values.emplace_back(std::to_string(outgoingBody.bytes.size()));
        }

        const std::vector<nghttp3_nv> headerFields = makeHeaderFieldViews(names, values);

        // 读正文的回调靠 stream_user_data 找回这段正文，因此必须先挂上去
        const bool hasBody = !isBodylessStatus && !outgoingBody.bytes.empty();
        if (nghttp3_conn_set_stream_user_data(m_connection, streamId, &outgoingBody) != 0)
        {
            // 与 submitStreamingResponseHead 同一处置：整条流没了（对端重置）只作废这一条响应，
            // 不牵连连接与其它流
            LOG_WARN_FMT("Http3Session: 流 {} 已经不在了（对端多半已重置该流），本次响应作废；连接与其它流不受影响", streamId);
            m_outgoingBodies.erase(streamId);
            return;
        }

        nghttp3_data_reader dataReader{};
        dataReader.read_data = readResponseBodyCallback;

        // dr 为空即「没有正文且就此收尾」（nghttp3 的接口约定）
        if (const int result = nghttp3_conn_submit_response(m_connection, streamId, headerFields.data(), headerFields.size(),
                                                           hasBody ? &dataReader : nullptr);
            result != 0)
        {
            markBroken(result, "提交响应头");
        }
    }

    void Http3Session::markBroken(const int errorCode, const char *const what)
    {
        m_isBroken = true;
        LOG_WARN_FMT("Http3Session: {}时 nghttp3 报错（{}），HTTP/3 会话作废", what, nghttp3_strerror(errorCode));
    }
} // namespace AsynGyanis::Net
