/**
 * @file StreamBuffer.h
 * @brief 用于零拷贝解析的环形缓冲区
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
    /**
     * @brief 环形缓冲区（Ring Buffer），支持高效的数据写入、读取和查看（peek），避免内存拷贝。
     *
     * 该实现使用 std::vector<std::byte> 作为底层存储，通过读写指针维护环形结构。
     * 支持：
     * - write()：向缓冲区写入数据
     * - read()：从缓冲区读取数据（消费）
     * - peek()：获取缓冲区中可读数据的 span（不消费）
     * - commit()：手动推进读指针（配合 peek 使用）
     * - 自动取模处理环形回绕
     *
     * 适用于网络协议解析等需要分段处理数据的场景，尤其适合配合非阻塞 I/O。
     */
    class StreamBuffer
    {
    public:
        /**
         * @brief 构造指定容量的环形缓冲区。
         * @param capacity 缓冲区容量（字节数）
         */
        explicit StreamBuffer(const size_t capacity) :
            m_buffer(capacity), m_readIdx(0), m_writeIdx(0), m_size(0)
        {
        }

        /**
         * @brief 默认析构函数。
         */
        ~StreamBuffer() = default;

        StreamBuffer(const StreamBuffer &)            = delete;
        StreamBuffer &operator=(const StreamBuffer &) = delete;
        StreamBuffer(StreamBuffer &&)                 = delete;
        StreamBuffer &operator=(StreamBuffer &&)      = delete;

        /**
         * @brief 向缓冲区写入数据。
         * @param data 指向要写入数据的指针
         * @param len  要写入的字节数
         * @return 实际写入的字节数（可能小于 len，当剩余空间不足时）
         *
         * 当剩余空间不足时只写入部分数据，剩余数据需由调用者后续重试。
         */
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

        /**
         * @brief 从缓冲区读取数据（消费数据）。
         * @param buf 存放读取数据的目标缓冲区指针
         * @param len 最大读取字节数
         * @return 实际读取的字节数（0 表示缓冲区为空）
         *
         * 读取后数据从缓冲区中移除，读指针向前移动。
         */
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

        /**
         * @brief 查看缓冲区中的数据（不消费）。
         * @return 包含可读数据的 std::span<const std::byte>，若缓冲区为空则返回空 span。
         *
         * 返回的 span 可能指向环形缓冲区中的连续区域，若数据跨越缓冲区末端，
         * 则仅返回从读指针到缓冲区末尾的连续部分（剩余部分需要再次使用 peek 并处理回绕，
         * 或改用 commit 与多次 peek 配合）。
         *
         * 注意：该实现只返回一段连续区域，若要完整查看所有可读数据，
         * 调用者需要检查 span.size() < available() 并处理第二次 peek。
         */
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

        /**
         * @brief 手动提交已消费的数据（移动读指针）。
         * @param len 要提交的字节数（不能超过当前可读数据大小）
         *
         * 常与 peek() 配合使用：用户通过 peek 获取数据并处理完后，调用 commit 通知缓冲区数据已消费。
         */
        void commit(const size_t len)
        {
            const size_t toCommit = std::min(len, m_size);
            m_readIdx             = (m_readIdx + toCommit) % m_buffer.size();
            m_size                -= toCommit;
        }

        /**
         * @brief 获取当前可读数据的字节数。
         * @return 可读字节数
         */
        size_t available() const
        {
            return m_size;
        }

        /**
         * @brief 获取当前剩余可写入的字节数。
         * @return 剩余空间大小
         */
        size_t freeSpace() const
        {
            return m_buffer.size() - m_size;
        }

        /**
         * @brief 清空缓冲区，重置读写位置。
         */
        void clear()
        {
            m_readIdx  = 0;
            m_writeIdx = 0;
            m_size     = 0;
        }

    private:
        std::vector<std::byte> m_buffer;   ///< 底层存储
        size_t                 m_readIdx;  ///< 读指针索引（逻辑位置）
        size_t                 m_writeIdx; ///< 写指针索引
        size_t                 m_size;     ///< 当前有效数据大小
    };

}

#endif
