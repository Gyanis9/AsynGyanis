/**
 * @file CoroutinePool.h
 * @brief 协程帧内存池（进程级单例），通过重载 operator new 接入 Task
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 协程帧内存池。
     *
     * 以固定大小块加空闲列表的方式为协程帧提供 O(1) 的分配与回收，
     * 请求大小不超过 blockSize 时不会走到通用堆分配器。
     *
     * @details 池是进程级单例而不是线程局部实例：协程帧会在 EventLoop 线程之间迁移
     *          （scheduleRemote 投递、stealFrom 窃取、Task 在别的线程析构都会换手），
     *          按线程拆池会让归还方把自己的块交给全局 ::operator delete，
     *          形成无效释放并破坏堆。跨线程访问由内部互斥锁保护。
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
         * @details 请求大小不超过 blockSize 时从空闲列表取块（列表空则先扩容）；
         *          超过 blockSize 的大请求直接交给全局 ::operator new，避免撑大固定块规格。
         *          池达到 kMaximumTotalBlocks 上限后不再扩容，此后的请求同样落到全局堆——
         *          因此 allocate() 在任何情况下都能成功返回，不存在「池满了就取不到块」的状态。
         * @param requiredSize 请求的字节数
         * @return void* 指向分配内存的指针
         * @throws std::bad_alloc 底层内存分配失败
         */
        [[nodiscard]] void *allocate(size_t requiredSize);

        /**
         * @brief 回收先前分配的内存。
         * @details 分支条件必须与 allocate() 保持一致：按大小判定该块来自池还是全局堆，
         *          来自池则压回空闲列表，否则交还全局 ::operator delete。
         * @param pointer 待回收的内存指针，允许为 nullptr
         * @param requiredSize 分配时请求的字节数
         * @note 传入不属于本池的指针时同样按大小回退到全局堆释放，不会写坏空闲列表。
         */
        void deallocate(void *pointer, size_t requiredSize) noexcept;

        /**
         * @brief 判断指针是否落在本池已切分的块范围内。
         * @param pointer 待检查的指针
         * @return true 指针属于本池管理的某个内存段
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
         */
        [[nodiscard]] size_t allocatedCount() const noexcept;

    private:
        /**
         * @brief 构造内存池并预分配若干块
         * @param blockSize 每个内存块的大小（字节）
         * @param initialBlocks 启动时一次性切分的块数
         */
        explicit CoroutinePool(size_t blockSize, size_t initialBlocks);

        /**
         * @brief 析构函数，释放所有内存段
         * @details 单例被刻意泄漏到进程结束，正常路径下不会执行；保留它是为了配对释放逻辑。
         */
        ~CoroutinePool();

        /**
         * @brief 向池中追加若干块
         * @param count 期望新增的块数，超出上限时按剩余容量截断
         * @return size_t 实际追加的块数；已到 kMaximumTotalBlocks 上限时为 0
         * @note 调用方必须持有 m_mutex；返回 0 时调用方应改从全局堆分配
         */
        size_t expand(size_t count);

        /**
         * @brief 判断指针是否属于本池，要求调用方已持有 m_mutex
         * @param pointer 待检查的指针
         * @return true 属于本池
         */
        [[nodiscard]] bool ownsUnlocked(const void *pointer) const noexcept;

        /// 一段连续的申请内存，被切分为 blockCount 个 blockSize 大小的块
        struct MemoryChunk
        {
            std::byte *data       = nullptr; ///< 内存段首地址
            size_t     blockCount = 0;       ///< 本段切分出的块数
        };

        static constexpr size_t kDefaultBlockSize     = 256; ///< 默认块大小，覆盖典型协程帧
        static constexpr size_t kDefaultInitialBlocks = 128; ///< 首次扩容的块数
        static constexpr size_t kMaximumTotalBlocks   = 16384; ///< 块数上限，256B × 16384 = 4MB

        size_t            m_blockSize;          ///< 每个固定块的大小（字节）
        size_t            m_allocatedCount = 0; ///< 已切分的块总数
        std::vector<void *> m_freeList;         ///< 空闲块指针列表，栈式取用
        std::vector<MemoryChunk> m_chunks;      ///< 底层内存段，进程内不归还直到池析构
        mutable std::mutex m_mutex;             ///< 保护空闲列表与内存段，协程可跨线程换手
    };

} // namespace AsynGyanis::Core
