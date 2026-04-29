/**
 * @file StreamBuffer.h
 * @brief Ring buffer for zero-copy parsing
 * @copyright Copyright (c) 2026
 */
#ifndef NET_STREAMBUFFER_H
#define NET_STREAMBUFFER_H

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace Net
{

    class StreamBuffer
    {
    public:
        explicit StreamBuffer(const size_t capacity) :
            m_buffer(capacity), m_readIdx(0), m_writeIdx(0), m_size(0)
        {
        }

        ~StreamBuffer() = default;

        StreamBuffer(const StreamBuffer &)            = delete;
        StreamBuffer &operator=(const StreamBuffer &) = delete;
        StreamBuffer(StreamBuffer &&)                 = delete;
        StreamBuffer &operator=(StreamBuffer &&)      = delete;

        size_t write(const void *data, const size_t len)
        {
            if (len == 0 || m_size == m_buffer.size())
                return 0;

            const size_t available = freeSpace();
            const size_t toWrite   = std::min(len, available);

            const size_t firstChunk = std::min(toWrite, m_buffer.size() - m_writeIdx);
            std::memcpy(m_buffer.data() + m_writeIdx, data, firstChunk);

            if (firstChunk < toWrite)
            {
                const size_t secondChunk = toWrite - firstChunk;
                std::memcpy(m_buffer.data(), static_cast<const std::byte *>(data) + firstChunk, secondChunk);
            }

            m_writeIdx = (m_writeIdx + toWrite) % m_buffer.size();
            m_size     += toWrite;
            return toWrite;
        }

        size_t read(void *buf, const size_t len)
        {
            if (len == 0 || m_size == 0)
                return 0;

            const size_t toRead = std::min(len, m_size);

            const size_t firstChunk = std::min(toRead, m_buffer.size() - m_readIdx);
            std::memcpy(buf, m_buffer.data() + m_readIdx, firstChunk);

            if (firstChunk < toRead)
            {
                const size_t secondChunk = toRead - firstChunk;
                std::memcpy(static_cast<std::byte *>(buf) + firstChunk, m_buffer.data(), secondChunk);
            }

            m_readIdx = (m_readIdx + toRead) % m_buffer.size();
            m_size    -= toRead;
            return toRead;
        }

        std::span<const std::byte> peek() const
        {
            if (m_size == 0)
                return {};

            if (m_readIdx < m_writeIdx || m_size <= m_buffer.size() - m_readIdx)
            {
                return {m_buffer.data() + m_readIdx, m_size};
            }

            return {m_buffer.data() + m_readIdx, m_buffer.size() - m_readIdx};
        }

        void commit(const size_t len)
        {
            const size_t toCommit = std::min(len, m_size);
            m_readIdx             = (m_readIdx + toCommit) % m_buffer.size();
            m_size                -= toCommit;
        }

        size_t available() const
        {
            return m_size;
        }

        size_t freeSpace() const
        {
            return m_buffer.size() - m_size;
        }

        void clear()
        {
            m_readIdx  = 0;
            m_writeIdx = 0;
            m_size     = 0;
        }

    private:
        std::vector<std::byte> m_buffer;
        size_t                 m_readIdx;
        size_t                 m_writeIdx;
        size_t                 m_size;
    };

}

#endif
