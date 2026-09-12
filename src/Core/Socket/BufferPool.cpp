/**
 * @file BufferPool.cpp
 * @brief 固定大小字节缓冲池
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/BufferPool.h"

namespace AsynGyanis::Core
{
    BufferPool::BufferPool(const size_t bufferSize, const size_t bufferCount) :
        m_bufferSize(bufferSize), m_bufferCount(bufferCount),
        m_memory(bufferSize * bufferCount), m_freeList(bufferCount), m_isOccupied(bufferCount, 0)
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

        const int index = m_freeList[--m_freeTop];
        m_isOccupied[static_cast<size_t>(index)] = 1;
        return index;
    }

    void BufferPool::release(const int index)
    {
        // 越界索引直接忽略：release 只接受 acquire 交出的索引
        if (index < 0 || static_cast<size_t>(index) >= m_bufferCount)
        {
            return;
        }

        // 未占用（重复释放、或释放从未取出的索引）时不做任何事：若照旧压栈，
        // 同一索引会在空闲栈里出现两次，之后 acquire() 会把同一块缓冲交给两个调用方，
        // 两个使用者互相覆写数据且不报任何错——静默损坏比忽略一次误用危险得多
        std::uint8_t &occupiedFlag = m_isOccupied[static_cast<size_t>(index)];
        if (occupiedFlag == 0)
        {
            return;
        }

        occupiedFlag               = 0;
        m_freeList[m_freeTop++]    = index;
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
