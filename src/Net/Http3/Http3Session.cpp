#include "Net/Http3/Http3Session.h"

#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpChunkFrame.h"
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http/Router.h"
#include "Net/WebSocket/PerMessageDeflate.h"
#include "Net/Http3/Http3Connection.h"
#include "Net/Http3/Qpack.h"

namespace AsynGyanis::Net
{
    namespace
    {
        /// HTTP/3 响应里唯一必须由本端补上的头：状态伪头（RFC 9114 §4.3.2）
        constexpr const char *kStatusHeaderName = ":status";

        constexpr const char *kContentTypeHeaderName = "content-type"; ///< 正文媒体类型
        constexpr const char *kContentLengthHeaderName = "content-length"; ///< 正文长度
        constexpr const char *kDateHeaderName = "date";                ///< 响应生成时刻
        constexpr const char *kDefaultContentTypeValue = "text/plain"; ///< 有正文却没设类型时的缺省值

        /// 把字符串按字节交给只认「指针 + 长度」的接口，不留零终止的假设
        [[nodiscard]] std::span<const std::uint8_t> asBytes(const std::string_view text) noexcept
        {
            return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        }

        /// 100..999 之外（RFC 9110 §15）的状态码不得上线：连接层会拒收这个 :status，
        /// 整条流就此发不出东西，改回 500 至少让对端拿到一份能读的响应
        [[nodiscard]] int normalizeWireStatusCode(const int responseStatus, const std::int64_t streamId)
        {
            if (responseStatus < 100 || responseStatus > 999)
            {
                LOG_ERROR_FMT("Http3Session: 响应状态码 {} 越界（应为 100..999），流 {} 已改回 500", responseStatus, streamId);
                return 500;
            }
            return responseStatus;
        }

        /**
         * @brief 把 HttpResponse 摊平成 h3 要交的字段行，口径与 Http2Session 的采集器一致
         * @details 三条默认补齐（类型/长度/日期）与「可重复头逐条展开」必须与 h1/h2 逐字相同，
         *          否则同一份业务代码换个协议就会少发 Set-Cookie 或少发 date。
         * @param streamId 只用于日志
         * @param response 业务写好的响应
         * @param isStreamingResponse true 表示正文由后续 DATA 逐段给出，此刻算不出长度
         * @return 以 :status 开头的字段行
         */
        [[nodiscard]] std::vector<QpackHeaderField> collectResponseFieldLines(const std::int64_t streamId,
                                                                             const HttpResponse &response,
                                                                             const bool isStreamingResponse)
        {
            std::vector<QpackHeaderField> fieldLines;
            // 常见情形是一条头名展开一行，再加后面最多补的三行；可重复头（Set-Cookie）会多几条，
            // 那时多一次扩容比反复搬移便宜
            fieldLines.reserve(response.headers().size() + 3U);

            const int         wireStatusCode = normalizeWireStatusCode(response.status(), streamId);
            const bool        isBodylessStatus = HttpResponse::isBodylessStatusCode(wireStatusCode);
            const std::string_view responseBody = response.body();
            bool              hasContentTypeHeader = false;
            bool              hasContentLengthHeader = false;
            bool              hasDateHeader = false;

            fieldLines.push_back(QpackHeaderField{.name = kStatusHeaderName, .value = std::to_string(wireStatusCode)});

            // 视图只给「名字 → 一个值」，逐条取值必须再走 headerValues()，
            // 否则多条 Set-Cookie 只剩一条（HttpResponse.h 的类说明写明了这点）
            for (const auto &headerEntry: response.headers())
            {
                const std::string &headerName = headerEntry.first;
                if (isConnectionSpecificHeaderName(headerName))
                {
                    // h3 禁止连接特定字段（RFC 9114 §4.2）：HttpResponse 按 h1 口径可能带上它们，
                    // 带上会被对端判成报文格式错误，整条响应作废
                    LOG_DEBUG_FMT("Http3Session: 流 {} 的响应已丢弃 HTTP/3 禁止的连接特定头「{}」", streamId, headerName);
                    continue;
                }
                if (isStreamingResponse && headerName == kContentLengthHeaderName)
                {
                    // 流式响应的长度由 DATA 帧的总长给出：这个数字与随后陆续发出的正文对不上，
                    // 留着反而让对端按它定界、把后面的段当多余字节
                    LOG_DEBUG_FMT("Http3Session: 流 {} 的流式响应正文长度由 DATA 给出，已丢弃 content-length 响应头", streamId);
                    continue;
                }
                if (headerName == kContentTypeHeaderName)
                {
                    hasContentTypeHeader = true;
                }
                else if (headerName == kContentLengthHeaderName)
                {
                    hasContentLengthHeader = true;
                }
                else if (headerName == kDateHeaderName)
                {
                    hasDateHeader = true;
                }

                for (const std::string &headerValue: response.headerValues(headerName))
                {
                    fieldLines.push_back(QpackHeaderField{.name = headerName, .value = headerValue});
                }
            }

            if (!hasContentTypeHeader && !responseBody.empty())
            {
                fieldLines.push_back(QpackHeaderField{.name = kContentTypeHeaderName, .value = kDefaultContentTypeValue});
            }
            // 没有长度对端就只能靠 END_STREAM 定界；1xx/204/304 补出去是让它白等一段正文
            if (!hasContentLengthHeader && !isStreamingResponse && !isBodylessStatus)
            {
                fieldLines.push_back(QpackHeaderField{.name = kContentLengthHeaderName, .value = std::to_string(responseBody.size())});
            }
            if (!hasDateHeader)
            {
                fieldLines.push_back(QpackHeaderField{.name = kDateHeaderName, .value = formatHttpDate(std::chrono::system_clock::now())});
            }
            return fieldLines;
        }
    } // namespace

    Http3Session::Http3Session(StreamOpener opener, StreamWriter writer, StreamCrediter crediter,
                               std::shared_ptr<HttpMetricsCollector> metrics, std::shared_ptr<HttpMemoryBudget> memoryBudget) :
        m_writer(std::move(writer)), m_crediter(std::move(crediter)), m_metrics(std::move(metrics)),
        m_memoryBudget(std::move(memoryBudget))
    {
        if (!opener || !m_writer)
        {
            LOG_ERROR("Http3Session: 缺少单向流的开流口或流数据出口，HTTP/3 会话不可用");
            return;
        }

        // 连接层的通知逐个接回本会话：它们都在调用方线程上同步触发，这里只是把函数对象绑回来
        Http3Connection::Callbacks callbacks;
        // 伪头与普通头都从这一路进来：归位（:method/:path/:authority/:protocol）与限额判定都在 addRequestHeader 里
        callbacks.onHeaderField = [this](const std::int64_t streamId, const std::string_view name, const std::string_view value)
                                  { addRequestHeader(streamId, std::string(name), std::string(value)); };
        // 头块收齐：方法/路径此刻可判，命中流式正文路由或扩展 CONNECT 就在这里提前派发。
        // 尾段也走同一个入口——那条流早已从 m_incomingRequests 搬走，函数自己会判出「已派发过」而什么都不做
        callbacks.onHeaderBlockReceived = [this](const std::int64_t streamId, bool)
                                          { beginStreamingRequestIfMatched(streamId); };
        // 正文段：DATA 载荷的额度何时归还由 addRequestBody 决定（非流式到达即还、流式消费才还）
        callbacks.onBodyBytes = [this](const std::int64_t streamId, const std::span<const std::uint8_t> bytes)
                                { addRequestBody(streamId, bytes); };
        callbacks.onRequestEnded = [this](const std::int64_t streamId) { finishRequest(streamId); };
        callbacks.onStreamClosed = [this](const std::int64_t streamId) { dropRequest(streamId); };
        // 该流已被放弃（对端重置、或本端按协议判错）：先按「还没答完」计数，再丢掉本会话的状态
        callbacks.onStreamReset = [this](const std::int64_t streamId, Http3ErrorCode)
                                  {
                                      noteStreamResetByPeer(streamId);
                                      dropRequest(streamId);
                                  };
        // RFC 9114 §4.1.2 允许服务端在重置之前先答一个错误响应，回哪个状态码只有业务层知道
        callbacks.onMalformedRequest = [this](const std::int64_t streamId, const std::string_view reason)
                                       { answerMalformedRequest(streamId, reason); };
        // 连接级收口（对端 GOAWAY、关键流被关、协议错误）：此后唯一合法的动作是销毁
        callbacks.onConnectionClosed = [this](const Http3ErrorCode errorCode, const std::string_view reason)
                                       { markBroken(errorCode, reason); };

        // 三条本端单向流由连接层自己开（流号来自传输层）、SETTINGS 由它写进控制流、QPACK 两侧由它接上；
        // 接收额度的归还口一并交给它——非 DATA 字节的额度现在在那里还
        m_connection = std::make_unique<Http3Connection>(std::move(opener), m_writer, m_crediter, std::move(callbacks));
        m_isUsable   = m_connection->isUsable();
        if (!m_isUsable)
        {
            LOG_WARN("Http3Session: HTTP/3 连接层没能在本端开齐三条单向流，会话不可用");
            return;
        }
        LOG_DEBUG("Http3Session: HTTP/3 会话已建立");
    }

    Http3Session::~Http3Session() = default;

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

        // 帧头与被丢弃的帧这类非 DATA 字节的接收额度由连接层就地归还，本处不再按返回值补账
        m_connection->consumeStreamData(streamId, data, isEndStream);

        // 此刻已经在连接层的回调之外了：等正文的处理器可以安全唤醒（它们醒来会回头推响应），
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

        // 连接层按流轮转把攒下的待发字节交给传输层，一次最多搬固定几轮，不会在一条连接上转太久
        m_connection->flush();
        // 交给传输层即视为排空（重传由传输层负责）：等缓冲空间的生产者在这里醒来
        resumeStreamingResponseWaiters();
    }

    void Http3Session::resumeStreamingResponseWaiters()
    {
        // 先把句柄摘下来再逐个唤醒：被唤醒的生产者会接着推下一段正文，而推一段就要刷一次，
        // 那又会走进 flushPendingStreamData——边遍历边唤醒会踩到迭代器失效
        std::vector<std::coroutine_handle<>> waiters;
        for (const auto &entry: m_streamingResponses)
        {
            if (const std::coroutine_handle<> waiter = std::exchange(entry.second->spaceWaiter, {}); waiter != nullptr)
            {
                waiters.push_back(waiter);
            }
        }
        for (const std::coroutine_handle<> waiter: waiters)
        {
            waiter.resume();
        }
    }

    std::size_t Http3Session::streamingResponsePendingByteCount(const std::int64_t streamId) const noexcept
    {
        return m_connection == nullptr ? 0 : m_connection->pendingOutputByteCount(streamId);
    }

    void Http3Session::resumeDeferredWaiters()
    {
        if (m_deferredWaiterResumes.empty())
        {
            return;
        }
        std::vector<std::coroutine_handle<>> waiters;
        waiters.swap(m_deferredWaiterResumes);
        for (const std::coroutine_handle<> waiter: waiters)
        {
            if (waiter != nullptr)
            {
                waiter.resume();
            }
        }
    }

    void Http3Session::closeDeferredTunnels()
    {
        if (m_deferredTunnelClosures.empty())
        {
            return;
        }
        std::vector<std::int64_t> streamIds;
        streamIds.swap(m_deferredTunnelClosures);
        for (const std::int64_t streamId: streamIds)
        {
            if (const auto tunnel = m_webSocketTunnels.find(streamId); tunnel != m_webSocketTunnels.end())
            {
                tunnel->second->isStreamClosed = true;
                closeTunnel(streamId);
            }
        }
    }

    Core::Task<> Http3Session::pump()
    {
        if (m_connection == nullptr || m_isBroken)
        {
            co_return;
        }

        // 先回收承载层报来的「对端取消」：那些通知到的时候正在传输层的回调里，此刻（处理完
        // 一条报文之后）才是能安全动连接层与唤醒业务协程的安全点
        drainPeerCancelledStreams();
        // 同样在安全点做的两件事：唤醒被摘掉的流式生产者、收口已被对端收尾的隧道
        resumeDeferredWaiters();
        closeDeferredTunnels();

        while (!m_readyRequests.empty())
        {
            const std::int64_t streamId              = m_readyRequests.front().streamId;
            const bool         isBodyTooLarge        = m_readyRequests.front().isBodyTooLarge;
            const bool         isBudgetExceeded      = m_readyRequests.front().isBudgetExceeded;
            const bool         isHeaderLimitExceeded = m_readyRequests.front().isHeaderLimitExceeded;
            const bool         isUriTooLong          = m_readyRequests.front().isUriTooLong;
            HttpRequest        request        = std::move(m_readyRequests.front().request);
            m_readyRequests.pop_front();

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
            } else if (isBudgetExceeded)
            {
                // 与 h1/h2 同一处置：全局在途正文预算不足时回 503，把剩余额度留给已经收下正文的请求
                if (m_metrics != nullptr)
                {
                    m_metrics->countBadRequest();
                }
                LOG_ERROR_FMT("Http3Session: 流 {} 的请求正文超出全局在途预算，已按 503 应答且不交给业务", streamId);
                response.setStatus(503);
                response.setBody("Service Unavailable");
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
                    // 扩展协商要看请求里的原文：这里按值取出去，协程随后会在处理器上挂起
                    co_await serveWebSocketTunnel(streamId,
                                                  request.getHeader(std::string(kWebSocketExtensionsHeaderName)).value_or(std::string{}),
                                                  response);
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
                submitResponse(streamId, response, request.method() == HttpMethod::HEAD);
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
            // 会让业务协程立刻跑起来，它回头就推帧给连接层，而此刻还在连接层的回调里
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
        if (!incoming.bodyBudget.hasBudget() && m_memoryBudget != nullptr)
        {
            // 记录刚建出来：把这份共享预算绑上（额度随记录析构归还）
            incoming.bodyBudget.reset(m_memoryBudget.get());
        }

        // 体量越界：只标记与记日志，不再缓冲；此后到达的 DATA 一律丢弃，但窗口照还。
        // 响应在服务阶段统一按 413 发出（与 h1/h2 同一口径）
        if (!incoming.isBodyTooLarge && m_parserLimits.maximumBodySize != 0
            && incoming.body.size() + data.size() > m_parserLimits.maximumBodySize)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的请求正文超过上限 {} 字节，已停止缓冲并按 413 应答",
                          streamId, m_parserLimits.maximumBodySize);
            incoming.isBodyTooLarge = true;
        }
        if (!incoming.isBodyTooLarge && !incoming.isBudgetExceeded)
        {
            const std::size_t bufferedByteCount = incoming.body.size() + data.size();
            // 全局在途预算：单条流的上限挡不住「很多条流各压一份正文」，这里按增量预留，
            // 预留失败即表示此刻收下就会超预算——与 h1/h2 同一处置（回 503，额度随记录归还）
            if (!incoming.bodyBudget.growTo(bufferedByteCount))
            {
                LOG_ERROR_FMT("Http3Session: 流 {} 的请求正文超出全局在途预算，已停止缓冲并按 503 应答", streamId);
                incoming.isBudgetExceeded = true;
            } else
            {
                incoming.body.append(reinterpret_cast<const char *>(data.data()), data.size());
            }
        }

        // 非流式：正文整段收在请求对象里，本端等于立刻消费掉了，因此到达即归还接收额度
        // （DATA 载荷的额度只在这里还，帧头那一类字节由连接层还，两边加起来才是这条流消费掉的总量）；
        // 丢弃的字节同样要还
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
            if (m_webSocketTunnels.contains(streamId))
            {
                // 隧道已建成：收尾动作要动连接层（closeTunnel 里会交出 END_STREAM），而本函数是在
                // 连接层的回调里被调用的——只记流号，由 pump() 这个安全点统一处理
                m_deferredTunnelClosures.push_back(streamId);
                return;
            }
            // 隧道还没建成（扩展 CONNECT 的头与 END_STREAM 同一趟到达）：先把「对端已收尾」记下来，
            // 建成那一刻据此立刻收口——丢掉这个事实的话业务会永远挂在 receive() 上
            m_pendingTunnelStreamsEnded.insert(streamId);
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

        // 「还没答完」的几种形态：请求已收齐但还没派发、正在收或正在跑（流式正文的处理器挂在
        // m_streamingRequests 上，不在这里面就会漏掉）、流式响应还在写、隧道（含还没建起来的）
        const bool isStillPending =
                std::ranges::any_of(m_readyRequests, [streamId](const ReadyRequest &entry) { return entry.streamId == streamId; }) ||
                m_incomingRequests.contains(streamId) || m_streamingRequests.contains(streamId) ||
                m_streamingResponses.contains(streamId) || m_webSocketTunnels.contains(streamId) ||
                m_pendingTunnelStreams.contains(streamId);
        if (isStillPending)
        {
            m_metrics->countStreamCancelled();
        }
    }

    void Http3Session::dropRequest(const std::int64_t streamId)
    {
        m_incomingRequests.erase(streamId);
        // 还没派发的请求记录一并摘掉：对端已经重置了这条流，再派发就是给一条死流跑业务
        std::erase_if(m_readyRequests, [streamId](const ReadyRequest &entry) { return entry.streamId == streamId; });
        m_pendingTunnelStreams.erase(streamId);
        m_pendingTunnelBytes.erase(streamId);
        m_pendingTunnelStreamsEnded.erase(streamId);

        if (const auto streaming = m_streamingResponses.find(streamId); streaming != m_streamingResponses.end())
        {
            // 流没了：先叫醒等缓冲排空的生产者，再摘记录。等待器与生产者各持一份共享所有权，
            // 摘表后他们读到的是 isStreamClosed，不会踩空；漏掉这一步生产者会永远等不到唤醒。
            // **唤醒本身推到安全点**：被唤醒的业务会接着写响应（那要推正文给连接层），
            // 而本函数是经连接层回调进来的，回调期间重入连接层是未定义行为
            const std::shared_ptr<StreamingResponse> state = streaming->second;
            state->isStreamClosed = true;
            if (const std::coroutine_handle<> waiter = std::exchange(state->spaceWaiter, {}); waiter != nullptr)
            {
                m_deferredWaiterResumes.push_back(waiter);
            }
            m_streamingResponses.erase(streaming);
        }

        if (m_webSocketTunnels.contains(streamId))
        {
            // 隧道：承载侧的流没了，对端对象随之关闭、挂在 receive() 上的业务要醒来收尾。
            // 同理由安全点统一处理（收尾要走连接层）；记录先留着——业务协程可能还挂着，
            // 跑完由 reapFinishedTunnels() 一起摘掉
            m_deferredTunnelClosures.push_back(streamId);
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

    void Http3Session::cancelStreamByPeer(const std::int64_t streamId)
    {
        // 只记流号：本函数由传输层的流回调调用（此刻正在读报文），而回收要动连接层并唤醒可能
        // 立刻回写响应的业务协程——那属于「回调期间重入」。真正的处理在 drainPeerCancelledStreams()
        m_peerCancelledStreamIds.push_back(streamId);
    }

    void Http3Session::drainPeerCancelledStreams()
    {
        if (m_peerCancelledStreamIds.empty() || m_connection == nullptr || m_isBroken)
        {
            return;
        }

        // 整表换出来再逐条处理：回收过程会唤醒业务协程，它们可能立刻回写响应，flush 又触发新的
        // 流收尾回调往同一张表里追加——边遍历边追加会踩到迭代器失效
        std::vector<std::int64_t> cancelledStreamIds;
        cancelledStreamIds.swap(m_peerCancelledStreamIds);

        for (const std::int64_t streamId: cancelledStreamIds)
        {
            // 告诉连接层这条流没了：它先发出「该流已重置」的通知（回调里按「还没答完」计数、再走
            // dropRequest()），请求缓冲、流式等待者与隧道记录都跟着释放
            // （与正常收尾走同一条路，不另开清理分支，也不在这里重复计数）
            m_connection->noteStreamCancelledByPeer(streamId);
        }

        // 唤醒被这些取消波及的处理器：等正文的那些只记了「有新进展」，真正的唤醒在这里做，
        // 漏掉这一步它们就会一直挂到 QUIC 空闲超时
        wakeStreamingRequests();
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
                                               .isBudgetExceeded = incoming.isBudgetExceeded,
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
        // 这条流已经派发过（隧道或流式正文的记录在案）就不再派发：重复派发会让同一条流上出现两份响应
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

        // 头收齐、正文还在路上的这一刻回 100（与 h2 同一时机）。扩展 CONNECT 排除在外：
        // 隧道里没有「请求正文」这回事，对端随后发来的是 WebSocket 帧
        if (protocolText.empty())
        {
            answerExpectContinueIfRequested(streamId, incoming);
        }

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

        // 协程本身是惰性的，构造它不会执行任何一行；**但不在这里 resume**：此刻还在连接层的
        // 回调里，处理器一上来就可能提交响应，而回调期间重入连接层是未定义行为。只置标记，
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
            submitResponse(streamId, response, streamingRequest.request.method() == HttpMethod::HEAD);
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

    Core::Task<> Http3Session::serveWebSocketTunnel(const std::int64_t streamId, std::string requestedExtensions, HttpResponse &response)
    {
        if (!response.isWebSocketUpgradeRequested())
        {
            // 业务没登记升级：这条流不是隧道，按普通响应回（RFC 9220 也允许服务端不升级）
            finalizeResponseForHttp3(streamId, response);
            // 隧道路径只可能是扩展 CONNECT（:method=CONNECT），HEAD 不适用
            submitResponse(streamId, response, false);
            co_return;
        }

        // h3 里没有 101：RFC 9220 规定隧道以 2xx 应答，此后这条流上跑的就是 WebSocket 帧本身
        response.setStatus(200);

        // 扩展协商（RFC 7692 §7.1）用与 h1/h2 同一份实现：接受时把选定参数写进应答头，
        // 本端随后按同一结论收发压缩帧——回给对端的那一行与本端开关必须同源
        const PerMessageDeflateNegotiation deflateNegotiation = negotiatePerMessageDeflate(requestedExtensions);
        if (!deflateNegotiation.responseValue.empty())
        {
            static_cast<void>(response.setHeader(std::string(kWebSocketExtensionsHeaderName), deflateNegotiation.responseValue));
        }

        // 复用流式响应那套：应答头先出去且**不结束这条流**，出向帧之后一段一段推给连接层
        const std::shared_ptr<StreamingResponse> state = streamingResponseFor(streamId);
        if (!submitStreamingResponseHead(streamId, *state, response))
        {
            co_return;
        }
        // 升级计数以应答头排入待发字节为准（与 h1 以 101 写出、h2 以刷新成功为准同一口径）
        if (m_metrics != nullptr)
        {
            m_metrics->countWebSocketUpgrade();
        }

        auto tunnel     = std::make_unique<WebSocketTunnel>();
        tunnel->handler = response.webSocketHandler();
        tunnel->peer    = std::make_unique<WebSocketPeer>(
                [this, streamId](const std::string_view frameBytes) -> Core::Task<bool>
                { co_return co_await sendTunnelBytes(streamId, frameBytes); },
                m_metrics.get());
        // 协商结论交给对端对象：决定收发两侧是否用 RSV1 压缩帧
        tunnel->peer->setPerMessageDeflateEnabled(deflateNegotiation.accepted);

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

        if (m_pendingTunnelStreamsEnded.erase(streamId) != 0)
        {
            // 扩展 CONNECT 的头与 END_STREAM 同一趟到达：隧道建立即收尾（RFC 9220 §5 的关闭方式之一）
            LOG_DEBUG_FMT("Http3Session: 流 {} 的扩展 CONNECT 与 END_STREAM 同趟到达，隧道建立即收尾", streamId);
            created.isStreamClosed = true;
            closeTunnel(streamId);
        }
        co_return;
    }

    Core::Task<> Http3Session::runTunnelBusiness(const std::int64_t streamId)
    {
        const auto found = m_webSocketTunnels.find(streamId);
        if (found == m_webSocketTunnels.end())
        {
            // 记录已经不在了说明这条隧道早被收口：安静退出，别在 closeTunnel 之外再报一次
            co_return;
        }

        WebSocketTunnel &tunnel = *found->second;
        try
        {
            // 处理器一路 co_await 隧道两端的字节，返回即业务收工
            co_await tunnel.handler(*(tunnel.peer));
        } catch (const std::exception &exception)
        {
            // 异常必须在这里接住：本协程是从连接层的解帧回调里直接 resume 的，外抛会一路穿出
            // 回调栈，牵连同一条连接上其它流的字节
            LOG_ERROR_EXCEPTION(exception, "Http3Session: 流 {} 的 WebSocket 业务处理器抛出异常，已按连接不可用收口。原因：{}", streamId, exception.what());
        } catch (...)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的 WebSocket 业务处理器抛出非标准异常（无 what() 描述）", streamId);
        }

        // 挂起期间这条隧道可能已被对端重置并回收，因此重新查一遍而不是复用 found
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

        // 只查表：对端已经重置这条流时它不在表里，重建只会留下永远清理不掉的条目。
        // 出向帧与流式响应正文走同一条路：推一段、刷一次、超过闸门就等一跳
        co_return co_await pushStreamingResponseBody(streamId, findStreamingResponse(streamId), frameBytes);
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

        // 出向到此为止：把收尾的 END_STREAM 交给连接层。已经收过口（或对端已重置）就不再重复
        const std::shared_ptr<StreamingResponse> state = findStreamingResponse(streamId);
        if (state == nullptr || state->isFinished || m_connection == nullptr || m_isBroken)
        {
            return;
        }
        state->isFinished = true;
        static_cast<void>(m_connection->appendResponseBody(streamId, std::span<const std::uint8_t>{}, true));
        flushPendingStreamData();
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
        if (m_connection == nullptr)
        {
            return false;
        }

        // 流式路径按「正文由 DATA 逐段给出」采集：content-length 一律不出，date 照补
        std::vector<QpackHeaderField> fieldLines = collectResponseFieldLines(streamId, response, true);

        // 只交响应头、不结束这条流：正文随后一段一段推过来
        if (const auto submitted = m_connection->submitResponseHead(streamId, fieldLines, false); !submitted)
        {
            handleResponseSubmissionFailure(streamId, "提交流式响应头", submitted.error().message, toHttp3ErrorCode(submitted.error().kind));
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
            co_return submitStreamingResponseHead(streamId, *state, response);
        }

        // 其余段落是 h1 分块帧成帧的正文（RFC 9112 §7.1）：h3 里没有分块帧这一层，只把帧里的负载收下
        co_return co_await pushStreamingResponseBody(streamId, state, chunkFramePayload(chunk));
    }

    Core::Task<bool> Http3Session::pushStreamingResponseBody(const std::int64_t streamId, const std::shared_ptr<StreamingResponse> &state,
                                                            const std::string_view bytes)
    {
        // 状态不在（流已被重置）或承载侧的流已经关闭：写多少都出不去，直接按失败收手
        if (state == nullptr || state->isStreamClosed || m_connection == nullptr || m_isBroken)
        {
            co_return false;
        }

        if (!bytes.empty())
        {
            if (const auto appended = m_connection->appendResponseBody(streamId, asBytes(bytes), false); !appended)
            {
                handleResponseSubmissionFailure(streamId, "追加流式响应正文", appended.error().message, toHttp3ErrorCode(appended.error().kind));
                co_return false;
            }
        }
        // 推完就往外送：连接层把这流的待发字节一次性交给传输层，本端不留副本也不等人来取
        flushPendingStreamData();

        // 与流式响应同一道闸：生产者跑得比网络快就挂起等排空；流一关闭立即收手
        while (!state->isStreamClosed && streamingResponsePendingByteCount(streamId) > kStreamingResponseBufferByteCount)
        {
            co_await ResponseSpaceAwaiter(*this, streamId, state);
        }
        co_return !state->isStreamClosed;
    }

    void Http3Session::finishStreamingResponse(const std::int64_t streamId, HttpResponse &response)
    {
        const std::shared_ptr<StreamingResponse> state = streamingResponseFor(streamId);
        if (!state->isHeadSent)
        {
            if (!submitStreamingResponseHead(streamId, *state, response))
            {
                return; // 提交失败时已经记过日志
            }
        }
        if (state->isFinished || m_connection == nullptr || m_isBroken)
        {
            return; // 已经收过口（或会话已作废）：再交一次 END_STREAM 只会报错
        }

        // 收尾：交出「正文到此为止」，连接层把最后一段与 END_STREAM 一起送出去
        state->isFinished = true;
        if (const auto appended = m_connection->appendResponseBody(streamId, std::span<const std::uint8_t>{}, true); !appended)
        {
            handleResponseSubmissionFailure(streamId, "收尾流式响应", appended.error().message, toHttp3ErrorCode(appended.error().kind));
            return;
        }
        flushPendingStreamData();
    }

    void Http3Session::submitResponse(const std::int64_t streamId, const HttpResponse &response, const bool isHeadRequest)
    {
        if (m_connection == nullptr)
        {
            return;
        }

        const std::string_view body = response.body();
        // 无正文的状态码（1xx/204/304）不给出 content-length；HEAD 与其余状态码都要给长度，只是正文不发
        const bool isBodylessStatus = HttpResponse::isBodylessStatusCode(response.status());
        // HEAD：正文一个字节都不发（与 h2 的 isHeadRequest ? {} : body 同一处置），content-length
        // 下面仍按完整正文长度给出——这正是 HEAD 的语义（RFC 9110 §9.3.2）
        const bool hasBody = !isBodylessStatus && !isHeadRequest && !body.empty();

        // 头部与 h1/h2 逐字同源：采集器负责丢连接特定字段、逐条展开可重复头并补齐类型/长度/日期
        const std::vector<QpackHeaderField> fieldLines = collectResponseFieldLines(streamId, response, false);

        // 没有正文时交完头就收尾；有正文则头先走（不结束流），紧接一次把整段正文推过去并收尾
        if (const auto submitted = m_connection->submitResponseHead(streamId, fieldLines, !hasBody); !submitted)
        {
            handleResponseSubmissionFailure(streamId, "提交响应头", submitted.error().message, toHttp3ErrorCode(submitted.error().kind));
            return;
        }
        if (hasBody)
        {
            if (const auto appended = m_connection->appendResponseBody(streamId, asBytes(body), true); !appended)
            {
                handleResponseSubmissionFailure(streamId, "提交响应正文", appended.error().message, toHttp3ErrorCode(appended.error().kind));
            }
        }
    }

    void Http3Session::answerExpectContinueIfRequested(const std::int64_t streamId, const IncomingRequest &incoming)
    {
        if (m_connection == nullptr)
        {
            return;
        }
        if (!isContinueExpected(incoming.request.getHeader("expect").value_or(std::string{})))
        {
            return;
        }

        // 这一刻还没有 DATA，也无从知道正文到底会不会来：只在请求声明了正的 content-length 时回，
        // 免得给「带 Expect 却没有正文」的请求凭空塞一个 100。真没声明长度又确实要发正文的对端，
        // 按 RFC 9110 §10.1.1 的兜底走「等自己的 expect 超时后照发」，不会卡死
        std::size_t declaredBodyByteCount = 0;
        if (!parseContentLengthValue(incoming.request.getHeader("content-length").value_or(std::string{}), declaredBodyByteCount) ||
            declaredBodyByteCount == 0)
        {
            LOG_DEBUG_FMT("Http3Session: 流 {} 带 Expect: 100-continue 却没声明正的正文长度，不回 100", streamId);
            return;
        }

        // 只交一个 :status 100 的头块，不收尾也不带正文（RFC 9114 §5.3.2 的信息性响应）。
        // 失败不外抛也不改答：100 只是催对端发正文，真正的问题会在随后交最终响应时暴露出来
        const std::vector<QpackHeaderField> fieldLines{QpackHeaderField{.name = kStatusHeaderName, .value = "100"}};
        if (const auto submitted = m_connection->submitResponseHead(streamId, fieldLines, false); !submitted)
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 的 100 Continue 未能排进待发字节。原因：{}", streamId, submitted.error().message);
        }
    }

    void Http3Session::answerMalformedRequest(const std::int64_t streamId, const std::string_view reason)
    {
        // 这条流已经派发过或已经答过就只记日志：一条流只能有一份响应，业务此刻可能正在往里写正文
        const bool isAlreadyDispatched =
                m_streamingResponses.contains(streamId) || m_streamingRequests.contains(streamId) ||
                m_pendingTunnelStreams.contains(streamId) || m_webSocketTunnels.contains(streamId) ||
                std::ranges::any_of(m_readyRequests, [streamId](const ReadyRequest &entry) { return entry.streamId == streamId; });
        if (isAlreadyDispatched || (m_connection != nullptr && m_connection->isLocalStreamFinished(streamId)))
        {
            LOG_WARN_FMT("Http3Session: 流 {} 的请求头部被连接层判为畸形（{}），但该流已派发或已作答，只记日志不再应答", streamId, reason);
            return;
        }

        LOG_WARN_FMT("Http3Session: 流 {} 的请求头部畸形（{}），按 RFC 9114 §4.1.2 先回 400 再结束该流", streamId, reason);
        HttpResponse response;
        response.setStatus(400);
        response.setBody(reason);
        static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
        submitResponse(streamId, response, false);
        // 作答之后这条流上不再派发业务，也不留请求缓冲：连接层此刻只看不再解释，剩下的字节会直接还额度
        dropRequest(streamId);
    }

    void Http3Session::handleResponseSubmissionFailure(const std::int64_t streamId, const char *const what, const std::string_view reason,
                                                       const Http3ErrorCode errorCode)
    {
        if (m_connection == nullptr || m_isBroken)
        {
            return; // 整条会话已经作废，剩下的只有销毁
        }
        if (m_connection->isLocalStreamFinished(streamId))
        {
            // 连接层找不到这条流，或本端早已收尾：对端多半已经把它 RST 掉了（慢业务上很常见）。
            // 这只是**这一条流**的响应发不出去，连接与其它流都还健康——按流级作废，
            // 不置 m_isBroken（那会连带关掉整条 QUIC 连接；h2 同场景也只作废该流）
            LOG_WARN_FMT("Http3Session: 流 {} 已经不在了（对端多半已重置该流），{}作废；连接与其它流不受影响", streamId, what);
            dropRequest(streamId);
            return;
        }
        markBroken(errorCode, std::string(what) + "失败：" + std::string(reason));
    }

    void Http3Session::markBroken(const Http3ErrorCode errorCode, const std::string_view reason)
    {
        m_isBroken = true;
        LOG_WARN_FMT("Http3Session: {}（{}），HTTP/3 会话作废", reason, http3ErrorCodeName(errorCode));
    }
} // namespace AsynGyanis::Net
