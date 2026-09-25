// Net 出站 keep-alive 连接与连接池的实现

#include "Net/Http/Client/HttpOutboundConnectionPool.h"

#include "Core/Tls/TlsSocket.h"

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

    HttpOutboundConnection::HttpOutboundConnection(const HttpOutboundEndpointKey endpointKey,
                                                   std::unique_ptr<TcpStream> plainSocket,
                                                   std::unique_ptr<Core::TlsSocket> tlsSocket)
        : m_plainSocket(std::move(plainSocket))
        , m_tlsSocket(std::move(tlsSocket))
        , m_endpointKey(endpointKey)
    {
    }

    std::unique_ptr<HttpOutboundConnection> HttpOutboundConnection::forPlain(const HttpOutboundEndpointKey endpointKey,
                                                                            TcpStream stream)
    {
        // 堆上是必需的而不是习惯：连接要进池、要被取出，地址得稳定（挂起的收发协程持有它的引用）
        return std::unique_ptr<HttpOutboundConnection>(
                new HttpOutboundConnection(endpointKey, std::make_unique<TcpStream>(std::move(stream)), nullptr));
    }

    std::unique_ptr<HttpOutboundConnection> HttpOutboundConnection::forSecure(const HttpOutboundEndpointKey endpointKey,
                                                                              std::unique_ptr<Core::TlsSocket> socket)
    {
        return std::unique_ptr<HttpOutboundConnection>(
                new HttpOutboundConnection(endpointKey, nullptr, std::move(socket)));
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
            const char *const segmentBegin = data.data() + writtenByteCount;
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

    HttpOutboundConnectionPool::HttpOutboundConnectionPool(const Config config) noexcept
        : m_config(config)
    {
    }

    std::unique_ptr<HttpOutboundConnection> HttpOutboundConnectionPool::acquire(const HttpOutboundEndpointKey &endpointKey)
    {
        const auto groupIterator = m_idleByEndpoint.find(endpointKey);
        if (groupIterator == m_idleByEndpoint.end())
        {
            return nullptr;
        }

        const Clock::time_point now = Clock::now();
        std::vector<IdleEntry> &entries = groupIterator->second;
        // 从队尾取：队尾是最近用完的那条，还热着（对端的空闲计时也还没走完）。留在队头的先过期，
        // 由下面的过期检查顺手收掉
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

        std::vector<IdleEntry> &entries = m_idleByEndpoint[connection->endpointKey()];
        const Clock::time_point now = Clock::now();

        // 先把过期的清掉再腾位置：越界时收的是队头（最旧的那条），新用完的这条留在队尾
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

    std::size_t HttpOutboundConnectionPool::purgeIdle()
    {
        const Clock::time_point now = Clock::now();
        std::size_t purgedConnectionCount = 0;
        for (auto groupIterator = m_idleByEndpoint.begin(); groupIterator != m_idleByEndpoint.end();)
        {
            std::vector<IdleEntry> &entries = groupIterator->second;
            // 就地压实而不是逐条 erase：逐条摘会把后面的元素整段前移，一次清理变成 O(n²)
            std::size_t keptCount = 0;
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                if (now - entries[index].idleSince > m_config.idleTimeout || !entries[index].connection->isOpen())
                {
                    ++purgedConnectionCount;
                    continue;
                }
                if (keptCount != index)
                {
                    entries[keptCount] = std::move(entries[index]);
                }
                ++keptCount;
            }
            entries.resize(keptCount);

            if (entries.empty())
            {
                groupIterator = m_idleByEndpoint.erase(groupIterator);
                continue;
            }
            ++groupIterator;
        }
        return purgedConnectionCount;
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
    }
} // namespace AsynGyanis::Net
