#include "Net/Http2/Http2Session.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpDate.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 接收窗口大小，单位字节：与 HTTP/1.1 会话同档，只用来接住刚到的字节，
        /// 跨读的半帧由帧解码器自己缓冲，因此固定大小就够，不需要按报文长度增长
        constexpr std::size_t kHttp2ReceiveWindowByteCount = 8ull * 1024;

        /// 映射给 HttpRequest 的协议版本原文：HTTP/2 报文里没有版本字段，用协议名补齐
        constexpr std::string_view kHttp2RequestVersion = "HTTP/2";

        /// 响应头自动补齐规则要认出的三个头名（与 HttpResponse::appendHead() 同一套）
        constexpr std::string_view kContentTypeHeaderName = "content-type";
        constexpr std::string_view kContentLengthHeaderName = "content-length";
        constexpr std::string_view kDateHeaderName = "date";

        /// 自动补出的内容类型：与 HttpResponse 的兜底选择一致（不会被浏览器当脚本执行）
        constexpr std::string_view kDefaultContentTypeValue = "text/plain";

        /// 报文行的分隔符：HttpResponse::writeChunk() 交出的段落一律以它定行（RFC 9112 §2.2）
        constexpr std::string_view kCrLf = "\r\n";
        constexpr std::size_t kCrLfLength = 2;

        /// 分块帧长度行的位数上限：帧长按每字节两位十六进制写出，与 HttpResponse 侧同一算法
        constexpr std::size_t kChunkLengthLineMaximumLength = sizeof(std::size_t) * 2;

        /**
         * @brief 把异常的指针取成可读文本
         * @details 流式响应中途失败时头部已经上线，改状态码已不可能，日志是唯一能交代原因的地方。
         * @param exceptionPointer 捕获到的异常指针；为空时返回空串
         * @return std::string 异常的 what() 文本；非标准异常给一句中文占位
         */
        std::string describeException(const std::exception_ptr &exceptionPointer)
        {
            if (!exceptionPointer)
            {
                return {};
            }

            std::string description;
            try
            {
                std::rethrow_exception(exceptionPointer);
            } catch (const std::exception &exception)
            {
                description = exception.what();
            } catch (...)
            {
                // 非标准异常没有 what()：给一句中文占位，好过把它当成「没有异常」
                description = "非标准异常（无 what() 描述）";
            }
            return description;
        }

        /**
         * @brief 判断响应头名是否是 HTTP/2 禁止的连接特定头
         * @details 与连接层的同名判定同源（RFC 9113 §8.2.2，即 RFC 7540 §8.1.2.2）：连接层是最终
         *          把关者，这里先一步把它们丢掉，否则整条响应会被连接层拒绝、对端一个字节都收不到。
         * @param name 头名（已由 HttpResponse 归一化为小写）
         * @return true 表示这是连接特定头，不能出现在 HTTP/2 报文里
         */
        bool isConnectionSpecificHeaderName(const std::string_view name) noexcept
        {
            constexpr std::string_view kForbiddenHeaderNames[] = {"connection", "keep-alive", "proxy-connection",
                                                                  "transfer-encoding", "upgrade"};
            return std::find(std::begin(kForbiddenHeaderNames), std::end(kForbiddenHeaderNames), name) !=
                   std::end(kForbiddenHeaderNames);
        }

        /**
         * @brief 判断状态码是否不得自动补 content-length
         * @details 与 HttpResponse::mustNotDeclareContentLength() 同口径：1xx 与 204 不补，
         *          304 明确允许携带（RFC 7230 §3.3.2）
         * @param statusCode 响应状态码
         * @return true 表示不补 content-length
         */
        bool mustNotDeclareContentLength(const int statusCode) noexcept
        {
            return statusCode / 100 == 1 || statusCode == 204;
        }
    } // namespace

    Http2Session::Http2Session(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router,
                               std::shared_ptr<const HttpServerLimits> limits,
                               std::shared_ptr<HttpMetricsCollector> metrics,
                               std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator,
                               HttpParserLimits parserLimits) :
        // 基类负责 TLS 通道的所有权与 HTTP/1.1 路径；本类只补一条 HTTP/2 循环，因此形参与基类逐项对应。
        // 共享指针按值传两份（基类一份、本类一份）而不是移走：两边指向的是同一批对象，
        // 不存在两份配置或两个采集端
        HttpsSession(loop, std::move(tlsSocket), router, limits, metrics, requestIdGenerator, parserLimits),
        m_router(router),
        m_parserLimits(parserLimits),
        // 基类的同名成员都是私有的：HTTP/2 循环要跨协程挂起使用这些装配，因此本类各持一份引用/共享指针
        m_limits(limits != nullptr ? std::move(limits) : std::make_shared<const HttpServerLimits>()),
        m_metrics(std::move(metrics)),
        m_requestIdGenerator(std::move(requestIdGenerator))
    {
    }

    Core::Task<> Http2Session::start()
    {
        /**
         * @brief 退出时无条件收口的守卫：与 HttpsSession 的同一做法，覆盖协程内抛异常的路径
         */
        struct TransportCloser
        {
            Http2Session *session = nullptr; ///< 需要在退出时收口的会话

            ~TransportCloser()
            {
                if (session != nullptr)
                {
                    // 基类重写后的 close()：先收 TLS 通道再复位存活位与取消源
                    session->close();
                }
            }
        } closer{this};

        try
        {
            co_await tlsSocket().handshake();
        } catch (const std::exception &handshakeException)
        {
            // 握手失败没有可信的明文可回：记日志后直接结束会话，收口交给上面的 RAII 守卫
            LOG_ERROR_FMT("Http2Session: TLS 握手失败，已关闭连接（描述符={}）。原因：{}", tlsSocket().fileDescriptor(),
                          handshakeException.what());
            co_return;
        }

        // ALPN 分流点：结果产生于握手过程，因此只有这里读到的那一个值是可信的。
        // 非 h2（http/1.1 或客户端没提 ALPN）原样交回基类的 HTTP/1.1 事务循环，行为与继承前一致
        const std::string selectedProtocol = tlsSocket().selectedAlpnProtocol();
        if (selectedProtocol != kHttp2AlpnProtocolName)
        {
            LOG_DEBUG_FMT("Http2Session: ALPN 协商结果是「{}」，按 HTTP/1.1 会话继续（描述符={}）", selectedProtocol,
                          tlsSocket().fileDescriptor());
            co_await HttpsSession::start();
            co_return;
        }

        LOG_DEBUG_FMT("Http2Session: ALPN 协商出 h2，进入 HTTP/2 循环（描述符={}）", tlsSocket().fileDescriptor());
        co_await runHttp2Loop();
        co_return;
    }

    Core::Task<> Http2Session::runHttp2Loop()
    {
        std::vector<char> receiveBuffer(kHttp2ReceiveWindowByteCount);

        // 连接被关停时把停止请求转成当前在途请求的协作式取消：业务只认 request.cancelToken() 一处，
        // 与 HTTP/1.1 侧的 ConnectionCancelForwarder 同一约定。回调随本协程帧存活，析构即注销
        std::stop_callback cancelForwarder(cancelable().stopToken(),
                                           [this]()
                                           {
                                               if (m_servingRequest != nullptr)
                                               {
                                                   m_servingRequest->requestCancel();
                                               }
                                           });

        while (isAlive() && isTlsTransportOpen())
        {
            // 相位时限与 HTTP/1.1 侧同口径：已经在收某条请求（有挂着等正文的流）就按读超时约束
            // 相邻两次成功读取的间隔，纯粹等新请求才用空闲容忍度
            const bool isRequestInProgress = !m_pendingRequests.empty();
            refreshIdleDeadline(isRequestInProgress ? m_limits->readTimeout : m_limits->idleTimeout);

            ssize_t receivedLength = 0;
            try
            {
                receivedLength = co_await tlsSocket().asyncReceive(receiveBuffer.data(), receiveBuffer.size());
            } catch (const std::exception &)
            {
                // 传输层读失败（对端 RST、描述符被清扫协程关掉、TLS 记录错误）：字节流已断，只剩收尾
                break;
            }
            if (receivedLength <= 0)
            {
                // 0 是对端正常关闭，负值是连接不可用，两者都只剩收尾
                break;
            }
            refreshIdleDeadline(m_limits->readTimeout);

            // 驱动顺序：喂字节（协议失败时 GOAWAY 已排进待发）→ 立刻写出（初始 SETTINGS、ACK、
            // WINDOW_UPDATE、RST_STREAM、GOAWAY）→ 取请求与正文 → 路由 → 再写出响应。
            // 流式响应是这条顺序里的例外：它的头部与每个正文段在路由期间就当场写出（见
            // sendStreamingSegment()），否则「边写边到」会退化成「攒到处理器结束再发」
            const Http2ConnectionFeedStatus feedStatus =
                    m_connection.feedBytes(receiveBuffer.data(), static_cast<std::size_t>(receivedLength));
            if (!co_await flushOutgoingBytes())
            {
                break;
            }

            absorbPendingRequests();
            absorbReceivedData();

            if (feedStatus == Http2ConnectionFeedStatus::Failed || m_connection.hasFailed())
            {
                // 状态机已把带 errorCode() 的 GOAWAY 排进待发字节，上面那次写出已经把它送出去
                if (m_metrics != nullptr)
                {
                    m_metrics->countBadRequest();
                }
                LOG_ERROR_FMT("Http2Session: HTTP/2 连接层失败，已按错误码 {} 发 GOAWAY 并收口。原因：{}",
                              http2ErrorCodeName(m_connection.errorCode()), m_connection.errorMessage());
                break;
            }

            if (!co_await servePendingRequests())
            {
                break;
            }
            if (!co_await flushOutgoingBytes())
            {
                break;
            }

            // 收尾通告已经随上面那次写出送到对端（GOAWAY 排在响应字节之后），此刻又没有在途请求：
            // 按 h1 侧「回完当前响应即收口」的同一口径收口——h2 没有连接级 close 头可用（RFC 9113 §8.2.2），
            // 关闭意图已经由 GOAWAY 表达完，继续留着这条连接只会占资源
            if (m_isGoAwaySent && m_pendingRequests.empty())
            {
                LOG_DEBUG_FMT("Http2Session: GOAWAY 已写出且没有在途请求，收口连接（已服务 {} 条请求）", m_servedRequestCount);
                break;
            }
        }

        // 每一轮都以「取出并写出待发字节」收尾，因此退出时不必再补一次写出：对端已关闭或本侧
        // 写不出去时重试只会多留一条无意义的错误日志。收口统一交给 start() 里的 RAII 守卫
        co_return;
    }

    void Http2Session::absorbPendingRequests()
    {
        for (Http2Request &http2Request: m_connection.takeRequests())
        {
            PendingRequest pending;
            pending.streamId = http2Request.streamId;
            // 头块带 END_STREAM 的请求没有正文，当场就是「收齐」状态
            pending.isRemoteEndStream = !http2Request.hasBody;
            pending.request = mapToHttpRequest(http2Request);
            m_pendingRequests.insert_or_assign(http2Request.streamId, std::move(pending));
        }
    }

    void Http2Session::absorbReceivedData()
    {
        for (const Http2ReceivedData &receivedData: m_connection.takeReceivedData())
        {
            const auto requestIterator = m_pendingRequests.find(receivedData.streamId);
            if (requestIterator != m_pendingRequests.end())
            {
                PendingRequest &pending = requestIterator->second;
                const std::size_t bodyByteCount = pending.request.body().size() + receivedData.data.size();
                if (m_parserLimits.maximumBodySize != 0 && bodyByteCount > m_parserLimits.maximumBodySize)
                {
                    // 与 HTTP/1.1 侧同口径：体量越界的请求回 413。这里只标记与记日志，
                    // 响应在服务阶段统一发出；此后到达的 DATA 一律丢弃（但仍要还窗口）
                    if (!pending.isBodyTooLarge)
                    {
                        LOG_ERROR_FMT("Http2Session: 流 {} 的请求正文超过上限 {} 字节，已停止缓冲并按 413 应答",
                                      receivedData.streamId, m_parserLimits.maximumBodySize);
                        pending.isBodyTooLarge = true;
                    }
                }
                if (!pending.isBodyTooLarge)
                {
                    pending.request.appendBody(receivedData.data.data(), receivedData.data.size());
                }
                if (receivedData.endStream)
                {
                    pending.isRemoteEndStream = true;
                }
            }

            // 消费即还窗口：按帧负载原长（含 padding）报量，否则对端的发送窗口会一路耗尽、
            // 大请求停在半途等本端的 WINDOW_UPDATE
            std::string errorText;
            if (!m_connection.creditReceivedData(receivedData.streamId, receivedData.flowControlByteCount, &errorText))
            {
                LOG_ERROR_FMT("Http2Session: 归还接收窗口失败（流 {}，{} 字节）：{}", receivedData.streamId,
                              receivedData.flowControlByteCount, errorText);
            }
        }
    }

    Core::Task<bool> Http2Session::servePendingRequests()
    {
        // 按流号升序服务：对端流号严格递增，因此遍历顺序就是请求的到达顺序
        for (auto requestIterator = m_pendingRequests.begin(); requestIterator != m_pendingRequests.end();)
        {
            PendingRequest &pending = requestIterator->second;
            // 正文还没收齐的请求继续攒着：它后面的请求可以照常服务（HTTP/2 允许响应乱序）
            if (!pending.isRemoteEndStream && !pending.isBodyTooLarge)
            {
                ++requestIterator;
                continue;
            }

            // 从路由到响应排入待发字节算「在途工作」：优雅关闭（drain）据此只等真正在做事的连接
            setBusy(true);
            m_servingRequest = &pending.request;
            const bool isServed = co_await serveOneRequest(pending);
            m_servingRequest = nullptr;
            setBusy(false);

            // 这条流已经不会再拿到新的正文：挂起记录就此摘掉，其后到达的 DATA 由连接层丢弃
            requestIterator = m_pendingRequests.erase(requestIterator);
            if (!isServed)
            {
                co_return false;
            }
        }
        co_return true;
    }

    Core::Task<bool> Http2Session::serveOneRequest(PendingRequest &pending)
    {
        HttpRequest &request = pending.request;
        const std::uint32_t streamId = pending.streamId;

        if (m_requestIdGenerator != nullptr)
        {
            request.setRequestId(m_requestIdGenerator->resolve(request));
        }

        // HEAD 只发头部，一个正文字节都不发（RFC 9110 §9.1）：抑制放在这里而不是响应层——
        // 响应层的「无正文」语义只由状态码决定，与请求方法无关
        const bool isHeadRequest = request.method() == HttpMethod::HEAD;

        if (pending.isBodyTooLarge)
        {
            // 体量越界按 HTTP/1.1 侧同一口径处置：只计入 badRequestCount，不计入已处理的请求条数
            // （那边的 413 由解析失败路径回，同样不落 totalRequestCount）
            if (m_metrics != nullptr)
            {
                m_metrics->countBadRequest();
            }
            HttpResponse tooLargeResponse;
            tooLargeResponse.setStatus(413);
            tooLargeResponse.setBody("Payload Too Large");
            static_cast<void>(tooLargeResponse.setHeader("content-type", "text/plain; charset=utf-8"));
            const bool isRejected = co_await sendResponse(streamId, tooLargeResponse, isHeadRequest);
            co_return isRejected;
        }

        // 与 HTTP/1.1 侧同口径：收齐的请求才计数，耗时从「请求收齐」算到「响应排入待发字节」
        if (m_metrics != nullptr)
        {
            m_metrics->countParsedRequest();
        }
        const std::chrono::steady_clock::time_point requestReceivedTime = std::chrono::steady_clock::now();

        // 响应对象按连接复用：容器容量跨请求保留，复用必须配一次复位
        m_response.reset();

        // 路由之前装 h2 版流式发送回调（与 h1 侧同一接线位置）：捕获本请求的流号，响应对象按连接
        // 复用而回调随请求重建。业务调 startChunkedResponse()/writeChunk() 时不必知道底层是哪种协议，
        // SseStream 这类建在 writeChunk 之上的工具因此零改动就能在 h2 上工作
        m_response.setChunkSender(
                [this, streamId](const std::string_view segment) -> Core::Task<bool>
                {
                    co_return co_await sendStreamingSegment(streamId, segment);
                });

        std::exception_ptr handlerException = nullptr;
        try
        {
            co_await m_router.route(request, m_response);
        } catch (...)
        {
            // 业务异常：等本轮结束再改写响应（改写走下面的 500 分支，与 HTTP/1.1 侧同一处置）
            handlerException = std::current_exception();
        }

        // 流式头部是否已经随首段正文上线：上线之后状态码与头部都改不了，异常路径也只能补末片收尾
        // （判据见 HttpResponse::hasSentChunkedHead() 的文档，与 h1 侧同一条）
        const bool isStreamingStarted = m_response.isChunkedResponse() && m_response.hasSentChunkedHead();

        if (handlerException != nullptr)
        {
            if (isStreamingStarted)
            {
                // 头部已在对端手里：任何「改 500」都是不可能的，只能补末片让消息收完整并把原因记进日志
                LOG_ERROR_FMT("Http2Session: 流式响应的业务处理中途抛出异常，头部已上线无法改写状态码，"
                              "已补末片 DATA 并收口该流。request-id {}，路径 {}，流 {}，原因：{}",
                              request.requestId(), request.uri(), streamId, describeException(handlerException));
            } else
            {
                // 业务可能已经写了一半头部与正文，必须整体重置再填 500，否则会发出一条半成品响应
                m_response.reset();
                m_response.setStatus(500);
                m_response.setBody("Internal Server Error");
                static_cast<void>(m_response.setHeader("content-type", "text/plain"));
            }
        }

        // WebSocket 升级走的是 HTTP/1.1 的 101 切换协议；HTTP/2 上的等价机制是 RFC 8441 的扩展
        // CONNECT，本片不做：明确回 501，不静默当成普通响应放过去
        if (m_response.isWebSocketUpgradeRequested())
        {
            if (isStreamingStarted)
            {
                // 两种报文形态互斥：升级要发 101 并交出连接，而流式头部已经在对端手里，改不了
                LOG_ERROR_FMT("Http2Session: 本响应已经按流式写出头部，又登记了 WebSocket 升级，两者互斥；"
                              "HTTP/2 上也不支持升级，已按流式响应补末片收尾。request-id {}，路径 {}",
                              request.requestId(), request.uri());
            } else
            {
                LOG_ERROR_FMT("Http2Session: HTTP/2 上不支持 WebSocket 升级（RFC 8441 的扩展 CONNECT 属于后续片），"
                              "已回 501 并保持连接可用。request-id {}，路径 {}",
                              request.requestId(), request.uri());
                m_response.reset();
                m_response.setStatus(501);
                m_response.setBody("WebSocket over HTTP/2 Not Implemented");
                static_cast<void>(m_response.setHeader("content-type", "text/plain; charset=utf-8"));
            }
        }

        // 响应自动带本次请求的 request-id（与 HTTP/1.1 侧同口径）：调用方显式设过就不覆盖
        if (!request.requestId().empty() && !m_response.getHeader(std::string(kRequestIdHeaderName)).has_value())
        {
            static_cast<void>(m_response.setHeader(std::string(kRequestIdHeaderName), std::string(request.requestId())));
        }

        // 流式响应：头部（首段时）与每个正文段都已由上面的回调当场发出，这里只补末片把消息收完整；
        // 其余响应照旧整份发出
        bool isServed = false;
        if (m_response.isChunkedResponse())
        {
            isServed = co_await finishStreamingResponse(streamId);
        } else
        {
            isServed = co_await sendResponse(streamId, m_response, isHeadRequest);
        }
        if (!isServed)
        {
            // 响应没排出去，不计状态码类与延迟——那会把「对端收不到」的请求算成已应答
            co_return false;
        }

        const int statusCode = m_response.status();
        const std::chrono::steady_clock::duration requestElapsed = std::chrono::steady_clock::now() - requestReceivedTime;
        // 流式响应中途出异常时正文只发了一半，落账等于把半成品记成已应答（状态码也不是真实结果），
        // 因此跳过——那条路径已经由上面的错误日志交代（与 h1 侧同一判据）
        const bool isTruncatedStreamingResponse = isStreamingStarted && handlerException != nullptr;
        if (m_metrics != nullptr && !isTruncatedStreamingResponse)
        {
            m_metrics->recordResponse(statusCode, requestElapsed);
        }

        // 一条请求一条日志：request-id 同时出现在响应头与这里，客户端报的响应与服务端的处理记录
        // 因此能按同一个键对齐（口径与 HTTP/1.1 侧一致）
        if (!request.requestId().empty() && !isTruncatedStreamingResponse)
        {
            LOG_INFO_FMT("Http2Session: 请求已完成。request-id {}，路径 {}，状态码 {}，耗时 {}us", request.requestId(),
                         request.uri(), statusCode, std::chrono::duration_cast<std::chrono::microseconds>(requestElapsed).count());
        }

        // 单连接请求上限的判定放在最后：本条响应已经排入待发字节，收尾通告排在它之后
        noteServedRequest();
        co_return true;
    }

    HttpRequest Http2Session::mapToHttpRequest(const Http2Request &http2Request)
    {
        HttpRequest request;
        // 方法原文经 methodFromString 映射：未收录的方法（CONNECT、TRACE、自定义动词）落到
        // HttpMethod::UNKNOWN，路由器按既有规则回 404/405，绝不静默降级成某条业务路由
        request.setMethod(HttpRequest::methodFromString(http2Request.method));
        // :path 与 :authority 分别对应 HttpRequest 的 uri 与 host；:scheme 在 HTTP/1.1 报文里
        // 没有对应位置（服务端已知自己在 TLS 上），因此有意不映射，也不伪造一个头部
        request.setUri(http2Request.path);
        request.setHttpVersion(std::string(kHttp2RequestVersion));

        bool hasHostHeader = false;
        for (const HpackHeaderField &headerField: http2Request.headerFields)
        {
            // 头名在连接层已校验为小写、头值已校验无控制字符，这里原样转交
            request.addHeader(headerField.name, headerField.value);
            if (headerField.name == "host")
            {
                hasHostHeader = true;
            }
        }
        // :authority 就是权威主机来源：对端没显式给 host 头时用它补齐，与 HTTP/1.1 侧「请求行
        // 目标 + host 头」的读取口径对齐（业务读 host 时两条协议拿到同一个值）
        if (!http2Request.authority.empty() && !hasHostHeader)
        {
            request.addHeader("host", http2Request.authority);
        }
        return request;
    }

    std::vector<HpackHeaderField> Http2Session::collectResponseHeaderFields(const HttpResponse &response)
    {
        std::vector<HpackHeaderField> headerFields;
        bool hasContentTypeHeader = false;
        bool hasContentLengthHeader = false;
        bool hasDateHeader = false;

        // 流式响应的正文由各 DATA 帧给出，长度在收尾前未知：content-length 与 DATA 负载总长必须一致
        // （RFC 9113 §8.1.2.6），此刻算不出正确值，因此这条路径一律不写它（h1 的分块模式同样不写）
        const bool isStreamingResponse = response.isChunkedResponse();

        // 遍历单值视图取头名、再用 headerValues() 逐条取全部取值：Set-Cookie 这类可重复头不丢
        for (const auto &headerEntry: response.headers())
        {
            const std::string &headerName = headerEntry.first;
            if (isConnectionSpecificHeaderName(headerName))
            {
                // 业务常按 HTTP/1.1 的习惯设 connection: close；HTTP/2 里它一律非法（§8.2.2），
                // 丢掉它而不是让整条响应被连接层拒绝
                LOG_DEBUG_FMT("Http2Session: 已丢弃 HTTP/2 禁止的连接特定响应头「{}」", headerName);
                continue;
            }
            if (isStreamingResponse && headerName == kContentLengthHeaderName)
            {
                // 流式模式按 h1 契约本就不该有这条头（startChunkedResponse 已删过一遍）：业务后补的
                // 一样丢掉，否则一个恰好等于某段长度的数字会让对端按它定界、把后续 DATA 当多余字节
                LOG_DEBUG_FMT("Http2Session: 流式响应的正文长度由 DATA 帧给出，已丢弃 content-length 响应头");
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

            for (const std::string &value: response.headerValues(headerName))
            {
                headerFields.push_back(HpackHeaderField{.name = headerName, .value = value});
            }
        }

        // 自动补齐与 HttpResponse::appendHead() 同口径：有正文却漏设媒体类型按纯文本下发，
        // 长度按正文实际字节数补，日期缺省补当前时刻
        const std::string_view responseBody = response.body();
        if (!hasContentTypeHeader && !responseBody.empty())
        {
            headerFields.push_back(HpackHeaderField{.name = std::string(kContentTypeHeaderName),
                                                    .value = std::string(kDefaultContentTypeValue)});
        }
        if (!hasContentLengthHeader && !isStreamingResponse && !mustNotDeclareContentLength(response.status()))
        {
            headerFields.push_back(HpackHeaderField{.name = std::string(kContentLengthHeaderName),
                                                    .value = std::to_string(responseBody.size())});
        }
        if (!hasDateHeader)
        {
            headerFields.push_back(HpackHeaderField{.name = std::string(kDateHeaderName),
                                                    .value = formatHttpDate(std::chrono::system_clock::now())});
        }
        return headerFields;
    }

    Core::Task<bool> Http2Session::sendResponse(const std::uint32_t streamId, const HttpResponse &response, const bool isHeadRequest)
    {
        // 状态码越界（HttpResponse::setStatus 不校验取值范围）时由助手改回 500：连接层只接受 100..999，
        // 把越界值原样交出去会让这条流一个字节都发不出去，对端只能等到超时
        const std::uint32_t wireStatusCode = normalizeWireStatusCode(response.status(), streamId);

        const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(response);
        // HEAD 只发头：正文视图换成空，头部里的 content-length 仍按完整正文补齐
        const std::string_view responseBody = isHeadRequest ? std::string_view{} : response.body();
        const bool isBodyEmpty = responseBody.empty();

        std::string errorText;
        if (!m_connection.sendResponseHeaders(streamId, wireStatusCode, headerFields, isBodyEmpty, &errorText))
        {
            LOG_ERROR_FMT("Http2Session: 流 {} 的响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
            co_return false;
        }
        if (!isBodyEmpty && !m_connection.sendResponseData(streamId, responseBody, true, &errorText))
        {
            LOG_ERROR_FMT("Http2Session: 流 {} 的响应正文未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
            co_return false;
        }
        co_return true;
    }

    std::uint32_t Http2Session::normalizeWireStatusCode(const int responseStatus, const std::uint32_t streamId)
    {
        // 100..999 是 RFC 9110 §15 的状态码区间（也是连接层的接受范围）：越界值上线等于把这条流废掉
        if (responseStatus < 100 || responseStatus > 999)
        {
            LOG_ERROR_FMT("Http2Session: 响应状态码 {} 越界（应为 100..999），流 {} 已改回 500", responseStatus, streamId);
            return 500U;
        }
        return static_cast<std::uint32_t>(responseStatus);
    }

    std::string_view Http2Session::chunkFramePayload(const std::string_view chunkFrame)
    {
        // 长度行到第一个 CRLF 为止，位数不会超过一个 size_t 的十六进制位数
        const std::size_t lengthLineEndIndex = chunkFrame.find(kCrLf);
        if (lengthLineEndIndex == std::string_view::npos || lengthLineEndIndex == 0 ||
            lengthLineEndIndex > kChunkLengthLineMaximumLength)
        {
            throw Base::LogicException("Http2Session: writeChunk 交出的分块帧没有合法的长度行（应为「<十六进制字节数>\\r\\n」），"
                                       "本段未发出；请检查 HttpResponse::writeChunk() 的实现与其文档是否一致");
        }

        // 长度前缀是负载长度的唯一权威来源：正文里出现 CRLF 也不影响边界判定
        std::size_t payloadLength = 0;
        const auto [parseEnd, parseError] =
                std::from_chars(chunkFrame.data(), chunkFrame.data() + lengthLineEndIndex, payloadLength, 16);
        if (parseError != std::errc() || parseEnd != chunkFrame.data() + lengthLineEndIndex)
        {
            throw Base::LogicException("Http2Session: 分块帧的长度行不是合法的十六进制字节数（「" +
                                       std::string(chunkFrame.substr(0, lengthLineEndIndex)) + "」），本段未发出；"
                                       "请检查 HttpResponse::writeChunk() 的实现与其文档是否一致");
        }

        // 长度行 + 负载 + 结尾 CRLF 必须恰好用满整段字节：多一个字节或少一个都说明帧布局与文档不符，
        // 此时宁可当场报错，也绝不把帧头或残缺的负载当成正文发出去
        if (chunkFrame.size() != lengthLineEndIndex + kCrLfLength + payloadLength + kCrLfLength ||
            chunkFrame.compare(chunkFrame.size() - kCrLfLength, kCrLfLength, kCrLf) != 0)
        {
            throw Base::LogicException(std::format("Http2Session: 分块帧的实际长度 {} 字节与长度行声明的 {} 字节不一致，本段未发出；"
                                                   "请检查 HttpResponse::writeChunk() 的实现与其文档是否一致",
                                                   chunkFrame.size(), payloadLength));
        }
        return chunkFrame.substr(lengthLineEndIndex + kCrLfLength, payloadLength);
    }

    Core::Task<bool> Http2Session::sendStreamingSegment(const std::uint32_t streamId, const std::string_view segment)
    {
        // 本侧已判定写不出去：与 h1 契约一致，短路返回 false 且不新增日志（首次失败已经交代过原因）
        if (m_isConnectionUnusable)
        {
            co_return false;
        }

        std::string errorText;
        if (!m_response.hasSentChunkedHead())
        {
            // 首个段落是 HttpResponse::writeChunk() 推上来的 HTTP/1.1 头部文本（状态行 + 头部块），
            // 内容是 h1 线格式而不是 h2 要发的头块：这次调用只当「头部该上线了」的信号，
            // 真正发出的字段按响应对象现取（HPACK 编码与连接特定头剥离都在下一跳完成）。
            // 不带 END_STREAM：正文段随后还要发，与 h1 侧「头部随首段正文上线」同一时机
            const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(m_response);
            if (!m_connection.sendResponseHeaders(streamId, normalizeWireStatusCode(m_response.status(), streamId), headerFields, false,
                                                  &errorText))
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的流式响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
                co_return false;
            }
        } else
        {
            // 其余段落是 HttpResponse::writeChunk() 按 h1 分块帧成帧的正文（RFC 9112 §7.1）：HTTP/2 里
            // 没有分块帧这一层（transfer-encoding 属禁止头，RFC 9113 §8.2.2），只把帧里的负载发成 DATA 帧
            const std::string_view payload = chunkFramePayload(segment);
            if (!m_connection.sendResponseData(streamId, payload, false, &errorText))
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的流式正文段未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                co_return false;
            }
        }

        // 当场写出：流式响应的要点是「边写边到」，把数据攒在待发缓冲里等主循环下一轮就等于推迟一段。
        // 窗口不足时连接层已把整段排进该流的发送队列（返回 true），这一次没有字节可写也不算失败：
        // 等对端 WINDOW_UPDATE 到达后由连接层在同一入口内续发，正是 writeChunk 文档里「入队而非失败」的含义
        co_return co_await flushOutgoingBytes();
    }

    Core::Task<bool> Http2Session::finishStreamingResponse(const std::uint32_t streamId)
    {
        // 本侧已判定写不出去：不再重试，也不重复记日志（与 h1 契约一致）
        if (m_isConnectionUnusable)
        {
            co_return false;
        }

        std::string errorText;
        if (!m_response.hasSentChunkedHead())
        {
            // 一段正文都没写出来（业务只调了 startChunkedResponse()）：头部与 END_STREAM 一起发出，
            // 对端因此拿到一条没有正文的完整响应，而不是挂在一条永远收不满的消息上；
            // 与 h1 侧「空流式响应补出头部与终止块」是同一个位置
            const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(m_response);
            if (!m_connection.sendResponseHeaders(streamId, normalizeWireStatusCode(m_response.status(), streamId), headerFields, true,
                                                  &errorText))
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的空流式响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
                co_return false;
            }
            co_return co_await flushOutgoingBytes();
        }

        // 头部已随首段上线：补一个零长 DATA 帧带 END_STREAM（RFC 9113 §6.1 允许零长 DATA，且不占流控窗口），
        // 消息边界由末片给出——h1 侧补的是 `0\r\n\r\n` 终止块，位置与语义都对应
        if (!m_connection.sendResponseData(streamId, std::string_view{}, true, &errorText))
        {
            LOG_ERROR_FMT("Http2Session: 流 {} 的流式响应末片未能排入待发字节，对端收不到消息结尾。原因：{}", streamId, errorText);
            co_return false;
        }
        co_return co_await flushOutgoingBytes();
    }

    void Http2Session::noteServedRequest()
    {
        ++m_servedRequestCount;

        // 上限为 0 表示不限；已经发过收尾通告就不再重复触发（同一原因只通告一次）
        if (m_isGoAwaySent || m_limits->maximumRequestsPerConnection == 0 ||
            m_servedRequestCount < m_limits->maximumRequestsPerConnection)
        {
            return;
        }

        // h1 那一侧靠 connection: close 告诉对端「连接用完了」，HTTP/2 里这类头一律禁止（RFC 9113 §8.2.2），
        // 同一个意思只能由 GOAWAY 表达（§6.8：通告之后新流一律被回 REFUSED_STREAM，已受理的流继续做完）
        m_isGoAwaySent = true;
        std::string errorText;
        if (!m_connection.sendGoAway(std::format("本连接已服务 {} 条请求，达到 HttpServerLimits::maximumRequestsPerConnection 上限 {}："
                                                 "不再受理新流，请在新连接上重试",
                                                 m_servedRequestCount, m_limits->maximumRequestsPerConnection),
                                     &errorText))
        {
            LOG_ERROR_FMT("Http2Session: 达到单连接请求上限后未能发出 GOAWAY 收尾通告，连接将按对端关闭或读超时收口。原因：{}", errorText);
            return;
        }
        LOG_INFO_FMT("Http2Session: 已达到单连接请求上限 {} 条，已发 GOAWAY 收尾通告并不再受理新流",
                     m_limits->maximumRequestsPerConnection);
    }

    Core::Task<bool> Http2Session::flushOutgoingBytes()
    {
        // 本侧已判定写不出去：不再重试，也不重复记日志（首个失败已经交代过原因）
        if (m_isConnectionUnusable)
        {
            co_return false;
        }

        std::string outgoingBytes = m_connection.takeOutgoingBytes();
        if (outgoingBytes.empty())
        {
            co_return true;
        }

        // 写之前把截止时间刷成写超时：对端只连不读（慢消费者）时写侧会挂起，
        // 超过容忍度由清扫协程收口，而不是把连接永远挂在发送上
        refreshIdleDeadline(m_limits->writeTimeout);

        std::string failureReason;
        bool isSucceeded = false;
        try
        {
            // 一直写到整段出门：TLS 记录层的 asyncSend 允许部分写，截断的帧对端再也找不回边界
            std::size_t writtenByteCount = 0;
            while (writtenByteCount < outgoingBytes.size())
            {
                const ssize_t writeLength =
                        co_await tlsSocket().asyncSend(outgoingBytes.data() + writtenByteCount, outgoingBytes.size() - writtenByteCount);
                if (writeLength <= 0)
                {
                    // 对端已在底层关闭（非正值而不是异常）：同样属于传输失败，原因在收尾处补一句
                    break;
                }
                writtenByteCount += static_cast<std::size_t>(writeLength);
            }
            isSucceeded = writtenByteCount == outgoingBytes.size();
        } catch (const Base::Exception &exception)
        {
            // 传输层写失败（对端 RST、描述符被清扫协程关掉、等可写期间被关闭）：字节流已断，
            // 原因只暂存，等收尾处连同结论记一次
            failureReason = exception.what();
        }

        if (!isSucceeded)
        {
            if (failureReason.empty())
            {
                failureReason = "对端已关闭连接或连接不可用";
            }
            LOG_ERROR_FMT("Http2Session: 待发字节写出失败，连接已不可用，本条数据未完整发出，请停止继续写并收口连接。原因：{}",
                          failureReason);
            m_isConnectionUnusable = true;
        }
        co_return isSucceeded;
    }
} // namespace AsynGyanis::Net
