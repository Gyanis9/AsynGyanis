#include "CoroutinePool.h"

#include <new>

namespace Core
{
    CoroutinePool::CoroutinePool(const size_t blockSize, const size_t initialBlocks) :
        m_blockSize(blockSize)
    {
        expand(initialBlocks);
    }

    CoroutinePool::~CoroutinePool()
    {
        for (const auto &chunk: m_chunks)
        {
            ::operator delete(chunk);
        }
    }

    void *CoroutinePool::allocate(const size_t n)
    {
        // 如果请求大小超过块大小，回退到全局 new
        if (n > m_blockSize)
        {
            return ::operator new(n);
        }

        if (m_freeList.empty())
        {
            expand(m_allocatedCount > 0 ? m_allocatedCount : 64);
        }

        void *p = m_freeList.back();
        m_freeList.pop_back();
        return p;
    }

    void CoroutinePool::deallocate(void *const p, const size_t n)
    {
        if (!p)
        {
            return;
        }

        if (n > m_blockSize)
        {
            ::operator delete(p);
            return;
        }

        // 跨线程安全：指针不属于本池则回退到全局 delete
        if (!owns(p))
        {
            ::operator delete(p);
            return;
        }

        m_freeList.push_back(p);
    }

    bool CoroutinePool::owns(const void *const p) const noexcept
    {
        for (const auto *chunk: m_chunks)
        {
            const auto *begin = static_cast<const std::byte *>(chunk);
            if (const auto *ptr = static_cast<const std::byte *>(p); ptr >= begin && ptr < begin + m_allocatedCount * m_blockSize)
                return true;
        }
        return false;
    }

    CoroutinePool &CoroutinePool::instance()
    {
        thread_local CoroutinePool pool(256, 128);
        return pool;
    }

    size_t CoroutinePool::blockSize() const noexcept
    {
        return m_blockSize;
    }

    size_t CoroutinePool::allocatedCount() const noexcept
    {
        return m_allocatedCount;
    }

    void CoroutinePool::expand(const size_t count)
    {
        // 上限保护：防止无限增长（256 字节块，16384 块 = 4MB 每线程）
        static constexpr size_t kMaxBlocks = 16384;
        const size_t            newCount   = m_allocatedCount + count > kMaxBlocks
                                    ? (kMaxBlocks > m_allocatedCount ? kMaxBlocks - m_allocatedCount : 0)
                                    : count;
        if (newCount == 0)
            return;

        auto *chunk = static_cast<std::byte *>(::operator new(m_blockSize * newCount, static_cast<std::align_val_t>(alignof(std::max_align_t))));
        m_chunks.push_back(chunk);

        for (size_t i = 0; i < newCount; ++i)
        {
            m_freeList.push_back(chunk + i * m_blockSize);
        }
        m_allocatedCount += newCount;
    }

}
