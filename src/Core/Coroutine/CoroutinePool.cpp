#include "Core/Coroutine/CoroutinePool.h"

#include <algorithm>
#include <new>

namespace AsynGyanis::Core
{
    CoroutinePool::CoroutinePool(const size_t blockSize, const size_t initialBlocks) :
        m_blockSize(blockSize)
    {
        const std::lock_guard lock(m_mutex);
        expand(initialBlocks);
    }

    CoroutinePool::~CoroutinePool()
    {
        for (const auto &chunk: m_chunks)
        {
            ::operator delete(chunk.data);
        }
    }

    CoroutinePool &CoroutinePool::instance()
    {
        // 单例刻意不进入 RAII 销毁队列：协程帧的销毁时机可能晚于任何函数局部静态对象，
        // 一旦池先退出，回收路径就无从判断块归属，只能泄漏少量池内存换取绝对安全
        static CoroutinePool *const pool = new CoroutinePool(kDefaultBlockSize, kDefaultInitialBlocks);
        return *pool;
    }

    void *CoroutinePool::allocate(const size_t requiredSize)
    {
        // 超过块容量的请求交给全局堆：固定规格被大帧撑大会让后续所有分配跟着膨胀
        if (requiredSize > m_blockSize)
        {
            return ::operator new(requiredSize);
        }

        const std::lock_guard lock(m_mutex);
        // 空闲列表耗尽时按当前容量翻倍扩容，摊薄连续分配时的扩容次数
        if (m_freeList.empty())
        {
            expand(m_allocatedCount > 0 ? m_allocatedCount : kDefaultInitialBlocks);
        }

        void *pointer = m_freeList.back();
        m_freeList.pop_back();
        return pointer;
    }

    void CoroutinePool::deallocate(void *const pointer, const size_t requiredSize) noexcept
    {
        if (!pointer)
        {
            return;
        }

        // 判定分支必须与 allocate() 完全一致，否则会把全局堆内存塞进空闲列表
        if (requiredSize > m_blockSize)
        {
            ::operator delete(pointer);
            return;
        }

        const std::lock_guard lock(m_mutex);
        // 指针确实属于本池的内存段才回收；外部误传的指针交还全局堆，避免污染空闲列表
        if (ownsUnlocked(pointer))
        {
            m_freeList.push_back(pointer);
        } else
        {
            ::operator delete(pointer);
        }
    }

    bool CoroutinePool::owns(const void *const pointer) const noexcept
    {
        const std::lock_guard lock(m_mutex);
        return ownsUnlocked(pointer);
    }

    size_t CoroutinePool::blockSize() const noexcept
    {
        return m_blockSize;
    }

    size_t CoroutinePool::allocatedCount() const noexcept
    {
        const std::lock_guard lock(m_mutex);
        return m_allocatedCount;
    }

    void CoroutinePool::expand(const size_t count)
    {
        // 上限保护：达到 kMaximumTotalBlocks 后停止扩张，由全局堆承接后续需求
        if (m_allocatedCount >= kMaximumTotalBlocks)
        {
            return;
        }
        const size_t newCount = std::min(count, kMaximumTotalBlocks - m_allocatedCount);

        // 默认对齐即可满足协程帧要求：x64 上 ::operator new 的默认对齐为 16 字节，
        // 与 delete 的自然配对，避免带 align_val_t 分配却用不带对齐参数的释放
        auto *const data = static_cast<std::byte *>(::operator new(m_blockSize * newCount));
        m_chunks.push_back(MemoryChunk{data, newCount});

        for (size_t blockIndex = 0; blockIndex < newCount; ++blockIndex)
        {
            m_freeList.push_back(data + blockIndex * m_blockSize);
        }
        m_allocatedCount += newCount;
    }

    bool CoroutinePool::ownsUnlocked(const void *const pointer) const noexcept
    {
        const auto *const target = static_cast<const std::byte *>(pointer);
        for (const auto &chunk: m_chunks)
        {
            // 逐段用该段自身的块数计算边界：早期实现误用全池总块数，
            // 会让前一段的判定区间越过自身末尾而错误认领后一段的指针
            if (target >= chunk.data && target < chunk.data + chunk.blockCount * m_blockSize)
            {
                return true;
            }
        }
        return false;
    }

} // namespace AsynGyanis::Core
