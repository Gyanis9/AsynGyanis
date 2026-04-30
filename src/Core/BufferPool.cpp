#include "BufferPool.h"

namespace Core
{
    BufferPool::BufferPool(const size_t bufferSize, const size_t bufferCount) :
        m_bufferSize(bufferSize), m_bufferCount(bufferCount),
        m_memory(bufferSize * bufferCount), m_used(bufferCount, false)
    {
    }

    BufferPool::~BufferPool() = default;

    int BufferPool::acquire()
    {
        for (size_t i = 0; i < m_bufferCount; ++i)
        {
            if (!m_used[i])
            {
                m_used[i] = true;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void BufferPool::release(const int index)
    {
        if (index >= 0 && static_cast<size_t>(index) < m_bufferCount)
        {
            m_used[static_cast<size_t>(index)] = false;
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
