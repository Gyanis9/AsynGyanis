#include "TcpStream.h"

#include "Base/Exception.h"

#include <algorithm>
#include <cstring>

namespace Net
{

    TcpStream::TcpStream(Core::AsyncSocket socket) :
        m_socket(std::move(socket)),
        m_readBuffer(4096),
        m_readPos(0)
    {
    }

    Core::Task<ssize_t> TcpStream::read(void *const buf, const size_t len)
    {
        if (len == 0)
            co_return 0;

        // Refill buffer if exhausted
        if (m_readPos >= m_readBuffer.size())
        {
            co_await fillBuffer();
            if (m_readBuffer.empty())
                co_return 0; // EOF
        }

        const size_t available = m_readBuffer.size() - m_readPos;
        const size_t toCopy    = std::min(len, available);
        std::memcpy(buf, m_readBuffer.data() + m_readPos, toCopy);
        m_readPos += toCopy;

        co_return static_cast<ssize_t>(toCopy);
    }

    Core::Task<> TcpStream::readExact(void *const buf, const size_t len)
    {
        auto * dst       = static_cast<char *>(buf);
        size_t remaining = len;

        while (remaining > 0)
        {
            const ssize_t n = co_await read(dst, remaining);
            if (n <= 0)
            {
                throw Base::Exception("TcpStream::readExact: connection closed or error before "
                        "reading required bytes");
            }
            dst       += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }

        co_return;
    }

    Core::Task<std::string> TcpStream::readUntil(const char delimiter, const size_t maxSize)
    {
        std::string result;

        while (true)
        {
            if (maxSize > 0 && result.size() >= maxSize)
                co_return result;

            // Scan buffered data for delimiter
            if (m_readPos < m_readBuffer.size())
            {
                const char *start = m_readBuffer.data() + m_readPos;
                const char *end   = m_readBuffer.data() + m_readBuffer.size();

                if (const auto *found = static_cast<const char *>(
                    std::memchr(start, delimiter,
                                static_cast<size_t>(end - start))))
                {
                    const size_t chunkLen = static_cast<size_t>(found - start);
                    if (maxSize > 0 && result.size() + chunkLen > maxSize)
                    {
                        result.append(start, maxSize - result.size());
                        co_return result;
                    }
                    result.append(start, chunkLen);
                    m_readPos += chunkLen + 1; // skip delimiter
                    co_return result;
                }

                // Delimiter not found — append all buffered data and refill
                const size_t appendLen = static_cast<size_t>(end - start);
                if (maxSize > 0 && result.size() + appendLen > maxSize)
                {
                    result.append(start, maxSize - result.size());
                    co_return result;
                }
                result.append(start, appendLen);
                m_readPos = m_readBuffer.size();
            }

            try
            {
                co_await fillBuffer();
            } catch (const Base::SystemException &)
            {
                co_return result; // return what we have on error
            }

            if (m_readBuffer.empty())
                co_return result; // EOF
        }
    }

    Core::Task<ssize_t> TcpStream::write(const void *const buf, const size_t len) const
    {
        co_return co_await m_socket.asyncSend(buf, len);
    }

    Core::Task<> TcpStream::writeAll(const void *const buf, const size_t len) const
    {
        auto * src       = static_cast<const char *>(buf);
        size_t remaining = len;

        while (remaining > 0)
        {
            const ssize_t n = co_await m_socket.asyncSend(src, remaining);
            if (n <= 0)
            {
                throw Base::Exception("TcpStream::writeAll: send failed or connection closed");
            }
            src       += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }

        co_return;
    }

    void TcpStream::close()
    {
        m_readBuffer.clear();
        m_readPos = 0;
        m_socket.close();
    }

    Core::AsyncSocket &TcpStream::socket()
    {
        return m_socket;
    }

    Core::Task<> TcpStream::fillBuffer()
    {
        m_readBuffer.resize(4096);
        m_readPos = 0;

        const ssize_t n = co_await m_socket.asyncRecv(m_readBuffer.data(), m_readBuffer.size());
        if (n > 0)
        {
            m_readBuffer.resize(static_cast<size_t>(n));
            co_return;
        }

        m_readBuffer.clear();
        if (n < 0)
        {
            throw Base::SystemException("TcpStream::fillBuffer: recv failed");
        }
        // n == 0: EOF, buffer stays empty
        co_return;
    }

}
