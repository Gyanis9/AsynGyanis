#include "Net/Http3/Http3ClientConnection.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http3/Qpack.h"

#include <charconv>
#include <cstdlib>
#include <optional>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一轮「送字节 + 收报文」之后让出一次循环，等对端的上限：防呆用的硬上界，不是超时机制
        constexpr std::size_t kMaximumDriveRounds = 100000U;
    } // namespace

    Http3ClientConnection::Http3ClientConnection(QuicClientConnection &connection, const Config config) : m_connection(connection), m_config(config)
    {
    }

    Core::Task<bool> Http3ClientConnection::start()
    {
        Http3Connection::Callbacks callbacks;
        callbacks.onHeaderField = [this](const std::int64_t streamId, const std::string_view name, const std::string_view value, const bool /*isTrailers*/)
        { noteHeaderField(streamId, name, value); };
        callbacks.onBodyBytes        = [this](const std::int64_t streamId, const std::span<const std::uint8_t> bytes) { noteBodyBytes(streamId, bytes); };
        callbacks.onRequestEnded     = [this](const std::int64_t streamId) { noteMessageEnded(streamId); };
        callbacks.onStreamClosed     = [this](const std::int64_t streamId) { noteMessageEnded(streamId); };
        callbacks.onStreamReset      = [this](const std::int64_t streamId, const Http3ErrorCode /*errorCode*/) { noteStreamFailed(streamId, "对端在本条流上发了 RESET_STREAM"); };
        callbacks.onMalformedRequest = [this](const std::int64_t streamId, const std::string_view reason) { noteStreamFailed(streamId, "响应不合规范：" + std::string{reason}); };
        callbacks.onConnectionClosed = [this](const Http3ErrorCode /*errorCode*/, const std::string_view reason)
        {
            m_isHealthy = false;
            failAllPending("连接被收口：" + std::string{reason});
        };

        Http3Connection::LocalSettings settings;
        settings.maximumFieldSectionSizeByteCount = m_config.maximumFieldSectionSizeByteCount;
        m_protocol                                = std::make_unique<Http3Connection>(
                [this] { return m_connection.openUnidirectionalStream(); }, [this](const std::int64_t streamId, const std::span<const std::uint8_t> bytes, const bool endStream)
                { return m_connection.writeStream(streamId, bytes, endStream); }, [this](const std::int64_t streamId, const std::size_t consumedByteCount)
                { m_connection.extendReceiveWindow(streamId, consumedByteCount); }, std::move(callbacks), settings, QuicConnectionRole::Client);

        // 收到的字节直接交给协议层：中间留一层「按流排队等谁来取」会把同一批字节存两遍，
        // 还会让 h3 看不到收尾（FIN）
        m_connection.setStreamDataSink([this](const std::int64_t streamId, const std::span<const std::uint8_t> bytes, const bool isEndStream)
                                       { m_protocol->consumeStreamData(streamId, bytes, isEndStream); });

        if (!m_protocol->isUsable())
        {
            LOG_WARN("Http3ClientConnection: 三条本端单向流没都能开出来（单向流额度为 0 或连接已收口），"
                     "HTTP/3 出站不可用");
            m_isHealthy = false;
            co_return false;
        }
        m_isHealthy = true;
        m_protocol->flush();
        co_await m_connection.sendPending();
        co_return true;
    }

    Core::Task<Http3ClientResponse> Http3ClientConnection::request(const std::string_view scheme, const std::string_view authority, const std::string_view method,
                                                                   const std::string_view path, const std::vector<std::pair<std::string, std::string>> &extraHeaders,
                                                                   const std::string_view body, const std::chrono::milliseconds waitTimeout,
                                                                   const Http3ResponseBodyReceiver &responseReceiver)
    {
        Http3ClientResponse outcome{};
        if (!m_isHealthy || m_protocol == nullptr)
        {
            outcome.errorMessage = "这条 HTTP/3 连接不可用，请求没有发出";
            co_return outcome;
        }
        if (m_openedStreamCount >= m_config.maximumOpenedStreamCount)
        {
            // 流号到顶之后没有合法的新号可提（RFC 9000 §2.1 的客户端流号严格递增），只能换一条连接
            outcome.errorMessage = "这条 HTTP/3 连接上开过的流已到达上限，请换一条连接重来";
            co_return outcome;
        }

        const std::int64_t streamId = m_connection.openStream();
        if (streamId < 0)
        {
            outcome.errorMessage = "开不出请求流（对端给的双向流额度已用尽，或连接已收口）";
            co_return outcome;
        }
        ++m_openedStreamCount;

        std::vector<QpackHeaderField> fieldLines;
        fieldLines.reserve(extraHeaders.size() + 4U);
        // 顺序与 h2 出站侧逐字一致（:method、:path、:scheme、:authority）：伪头之间 RFC 9114 不排序，
        // 但自家两型用同一份顺序，校验出问题时的可读性更好
        fieldLines.push_back({":method", std::string{method}});
        fieldLines.push_back({":path", std::string{path}});
        fieldLines.push_back({":scheme", std::string{scheme}});
        fieldLines.push_back({":authority", std::string{authority}});
        for (const auto &[name, value]: extraHeaders)
        {
            fieldLines.push_back({name, value});
        }

        PendingExchange &exchange = exchangeFor(streamId);
        // 接收口按值存进这条流的账：调用方那份只保证活到 co_await 返回，而这份账要在好几轮里
        // 都认得「该把正文交给谁」
        if (static_cast<bool>(responseReceiver))
        {
            exchange.responseReceiver = responseReceiver;
        }
        const bool hasBody = !body.empty();
        if (const auto submitted = m_protocol->submitRequestHead(streamId, fieldLines, !hasBody); !submitted)
        {
            // 头段没被编出去，也就没有需要收口的流：把这条记账摘掉直接回因即可
            m_pendingStreams.erase(streamId);
            outcome.errorMessage = submitted.error().message;
            co_return outcome;
        }
        if (hasBody)
        {
            const std::span<const std::uint8_t> bodyBytes{reinterpret_cast<const std::uint8_t *>(body.data()), body.size()};
            if (const auto appended = m_protocol->appendRequestBody(streamId, bodyBytes, true); !appended)
            {
                m_pendingStreams.erase(streamId);
                outcome.errorMessage = appended.error().message;
                co_return outcome;
            }
        }

        // 时限挂在**底层连接**上：到点 close() 会把仍挂在可读上的收发协程叫醒，本循环才能退出。
        // 块作用域是为了在正常收齐时立刻撤销看门狗——协程帧的局部量要等调用方丢掉 Task 才析构，
        // 放在函数体上会留下一只醒着的看门狗，随时可能把这条已可用的连接掐掉
        {
            const Core::DeadlineGuard<QuicClientConnection> requestWatchdog(m_connection.eventLoop(), m_connection, waitTimeout, "HTTP/3 出站请求");
            for (std::size_t round = 0; round < kMaximumDriveRounds; ++round)
            {
                // 交货排在推动通路之前：这一批交完才归还额度，而归还的 MAX_STREAM_DATA 要靠下面
                // 那两句送出去——先推后交就会把「还窗口」推迟到对端已经没得发的时候
                if (exchange.responseReceiver && !co_await deliverReceivedBody(exchange, streamId))
                {
                    break; // 接收口收口：这条流已被本端结掉，连接留着给别的请求用
                }
                m_protocol->flush();
                co_await m_connection.sendPending();
                // 只要送过一轮就算「字节上过通路」：h3 的正文是边编边送，事后无从分辨哪一段先上线，
                // 而这条判据要回答的问题只是「重来会不会把非幂等请求做两遍」
                exchange.response.isAnyByteSent = true;
                if (exchange.isComplete)
                {
                    break;
                }
                if (m_connection.isClosed())
                {
                    noteStreamFailed(streamId, exchange.response.isAnyByteReceived ? "响应没收完，连接已被对端或本端收口" : "连接在收到任何字节之前就被收口");
                    break;
                }
                co_await m_connection.pumpOnce();
            }
        }

        outcome = exchange.response;
        if (!exchange.isComplete && outcome.errorMessage.empty())
        {
            outcome.errorMessage = "请求在本条流上没有收齐";
        }
        m_pendingStreams.erase(streamId);
        co_return outcome;
    }

    Core::Task<void> Http3ClientConnection::shutdown()
    {
        if (m_protocol != nullptr && m_isHealthy)
        {
            if (const auto drainResult = m_protocol->beginGracefulDrain(); !drainResult)
            {
                // 通告发不出去，就只能靠关掉这条连接告诉对端「别再收新的了」。这里必须留一条账：
                // 否则对端日志里是「客户端半途断线」，而本端一行都不说，两边看不出这是收尾还是故障
                LOG_WARN_FMT("Http3ClientConnection: 收尾时没能发出 GOAWAY：{}；本条连接按直接关闭收口", drainResult.error().message);
            }
            m_protocol->flush();
            co_await m_connection.sendPending();
        }
        m_connection.close();
        m_isHealthy = false;
        co_return;
    }

    void Http3ClientConnection::close() noexcept
    {
        m_isHealthy = false;
        m_connection.close();
    }

    bool Http3ClientConnection::isHealthy() const noexcept
    {
        // 「还能不能提请求」包含流号余量：客户端流号严格递增且到顶之后没有合法的新号可提
        // （RFC 9000 §2.1），一条只会回「请换一条连接」的连接留在池里，等于让每次取用都先撞一次失败
        return m_isHealthy && !m_connection.isClosed() && m_openedStreamCount < m_config.maximumOpenedStreamCount;
    }

    std::size_t Http3ClientConnection::inFlightStreamCount() const noexcept
    {
        return m_pendingStreams.size();
    }

    void Http3ClientConnection::noteHeaderField(const std::int64_t streamId, const std::string_view name, const std::string_view value)
    {
        // 本端已经不认这条流（收完或被结掉）：字段没有落账的地方，丢掉
        PendingExchange *exchange = liveExchange(streamId);
        if (exchange == nullptr)
        {
            return;
        }
        exchange->response.isAnyByteReceived = true;
        if (name == ":status")
        {
            // 与 h2 侧同一条判据（RFC 9110 §4.1 的 status-code = 3DIGITS）：atoi 会把 "abc" 折成 0、
            // 把 "2000" 折成 2000，上层按 2xx 分支时假号与真号长得一样。h3 这侧只结当前这条流
            // （RFC 9114 §4.1 允许按 H3_MESSAGE_ERROR 收口这条消息），连接留给别的流
            const std::optional<int> parsedStatusCode = parseStatusCodeText(value);
            if (!parsedStatusCode.has_value())
            {
                noteStreamFailed(streamId, std::format("响应伪头 :status 的取值「{}」不是三位十进制状态码（RFC 9110 §4.1），本端不猜它想写什么", value));
                m_connection.abortStream(streamId, static_cast<std::uint64_t>(Http3ErrorCode::MessageError));
                return;
            }
            exchange->response.statusCode = *parsedStatusCode;
            return;
        }
        exchange->response.headers.emplace_back(std::string{name}, std::string{value});
    }

    void Http3ClientConnection::noteBodyBytes(const std::int64_t streamId, const std::span<const std::uint8_t> bytes)
    {
        // 本端已经不认这条流：字节仍占着接收额度，账要还，内容丢掉。这里**不能**顺手立一条新账——
        // 请求协程收口时把这条流的记录摘掉了，而本端刚结掉一条流时 STOP_SENDING 至少还要一个来回
        // 才到对端，那之后到的字节正是走这一支；立了新账就没有人再来摘它，在途数再也回不到 0
        PendingExchange *exchange = liveExchange(streamId);
        if (exchange == nullptr)
        {
            m_connection.extendReceiveWindow(streamId, bytes.size());
            return;
        }
        exchange->response.isAnyByteReceived = true;
        // 上限按**累计**收到的字节判：挂了接收口时正文交一批就腾空一批，只量缓冲的话这道闸门
        // 就只剩「一次能堆多大」，对端可以一直发下去
        exchange->receivedBodyByteCount += bytes.size();
        if (!exchange->response.errorMessage.empty())
        {
            // 已判死的流不再累计：结论不会再变，继续攒只是让内存跟着对端的节奏长。但账要还——
            // 与 h2 侧 `handleDataFrame` 那条同解：收下不还等于让连接级窗口一路漏，
            // 而那一本账是这条连接上所有流共用的
            m_connection.extendReceiveWindow(streamId, bytes.size());
            return;
        }
        if (m_config.maximumResponseBodyBytes != 0U && exchange->receivedBodyByteCount > m_config.maximumResponseBodyBytes)
        {
            // 上限是本端的胃口，不是对端犯了协议错：只结这一条流（RESET + STOP_SENDING），连接留给别的流
            noteStreamFailed(streamId, std::format("响应正文超过本端上限 {} 字节：不打算收这么大的响应就把 Config::maximumResponseBodyBytes 调高（填 0 表示不限）",
                                                   m_config.maximumResponseBodyBytes));
            exchange->response.body.clear(); // 半份正文不交回调用方：它连一个完整的字段段都不构成
            exchange->undeliveredBodyBytes.clear();
            m_connection.abortStream(streamId, static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled));
            // 越界这一段仍然要还额度：流已判死，本层不会有人再来取它，不还就是让对端的连接级窗口一路漏
            m_connection.extendReceiveWindow(streamId, bytes.size());
            return;
        }
        if (exchange->responseReceiver)
        {
            // 挂了接收口：这一段攒着待交，额度等这一批交完再还。还得早了就没有背压——
            // 对端会照着窗口继续往下灌，本端缓冲的上界就成了正文总长而不是一档窗口
            exchange->undeliveredBodyBytes.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            return;
        }
        exchange->response.body.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        // 拷进本端缓冲就是消费掉了，当场把额度还回去。协议层刻意把 DATA 载荷的归还留给接收方
        // （`Http3Connection::creditConsumedBytes` 只就地归还非载荷字节），不还在这里还就没有别处还：
        // 对端写满本端宣告的流窗口（每条 256 KiB）就不再发，而本端还在等它继续发
        m_connection.extendReceiveWindow(streamId, bytes.size());
    }

    Core::Task<bool> Http3ClientConnection::deliverReceivedBody(PendingExchange &exchange, const std::int64_t streamId)
    {
        if (!exchange.response.errorMessage.empty())
        {
            co_return true; // 已判死的响应不再交：调用方从「返回的响应为空」就能看出这条没成
        }
        const bool isLastBatch = exchange.isComplete;
        if (exchange.undeliveredBodyBytes.empty() && (!isLastBatch || exchange.isFinalBatchDelivered))
        {
            co_return true; // 还没到货；或收尾那一批（含零长收尾）已经交过了
        }

        // 取走这一批再交：本端缓冲就此腾空，而额度要到交完才还，对端能压在本端窗口里的字节
        // 因此不超过 Config 里那档接收窗口
        std::string batch;
        batch.swap(exchange.undeliveredBodyBytes);
        const std::size_t takenByteCount = batch.size();
        exchange.isFinalBatchDelivered   = exchange.isFinalBatchDelivered || isLastBatch;

        if (!co_await exchange.responseReceiver(exchange.response, std::string_view{batch}, isLastBatch))
        {
            // 调用方主动收的口：头部仍是完整可信的响应，正文到此为止，这一条按成功交出。
            // 结掉这一条流就够（RESET + STOP_SENDING），连接上的别的请求不受牵连
            exchange.isComplete = true;
            m_connection.abortStream(streamId, static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled));
            co_return false;
        }
        // 交完才还这一批的额度：这就是背压的落点。还不出去也没关系——本层每一轮都先 flush
        // 再 sendPending，MAX_STREAM_DATA 跟着下一轮的包走
        m_connection.extendReceiveWindow(streamId, takenByteCount);
        co_return true;
    }

    void Http3ClientConnection::noteMessageEnded(const std::int64_t streamId)
    {
        const auto entry = m_pendingStreams.find(streamId);
        if (entry == m_pendingStreams.end())
        {
            return;
        }
        entry->second.isComplete = true;
    }

    void Http3ClientConnection::noteStreamFailed(const std::int64_t streamId, const std::string_view reason)
    {
        PendingExchange &exchange = exchangeFor(streamId);
        if (exchange.response.errorMessage.empty())
        {
            exchange.response.errorMessage = std::string{reason};
        }
        exchange.isComplete = true;
    }

    void Http3ClientConnection::failAllPending(const std::string_view reason)
    {
        for (auto &[streamId, exchange]: m_pendingStreams)
        {
            static_cast<void>(streamId);
            if (exchange.response.errorMessage.empty())
            {
                exchange.response.errorMessage = std::string{reason};
            }
            exchange.isComplete = true;
        }
    }

    Http3ClientConnection::PendingExchange &Http3ClientConnection::exchangeFor(const std::int64_t streamId)
    {
        return m_pendingStreams[streamId];
    }

    Http3ClientConnection::PendingExchange *Http3ClientConnection::liveExchange(const std::int64_t streamId) noexcept
    {
        const auto entry = m_pendingStreams.find(streamId);
        return entry == m_pendingStreams.end() ? nullptr : &entry->second;
    }


    Http3OutboundLink::Http3OutboundLink(Core::EventLoop &loop, QuicClientConnection::Configuration configuration, Http3ClientConnection::Config http3Configuration) :
        m_quic(std::make_unique<QuicClientConnection>(loop, std::move(configuration))), m_http3(*m_quic, http3Configuration)
    {
    }

    Core::Task<bool> Http3OutboundLink::connect(const Core::InetAddress &serverAddress)
    {
        if (!co_await m_quic->connect(serverAddress))
        {
            co_return false;
        }
        co_return co_await m_http3.start();
    }

    bool Http3OutboundLink::isHealthy() const noexcept
    {
        // 两层都要问：会话那边的「健康」只管 h3 自己（GOAWAY、协议错误、流号用尽），而 QUIC 连接可能
        // 先被对端或对端的网络收掉——那时会话那边还没收到任何信号，只看它会给出一个能提请求的假象
        return m_http3.isHealthy() && !m_quic->isClosed();
    }

} // namespace AsynGyanis::Net
