#include "TcpStream.h"

#include <algorithm>
#include <cstring>

namespace Net
{

    TcpStream::TcpStream(Core::AsyncSocket socket) :
        m_socket(std::move(socket))
        , m_readBuffer(4096)
        , m_readPos(0)
    {
    }

    Core::Task<ssize_t> TcpStream::read(void *buf, const size_t len)
    {
        if (m_readPos > 0 && m_readPos < m_readBuffer.size())
        {
            const size_t buffered = m_readBuffer.size() - m_readPos;
            const size_t toCopy   = std::min(len, buffered);
            std::memcpy(buf, m_readBuffer.data() + m_readPos, toCopy);
            m_readPos += toCopy;

            if (m_readPos >= m_readBuffer.size())
            {
                m_readBuffer.clear();
                m_readPos = 0;
            }

            co_return static_cast<ssize_t>(toCopy);
        }

        m_readBuffer.resize(len);
        ssize_t n = co_await m_socket.asyncRecv(m_readBuffer.data(), len);
        if (n <= 0)
            co_return n;

        std::memcpy(buf, m_readBuffer.data(), static_cast<size_t>(n));
        m_readBuffer.clear();
        co_return n;
    }

    Core::Task<> TcpStream::readExact(void *buf, const size_t len)
    {
        size_t totalRead = 0;
        auto * dst       = static_cast<char *>(buf);

        while (totalRead < len)
        {
            const ssize_t n = co_await read(dst + totalRead, len - totalRead);
            if (n <= 0)
            {
                co_return;
            }
            totalRead += static_cast<size_t>(n);
        }
    }

    Core::Task<std::string> TcpStream::readUntil(const char delimiter)
    {
        std::string result;

        while (true)
        {
            if (m_readPos < m_readBuffer.size())
            {
                auto *      start = m_readBuffer.data() + m_readPos;
                const auto *end   = m_readBuffer.data() + m_readBuffer.size();

                if (const auto *found = static_cast<char *>(std::memchr(start, delimiter, end - start)))
                {
                    const size_t chunkLen = found - start;
                    result.append(start, chunkLen);
                    m_readPos += chunkLen + 1;

                    if (m_readPos >= m_readBuffer.size())
                    {
                        m_readBuffer.clear();
                        m_readPos = 0;
                    }

                    co_return result;
                } else
                {
                    result.append(start, end - start);
                    m_readBuffer.clear();
                    m_readPos = 0;
                }
            }

            m_readBuffer.resize(4096);
            const ssize_t n = co_await m_socket.asyncRecv(m_readBuffer.data(), m_readBuffer.size());
            if (n <= 0)
            {
                m_readBuffer.clear();
                co_return result;
            }
            m_readBuffer.resize(static_cast<size_t>(n));
            m_readPos = 0;
        }
    }

    Core::Task<ssize_t> TcpStream::write(const void *buf, const size_t len) const
    {
        co_return co_await m_socket.asyncSend(buf, len);
    }

    Core::Task<> TcpStream::writeAll(const void *buf, const size_t len) const
    {
        size_t totalWritten = 0;
        auto * src          = static_cast<const char *>(buf);

        while (totalWritten < len)
        {
            const ssize_t n = co_await m_socket.asyncSend(src + totalWritten, len - totalWritten);
            if (n <= 0)
            {
                co_return;
            }
            totalWritten += static_cast<size_t>(n);
        }
    }

    void TcpStream::close()
    {
        m_socket.close();
    }

    Core::AsyncSocket &TcpStream::socket()
    {
        return m_socket;
    }

}
