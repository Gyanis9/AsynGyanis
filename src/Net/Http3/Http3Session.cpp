#include "Net/Http3/Http3Session.h"

#include "Base/Log/LogMacros.h"
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

    Http3Session::Http3Session(StreamOpener opener, StreamWriter writer, StreamCrediter crediter) :
        m_writer(std::move(writer)), m_crediter(std::move(crediter))
    {
        if (!opener || !m_writer)
        {
            LOG_ERROR("Http3Session: 缺少单向流的开流口或流数据出口，HTTP/3 会话不可用");
            return;
        }

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);

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
            markBroken(static_cast<int>(consumedLength), "读入流数据");
            return;
        }
        if (consumedLength > 0 && m_crediter)
        {
            m_crediter(streamId, static_cast<std::size_t>(consumedLength));
        }

        // 此刻已经在 nghttp3 的回调之外了：等正文的处理器可以安全唤醒（它们醒来会回头调 nghttp3）
        wakeStreamingRequests();

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
            const std::int64_t streamId = m_readyRequests.front().first;
            HttpRequest        request  = std::move(m_readyRequests.front().second);
            m_readyRequests.pop_front();

            HttpResponse response;
            if (m_router != nullptr)
            {
                // 与 h1/h2 同一套路由与处理器：业务不需要知道自己在哪条协议上跑。
                // 流式正文路由不在这里派发——它们在头收齐时就转给了 m_streamingRequests
                co_await m_router->route(request, response);
            } else
            {
                LOG_WARN_FMT("Http3Session: 流 {} 上的请求没有接上路由器，回 503", streamId);
                response.setStatus(503);
                response.setBody("HTTP/3 会话尚未接上路由器");
            }
            finalizeResponseForHttp3(streamId, response);
            submitResponse(streamId, response);
        }

        flushPendingStreamData();
        co_return;
    }

    void Http3Session::addRequestHeader(const std::int64_t streamId, std::string name, std::string value)
    {
        IncomingRequest &incoming = m_incomingRequests[streamId];
        if (name == ":method")
        {
            incoming.method = std::move(value);
            return;
        }
        if (name == ":path")
        {
            incoming.path = std::move(value);
            return;
        }
        if (name == ":authority")
        {
            incoming.authority = std::move(value);
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
        if (const auto found = m_streamingRequests.find(streamId); found != m_streamingRequests.end())
        {
            // 流式：字节进本流自己的缓冲，**窗口在字节被处理器取走时才还**（消费回调已绑好）。
            // 这里顺手还掉就等于「到达即归还」，背压随之失效——那正是流式路径要保住的东西
            found->second->body.append(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), data.size(), false);
            return;
        }

        IncomingRequest &incoming = m_incomingRequests[streamId];
        incoming.body.append(reinterpret_cast<const char *>(data.data()), data.size());

        // 非流式：正文整段收在请求对象里，本端等于立刻消费掉了，因此到达即归还接收额度
        // （DATA 帧的字节不在 read_stream2 的消费计数里，要在这里单独还）
        if (m_crediter && !data.empty())
        {
            m_crediter(streamId, data.size());
        }
    }

    void Http3Session::finishRequest(const std::int64_t streamId)
    {
        if (const auto found = m_streamingRequests.find(streamId); found != m_streamingRequests.end())
        {
            // 流式：正文到此为止。这次空追加只带「收尾」一个信息，等正文的处理器随后就能看到终点
            found->second->body.append({}, 0, true);
            return;
        }
        enqueueRequest(streamId);
    }

    void Http3Session::dropRequest(const std::int64_t streamId)
    {
        m_outgoingBodies.erase(streamId);
        m_incomingRequests.erase(streamId);

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

        IncomingRequest incoming = std::move(found->second);
        m_incomingRequests.erase(found);

        HttpRequest request = std::move(incoming.request);
        // 方法原文经 methodFromString 映射：未收录的方法落到 UNKNOWN，路由器按既有规则回 404/405，
        // 绝不静默降级成某条业务路由
        request.setMethod(HttpRequest::methodFromString(incoming.method));
        request.setUri(incoming.path.empty() ? std::string("/") : incoming.path);
        request.setHttpVersion(std::string(kHttp3RequestVersion));
        request.setBody(std::move(incoming.body));
        // :authority 就是权威主机来源：对端没显式给 host 头时用它补齐，与 h1/h2 读 host 的口径对齐
        if (!incoming.authority.empty() && !incoming.hasHostHeader)
        {
            request.addHeader("host", incoming.authority);
        }

        m_readyRequests.emplace_back(streamId, std::move(request));
    }

    void Http3Session::beginStreamingRequestIfMatched(const std::int64_t streamId)
    {
        const auto found = m_incomingRequests.find(streamId);
        if (found == m_incomingRequests.end() || m_router == nullptr || m_isBroken)
        {
            return;
        }

        // 判定要用的东西先取出来：判定通过后这份记录就要从 m_incomingRequests 里搬走
        IncomingRequest   &incoming        = found->second;
        const std::string  methodText      = incoming.method;
        const std::string  pathText        = incoming.path;
        const std::string  authorityText   = incoming.authority;
        const bool         hasHostHeader   = incoming.hasHostHeader;
        const HttpMethod   method          = HttpRequest::methodFromString(methodText);
        const std::string  uri             = pathText.empty() ? std::string("/") : pathText;

        // 头已收齐，方法/路径此刻可判：命中的是流式正文路由就提前派发——正文边收边交，
        // 业务不必等整份正文；其余路由照旧等 end_stream
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
        co_await m_router->route(streamingRequest.request, response);
        finalizeResponseForHttp3(streamId, response);
        submitResponse(streamId, response);
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
        // 流式响应（分块/SSE）与 WebSocket 升级在 h3 上还没有实现。与其把一份错的响应发出去，
        // 不如明确回 500 并留一条日志——静默给错比明确失败难查得多
        if (response.isChunkedResponse() || response.isWebSocketUpgradeRequested())
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 上的处理器要求流式响应或 WebSocket 升级，HTTP/3 尚未支持，已改回 500", streamId);
            response.reset();
            response.setStatus(500);
            response.setBody("HTTP/3 暂不支持流式响应与 WebSocket 升级");
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

        std::vector<nghttp3_nv> headerFields;
        headerFields.reserve(names.size());
        for (std::size_t headerIndex = 0; headerIndex < names.size(); ++headerIndex)
        {
            headerFields.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t *>(names[headerIndex].data()),
                                              reinterpret_cast<const std::uint8_t *>(values[headerIndex].data()), names[headerIndex].size(),
                                              values[headerIndex].size(), NGHTTP3_NV_FLAG_NONE});
        }

        // 读正文的回调靠 stream_user_data 找回这段正文，因此必须先挂上去
        const bool hasBody = !isBodylessStatus && !outgoingBody.bytes.empty();
        if (nghttp3_conn_set_stream_user_data(m_connection, streamId, &outgoingBody) != 0)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的响应正文挂不上（nghttp3 找不到该流），本次响应作废", streamId);
            m_isBroken = true;
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
