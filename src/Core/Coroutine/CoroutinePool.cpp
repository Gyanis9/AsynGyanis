#include "Core/Coroutine/CoroutinePool.h"

#include <algorithm>
#include <new>

namespace AsynGyanis::Core
{
    CoroutinePool::CoroutinePool(const size_t blockSize, const size_t initialBlocks) :
        m_blockSize(blockSize)
    {
        // 启动时先备一批块：把「首次分配就扩容」这条冷路径提前到构造期
        const std::lock_guard lock(m_mutex);
        expand(0, initialBlocks);
    }

    CoroutinePool::~CoroutinePool()
    {
        const size_t publishedChunks = m_chunkCount.load(std::memory_order_acquire);
        for (size_t chunkIndex = 0; chunkIndex < publishedChunks; ++chunkIndex)
        {
            ::operator delete(m_chunks[chunkIndex].data);
        }
    }

    CoroutinePool &CoroutinePool::instance()
    {
        // 单例刻意不进入 RAII 销毁队列：协程帧的销毁时机可能晚于任何函数局部静态对象，
        // 一旦池先退出，回收路径就无从判断块归属，只能泄漏少量池内存换取绝对安全
        static CoroutinePool *const pool = new CoroutinePool(kDefaultBlockSize, kDefaultInitialBlocks);
        return *pool;
    }

    CoroutinePool::ThreadCache &CoroutinePool::threadCache() noexcept
    {
        // 函数内 thread_local：每线程一份，首次使用时构造，线程退出时析构（析构里归还剩余块）
        static thread_local ThreadCache cache;
        return cache;
    }

    CoroutinePool::ThreadCache::~ThreadCache()
    {
        // 归还到全局池：不这么做的话这些块会滞留在已经结束的线程上再也拿不回来。
        // 此处仍可安全调用 instance()：单例是「函数内静态指针 + 堆对象」，它本身从不析构。
        // 按档分别归还——两档的块互不通用，混着还回去就是把大块的指针留给小档发放
        CoroutinePool &pool = instance();
        const std::lock_guard lock(pool.m_mutex);
        for (size_t tier = 0; tier < kTierCount; ++tier)
        {
            while (freeHeads[tier] != nullptr)
            {
                void *const block = freeHeads[tier];
                freeHeads[tier]   = *static_cast<void **>(block);
                pool.returnToGlobalUnlocked(tier, block);
            }
            freeCounts[tier] = 0;
        }
    }

    void *CoroutinePool::allocate(const size_t requiredSize)
    {
        // 超过最大档的请求交给全局堆：固定规格被大帧撑大会让后续所有分配跟着膨胀
        const size_t tier = tierForSize(requiredSize);
        if (tier >= kTierCount)
        {
            return ::operator new(requiredSize);
        }

        ThreadCache &cache = threadCache();
        // 本地缓存空才去碰全局池：稳态下（同一线程反复申请与释放）这一步不会发生
        if (cache.freeHeads[tier] == nullptr)
        {
            refillLocalCache(tier, cache);
        }

        // 池到达上限且本地缓存也没货时回退全局堆，保证 allocate() 永远能返回
        if (cache.freeHeads[tier] == nullptr)
        {
            return ::operator new(requiredSize);
        }

        // 侵入式出栈：空闲块的头 8 字节存的正是下一块地址，取出后这段空间交还给使用者
        void *const block     = cache.freeHeads[tier];
        cache.freeHeads[tier] = *static_cast<void **>(block);
        --cache.freeCounts[tier];
        return block;
    }

    void CoroutinePool::deallocate(void *const pointer, const size_t requiredSize) noexcept
    {
        if (!pointer)
        {
            return;
        }

        // 判定分支必须与 allocate() 完全一致，否则会把全局堆内存塞进空闲链表
        const size_t tier = tierForSize(requiredSize);
        if (tier >= kTierCount)
        {
            ::operator delete(pointer);
            return;
        }

        // 归属判定与线程无关：块落在哪个内存段只看地址，因此「A 线程分配、B 线程释放」
        // 同样进池——这正是跨线程归还不会漏块的原因
        if (!isOwnedBlock(pointer))
        {
            ::operator delete(pointer);
            return;
        }

        ThreadCache &cache = threadCache();
        // 缓存满了就把多余的还给全局池：本线程的滞留量因此有上限，
        // 避免一个线程囤积大量空闲块而其他线程无块可用
        if (cache.freeCounts[tier] >= kLocalCacheCapacity)
        {
            const std::lock_guard lock(m_mutex);
            returnToGlobalUnlocked(tier, pointer);
            return;
        }

        *static_cast<void **>(pointer) = cache.freeHeads[tier];
        cache.freeHeads[tier]          = pointer;
        ++cache.freeCounts[tier];
    }

    size_t CoroutinePool::tierForSize(const size_t requiredSize) const noexcept
    {
        // 小档之外一律进大档：档位判定在分配与回收两侧走同一份代码，因此不会串档。
        // 超过大档的极少数帧（更大的会话帧、未来 HTTP/2 类实现）返回 kTierCount 交给全局堆
        if (requiredSize <= m_blockSize)
        {
            return 0;
        }
        return requiredSize <= kLargeBlockSize ? 1 : kTierCount;
    }

    bool CoroutinePool::owns(const void *const pointer) const noexcept
    {
        return isOwnedBlock(pointer);
    }

    size_t CoroutinePool::blockSize() const noexcept
    {
        return m_blockSize;
    }

    size_t CoroutinePool::allocatedCount() const noexcept
    {
        return m_allocatedCount.load(std::memory_order_relaxed);
    }

    size_t CoroutinePool::expand(const size_t tier, const size_t count)
    {
        // 上限保护：达到 kMaximumTotalBlocks 后不再扩张，返回 0 让调用方改走全局堆
        const size_t allocatedBlocks = m_allocatedCount.load(std::memory_order_relaxed);
        if (allocatedBlocks >= kMaximumTotalBlocks)
        {
            return 0;
        }
        const size_t publishedChunks = m_chunkCount.load(std::memory_order_relaxed);
        if (publishedChunks >= kMaximumChunkCount)
        {
            // 段数组是定长的（无锁归属判定要求它不能被重新分配），段数用尽时同样停止扩张
            return 0;
        }
        const size_t newCount = std::min(count, kMaximumTotalBlocks - allocatedBlocks);

        // 段内块规格由档位决定，段描述里必须记下这一点——归属判定要按「本段自己的块大小」算边界，
        // 用全池某一个规格去乘会把前一段的区间越到后一段上
        const size_t chunkBlockSize = tier == 0 ? m_blockSize : kLargeBlockSize;

        // 默认对齐即可满足协程帧要求：x64 上 ::operator new 的默认对齐为 16 字节，
        // 与 delete 的自然配对，避免带 align_val_t 分配却用不带对齐参数的释放
        auto *const data = static_cast<std::byte *>(::operator new(chunkBlockSize * newCount));

        // 顺序要紧：先写段描述、再以 release 发布段数量，最后才把块放进空闲链表。
        // 反过来会让别的线程先拿到块、却在释放时判定「不属于本池」而交给 ::operator delete，
        // 把池内存还给通用堆 —— 那是堆损坏
        m_chunks[publishedChunks] = MemoryChunk{data, newCount, chunkBlockSize};
        m_chunkCount.store(publishedChunks + 1, std::memory_order_release);
        m_allocatedCount.store(allocatedBlocks + newCount, std::memory_order_relaxed);
        m_tierAllocatedCount[tier] += newCount;

        for (size_t blockIndex = 0; blockIndex < newCount; ++blockIndex)
        {
            returnToGlobalUnlocked(tier, data + blockIndex * chunkBlockSize);
        }
        return newCount;
    }

    void CoroutinePool::refillLocalCache(const size_t tier, ThreadCache &cache)
    {
        const std::lock_guard lock(m_mutex);

        // 该档的全局链表为空时先扩容：按本档已切分的块数翻倍，摊薄连续分配时的扩容次数。
        // 不能用两档合计的块数做基数——大档规格是小档的 8 倍，一个被小帧跑热过的池第一次
        // 碰到大帧就会一次要下十几 MiB，既是一记长缺页停顿也一口吃满共享的总块数预算
        if (m_globalFreeHeads[tier] == nullptr)
        {
            const size_t tierAllocatedBlocks = m_tierAllocatedCount[tier];
            expand(tier, tierAllocatedBlocks > 0 ? tierAllocatedBlocks : kDefaultInitialBlocks);
        }

        // 一次性搬一批：把取锁频率摊薄到 1/kLocalCacheCapacity，而不是每次分配都取锁
        while (cache.freeCounts[tier] < kLocalCacheCapacity && m_globalFreeHeads[tier] != nullptr)
        {
            void *const block           = m_globalFreeHeads[tier];
            m_globalFreeHeads[tier]     = *static_cast<void **>(block);
            --m_globalFreeCounts[tier];

            *static_cast<void **>(block) = cache.freeHeads[tier];
            cache.freeHeads[tier]        = block;
            ++cache.freeCounts[tier];
        }
    }

    void CoroutinePool::returnToGlobalUnlocked(const size_t tier, void *const block) noexcept
    {
        *static_cast<void **>(block) = m_globalFreeHeads[tier];
        m_globalFreeHeads[tier]      = block;
        ++m_globalFreeCounts[tier];
    }

    bool CoroutinePool::isOwnedBlock(const void *const pointer) const noexcept
    {
        const auto *const target = static_cast<const std::byte *>(pointer);
        // 段数量用 acquire 读取：读到 N 就保证前 N 个段描述已经写完并对本线程可见
        const size_t publishedChunks = m_chunkCount.load(std::memory_order_acquire);
        for (size_t chunkIndex = 0; chunkIndex < publishedChunks; ++chunkIndex)
        {
            const MemoryChunk &chunk = m_chunks[chunkIndex];
            // 逐段用该段自身的块规格与块数计算边界：档位不同，段的块大小也不同，
            // 拿全池统一的规格去算会让判定区间越界，错误认领相邻段的指针
            if (target >= chunk.data && target < chunk.data + chunk.blockCount * chunk.blockSize)
            {
                return true;
            }
        }
        return false;
    }

} // namespace AsynGyanis::Core
