#include "BufferPool.h"

namespace Core
{
    BufferPool::BufferPool(const size_t bufferSize, const size_t bufferCount) :
        m_bufferSize(bufferSize), m_bufferCount(bufferCount),
        m_memory(bufferSize * bufferCount), m_freeList(bufferCount)
    {
        for (size_t i = 0; i < bufferCount; ++i)
        {
            m_freeList[i] = static_cast<int>(bufferCount - 1 - i);
        }
        m_freeTop = bufferCount;
    }

    BufferPool::~BufferPool() = default;

    int BufferPool::acquire()
    {
        if (m_freeTop == 0)
            return -1;
        return m_freeList[--m_freeTop];
    }

    void BufferPool::release(const int index)
    {
        if (index >= 0 && static_cast<size_t>(index) < m_bufferCount)
        {
            m_freeList[m_freeTop++] = index;
        }
    }

    void *BufferPool::data(const int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= m_bufferCount)
            return nullptr;
        return m_memory.data() + static_cast<size_t>(index) * m_bufferSize;
    }

    size_t BufferPool::bufferSize() const noexcept
    {
        return m_bufferSize;
    }

    size_t BufferPool::bufferCount() const noexcept
    {
        return m_bufferCount;
    }

}
