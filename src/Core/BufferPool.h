/**
 * @file BufferPool.h
 * @brief 固定大小字节缓冲池
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_BUFFERPOOL_H
#define CORE_BUFFERPOOL_H

#include <cstddef>
#include <vector>

namespace Core
{
    class BufferPool
    {
    public:
        BufferPool(size_t bufferSize, size_t bufferCount);

        ~BufferPool();

        BufferPool(const BufferPool &)            = delete;
        BufferPool &operator=(const BufferPool &) = delete;
        BufferPool(BufferPool &&)                 = delete;
        BufferPool &operator=(BufferPool &&)      = delete;

        int                  acquire();
        void                 release(int index);
        [[nodiscard]] void * data(int index);
        [[nodiscard]] size_t bufferSize() const noexcept;
        [[nodiscard]] size_t bufferCount() const noexcept;

    private:
        size_t                 m_bufferSize;
        size_t                 m_bufferCount;
        std::vector<std::byte> m_memory;
        std::vector<bool>      m_used;
    };

}

#endif
