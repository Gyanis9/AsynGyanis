/**
 * @file CoroutinePool.h
 * @brief 协程帧内存池（进程级单例），通过重载 operator new 接入 Task
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace AsynGyanis::Core
{
    /**
     * @brief 协程帧内存池。
     *
     * 以固定大小块加空闲链表的方式为协程帧提供 O(1) 的分配与回收，
     * 请求大小不超过 blockSize 时不会走到通用堆分配器。
     *
     * @details 池是进程级单例而非线程局部：协程帧会在 EventLoop 线程之间迁移（scheduleRemote
     *          投递、跨线程析构），按线程拆池会让归还方把块交给全局
     *          ::operator delete，形成无效释放并破坏堆。稳态下分配与归还只操作本线程的缓存链表，
     *          不取锁、不做原子操作；块归属按「内存段 + 段内偏移」判定，与分配线程无关。
     *
     * @note Task::promise_type 的 operator new/delete 依赖本池，进程内所有协程帧共用它。
     */
    class CoroutinePool
    {
    public:
        CoroutinePool(const CoroutinePool &) = delete;

        CoroutinePool &operator=(const CoroutinePool &) = delete;

        CoroutinePool(CoroutinePool &&) = delete;

        CoroutinePool &operator=(CoroutinePool &&) = delete;

        /**
         * @brief 获取进程级内存池单例
         * @details 单例在首次调用时构造，并刻意在进程退出时不析构：
         *          协程帧的销毁可能晚于任何函数局部静态对象，回收方必须始终能看到本池。
         * @return CoroutinePool& 全局唯一的内存池引用
         */
        static CoroutinePool &instance();

        /**
         * @brief 从池中分配一块内存。
         * @details 优先取本线程缓存（无锁）；缓存空时从全局池批量搬运；池达到
         *          kMaximumTotalBlocks 上限后不再扩容，此后的请求落到全局 ::operator new——
         *          因此 allocate() 在任何情况下都能成功返回，不存在「池满了就取不到块」的状态。
         * @param requiredSize 请求的字节数
         * @return void* 指向分配内存的指针
         * @throws std::bad_alloc 底层内存分配失败
         */
        [[nodiscard]] void *allocate(size_t requiredSize);

        /**
         * @brief 回收先前分配的内存。
         * @details 分支条件必须与 allocate() 保持一致：按大小判定该块来自池还是全局堆，
         *          来自池则压回**当前线程**的空闲链表（无锁，这也是跨线程归还之所以成立的原因——
         *          块归哪个线程的缓存与其分配者无关，只要还在池里就不会丢），
         *          否则交还全局 ::operator delete。
         * @param pointer 待回收的内存指针，允许为 nullptr
         * @param requiredSize 分配时请求的字节数
         * @note 传入不属于本池的指针时同样按大小回退到全局堆释放，不会写坏空闲链表。
         */
        void deallocate(void *pointer, size_t requiredSize) noexcept;

        /**
         * @brief 判断指针是否落在本池已切分的块范围内。
         * @param pointer 待检查的指针
         * @return true 指针属于本池管理的某个内存段
         * @note 无锁实现：内存段的描述在写入后以 release 语义发布段数量，读者据此判定可见性。
         */
        [[nodiscard]] bool owns(const void *pointer) const noexcept;

        /**
         * @brief 获取池中每个内存块的大小（分配单元）。
         * @return size_t 块大小（字节）
         */
        [[nodiscard]] size_t blockSize() const noexcept;

        /**
         * @brief 获取池已切分的块总数（含空闲与在用）。
         * @return size_t 块总数
         * @note 这是「池向系统申请过的块数」，不含滞留在各线程缓存里的部分——它们仍属于池。
         */
        [[nodiscard]] size_t allocatedCount() const noexcept;

    private:
        // 规格分档：协程帧大小分布很宽（框架里实测从几十字节到 2 KB 以上），单一规格必然二选一地亏——
        // 按小的定，大帧每次都要向全局分配器要内存；按大的定，小帧要占着大块、活跃连接一多就是成倍常驻内存。
        // 两档把两侧都盖住，仍超过大档的极少数帧才回退全局堆
        static constexpr size_t kDefaultBlockSize     = 256;   ///< 默认（小档）块大小：几十到两百字节的小帧用这一档，不浪费
        static constexpr size_t kLargeBlockSize       = 2048;  ///< 大档块大小：框架里路由、会话这类帧实测 1.2–2.2 KB，小档装不下，落到全局堆就是每请求一次分配器调用
        static constexpr size_t kTierCount            = 2;      ///< 规格档数：小档与大档各有自己的空闲链表、每线程缓存与内存段
        static constexpr size_t kDefaultInitialBlocks = 128;   ///< 首次扩容的块数
        static constexpr size_t kMaximumTotalBlocks   = 16384; ///< 块数上限（两档合计），小块规格下约 4MB
        static constexpr size_t kLocalCacheCapacity   = 64;    ///< 每线程每档缓存上限
        static constexpr size_t kMaximumChunkCount    = 64;    ///< 段数上限（两档合计；倍增扩容下 16384 块只需约 8 段）

        /**
         * @brief 构造内存池并预分配若干块
         * @param blockSize 小档块大小（字节），大档固定取 kLargeBlockSize
         * @param initialBlocks 启动时一次性切分的块数
         */
        explicit CoroutinePool(size_t blockSize, size_t initialBlocks);

        /**
         * @brief 析构函数，释放所有内存段
         * @details 单例被刻意泄漏到进程结束，正常路径下不会执行；保留它是为了配对释放逻辑。
         */
        ~CoroutinePool();

        /**
         * @brief 一段连续的申请内存，被切分为 blockCount 个同规格的块
         */
        struct MemoryChunk
        {
            std::byte *data       = nullptr; ///< 内存段首地址
            size_t     blockCount = 0;       ///< 本段切分出的块数
            size_t     blockSize  = 0;       ///< 本段每块的大小：归属判定按段自己的规格算边界，档位不同则规格不同
        };

        /**
         * @brief 线程本地空闲块缓存，按规格档各有一条链
         * @details 块用侵入式单链表串起来（空闲块的头部 8 字节存下一块地址），
         *          因此稳态下分配与归还只改几个指针，不取锁、不做原子操作。
         * @note 两档必须分开成链：混在一条链上就会出现「大块的指针按小档发放」，
         *       调用方按小档写入会立刻越界——这不是浪费，是内存踩踏。
         */
        struct ThreadCache
        {
            std::array<void *, kTierCount>   freeHeads{};  ///< 各档的空闲链表头（空闲块首字节存 next）
            std::array<size_t, kTierCount>   freeCounts{}; ///< 各档链表长度

            /**
             * @brief 线程退出时把缓存里剩余的块按档全部归还全局池
             * @details 不归还的话，这些块会永久滞留在已经结束的线程上，本池又从不缩小，
             *          等于被泄漏；归还只需要一次取锁。
             */
            ~ThreadCache();
        };

        /**
         * @brief 取得本线程的缓存对象（函数内 thread_local，首次调用时构造）
         * @return ThreadCache& 本线程专属的缓存引用
         */
        [[nodiscard]] static ThreadCache &threadCache() noexcept;

        /**
         * @brief 判断请求大小落在哪一档
         * @param requiredSize 请求的字节数
         * @return size_t 档位下标；超过最大档（kLargeBlockSize）时返回 kTierCount
         */
        [[nodiscard]] size_t tierForSize(size_t requiredSize) const noexcept;

        /**
         * @brief 向池中追加若干块，并把它们放入指定档的全局空闲链表
         * @param tier 目标档位
         * @param count 期望新增的块数，超出上限时按剩余容量截断
         * @return size_t 实际追加的块数；已到 kMaximumTotalBlocks 上限或段数用尽时为 0
         * @note 调用方必须持有 m_mutex；返回 0 时调用方应改从全局堆分配
         */
        size_t expand(size_t tier, size_t count);

        /**
         * @brief 从全局池批量搬运若干块填充本线程缓存的指定档，内部取锁
         * @param tier 目标档位
         * @param cache 目标线程缓存
         */
        void refillLocalCache(size_t tier, ThreadCache &cache);

        /**
         * @brief 把一块放回指定档的全局空闲链表，要求调用方已持有 m_mutex
         * @param tier 目标档位
         * @param block 待归还的池内块
         */
        void returnToGlobalUnlocked(size_t tier, void *block) noexcept;

        /**
         * @brief 判断指针是否属于本池，无锁
         * @param pointer 待检查的指针
         * @return true 属于本池
         */
        [[nodiscard]] bool isOwnedBlock(const void *pointer) const noexcept;

        size_t                          m_blockSize;               ///< 小档块大小（构造参数；大档固定 kLargeBlockSize）
        std::array<MemoryChunk, kMaximumChunkCount> m_chunks{};    ///< 内存段描述：先写描述、再发布段数量，故可无锁读
        std::atomic<size_t>             m_chunkCount{0};           ///< 已发布的内存段数量（release 发布，acquire 读取）
        std::atomic<size_t>             m_allocatedCount{0};       ///< 已切分的块总数（两档合计，跨线程可读）
        std::array<void *, kTierCount>  m_globalFreeHeads{};       ///< 各档全局空闲链表头（仅持锁访问）
        std::array<size_t, kTierCount>  m_globalFreeCounts{};      ///< 各档全局空闲链表长度
        mutable std::mutex              m_mutex;                   ///< 只保护全局空闲链表与扩容
    };

} // namespace AsynGyanis::Core
