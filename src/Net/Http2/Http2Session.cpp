#include "Net/Http2/Http2Session.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpDate.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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

        /**
         * @brief 判断响应头名是否是 HTTP/2 禁止的连接特定头
         * @details 与连接层的同名判定同源（RFC 7540 §8.1.2.2）：连接层是最终把关者，这里先一步
         *          把它们丢掉，否则整条响应会被连接层拒绝、对端一个字节都收不到。
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
            // WINDOW_UPDATE、RST_STREAM、GOAWAY）→ 取请求与正文 → 路由 → 再写出响应
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

        std::exception_ptr handlerException = nullptr;
        try
        {
            co_await m_router.route(request, m_response);
        } catch (...)
        {
            // 业务异常：等本轮结束再改写响应（改写走下面的 500 分支，与 HTTP/1.1 侧同一处置）
            handlerException = std::current_exception();
        }

        if (handlerException != nullptr)
        {
            // 业务可能已经写了一半头部与正文，必须整体重置再填 500，否则会发出一条半成品响应
            m_response.reset();
            m_response.setStatus(500);
            m_response.setBody("Internal Server Error");
            static_cast<void>(m_response.setHeader("content-type", "text/plain"));
        }

        // WebSocket 升级走的是 HTTP/1.1 的 101 切换协议；HTTP/2 上的等价机制是 RFC 8441 的扩展
        // CONNECT，本片不做：明确回 501，不静默当成普通响应放过去
        if (m_response.isWebSocketUpgradeRequested())
        {
            LOG_ERROR_FMT("Http2Session: HTTP/2 上不支持 WebSocket 升级（RFC 8441 的扩展 CONNECT 属于后续片），"
                          "已回 501 并保持连接可用。request-id {}，路径 {}",
                          request.requestId(), request.uri());
            m_response.reset();
            m_response.setStatus(501);
            m_response.setBody("WebSocket over HTTP/2 Not Implemented");
            static_cast<void>(m_response.setHeader("content-type", "text/plain; charset=utf-8"));
        }

        // 响应自动带本次请求的 request-id（与 HTTP/1.1 侧同口径）：调用方显式设过就不覆盖
        if (!request.requestId().empty() && !m_response.getHeader(std::string(kRequestIdHeaderName)).has_value())
        {
            static_cast<void>(m_response.setHeader(std::string(kRequestIdHeaderName), std::string(request.requestId())));
        }

        const bool isServed = co_await sendResponse(streamId, m_response, isHeadRequest);
        if (!isServed)
        {
            // 响应没排出去，不计状态码类与延迟——那会把「对端收不到」的请求算成已应答
            co_return false;
        }

        const int statusCode = m_response.status();
        const std::chrono::steady_clock::duration requestElapsed = std::chrono::steady_clock::now() - requestReceivedTime;
        if (m_metrics != nullptr)
        {
            m_metrics->recordResponse(statusCode, requestElapsed);
        }

        // 一条请求一条日志：request-id 同时出现在响应头与这里，客户端报的响应与服务端的处理记录
        // 因此能按同一个键对齐（口径与 HTTP/1.1 侧一致）
        if (!request.requestId().empty())
        {
            LOG_INFO_FMT("Http2Session: 请求已完成。request-id {}，路径 {}，状态码 {}，耗时 {}us", request.requestId(),
                         request.uri(), statusCode, std::chrono::duration_cast<std::chrono::microseconds>(requestElapsed).count());
        }
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

        // 遍历单值视图取头名、再用 headerValues() 逐条取全部取值：Set-Cookie 这类可重复头不丢
        for (const auto &headerEntry: response.headers())
        {
            const std::string &headerName = headerEntry.first;
            if (isConnectionSpecificHeaderName(headerName))
            {
                // 业务常按 HTTP/1.1 的习惯设 connection: close；HTTP/2 里它一律非法（§8.1.2.2），
                // 丢掉它而不是让整条响应被连接层拒绝
                LOG_DEBUG_FMT("Http2Session: 已丢弃 HTTP/2 禁止的连接特定响应头「{}」", headerName);
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
        if (!hasContentLengthHeader && !mustNotDeclareContentLength(response.status()))
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
        // 状态码越界（HttpResponse::setStatus 不校验取值范围）时回 500：连接层只接受 100..999，
        // 把越界值原样交出去会让这条流一个字节都发不出去，对端只能等到超时
        const int responseStatus = response.status();
        std::uint32_t wireStatusCode = static_cast<std::uint32_t>(responseStatus);
        if (responseStatus < 100 || responseStatus > 999)
        {
            LOG_ERROR_FMT("Http2Session: 响应状态码 {} 越界（应为 100..999），流 {} 已改回 500", responseStatus, streamId);
            wireStatusCode = 500U;
        }

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
