#include "Net/Http3/Http3ClientConnection.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Net/Http3/Qpack.h"

#include <charconv>
#include <cstdlib>
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
                                                                   const std::string_view body, const std::chrono::milliseconds waitTimeout)
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
        const bool       hasBody  = !body.empty();
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
            static_cast<void>(m_protocol->beginGracefulDrain());
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
        return m_isHealthy && !m_connection.isClosed();
    }

    std::size_t Http3ClientConnection::inFlightStreamCount() const noexcept
    {
        return m_pendingStreams.size();
    }

    void Http3ClientConnection::noteHeaderField(const std::int64_t streamId, const std::string_view name, const std::string_view value)
    {
        PendingExchange &exchange           = exchangeFor(streamId);
        exchange.response.isAnyByteReceived = true;
        if (name == ":status")
        {
            exchange.response.statusCode = std::atoi(std::string{value}.c_str());
            return;
        }
        exchange.response.headers.emplace_back(std::string{name}, std::string{value});
    }

    void Http3ClientConnection::noteBodyBytes(const std::int64_t streamId, const std::span<const std::uint8_t> bytes)
    {
        PendingExchange &exchange           = exchangeFor(streamId);
        exchange.response.isAnyByteReceived = true;
        exchange.response.body.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
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

} // namespace AsynGyanis::Net
