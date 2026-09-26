// Net 出站 keep-alive 连接与连接池的实现

#include "Net/Http/Client/HttpOutboundConnectionPool.h"

#include "Core/Tls/TlsSocket.h"
#include "Net/Http2/Http2ClientConnection.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Net
{
    bool HttpOutboundEndpointKey::operator==(const HttpOutboundEndpointKey &other) const noexcept
    {
        return host == other.host && port == other.port && isTls == other.isTls;
    }

    bool HttpOutboundEndpointKey::operator<(const HttpOutboundEndpointKey &other) const noexcept
    {
        if (isTls != other.isTls)
        {
            return !isTls;
        }
        if (port != other.port)
        {
            return port < other.port;
        }
        return host < other.host;
    }

    HttpOutboundConnection::HttpOutboundConnection(const HttpOutboundEndpointKey endpointKey, std::unique_ptr<TcpStream> plainSocket, std::unique_ptr<Core::TlsSocket> tlsSocket) :
        m_plainSocket(std::move(plainSocket)), m_tlsSocket(std::move(tlsSocket)), m_endpointKey(endpointKey)
    {
    }

    std::unique_ptr<HttpOutboundConnection> HttpOutboundConnection::forPlain(const HttpOutboundEndpointKey endpointKey, TcpStream stream)
    {
        // 堆上是必需的而不是习惯：连接要进池、要被取出，地址得稳定（挂起的收发协程持有它的引用）
        return std::unique_ptr<HttpOutboundConnection>(new HttpOutboundConnection(endpointKey, std::make_unique<TcpStream>(std::move(stream)), nullptr));
    }

    std::unique_ptr<HttpOutboundConnection> HttpOutboundConnection::forSecure(const HttpOutboundEndpointKey endpointKey, std::unique_ptr<Core::TlsSocket> socket)
    {
        return std::unique_ptr<HttpOutboundConnection>(new HttpOutboundConnection(endpointKey, nullptr, std::move(socket)));
    }

    HttpOutboundConnection::~HttpOutboundConnection()
    {
        close();
    }

    Core::Task<ssize_t> HttpOutboundConnection::receive(void *const buffer, const std::size_t length)
    {
        if (!m_isOpen)
        {
            co_return -1;
        }
        if (m_tlsSocket != nullptr)
        {
            // TLS 侧不返回负数：对端正常收口给 0，其余一律抛异常（会话失效、已被 close()）。
            // 这里把异常折成 -1，是为了让「这条连接不能再复用」成为唯一的结论——把异常抛给池的
            // 持有者只会让同一个失败在两个地方各写一遍收口逻辑
            try
            {
                const ssize_t receivedByteCount = co_await m_tlsSocket->asyncReceive(buffer, length);
                if (receivedByteCount == 0)
                {
                    m_isOpen = false;
                }
                co_return receivedByteCount;
            } catch (const std::exception &)
            {
                m_isOpen = false;
                co_return -1;
            }
        }

        const ssize_t receivedByteCount = co_await m_plainSocket->read(buffer, length);
        if (receivedByteCount <= 0)
        {
            // 0 是对端正常收口，负数是读错误：两种都不能再复用
            m_isOpen = false;
        }
        co_return receivedByteCount;
    }

    Core::Task<bool> HttpOutboundConnection::send(const std::string_view data)
    {
        if (!m_isOpen)
        {
            co_return false;
        }
        std::size_t writtenByteCount = 0;
        while (writtenByteCount < data.size())
        {
            const std::size_t remainingByteCount = data.size() - writtenByteCount;
            const char *const segmentBegin       = data.data() + writtenByteCount;
            if (m_tlsSocket != nullptr)
            {
                // asyncSend 可能只写出一部分（也可能抛异常）：按返回值推进，别假设一次就写完
                try
                {
                    const ssize_t segmentWrittenByteCount = co_await m_tlsSocket->asyncSend(segmentBegin, remainingByteCount);
                    if (segmentWrittenByteCount <= 0)
                    {
                        m_isOpen = false;
                        co_return false;
                    }
                    writtenByteCount += static_cast<std::size_t>(segmentWrittenByteCount);
                } catch (const std::exception &)
                {
                    m_isOpen = false;
                    co_return false;
                }
                continue;
            }

            const ssize_t segmentWrittenByteCount = co_await m_plainSocket->write(segmentBegin, remainingByteCount);
            if (segmentWrittenByteCount <= 0)
            {
                m_isOpen = false;
                co_return false;
            }
            writtenByteCount += static_cast<std::size_t>(segmentWrittenByteCount);
        }
        co_return true;
    }

    bool HttpOutboundConnection::isOpen() const noexcept
    {
        return m_isOpen;
    }

    std::string HttpOutboundConnection::selectedAlpnProtocol() const
    {
        // 明文侧没有 ALPN 这回事（h2c 要靠 prior-knowledge 或 Upgrade，都不在这一层）
        return m_tlsSocket != nullptr ? m_tlsSocket->selectedAlpnProtocol() : std::string{};
    }

    void HttpOutboundConnection::close() noexcept
    {
        m_isOpen = false;
        if (m_tlsSocket != nullptr)
        {
            m_tlsSocket->close();
        }
        if (m_plainSocket != nullptr)
        {
            m_plainSocket->close();
        }
    }

    void HttpOutboundConnection::prepareForNextRequest() noexcept
    {
        m_parser.reset();
    }

    HttpOutboundConnectionPool::HttpOutboundConnectionPool(const Config config) noexcept : m_config(config)
    {
    }

    std::unique_ptr<HttpOutboundConnection> HttpOutboundConnectionPool::acquire(const HttpOutboundEndpointKey &endpointKey)
    {
        const auto groupIterator = m_idleByEndpoint.find(endpointKey);
        if (groupIterator == m_idleByEndpoint.end())
        {
            return nullptr;
        }

        const Clock::time_point now     = Clock::now();
        std::vector<IdleEntry> &entries = groupIterator->second;
        // 从队尾取：队尾是最近用完的那条，还热着（对端的空闲计时也还没走完）。留在队头的先过期，
        // 由下面的可用性判据顺手收掉
        while (!entries.empty())
        {
            IdleEntry entry = std::move(entries.back());
            entries.pop_back();
            if (now - entry.idleSince > m_config.idleTimeout || !entry.connection->isOpen())
            {
                continue;
            }
            if (entries.empty())
            {
                m_idleByEndpoint.erase(groupIterator);
            }
            return std::move(entry.connection);
        }
        m_idleByEndpoint.erase(groupIterator);
        return nullptr;
    }

    void HttpOutboundConnectionPool::release(std::unique_ptr<HttpOutboundConnection> connection)
    {
        if (!connection || !connection->isOpen())
        {
            return;
        }
        if (m_config.maximumIdlePerEndpoint == 0U)
        {
            // 每键留 0 条就是「不池化」：当场收口。少了这一句，下面按条数腾位置的循环会在空表上
            // erase(begin())——那是有未定义行为的代码，而 0 是公开 Config 允许填进来的值
            connection->close();
            return;
        }

        std::vector<IdleEntry> &entries = m_idleByEndpoint[connection->endpointKey()];
        const Clock::time_point now     = Clock::now();

        // 先把过期的清掉再腾位置：越界时收的是队头（最旧的那条），新用完的这条留在队尾。
        // 这里只判时间——空闲表里的连接不可能已被关掉（关连接的那几条路径都不会把它交回池），
        // 取用侧多加的那道 isOpen() 是给「交出去之前必须可用」兜底的，两处问的不是一个问题
        for (std::size_t index = entries.size(); index > 0; --index)
        {
            if (now - entries[index - 1].idleSince > m_config.idleTimeout)
            {
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index - 1));
            }
        }
        while (entries.size() >= m_config.maximumIdlePerEndpoint)
        {
            entries.erase(entries.begin());
        }
        entries.push_back(IdleEntry{.connection = std::move(connection), .idleSince = now});
    }

    std::size_t HttpOutboundConnectionPool::idleConnectionCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto &group: m_idleByEndpoint)
        {
            count += group.second.size();
        }
        return count;
    }

    void HttpOutboundConnectionPool::closeAll() noexcept
    {
        for (auto &group: m_idleByEndpoint)
        {
            for (auto &entry: group.second)
            {
                entry.connection->close();
            }
        }
        m_idleByEndpoint.clear();
        for (auto &entry: m_http2ByEndpoint)
        {
            // 只收空闲的那部分（与 h1 侧同一个契约）：h2 的连接是与调用方共同持有的，这里 close()
            // 一条正被人用的连接等于把别人的请求掐了。只 close 不 shutdown：这条入口的语义是「立刻
            // 把描述符还掉」，而发 GOAWAY 要等一次写出
            if (entry.second->inFlightStreamCount() == 0U)
            {
                entry.second->close();
            }
        }
        m_http2ByEndpoint.clear();
    }

    HttpOutboundConnectionPool::~HttpOutboundConnectionPool() = default;

    std::shared_ptr<Http2ClientConnection> HttpOutboundConnectionPool::acquireHttp2(const HttpOutboundEndpointKey &endpointKey)
    {
        const auto iterator = m_http2ByEndpoint.find(endpointKey);
        if (iterator == m_http2ByEndpoint.end())
        {
            return nullptr;
        }
        // 交出去之前先问一句还能不能用：对端在空闲期间把连接收掉时，连接层已经知道这件事了。
        // 判死就不回头——HPACK 动态表跟着连接一起作废，再拿它发下一条只会解歪
        if (!iterator->second->isHealthy())
        {
            m_http2ByEndpoint.erase(iterator);
            return nullptr;
        }
        // 不摘走：h2 的连接能同时供几条请求用，摘走等于每次取用都独占一条
        return iterator->second;
    }

    std::size_t HttpOutboundConnectionPool::idleHttp2ConnectionCount() const noexcept
    {
        return m_http2ByEndpoint.size();
    }

    std::size_t HttpOutboundConnectionPool::http2MaximumInFlightStreamCount() const noexcept
    {
        std::size_t maximumStreamCount = 0;
        for (const auto &entry: m_http2ByEndpoint)
        {
            // 逐条取最大而不是求和：求和会把「两条连接各一条流」也算成 2，那样就分不出复用了
            maximumStreamCount = std::max(maximumStreamCount, entry.second->inFlightStreamCount());
        }
        return maximumStreamCount;
    }

    std::string HttpOutboundConnectionPool::establishmentKeyOf(const HttpOutboundEndpointKey &endpointKey)
    {
        // 字段之间用单元分隔符而不是冒号：主机文本本身就有冒号（IPv6 字面量），拿冒号当分隔符会让
        // 「主机里带端口样子文本」的两个端点撞进同一格，合并错对象比不合并更糟
        std::string key;
        key.reserve(endpointKey.host.size() + 16U);
        key += endpointKey.host;
        key += '\x1F';
        key += std::to_string(endpointKey.port);
        key += '\x1F';
        key += endpointKey.isTls ? 'T' : 'P';
        return key;
    }

    bool HttpOutboundConnectionPool::tryBeginEstablishment(const HttpOutboundEndpointKey &endpointKey)
    {
        return m_establishments->tryBecomeLeader(establishmentKeyOf(endpointKey));
    }

    void HttpOutboundConnectionPool::settleEstablishment(const HttpOutboundEndpointKey &endpointKey)
    {
        m_establishments->settle(establishmentKeyOf(endpointKey));
    }

    HttpEstablishmentAwait HttpOutboundConnectionPool::awaitEstablishment(const HttpOutboundEndpointKey &endpointKey, Core::EventLoop &loop)
    {
        return HttpEstablishmentAwait(m_establishments, establishmentKeyOf(endpointKey), loop);
    }

    void HttpOutboundConnectionPool::adoptHttp2(const HttpOutboundEndpointKey &endpointKey, std::shared_ptr<Http2ClientConnection> connection)
    {
        if (connection == nullptr || !connection->isHealthy())
        {
            return; // 不可用的那条不收：最后一个持有者放手时通路随之关掉
        }
        // 一台主机只留一条：h2 的并发在流上，第二条连接换不来更多吞吐，只多占一个描述符与一张
        // HPACK 动态表。已经有货就收下一条（旧的那条继续被在途请求持有，去留不由这里做主）
        const auto iterator = m_http2ByEndpoint.find(endpointKey);
        if (iterator != m_http2ByEndpoint.end() && iterator->second->isHealthy())
        {
            return;
        }
        m_http2ByEndpoint[endpointKey] = std::move(connection);
    }

} // namespace AsynGyanis::Net
