/**
 * @file TcpStream.cpp
 * @brief 基于 AsyncSocket 的带缓冲 TCP 流，提供高阶读写接口
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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

    Core::Task<ssize_t> TcpStream::read(void *const buffer, const size_t length)
    {
        if (length == 0)
            co_return 0;

        // 如果缓冲区已耗尽则重新填充
        if (m_readPos >= m_readBuffer.size())
        {
            co_await fillBuffer();
            if (m_readBuffer.empty())
                co_return 0; // EOF
        }

        const size_t available = m_readBuffer.size() - m_readPos;
        const size_t toCopy    = std::min(length, available);
        std::memcpy(buffer, m_readBuffer.data() + m_readPos, toCopy);
        m_readPos += toCopy;

        co_return static_cast<ssize_t>(toCopy);
    }

    Core::Task<> TcpStream::readExact(void *const buffer, const size_t length)
    {
        auto * destination = static_cast<char *>(buffer);
        size_t remaining   = length;

        while (remaining > 0)
        {
            const ssize_t n = co_await read(destination, remaining);
            if (n <= 0)
            {
                throw Base::Exception("TcpStream::readExact: connection closed or error before "
                        "reading required bytes");
            }
            destination += static_cast<size_t>(n);
            remaining   -= static_cast<size_t>(n);
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

            // 扫描缓冲数据中的分隔符
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

                // 未找到分隔符 — 追加所有缓冲数据并重新填充
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

    Core::Task<ssize_t> TcpStream::write(const void *const buffer, const size_t length) const
    {
        co_return co_await m_socket.asyncSend(buffer, length);
    }

    Core::Task<> TcpStream::writeAll(const void *const buffer, const size_t length) const
    {
        auto * source    = static_cast<const char *>(buffer);
        size_t remaining = length;

        while (remaining > 0)
        {
            const ssize_t n = co_await m_socket.asyncSend(source, remaining);
            if (n <= 0)
            {
                throw Base::Exception("TcpStream::writeAll: send failed or connection closed");
            }
            source    += static_cast<size_t>(n);
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

        const ssize_t n = co_await m_socket.asyncReceive(m_readBuffer.data(), m_readBuffer.size());
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
