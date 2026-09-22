#include "Net/Http/HttpChunkFrame.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http2/Http2Session.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpDate.h"
#include "Net/WebSocket/PerMessageDeflate.h"
#include "Net/WebSocket/WebSocketHandshake.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
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

        /// 无效描述符的取值：与 Core::AsyncSocket::close() 之后的 fileDescriptor() 一致
        constexpr int kInvalidSocketDescriptor = -1;

        /// RFC 8441 里 WebSocket 隧道用的 :protocol 取值（扩展 CONNECT 的协议名 token）
        constexpr std::string_view kWebSocketProtocolName = "websocket";

        /// 扩展 CONNECT 的升级应答：状态码与应答头名（RFC 8441 §5 用 2xx 而不是 101）
        constexpr std::uint32_t kWebSocketAcceptedStatusCode = 200U;
        constexpr std::string_view kWebSocketAcceptHeaderName = "sec-websocket-accept";

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
         * @brief 判断状态码是否不得自动补 content-length
         * @details 与 HttpResponse::mustNotDeclareContentLength() 同口径：1xx / 204 / 304 都不补。
         *          304 允许携带 content-length，但只允许取「同一请求的 200 会发出的正文长度」
         *          （RFC 9112 §6.2）——自动补出来的是 0，正好是这条 MUST NOT 禁止的取值
         * @param statusCode 响应状态码
         * @return true 表示不补 content-length
         */
        bool mustNotDeclareContentLength(const int statusCode) noexcept
        {
            return statusCode / 100 == 1 || statusCode == 204 || statusCode == 304;
        }
    } // namespace

    Http2Session::Http2Session(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router,
                               std::shared_ptr<const HttpServerLimits> limits,
                               std::shared_ptr<HttpMetricsCollector> metrics,
                               std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator,
                               HttpParserLimits parserLimits,
                               std::shared_ptr<HttpMemoryBudget> memoryBudget) :
        // TLS 模式下基类只能拿到一条不持有描述符的占位套接字：真实描述符的所有权必须独一份，
        // 归 TlsSocket 管（它负责先 SSL_shutdown 再关描述符）
        HttpSession(Core::AsyncSocket(loop, kInvalidSocketDescriptor), router, limits, metrics, requestIdGenerator, parserLimits, memoryBudget),
        // 回退路径的解析器按调用方给的解析上限构造，否则回退到 HTTP/1.1 时
        // 头部/正文上限会退回默认值，该回的 431/413 就不出现了
        m_parser(parserLimits),
        m_scheduler(loop.scheduler()),
        m_router(router),
        m_parserLimits(parserLimits),
        // 基类那条套接字是占位，本类要用到的装配各持一份引用/共享指针；共享指针按值传两份
        // 而不是移走：两边指向的是同一批对象，不存在两份配置或两个采集端
        m_limits(limits != nullptr ? std::move(limits) : std::make_shared<const HttpServerLimits>()),
        m_metrics(std::move(metrics)),
        m_requestIdGenerator(std::move(requestIdGenerator)),
        m_memoryBudget(std::move(memoryBudget))
    {
        m_tlsSocket.emplace(std::move(tlsSocket));
    }

    Http2Session::Http2Session(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router,
                               std::shared_ptr<const HttpServerLimits> limits,
                               std::shared_ptr<HttpMetricsCollector> metrics,
                               std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator,
                               HttpParserLimits parserLimits,
                               std::shared_ptr<HttpMemoryBudget> memoryBudget) :
        // 明文模式：没有第二条通道，套接字直接交给基类持有，本类不留 TLS 通道（m_tlsSocket 保持空）
        HttpSession(std::move(socket), router, limits, metrics, requestIdGenerator, parserLimits, memoryBudget),
        m_parser(parserLimits),
        m_scheduler(loop.scheduler()),
        m_router(router),
        m_parserLimits(parserLimits),
        m_limits(limits != nullptr ? std::move(limits) : std::make_shared<const HttpServerLimits>()),
        m_metrics(std::move(metrics)),
        m_requestIdGenerator(std::move(requestIdGenerator)),
        m_memoryBudget(std::move(memoryBudget))
    {
    }

    Core::Task<> Http2Session::start()
    {
        /**
         * @brief 退出时无条件收口的守卫：覆盖协程内抛异常的路径
         */
        struct TransportCloser
        {
            Http2Session *session = nullptr; ///< 需要在退出时收口的会话

            ~TransportCloser()
            {
                if (session != nullptr)
                {
                    // 本类重写后的 close()：先收传输通道（TLS 或明文套接字）再复位存活位与取消源
                    session->close();
                }
            }
        } closer{this};

        // 明文会话（h2c 先验知识）：该端口上的协议由部署决定，没有可协商的余地，
        // 直接进 HTTP/2 循环——对端若发的是 HTTP/1.1 报文，连接层会按前奏校验失败回 GOAWAY
        if (!m_tlsSocket.has_value())
        {
            LOG_DEBUG_FMT("Http2Session: 明文连接按 h2c 先验知识进入 HTTP/2 循环（描述符={}）", transportFileDescriptor());
            co_await runHttp2Loop();
            co_return;
        }

        try
        {
            co_await m_tlsSocket->handshake();
        } catch (const std::exception &handshakeException)
        {
            // 握手失败没有可信的明文可回：记日志后直接结束会话，收口交给上面的 RAII 守卫
            LOG_ERROR_EXCEPTION(handshakeException, "Http2Session: TLS 握手失败，已关闭连接（描述符={}）。原因：{}", transportFileDescriptor(),
                                handshakeException.what());
            co_return;
        }

        // ALPN 分流点：结果产生于握手过程，因此只有这里读到的那一个值是可信的。
        // 非 h2（http/1.1 或客户端没提 ALPN）按 HTTP/1.1 事务循环继续，与明文侧共用同一份实现
        const std::string selectedProtocol = m_tlsSocket->selectedAlpnProtocol();
        if (selectedProtocol != kHttp2AlpnProtocolName)
        {
            LOG_DEBUG_FMT("Http2Session: ALPN 协商结果是「{}」，按 HTTP/1.1 会话继续（描述符={}）", selectedProtocol,
                          transportFileDescriptor());
            const std::function<bool()> alivePredicate = [this]()
            {
                return isAlive();
            };
            // 正文预算必须一并传下去：漏传时 setMemoryBudget() 在 TLS 的 h1 回落下静默失效，
            // 而同样的请求走明文端会被 503 收口
            co_await detail::httpKeepAliveLoop(*m_tlsSocket, cancelable(), m_router, m_parser, m_receiveBuffer, alivePredicate,
                                              *this, *m_limits, m_metrics.get(), m_requestIdGenerator.get(), m_memoryBudget.get());
            co_return;
        }

        LOG_DEBUG_FMT("Http2Session: ALPN 协商出 h2，进入 HTTP/2 循环（描述符={}）", transportFileDescriptor());
        co_await runHttp2Loop();
        co_return;
    }

    void Http2Session::close()
    {
        // SETTINGS 仍待 ACK 且已过专项限额：这是「会话被关停」路径上唯一还能写字节的时刻——
        // 清扫协程正是先调用本函数再关描述符，而会话自己的读协程要等描述符关闭才被唤醒，
        // 那时一个字节都写不出去。先把收口原因告知对端（尽力），再走基类收口。
        // 已经失败过说明 GOAWAY 带着真正的错误码发过一次了，这里不再补第二张通告
        if (isTransportOpen() && !m_connection.hasFailed() && isSettingsAcknowledgementExpired() && queueSettingsTimeoutGoAway())
        {
            writeOutgoingBytesBestEffort();
        }

        // TLS 通道要先收：明文模式下它为空，等于直接走基类关闭真实套接字
        if (m_tlsSocket.has_value())
        {
            m_tlsSocket->close();
        }
        HttpSession::close();
    }

    bool Http2Session::isAlive() const noexcept
    {
        return HttpSession::isAlive() && isTransportOpen();
    }

    std::string Http2Session::remoteAddress() const
    {
        // TLS 模式下基类那条套接字不持有描述符，地址只能向真实通道要
        return m_tlsSocket.has_value() ? m_tlsSocket->remoteAddress().toString() : HttpSession::remoteAddress();
    }

    std::string Http2Session::localAddress() const
    {
        return m_tlsSocket.has_value() ? m_tlsSocket->localAddress().toString() : HttpSession::localAddress();
    }

    void Http2Session::onGracefulShutdownRequested()
    {
        // 通道已经不可用就不必尝试了：调用方紧接着会 close()，写出去也没人收
        if (!isTransportOpen())
        {
            return;
        }

        std::string errorText;
        if (!m_connection.sendGoAway("服务器正在优雅关停：本连接不再受理新流，已通告流号之前的请求照常处理完", &errorText))
        {
            // 已经发过收尾通告（达到单连接请求上限）或连接未完成协商/已失败：不是故障，不记日志
            return;
        }

        // 尽力送出：描述符还在，这正是「关停路径上唯一还能写字节」的时刻（close() 紧随其后）
        writeOutgoingBytesBestEffort();
    }

    bool Http2Session::isTransportOpen() const noexcept
    {
        return m_tlsSocket.has_value() ? m_tlsSocket->fileDescriptor() != kInvalidSocketDescriptor
                                       : socket().fileDescriptor() != kInvalidSocketDescriptor;
    }

    int Http2Session::transportFileDescriptor() const noexcept
    {
        return m_tlsSocket.has_value() ? m_tlsSocket->fileDescriptor() : socket().fileDescriptor();
    }

    Core::Task<ssize_t> Http2Session::transportReceive(void *const buffer, const std::size_t length)
    {
        if (m_tlsSocket.has_value())
        {
            co_return co_await m_tlsSocket->asyncReceive(buffer, length);
        }
        co_return co_await socket().asyncReceive(buffer, length);
    }

    Core::Task<ssize_t> Http2Session::transportSend(const void *const buffer, const std::size_t length)
    {
        if (m_tlsSocket.has_value())
        {
            co_return co_await m_tlsSocket->asyncSend(buffer, length);
        }
        co_return co_await socket().asyncSend(buffer, length);
    }

    Core::Task<> Http2Session::runHttp2Loop()
    {
        // 接收缓冲提到成员上：流式正文的泵与主循环交替驱动同一条连接，缓冲必须共用一份
        m_http2ReceiveBuffer.resize(kHttp2ReceiveWindowByteCount);

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

        while (isAlive() && isTransportOpen())
        {
            // 对端一直没 ACK 本端初始 SETTINGS：按 SETTINGS_TIMEOUT 收口（RFC 7540 §6.5.3 允许服务端
            // 以连接错误收口）。判定放在每轮读之前：对端若只发帧却不 ACK，到期后的第一轮就能收掉
            if (isSettingsAcknowledgementExpired())
            {
                if (queueSettingsTimeoutGoAway())
                {
                    // GOAWAY 能出去就出：写出失败也已由 flushOutgoingBytes() 记过原因，连接照常收口
                    static_cast<void>(co_await flushOutgoingBytes());
                }
                break;
            }

            // 相位时限与 HTTP/1.1 侧同口径：已经在收某条请求（有挂着等正文的流）就按读超时约束
            // 相邻两次成功读取的间隔，纯粹等新请求才用空闲容忍度；SETTINGS 待 ACK 的握手期用
            // 专项限额的剩余时长，到点由清扫协程收口——因此不新造定时器
            const std::chrono::milliseconds settingsIdleBudget = settingsAcknowledgementIdleBudget();
            if (settingsIdleBudget > std::chrono::milliseconds::zero())
            {
                refreshIdleDeadline(settingsIdleBudget);
            }
            else
            {
                const bool isRequestInProgress = !m_pendingRequests.empty();
                refreshIdleDeadline(isRequestInProgress ? m_limits->readTimeout : m_limits->idleTimeout);
            }

            // 驱动顺序：喂字节（协议失败时 GOAWAY 已排进待发）→ 立刻写出（初始 SETTINGS、ACK、
            // WINDOW_UPDATE、RST_STREAM、GOAWAY）→ 取请求与正文 → 路由 → 再写出响应。
            // 流式响应是这条顺序里的例外：它的头部与每个正文段在路由期间就当场写出（见
            // sendStreamingSegment()），否则「边写边到」会退化成「攒到处理器结束再发」
            if (!co_await driveConnectionOnce())
            {
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

    Core::Task<bool> Http2Session::driveConnectionOnce()
    {
        // 读之前的时限由调用方定，这里不动：主循环按相位（SETTINGS 待 ACK / 有在途请求 / 纯空闲）
        // 选不同的容忍度，流式正文的泵则在收请求期间按读超时——在此处一律刷成读超时会把
        // 空闲超时与 SETTINGS 超时这两道闸无声地拆掉

        ssize_t receivedLength = 0;
        try
        {
            receivedLength = co_await transportReceive(m_http2ReceiveBuffer.data(), m_http2ReceiveBuffer.size());
        } catch (const std::exception &)
        {
            // 传输层读失败（对端 RST、描述符被清扫协程关掉、TLS 记录错误）：字节流已断，只剩收尾
            co_return false;
        }
        if (receivedLength <= 0)
        {
            // 0 是对端正常关闭，负值是连接不可用，两者都只剩收尾
            co_return false;
        }
        refreshIdleDeadline(m_limits->readTimeout);

        const Http2ConnectionFeedStatus feedStatus =
                m_connection.feedBytes(m_http2ReceiveBuffer.data(), static_cast<std::size_t>(receivedLength));
        if (!co_await flushOutgoingBytes())
        {
            co_return false;
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
            co_return false;
        }
        co_return true;
    }

    void Http2Session::finishStreamingRequestBody(PendingRequest &pending, const bool isBodyTooLarge)
    {
        // 业务不再读了：把还挂着的正文按已消费处理。这一步顺带归还接收窗口——不还的话那些字节
        // 一直占着对端的发送窗口，而它们永远不会再被交付给任何人
        HttpStreamBody &streamBody = pending.streamBody;
        streamBody.consumePending();

        // 对端已收尾：这条流的接收侧自然结束，没有「还在发」的对端需要中止。
        // 对端已取消（RST_STREAM）时同理——流都不在了，再发 RST 也到不了对端手里
        if (streamBody.isComplete() || (streamBody.isBroken() && !isBodyTooLarge))
        {
            return;
        }

        // 对端可能还在发剩下的正文，而本端已经不需要了：按 RFC 9113 §8.1 请它停下（NO_ERROR 不是
        // 「出错」，而是「这条流不再需要了」）。不收下再丢掉，省的是对端可能还有几百 MB 的带宽
        std::string abortErrorText;
        if (!m_connection.abortStream(pending.streamId,
                                      isBodyTooLarge ? "请求正文超过上限，响应已发出，本端不再需要剩余正文"
                                                     : "业务未读完请求正文，本端不再需要剩余字节",
                                      &abortErrorText))
        {
            LOG_DEBUG_FMT("Http2Session: 流 {} 的流式正文收尾时未能请求对端中止发送。原因：{}", pending.streamId, abortErrorText);
        }
    }

    void Http2Session::absorbPendingRequests()
    {
        for (Http2Request &http2Request: m_connection.takeRequests())
        {
            PendingRequest pending;
            // 本条流的正文额度从这里开始记：记录被摘掉时由 Reservation 自己归还
            pending.bodyBudget.reset(m_memoryBudget.get());
            pending.streamId = http2Request.streamId;
            // 头块带 END_STREAM 的请求没有正文，当场就是「收齐」状态
            pending.isRemoteEndStream = !http2Request.hasBody;
            // RFC 8441 的扩展 CONNECT：隧道请求的后续处理与普通请求完全不同（200 + 流变隧道）
            pending.isExtendedConnect = !http2Request.protocol.empty();
            pending.isWebSocketTunnel = http2Request.protocol == kWebSocketProtocolName;
            if (pending.isExtendedConnect)
            {
                // 扩展 CONNECT 没有「请求正文」这一回事：它一收齐就该交给路由，不能等对端的 END_STREAM——
                // 对端在隧道收尾前根本不会发（它发来的是 WebSocket 帧，不是请求正文）
                pending.isRemoteEndStream = true;
            }
            pending.request = mapToHttpRequest(http2Request);
            // 流式路由：头部收齐即可派发，正文边收边交给业务，不必等 END_STREAM（与 h1 侧同一判据）。
            // 扩展 CONNECT 排除在外——它的「正文」是隧道里的帧，走隧道那条完全不同的路径
            pending.isStreamingBody = !pending.isExtendedConnect
                                      && m_router.hasStreamingRoute(pending.request.method(), pending.request.uri());
            // 期待 100-continue 与否要在**移入容器之前**取出来：pending 随后被 std::move 走，
            // 移后对象的字段（含映射好的头部）都成了空壳，读它只会得到空串
            const bool isContinueRequested =
                    http2Request.hasBody && isContinueExpected(pending.request.getHeader("expect").value_or(std::string{}));
            m_pendingRequests.insert_or_assign(http2Request.streamId, std::move(pending));

            // RFC 9110 §10.1.1 在 h2 上的等价物：对端声明了 Expect: 100-continue 且还有正文要发时，
            // 先回一个 100 的 HEADERS（不带 END_STREAM），免得对端等到自己的超时才发正文
            if (isContinueRequested)
            {
                std::string continueErrorText;
                const Http2ResponseSendStatus continueStatus =
                        m_connection.sendResponseHeaders(http2Request.streamId, 100U, {}, false, &continueErrorText);
                if (continueStatus != Http2ResponseSendStatus::Sent)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的 100 Continue 未能排入待发字节。原因：{}", http2Request.streamId,
                                  continueErrorText);
                }
            }
        }
    }

    void Http2Session::absorbReceivedData()
    {
        for (const Http2ReceivedData &receivedData: m_connection.takeReceivedData())
        {
            absorbOneReceivedData(receivedData);
        }
    }

    void Http2Session::absorbOneReceivedData(const Http2ReceivedData &receivedData)
    {
        const auto requestIterator = m_pendingRequests.find(receivedData.streamId);
        if (requestIterator != m_pendingRequests.end())
        {
            PendingRequest &pending = requestIterator->second;

            // 流式路由：正文进那条流自己的缓冲，业务按到达批次取走；窗口在**被消费**时才还
            // （见 HttpStreamBody），因此这里不调 creditReceivedData——那正是背压的落点
            if (pending.isStreamingBody)
            {
                HttpStreamBody &streamBody = pending.streamBody;
                if (m_parserLimits.maximumBodySize != 0
                    && streamBody.totalReceivedByteCount() + receivedData.data.size() > m_parserLimits.maximumBodySize)
                {
                    // 与 h1 侧同一口径：体量越界就不再收，响应在服务阶段按 413 发出。
                    // 此后到达的 DATA 一律丢弃，但窗口照还——不还的话对端会卡在自己耗尽的窗口上
                    if (!streamBody.isBodyTooLarge())
                    {
                        LOG_ERROR_FMT("Http2Session: 流 {} 的请求正文超过上限 {} 字节，已停止流式接收并按 413 应答",
                                      receivedData.streamId, m_parserLimits.maximumBodySize);
                        streamBody.markBodyTooLarge();
                    }
                }
                else
                {
                    streamBody.append(receivedData.data, receivedData.flowControlByteCount, receivedData.endStream);
                }

                // 越界标记之后到达的字节直接丢弃，但本片占用的窗口仍要还回去
                if (streamBody.isBodyTooLarge())
                {
                    std::string discardErrorText;
                    if (!m_connection.creditReceivedData(receivedData.streamId, receivedData.flowControlByteCount, &discardErrorText))
                    {
                        LOG_ERROR_FMT("Http2Session: 归还接收窗口失败（流 {}，{} 字节）：{}", receivedData.streamId,
                                      receivedData.flowControlByteCount, discardErrorText);
                    }
                }
                if (receivedData.endStream)
                {
                    pending.isRemoteEndStream = true;
                }
                return;
            }

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
            // 全局在途正文预算：与 HTTP/1.1 侧同一口径，只是记账挂在每条流上。
            // 同样必须在收的过程中判——等 END_STREAM 再判，内存已经占住了
            if (!pending.isBodyTooLarge && !pending.isBudgetExceeded && !pending.bodyBudget.growTo(bodyByteCount))
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的请求正文超出全局在途预算（已占 {} 字节），已停止缓冲并按 503 应答",
                              receivedData.streamId, m_memoryBudget->reservedByteCount());
                pending.isBudgetExceeded = true;
            }
            if (!pending.isBodyTooLarge && !pending.isBudgetExceeded)
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

    Core::Task<bool> Http2Session::servePendingRequests()
    {
        // 第一轮：先服务非隧道请求（快的，不会阻塞循环）
        for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end();)
        {
            PendingRequest &pending = it->second;
            // 流式正文的请求在头部收齐那一刻就可以服务：正文由业务边收边读，不等 END_STREAM。
            // 其余请求要等正文收齐（或已判超限/超预算）
            const bool isReadyToServe = pending.isStreamingBody || pending.isRemoteEndStream || pending.isBodyTooLarge
                                        || pending.isBudgetExceeded;
            if (!isReadyToServe)
            {
                // 对端可能已经 RST 掉了这条流（头收齐、正文没收完就取消）：那样的请求再也不会
                // 变成「可服务」，留着它既占着请求头与已收正文，又让「有待办就不算空闲」的判据
                // 一直为真（空闲超时随之失效）。按流已关闭清掉，并计入单流取消
                Http2StreamState streamState{};
                if (!m_connection.tryGetStreamState(it->first, streamState) || streamState == Http2StreamState::Closed)
                {
                    if (m_metrics != nullptr)
                    {
                        m_metrics->countStreamCancelled();
                    }
                    it = m_pendingRequests.erase(it);
                    continue;
                }
                ++it;
                continue;
            }

            if (pending.isWebSocketTunnel)
            {
                ++it;
                continue;
            }

            setBusy(true);
            m_servingRequest = &pending.request;
            const RequestServeOutcome serveOutcome = co_await serveOneRequest(pending);
            m_servingRequest = nullptr;
            setBusy(false);

            it = m_pendingRequests.erase(it);
            if (serveOutcome == RequestServeOutcome::ConnectionUnusable)
            {
                co_return false;
            }
        }

        // 第二轮：服务隧道请求（它会阻塞，但其它请求已先行应答）
        for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end(); ++it)
        {
            PendingRequest &pending = it->second;
            if (!pending.isWebSocketTunnel)
            {
                // 第一轮漏掉的不应该还有非隧道请求，但保留了保护
                continue;
            }
            setBusy(true);
            m_servingRequest = &pending.request;
            const RequestServeOutcome serveOutcome = co_await serveOneRequest(pending);
            m_servingRequest = nullptr;
            setBusy(false);
            m_pendingRequests.erase(it);
            if (serveOutcome == RequestServeOutcome::ConnectionUnusable)
            {
                co_return false;
            }
            // 隧道只有一个，不用继续循环
            break;
        }
        co_return true;
    }

    Core::Task<Http2Session::RequestServeOutcome> Http2Session::serveOneRequest(PendingRequest &pending)
    {
        HttpRequest &request = pending.request;
        const std::uint32_t streamId = pending.streamId;

        // 对端取消这条流（RFC 9113 §8.1 的 RST_STREAM CANCEL）不是故障：只记录、计入统计并停掉这一条，
        // 连接与其它流照旧工作。两处出口共用同一处记账，避免同一件事记成两种措辞或重复计数
        const auto noteStreamCancelled = [this, &request, streamId]()
        {
            if (m_metrics != nullptr)
            {
                m_metrics->countStreamCancelled();
            }
            LOG_INFO_FMT("Http2Session: 流 {} 已被对端取消（RST_STREAM），本条响应不再发送，连接继续服务其它流。"
                         "request-id {}，路径 {}",
                         streamId, request.requestId(), request.uri());
        };

        if (m_requestIdGenerator != nullptr)
        {
            m_requestIdGenerator->resolveInto(request);
        }

        // HEAD 只发头部，一个正文字节都不发（RFC 9110 §9.1）：抑制放在这里而不是响应层——
        // 响应层的「无正文」语义只由状态码决定，与请求方法无关
        const bool isHeadRequest = request.method() == HttpMethod::HEAD;

        // RFC 8441 §4 的协议协商：本端只认 :protocol=websocket。其它取值属于「扩展 CONNECT 说的协议本端
        // 没实现」，明确回 501——不能把它当成一条未知方法的普通请求交给路由，那会回 404/405，语义不对
        if (pending.isExtendedConnect && !pending.isWebSocketTunnel)
        {
            LOG_ERROR_FMT("Http2Session: 扩展 CONNECT 的 :protocol 本端未实现（只支持 websocket），已回 501 并保持连接可用。"
                          "request-id {}，路径 {}",
                          request.requestId(), request.uri());
            HttpResponse unsupportedResponse;
            unsupportedResponse.setStatus(501);
            unsupportedResponse.setBody("CONNECT Protocol Not Implemented");
            static_cast<void>(unsupportedResponse.setHeader("content-type", "text/plain; charset=utf-8"));
            const RequestServeOutcome unsupportedOutcome =
                    toRequestServeOutcome(co_await sendResponse(streamId, unsupportedResponse, isHeadRequest));
            if (unsupportedOutcome == RequestServeOutcome::StreamCancelled)
            {
                noteStreamCancelled();
            }
            co_return unsupportedOutcome;
        }

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
            const Http2ResponseSendStatus tooLargeSendStatus = co_await sendResponse(streamId, tooLargeResponse, isHeadRequest);
            const RequestServeOutcome tooLargeOutcome = toRequestServeOutcome(tooLargeSendStatus);
            if (tooLargeOutcome == RequestServeOutcome::StreamCancelled)
            {
                noteStreamCancelled();
            }
            if (tooLargeOutcome == RequestServeOutcome::Served)
            {
                // 413 已经完整发出：按 RFC 9113 §8.1 请对端无错地中止这条请求的正文发送。
                // 不中止的话本端会把剩下的字节全部收下再丢掉——对端可能有几百 MB 要传（纯白耗带宽）
                std::string abortErrorText;
                if (!m_connection.abortStream(streamId, "请求正文超过上限，响应（413）已发出，本端不再需要剩余正文", &abortErrorText))
                {
                    // 拿不到流（例如对端恰好同时收尾了）不影响已排入的 413：只记一条调试日志
                    LOG_DEBUG_FMT("Http2Session: 流 {} 的正文超限后未能请求对端中止发送。原因：{}", streamId, abortErrorText);
                }
            }
            co_return tooLargeOutcome;
        }

        if (pending.isBudgetExceeded)
        {
            // 全局预算用尽不是对端的错，因此不计入 badRequestCount：只是本端此刻没余量。
            // 同样请对端中止这条流的正文发送——剩余字节本端一律不要再收
            HttpResponse overloadedResponse;
            overloadedResponse.setStatus(503);
            overloadedResponse.setBody("服务繁忙，请稍后重试");
            static_cast<void>(overloadedResponse.setHeader("content-type", "text/plain; charset=utf-8"));
            static_cast<void>(overloadedResponse.setHeader("retry-after", "1"));
            const Http2ResponseSendStatus overloadedSendStatus = co_await sendResponse(streamId, overloadedResponse, isHeadRequest);
            const RequestServeOutcome overloadedOutcome = toRequestServeOutcome(overloadedSendStatus);
            if (overloadedOutcome == RequestServeOutcome::StreamCancelled)
            {
                noteStreamCancelled();
            }
            if (overloadedOutcome == RequestServeOutcome::Served)
            {
                std::string abortErrorText;
                if (!m_connection.abortStream(streamId, "全局在途正文预算已用尽，响应（503）已发出，本端不再需要剩余正文", &abortErrorText))
                {
                    LOG_DEBUG_FMT("Http2Session: 流 {} 因预算用尽收口后未能请求对端中止发送。原因：{}", streamId, abortErrorText);
                }
            }
            co_return overloadedOutcome;
        }

        // 与 HTTP/1.1 侧同口径：收齐的请求才计数，耗时从「请求收齐」算到「响应排入待发字节」
        if (m_metrics != nullptr)
        {
            m_metrics->countParsedRequest();
        }
        const std::chrono::steady_clock::time_point requestReceivedTime = std::chrono::steady_clock::now();

        // content-length 与实收正文必须一致（RFC 9113 §8.1.1 引用 RFC 9110 §8.6）：不一致按报文错误
        // 回 400，绝不把「声明一个长度、实收另一个长度」的正文交给业务——那正是走私的收益所在
        if (!pending.isStreamingBody)
        {
            // 只看首条：这里要的就是那一个声明值，为它构造整列 string 是每条请求一次的多余分配
            const std::optional<std::string> declaredLengthText = request.firstHeaderValue("content-length");
            std::size_t                      declaredLength     = 0;
            if (declaredLengthText.has_value() && parseContentLengthValue(*declaredLengthText, declaredLength) &&
                request.body().size() != declaredLength)
            {
                if (m_metrics != nullptr)
                {
                    m_metrics->countBadRequest();
                }
                LOG_ERROR_FMT("Http2Session: 流 {} 的 content-length 声明 {} 字节、实收 {} 字节，已按 400 收口",
                              streamId, declaredLength, request.body().size());
                HttpResponse mismatchResponse;
                mismatchResponse.setStatus(400);
                mismatchResponse.setBody("Content-Length mismatch");
                static_cast<void>(mismatchResponse.setHeader("content-type", "text/plain; charset=utf-8"));
                const RequestServeOutcome mismatchOutcome =
                        toRequestServeOutcome(co_await sendResponse(streamId, mismatchResponse, isHeadRequest));
                if (mismatchOutcome == RequestServeOutcome::StreamCancelled)
                {
                    noteStreamCancelled();
                }
                co_return mismatchOutcome;
            }
        }

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

        // 流式正文：来源是本流自己的正文缓冲，业务经 request.bodyStream() 边收边读。泵每推进一步
        // 就驱动一次连接（与主循环同一条路径），业务不拉就不驱动——背压的落点，也是「处理器拉一次、
        // 网络上才读一次」在 h2 上的等价物
        if (pending.isStreamingBody)
        {
            pending.streamBody.setConsumeHandler(
                    [this, streamId](const std::size_t consumedFlowControlByteCount)
                    {
                        // 消费即还窗口：由 HttpStreamBody 在字节被交付后回调进来
                        std::string creditErrorText;
                        if (!m_connection.creditReceivedData(streamId, consumedFlowControlByteCount, &creditErrorText))
                        {
                            LOG_ERROR_FMT("Http2Session: 归还接收窗口失败（流 {}，{} 字节）：{}", streamId,
                                          consumedFlowControlByteCount, creditErrorText);
                        }
                    });

            HttpStreamBody &streamBody       = pending.streamBody;
            const auto       pumpStreamingBody = [this, streamId, &streamBody]() -> Core::Task<bool>
            {
                // 收请求正文期间按读超时约束相邻两次成功读取的间隔（与主循环「有在途请求」那一档同口径）
                refreshIdleDeadline(m_limits->readTimeout);

                if (!co_await driveConnectionOnce())
                {
                    // 连接已不可用：让业务立刻看到流终止，不必再等下文
                    streamBody.markBroken();
                    co_return false;
                }

                // 对端把这条流收掉了（RST_STREAM）而正文还没收齐：同样只能终止——不再有字节会到
                if (!streamBody.isComplete() && !streamBody.isBroken())
                {
                    Http2StreamState streamState{};
                    if (!m_connection.tryGetStreamState(streamId, streamState) || streamState == Http2StreamState::Closed)
                    {
                        streamBody.markBroken();
                    }
                }
                co_return !streamBody.isBroken();
            };
            request.setBodyStream(&m_bodyStream);
            m_bodyStream.attach(pending.streamBody, pumpStreamingBody);
        }

        std::exception_ptr handlerException = nullptr;
        // HEAD 的响应只有头部（RFC 9113 §8.1）：流式路径下正文段一律不发，头部改在收尾时与
        // END_STREAM 一起发出（见 finishStreamingResponse 的「一段都没写」分支）
        if (request.method() == HttpMethod::HEAD)
        {
            m_response.suppressStreamingBody();
        }
        try
        {
            // 处理器相位按「响应产出预算」计时（writeTimeout）：本相位既不读套接字也不写，
            // 上一次读刷出的 readTimeout 会在这里悄悄到期，把慢处理器当成空闲连接掐掉
            refreshIdleDeadline(m_limits->writeTimeout);
            co_await m_router.route(request, m_response);
        } catch (...)
        {
            // 业务异常：等本轮结束再改写响应（改写走下面的 500 分支，与 HTTP/1.1 侧同一处置）
            handlerException = std::current_exception();
        }

        // 流式头部是否已经随首段正文上线：上线之后状态码与头部都改不了，异常路径也只能补末片收尾
        // （判据见 HttpResponse::hasSentChunkedHead() 的文档，与 h1 侧同一条）
        const bool isStreamingStarted = m_response.isChunkedResponse() && m_response.hasSentChunkedHead();

        // 流式正文：业务读完（或提前返回）之后收尾这条流的接收侧。体量越界要在这里改写响应——
        // 头部还没上线才能改，已上线就只剩日志（与上面那条 413 分支同一判据）
        const bool isStreamBodyTooLarge = pending.isStreamingBody && pending.streamBody.isBodyTooLarge();
        if (pending.isStreamingBody && isStreamBodyTooLarge && !isStreamingStarted)
        {
            if (m_metrics != nullptr)
            {
                m_metrics->countBadRequest();
            }
            m_response.reset();
            m_response.setStatus(413);
            m_response.setBody("Payload Too Large");
            static_cast<void>(m_response.setHeader("content-type", "text/plain; charset=utf-8"));
        }

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

        // WebSocket：h1 走 101 升级，h2 走 RFC 8441 的扩展 CONNECT（:protocol=websocket）——应答是 200，
        // 随后这条流变成隧道；以 101 形态登记升级的请求在 h2 上没有对应机制，仍按 501 明确拒绝
        if (m_response.isWebSocketUpgradeRequested())
        {
            if (pending.isWebSocketTunnel && !isStreamingStarted)
            {
                co_return co_await serveWebSocketTunnel(streamId, pending);
            }
            if (isStreamingStarted)
            {
                // 两种报文形态互斥：升级要发 101 并交出连接，而流式头部已经在对端手里，改不了
                LOG_ERROR_FMT("Http2Session: 本响应已经按流式写出头部，又登记了 WebSocket 升级，两者互斥；"
                              "HTTP/2 上也不支持升级，已按流式响应补末片收尾。request-id {}，路径 {}",
                              request.requestId(), request.uri());
            } else
            {
                LOG_ERROR_FMT("Http2Session: 该请求不是带 :protocol=websocket 的扩展 CONNECT，HTTP/2 上没有 101 升级这一形态，"
                              "已回 501 并保持连接可用。request-id {}，路径 {}",
                              request.requestId(), request.uri());
                m_response.reset();
                m_response.setStatus(501);
                m_response.setBody("WebSocket over HTTP/2 Not Implemented");
                static_cast<void>(m_response.setHeader("content-type", "text/plain; charset=utf-8"));
            }
        }

        // 响应自动带本次请求的 request-id（与 HTTP/1.1 侧同口径）：调用方显式设过就不覆盖
        if (!request.requestId().empty() && !m_response.hasHeader(kRequestIdHeaderName))
        {
            static_cast<void>(m_response.setHeader(kRequestIdHeaderName, request.requestId()));
        }

        // 流式响应：头部（首段时）与每个正文段都已由上面的回调当场发出，这里只补末片把消息收完整；
        // 其余响应照旧整份发出
        Http2ResponseSendStatus sendStatus = Http2ResponseSendStatus::Sent;
        if (m_response.isChunkedResponse())
        {
            sendStatus = co_await finishStreamingResponse(streamId);
        } else
        {
            sendStatus = co_await sendResponse(streamId, m_response, isHeadRequest);
        }

        const RequestServeOutcome serveOutcome = toRequestServeOutcome(sendStatus);
        if (serveOutcome != RequestServeOutcome::Served)
        {
            // 响应没排出去，不计状态码类与延迟——那会把「对端收不到」的请求算成已应答。
            // 对端取消这条流只停这一条；连接不可用与用法错误的日志已由各自的发送入口记过
            if (serveOutcome == RequestServeOutcome::StreamCancelled)
            {
                noteStreamCancelled();
            }
            co_return serveOutcome;
        }

        // 正文已拷进这条流的待发队列（Http2Connection 按帧 append 进 pendingData），映射与此后的发送无关了。
        // 本会话的响应对象是**按连接复用**的，不在此处解除的话，这条连接转入空闲后映射会一直攥到下一条
        // 请求开头 reset() 才放下：Windows 上就地改名/截断该文件因此被挡最长一个空闲超时，POSIX 上则是一条
        // fd 与一段地址空间预留按空闲连接计（与 h1 侧 HttpSession 同一判据）
        m_response.releaseMappedBody();

        // 流式正文的接收侧收尾必须排在响应之后：RST_STREAM 与响应排在同一条待发字节流里，
        // 先中止就会让对端先看到 RST，响应反而到不了（见 finishStreamingRequestBody 的说明）
        if (pending.isStreamingBody)
        {
            finishStreamingRequestBody(pending, isStreamBodyTooLarge);
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
        // 因此能按同一个键对齐。与 HTTP/1.1 侧同口径，同样定为 Debug：它也在每请求热路径上
        if (!request.requestId().empty() && !isTruncatedStreamingResponse)
        {
            LOG_DEBUG_FMT("Http2Session: 请求已完成。request-id {}，路径 {}，状态码 {}，耗时 {}us", request.requestId(),
                          request.uri(), statusCode, std::chrono::duration_cast<std::chrono::microseconds>(requestElapsed).count());
        }

        // 单连接请求上限的判定放在最后：本条响应已经排入待发字节，收尾通告排在它之后
        noteServedRequest();
        co_return RequestServeOutcome::Served;
    }

    Core::Task<Http2Session::RequestServeOutcome> Http2Session::serveWebSocketTunnel(const std::uint32_t streamId,
                                                                                    PendingRequest &pending)
    {
        HttpRequest &request = pending.request;
        const std::chrono::steady_clock::time_point tunnelStartTime = std::chrono::steady_clock::now();

        // 握手校验：h2 用扩展 CONNECT 代替 Upgrade 头，但版本与 key 两项与 h1 完全一致（同一份实现）
        std::string clientKey;
        std::string handshakeFailureReason;
        if (!validateWebSocketKeyAndVersion(request, clientKey, &handshakeFailureReason))
        {
            LOG_ERROR_FMT("Http2Session: 扩展 CONNECT 的 WebSocket 握手不合法，已按 400 应答。request-id {}，路径 {}，原因：{}",
                          request.requestId(), request.uri(), handshakeFailureReason);
            m_response.reset();
            m_response.setStatus(400);
            m_response.setBody("Bad WebSocket Handshake");
            static_cast<void>(m_response.setHeader("content-type", "text/plain; charset=utf-8"));
            const RequestServeOutcome handshakeOutcome = toRequestServeOutcome(co_await sendResponse(streamId, m_response, false));
            co_return handshakeOutcome;
        }

        // 升级应答是 200 且**不带 END_STREAM**（RFC 8441 §5）：这条流接下来要承载帧，消息还没结束。
        // 因此这里不能走 sendResponse()——它按「正文是否为空」决定 END_STREAM，会把隧道当场关掉
        std::vector<HpackHeaderField> acceptFields;
        acceptFields.push_back({.name = std::string(kWebSocketAcceptHeaderName), .value = computeWebSocketAcceptValue(clientKey)});

        // 扩展协商（RFC 7692 §7.1）：与 h1 侧同一份协商实现，接受时把结论一并写进应答头，
        // 本端随后按同一结论收发压缩帧——回给对端的那一行与本端的收发口径必须是同一个来源
        const std::optional<std::string> extensionsHeader = request.getHeader(kWebSocketExtensionsHeaderName);
        const PerMessageDeflateNegotiation deflateNegotiation =
                negotiatePerMessageDeflate(extensionsHeader.has_value() ? *extensionsHeader : std::string_view{});
        if (!deflateNegotiation.responseValue.empty())
        {
            acceptFields.push_back(
                    {.name = std::string(kWebSocketExtensionsHeaderName), .value = deflateNegotiation.responseValue});
        }
        if (!request.requestId().empty())
        {
            acceptFields.push_back({.name = std::string(kRequestIdHeaderName), .value = std::string(request.requestId())});
        }
        std::string headersErrorText;
        const Http2ResponseSendStatus headersStatus =
                m_connection.sendResponseHeaders(streamId, kWebSocketAcceptedStatusCode, acceptFields, false, &headersErrorText);
        if (headersStatus != Http2ResponseSendStatus::Sent)
        {
            if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的 WebSocket 升级应答未能排入待发字节，该流不会再有响应。原因：{}", streamId,
                              headersErrorText);
            }
            co_return toRequestServeOutcome(headersStatus);
        }
        if (!co_await flushOutgoingBytes())
        {
            co_return RequestServeOutcome::ConnectionUnusable;
        }
        if (m_metrics != nullptr)
        {
            m_metrics->recordResponse(static_cast<int>(kWebSocketAcceptedStatusCode), std::chrono::steady_clock::now() - tunnelStartTime);
            m_metrics->countWebSocketUpgrade();
        }
        LOG_DEBUG_FMT("Http2Session: 流 {} 已升级为 WebSocket 隧道（RFC 8441 扩展 CONNECT）。request-id {}，路径 {}", streamId,
                      request.requestId(), request.uri());
        noteServedRequest();

        // 业务写出的帧发成这条流上的 DATA 帧；写失败（流被对端取消、队列到上界或连接不可用）即 false，
        // 与 h1 侧同口径
        const auto sendFrameBytes = [this, streamId](const std::string_view frameBytes) -> Core::Task<bool>
        {
            // 与流式正文同一道闸：对端只读不授窗口时隧道帧同样会无限堆积
            if (m_connection.pendingResponseByteCount(streamId) > kStreamingSendQueueLimitByteCount)
            {
                LOG_ERROR_FMT("Http2Session: 隧道流 {} 的待发字节已超过上限 {} 字节（对端长期未发 WINDOW_UPDATE），"
                              "本帧不再入队",
                              streamId, kStreamingSendQueueLimitByteCount);
                co_return false;
            }

            std::string sendErrorText;
            if (m_connection.sendResponseData(streamId, frameBytes, false, &sendErrorText) != Http2ResponseSendStatus::Sent)
            {
                co_return false;
            }
            co_return co_await flushOutgoingBytes();
        };

        WebSocketPeer peer(sendFrameBytes, m_metrics.get());
        peer.setPerMessageDeflateEnabled(deflateNegotiation.accepted);

        bool isBusinessFinished = false;
        const auto runBusiness = [&isBusinessFinished](WebSocketHandler businessHandler, WebSocketPeer &businessPeer) -> Core::Task<>
        {
            try
            {
                co_await businessHandler(businessPeer);
            } catch (const std::exception &exception)
            {
                // 升级应答已经上线，此刻没有别的东西可回给对端：原因只能进日志
                LOG_ERROR_EXCEPTION(exception, "Http2Session: WebSocket 隧道里的业务处理器抛出异常，已按连接不可用收口。原因：{}", exception.what());
            } catch (...)
            {
                LOG_ERROR_FMT("Http2Session: WebSocket 隧道里的业务处理器抛出非标准异常（无 what() 描述）");
            }
            isBusinessFinished = true;
        };

        Core::Task<> businessTask = runBusiness(m_response.webSocketHandler(), peer);
        businessTask.handle().resume();

        // 对端可能在 200 到达之前就抢先发了帧（RFC 8441 允许它一收到 200 就发，但实现常提前发）：
        // 这些字节已经在正文缓冲里，按到达顺序喂给解码器，一条都不丢
        WebSocketFeedStatus feedStatus = WebSocketFeedStatus::Accepted;
        if (!pending.request.body().empty())
        {
            feedStatus = peer.feedBytes(pending.request.body().data(), pending.request.body().size());
        }

        std::vector<char> receiveBuffer(kHttp2ReceiveWindowByteCount);
        bool isRemoteEndStream = false;
        while (feedStatus == WebSocketFeedStatus::Accepted && !isBusinessFinished && peer.isOpen() && isAlive() && isTransportOpen()
               && !isRemoteEndStream)
        {
            // 隧道阶段没有「半条报文」这一相位：帧与帧之间的间隔就是这条连接的空闲，与 h1 阶段同口径
            refreshIdleDeadline(m_limits->idleTimeout);

            ssize_t receivedLength = 0;
            try
            {
                receivedLength = co_await transportReceive(receiveBuffer.data(), receiveBuffer.size());
            } catch (const std::exception &)
            {
                break;
            }
            if (receivedLength <= 0)
            {
                break;
            }
            if (m_connection.feedBytes(receiveBuffer.data(), static_cast<std::size_t>(receivedLength)) == Http2ConnectionFeedStatus::Failed
                || m_connection.hasFailed())
            {
                LOG_ERROR_FMT("Http2Session: WebSocket 隧道期间连接层失败，已收口。原因：{}", m_connection.errorMessage());
                break;
            }
            if (!co_await flushOutgoingBytes())
            {
                break;
            }

            for (const Http2ReceivedData &receivedData: m_connection.takeReceivedData())
            {
                if (receivedData.streamId == streamId)
                {
                    if (!receivedData.data.empty())
                    {
                        feedStatus = peer.feedBytes(receivedData.data.data(), receivedData.data.size());
                    }
                    if (receivedData.endStream)
                    {
                        isRemoteEndStream = true;
                    }
                    // 隧道自己的载荷不进请求体，但窗口照还：不还的话连接级接收窗口会随隧道流量
                    // 单调耗尽，对端发到一半停住等一个永不出现的 WINDOW_UPDATE
                    std::string creditErrorText;
                    if (!m_connection.creditReceivedData(receivedData.streamId, receivedData.flowControlByteCount, &creditErrorText))
                    {
                        LOG_ERROR_FMT("Http2Session: WebSocket 隧道期间归还接收窗口失败（流 {}，{} 字节）：{}", receivedData.streamId,
                                      receivedData.flowControlByteCount, creditErrorText);
                    }
                    continue;
                }
                // 其它流的正文：按与主循环完全相同的口径累积与还窗口——隧道期间它们同样要被服务
                absorbOneReceivedData(receivedData);
            }

            absorbPendingRequests();
            // 隧道期间本协程即这条连接的驱动者：其它流上已收齐的请求就地服务掉，
            // 而不是把它们晾到隧道结束（第二条隧道仍按 503 拒绝，见该函数注释）
            if (!co_await serveOtherStreamsDuringTunnel(streamId))
            {
                break;
            }
            if (!co_await flushOutgoingBytes())
            {
                break;
            }
        }

        // 收尾与 h1 阶段同一判据：协议错误按具体码收口，否则主动发正常关闭帧（本侧已收口时不补发）
        if (feedStatus == WebSocketFeedStatus::DecodeError)
        {
            const std::uint16_t errorCode = peer.decodeErrorCloseCode();
            LOG_ERROR_FMT("Http2Session: WebSocket 隧道里对端违反 RFC 6455，按状态码 {} 收口。原因：{}", errorCode, peer.decodeErrorText());
            if (m_metrics != nullptr)
            {
                m_metrics->countWebSocketProtocolErrorClose();
            }
            if (peer.isOpen() && !peer.isWriteInFlight() && isTransportOpen())
            {
                [[maybe_unused]] const bool isErrorCloseSent = co_await peer.close(errorCode);
            }
        } else if (peer.isOpen() && !peer.isWriteInFlight() && isTransportOpen())
        {
            [[maybe_unused]] const bool isNormalCloseSent = co_await peer.close(kWebSocketNormalClosureCode);
        }
        peer.markClosed();

        // 收口与 h1 阶段同一顺序（先唤醒再销毁）：业务可能正挂在 receive() 上，而 markClosed() 有意不唤醒它。
        // 随后等它跑完自己的收尾，本帧才结束——直接往下走会让业务帧随本帧一起销毁（Task 的析构无条件
        // destroy()），它 receive() 之后的代码全部丢失，而任何已投递给循环的恢复动作会指向已释放的帧
        peer.wakeDeliveryWaiter();
        co_await businessTask;

        // 本侧方向到此结束：零长 DATA 带 END_STREAM（对端据此知道隧道的这一半关完了）
        std::string endStreamErrorText;
        const Http2ResponseSendStatus endStatus = m_connection.sendResponseData(streamId, std::string_view{}, true, &endStreamErrorText);
        if (endStatus != Http2ResponseSendStatus::Sent && endStatus != Http2ResponseSendStatus::StreamNotWritable)
        {
            LOG_ERROR_FMT("Http2Session: 流 {} 的隧道末片未能排入待发字节。原因：{}", streamId, endStreamErrorText);
        }
        if (!co_await flushOutgoingBytes())
        {
            co_return RequestServeOutcome::ConnectionUnusable;
        }
        LOG_INFO_FMT("Http2Session: WebSocket 隧道已收尾（流 {}，存活 {}ms）。request-id {}，路径 {}", streamId,
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tunnelStartTime).count(),
                     request.requestId(), request.uri());
        co_return RequestServeOutcome::Served;
    }

    Core::Task<bool> Http2Session::serveOtherStreamsDuringTunnel(const std::uint32_t tunnelStreamId)
    {
        for (auto requestIterator = m_pendingRequests.begin(); requestIterator != m_pendingRequests.end();)
        {
            PendingRequest &pending = requestIterator->second;
            if (pending.streamId == tunnelStreamId)
            {
                ++requestIterator;
                continue;
            }

            // 第二条隧道不在本片范围：一条连接上同时跑两条隧道要嵌套驱动循环，
            // 明确回 503 而不是把它晾着
            if (pending.isWebSocketTunnel)
            {
                std::string errorText;
                const Http2ResponseSendStatus sendStatus = m_connection.sendResponseHeaders(pending.streamId, 503U, {}, true, &errorText);
                LOG_INFO_FMT("Http2Session: WebSocket 隧道（流 {}）期间收到流 {} 的扩展 CONNECT，已回 503：一条连接上"
                             "同时跑两条隧道需要嵌套驱动循环，本片不做，请对端另开连接",
                             tunnelStreamId, pending.streamId);
                if (sendStatus != Http2ResponseSendStatus::Sent)
                {
                    LOG_DEBUG_FMT("Http2Session: 流 {} 的 503 未能排入待发字节。原因：{}", pending.streamId, errorText);
                }
                requestIterator = m_pendingRequests.erase(requestIterator);
                continue;
            }

            // 正文还没收齐的流继续等：本协程的下一轮读会把剩余 DATA 攒进来
            if (!pending.isRemoteEndStream && !pending.isBodyTooLarge && !pending.isBudgetExceeded)
            {
                ++requestIterator;
                continue;
            }

            // 就地服务：响应排进待发字节，由隧道循环本轮的写出送到对端
            HttpRequest *const previousServingRequest = m_servingRequest;
            setBusy(true);
            m_servingRequest = &pending.request;
            const RequestServeOutcome serveOutcome = co_await serveOneRequest(pending);
            m_servingRequest = previousServingRequest;
            setBusy(false);

            requestIterator = m_pendingRequests.erase(requestIterator);
            if (serveOutcome == RequestServeOutcome::ConnectionUnusable)
            {
                co_return false;
            }
        }
        co_return true;
    }

    HttpRequest Http2Session::mapToHttpRequest(const Http2Request &http2Request)
    {
        HttpRequest request;
        // 方法原文经 methodFromString 映射：未收录的方法（CONNECT、TRACE、自定义动词）落到
        // HttpMethod::UNKNOWN，路由器按既有规则回 404/405，绝不静默降级成某条业务路由。
        // 例外是 RFC 8441 的扩展 CONNECT：它是「这条 :path 上的 WebSocket 隧道」，语义与 GET 同路，
        // 因此按 GET 交给路由——于是同一个 router.get(路径, 处理器) 同时服务 h1 的 101 升级与 h2 的隧道
        if (http2Request.protocol == kWebSocketProtocolName)
        {
            request.setMethod(HttpMethod::GET);
        } else
        {
            request.setMethod(HttpRequest::methodFromString(http2Request.method));
        }
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
        // 常见情形是「响应自己的头 + 后面补齐的三条」；这里给一个够用的起点，宁可留一点余量，
        // 也不为了算准数量先去把单值视图建出来（那是每次查询都要重建的哈希表）
        headerFields.reserve(8U);
        bool hasContentTypeHeader = false;
        bool hasContentLengthHeader = false;
        bool hasDateHeader = false;

        // 流式响应的正文由各 DATA 帧给出，长度在收尾前未知：content-length 与 DATA 负载总长必须一致
        // （RFC 9113 §8.1.2.6），此刻算不出正确值，因此这条路径一律不写它（h1 的分块模式同样不写）
        const bool isStreamingResponse = response.isChunkedResponse();

        // 按权威记录的设置顺序逐条取，与 h1 的 appendHead 同一条路径：
        // 先建单值视图再按名回查，既多付一张哈希表，又会因遍历顺序不稳而让同一份响应两次编码给出不同次序
        response.forEachHeaderField(
            [&](const std::string_view headerName, const std::string_view headerValue)
            {
                if (isConnectionSpecificHeaderName(headerName))
                {
                    // 业务常按 HTTP/1.1 的习惯设 connection: close；HTTP/2 里它一律非法（§8.2.2），
                    // 丢掉它而不是让整条响应被连接层拒绝
                    LOG_DEBUG_FMT("Http2Session: 已丢弃 HTTP/2 禁止的连接特定响应头「{}」", headerName);
                    return;
                }
                if (isStreamingResponse && headerName == kContentLengthHeaderName)
                {
                    // 流式模式按 h1 契约本就不该有这条头（startChunkedResponse 已删过一遍）：业务后补的
                    // 一样丢掉，否则一个恰好等于某段长度的数字会让对端按它定界、把后续 DATA 当多余字节
                    LOG_DEBUG_FMT("Http2Session: 流式响应的正文长度由 DATA 帧给出，已丢弃 content-length 响应头");
                    return;
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

                headerFields.push_back(HpackHeaderField{std::string(headerName), std::string(headerValue)});
            });

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
            // 定长文本按秒缓存在线程局部，这里显式构造拥有者（头字段要持有值）
            headerFields.push_back(HpackHeaderField{.name = std::string(kDateHeaderName),
                                                    .value = std::string(currentHttpDateText())});
        }
        return headerFields;
    }

    Core::Task<Http2ResponseSendStatus> Http2Session::sendResponse(const std::uint32_t streamId, const HttpResponse &response,
                                                                  const bool isHeadRequest)
    {
        // 状态码越界（HttpResponse::setStatus 不校验取值范围）时由助手改回 500：连接层只接受 100..999，
        // 把越界值原样交出去会让这条流一个字节都发不出去，对端只能等到超时
        const std::uint32_t wireStatusCode = normalizeWireStatusCode(response.status(), streamId);

        const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(response);
        // HEAD 只发头：正文视图换成空，头部里的 content-length 仍按完整正文补齐
        const std::string_view responseBody = isHeadRequest ? std::string_view{} : response.body();
        const bool isBodyEmpty = responseBody.empty();

        std::string errorText;
        const Http2ResponseSendStatus headersStatus =
                m_connection.sendResponseHeaders(streamId, wireStatusCode, headerFields, isBodyEmpty, &errorText);
        if (headersStatus != Http2ResponseSendStatus::Sent)
        {
            // 对端取消这条流的日志由调用方按结论统一记，这里只管其余失败（连接不可用、用法错误）
            if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
            }
            co_return headersStatus;
        }
        if (!isBodyEmpty)
        {
            const Http2ResponseSendStatus bodyStatus = m_connection.sendResponseData(streamId, responseBody, true, &errorText);
            if (bodyStatus != Http2ResponseSendStatus::Sent)
            {
                if (bodyStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的响应正文未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                }
                co_return bodyStatus;
            }
        }
        co_return Http2ResponseSendStatus::Sent;
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
        // 剥离逻辑已上收到 Net/Http/HttpChunkFrame：h2 与 h3 面对的是同一份 h1 分块帧，
        // 各自实现一遍只会让两边的边界判定慢慢走偏。这里保留一个转发的成员函数，叫法不变
        return Net::chunkFramePayload(chunkFrame);
    }

    Core::Task<bool> Http2Session::sendStreamingSegment(const std::uint32_t streamId, const std::string_view segment)
    {
        // HEAD：一段正文都不发（含头部）——头部与 END_STREAM 由 finishStreamingResponse 一起发出，
        // 只发头部不发正文正是 HEAD 的语义
        if (m_response.isStreamingBodySuppressed())
        {
            co_return true;
        }

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
            const Http2ResponseSendStatus headersStatus = m_connection.sendResponseHeaders(
                    streamId, normalizeWireStatusCode(m_response.status(), streamId), headerFields, false, &errorText);
            if (headersStatus != Http2ResponseSendStatus::Sent)
            {
                // 对端取消这条流的日志由 serveOneRequest() 按服务结论统一记，这里只管其余失败
                if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的流式响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
                }
                co_return false;
            }
        } else
        {
            // 上界先行：窗口何时放开完全由对端决定，对端长期不发 WINDOW_UPDATE 时再入队就是无界内存。
            // 到顶即失败，业务据此停写（h1 侧不设这道闸是因为套接字写满会自然挂住生产者）
            if (m_connection.pendingResponseByteCount(streamId) > kStreamingSendQueueLimitByteCount)
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的待发正文已超过上限 {} 字节（对端长期未发 WINDOW_UPDATE），"
                              "本段不再入队，业务应停止写入",
                              streamId, kStreamingSendQueueLimitByteCount);
                co_return false;
            }

            // 其余段落是 HttpResponse::writeChunk() 按 h1 分块帧成帧的正文（RFC 9112 §7.1）：HTTP/2 里
            // 没有分块帧这一层（transfer-encoding 属禁止头，RFC 9113 §8.2.2），只把帧里的负载发成 DATA 帧
            const std::string_view payload = chunkFramePayload(segment);
            const Http2ResponseSendStatus bodyStatus = m_connection.sendResponseData(streamId, payload, false, &errorText);
            if (bodyStatus != Http2ResponseSendStatus::Sent)
            {
                if (bodyStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的流式正文段未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                }
                co_return false;
            }
        }

        // 当场写出：流式响应的要点是「边写边到」，把数据攒在待发缓冲里等主循环下一轮就等于推迟一段。
        // 窗口不足时连接层已把整段排进该流的发送队列（返回 true），这一次没有字节可写也不算失败：
        // 等对端 WINDOW_UPDATE 到达后由连接层在同一入口内续发，正是 writeChunk 文档里「入队而非失败」的含义
        co_return co_await flushOutgoingBytes();
    }

    Core::Task<Http2ResponseSendStatus> Http2Session::finishStreamingResponse(const std::uint32_t streamId)
    {
        // 本侧已判定写不出去：不再重试，也不重复记日志（与 h1 契约一致）
        if (m_isConnectionUnusable)
        {
            co_return Http2ResponseSendStatus::ConnectionUnavailable;
        }

        std::string errorText;
        if (!m_response.hasSentChunkedHead())
        {
            // 一段正文都没写出来（业务只调了 startChunkedResponse()）：头部与 END_STREAM 一起发出，
            // 对端因此拿到一条没有正文的完整响应，而不是挂在一条永远收不满的消息上；
            // 与 h1 侧「空流式响应补出头部与终止块」是同一个位置
            const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(m_response);
            const Http2ResponseSendStatus headersStatus = m_connection.sendResponseHeaders(
                    streamId, normalizeWireStatusCode(m_response.status(), streamId), headerFields, true, &errorText);
            if (headersStatus != Http2ResponseSendStatus::Sent)
            {
                // 对端取消这条流的日志由 serveOneRequest() 按服务结论统一记，这里只管其余失败
                if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的空流式响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
                }
                co_return headersStatus;
            }
            co_return co_await flushOutgoingBytes() ? Http2ResponseSendStatus::Sent : Http2ResponseSendStatus::ConnectionUnavailable;
        }

        // 头部已随首段上线：补一个零长 DATA 帧带 END_STREAM（RFC 9113 §6.1 允许零长 DATA，且不占流控窗口），
        // 消息边界由末片给出——h1 侧补的是 `0\r\n\r\n` 终止块，位置与语义都对应
        const Http2ResponseSendStatus endStreamStatus = m_connection.sendResponseData(streamId, std::string_view{}, true, &errorText);
        if (endStreamStatus != Http2ResponseSendStatus::Sent)
        {
            if (endStreamStatus != Http2ResponseSendStatus::StreamNotWritable)
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的流式响应末片未能排入待发字节，对端收不到消息结尾。原因：{}", streamId, errorText);
            }
            co_return endStreamStatus;
        }
        co_return co_await flushOutgoingBytes() ? Http2ResponseSendStatus::Sent : Http2ResponseSendStatus::ConnectionUnavailable;
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
                        co_await transportSend(outgoingBytes.data() + writtenByteCount, outgoingBytes.size() - writtenByteCount);
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

        // 缓冲还回去复用（容量留下）：h2 的每条响应至少要交两次待发字节，不复用就是每条响应
        // 重新分配一整块。写失败路径同样还——那份缓冲已经没用处了
        m_connection.recycleOutgoingBytes(std::move(outgoingBytes));
        co_return isSucceeded;
    }

    bool Http2Session::isSettingsAcknowledgementExpired() const noexcept
    {
        // 限额为 0 表示不设这项保护：既不按 SETTINGS 超时切空闲截止时间，也不按 SETTINGS_TIMEOUT 收口
        if (m_limits->settingsAcknowledgementTimeout <= std::chrono::milliseconds::zero() ||
            !m_connection.hasSettingsAwaitingAcknowledgement())
        {
            return false;
        }
        // 期限从「本端发出 SETTINGS 的时刻」起算：不 ACK 却持续发帧的对端不能把截止时间一轮轮往后推
        return std::chrono::steady_clock::now() >=
               m_connection.lastSettingsSentTime() + m_limits->settingsAcknowledgementTimeout;
    }

    std::chrono::milliseconds Http2Session::settingsAcknowledgementIdleBudget() const noexcept
    {
        // 该项保护关掉或 SETTINGS 已经被 ACK：交回「不适用」，调用方按常规相位时限刷新
        if (m_limits->settingsAcknowledgementTimeout <= std::chrono::milliseconds::zero() ||
            !m_connection.hasSettingsAwaitingAcknowledgement())
        {
            return std::chrono::milliseconds::zero();
        }

        const std::chrono::steady_clock::time_point deadline =
                m_connection.lastSettingsSentTime() + m_limits->settingsAcknowledgementTimeout;
        // 剩余时长向上取整到毫秒：向零截断会让清扫协程看到的截止时间比上式的到期时刻早最多 1 毫秒，
        // 落在那一毫秒里的清扫会先关掉连接（它判的是「已到期」），close() 再判 isSettingsAcknowledgementExpired()
        // 就还是「没到期」，SETTINGS_TIMEOUT 的 GOAWAY 因此漏发。向上取整保证清扫只会在到期之后动手
        const std::chrono::milliseconds remainingTime =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        // 下限取 1 毫秒：已经过期时也得给出正数，让清扫协程在下一拍收口；0 是「清除截止时间」
        constexpr std::chrono::milliseconds kMinimumIdleBudget{1};
        return std::max(remainingTime, kMinimumIdleBudget);
    }

    bool Http2Session::queueSettingsTimeoutGoAway()
    {
        const std::string reason =
                std::format("对端在 {} 毫秒内没有 ACK 本端 SETTINGS（RFC 7540 §6.5.3 的 SETTINGS_TIMEOUT）：本端按该错误码收口连接，"
                            "请对端在收到 SETTINGS 后回一个 ACK 帧",
                            m_limits->settingsAcknowledgementTimeout.count());
        std::string errorText;
        if (!m_connection.failConnection(Http2ErrorCode::SettingsTimeout, reason, &errorText))
        {
            LOG_ERROR_FMT("Http2Session: 未能按 SETTINGS_TIMEOUT 收口（没有写入任何字节）。原因：{}", errorText);
            return false;
        }
        LOG_ERROR_FMT("Http2Session: 对端在 {} 毫秒内没有 ACK 本端 SETTINGS，已按 SETTINGS_TIMEOUT 发 GOAWAY 并收口连接",
                      m_limits->settingsAcknowledgementTimeout.count());
        return true;
    }

    void Http2Session::writeOutgoingBytesBestEffort()
    {
        const std::string outgoingBytes = m_connection.takeOutgoingBytes();
        if (outgoingBytes.empty())
        {
            return;
        }

        // 手动推进一次发送协程（与 WebSocket 阶段显式推进业务协程同一手法）：SSL_write 在非阻塞
        // 套接字上要么整段接受、要么直接报告需要等待，因此常规情形下这一次 resume 就写完了
        Core::Task<ssize_t> sendTask = transportSend(outgoingBytes.data(), outgoingBytes.size());
        sendTask.handle().resume();
        if (!sendTask.isReady())
        {
            // 发送挂起：放弃剩余字节。此刻连接已经注定关闭，为一条 GOAWAY 等可写只会把清扫协程拖住；
            // 挂起的帧随本函数返回销毁，其等待器析构时会自行从注册对象摘除
            LOG_WARN_FMT("Http2Session: SETTINGS_TIMEOUT 的 GOAWAY 未能在收口前写出（等可写），连接仍然关闭");
            return;
        }

        try
        {
            const ssize_t writtenByteCount = sendTask.await_resume();
            if (writtenByteCount <= 0)
            {
                LOG_WARN_FMT("Http2Session: SETTINGS_TIMEOUT 的 GOAWAY 写出失败（传输层返回 {}），连接仍然关闭", writtenByteCount);
            }
        } catch (const std::exception &exception)
        {
            LOG_WARN_FMT("Http2Session: SETTINGS_TIMEOUT 的 GOAWAY 写出失败（{}），连接仍然关闭", exception.what());
        }
    }

    Http2Session::RequestServeOutcome Http2Session::toRequestServeOutcome(const Http2ResponseSendStatus sendStatus) noexcept
    {
        switch (sendStatus)
        {
            case Http2ResponseSendStatus::Sent:
                return RequestServeOutcome::Served;
            case Http2ResponseSendStatus::StreamNotWritable:
                // 对端已经取消或收尾了这条流：本条再没有响应可发，连接与其它流不受影响
                return RequestServeOutcome::StreamCancelled;
            case Http2ResponseSendStatus::ConnectionUnavailable:
            case Http2ResponseSendStatus::Rejected:
                return RequestServeOutcome::ConnectionUnusable;
        }
        return RequestServeOutcome::ConnectionUnusable;
    }
} // namespace AsynGyanis::Net
