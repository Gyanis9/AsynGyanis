/**
 * @file CoroutinePool.h
 * @brief 协程帧线程本地内存池，通过重载 operator new 接入 Task
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_COROUTINEPOOL_H
#define CORE_COROUTINEPOOL_H

#include <cstddef>
#include <vector>

namespace Core
{
    /**
     * @brief 协程帧线程本地内存池。
     *
     * 通过重载 Task::promise_type::operator new 自动使用。
     * 使用简单的空闲列表实现快速分配/回收。
     */
    class CoroutinePool
    {
    public:
        /**
         * @brief 构造内存池。
         * @param blockSize 每个内存块大小（字节），默认 256（典型协程帧大小）
         * @param initialBlocks 初始预分配块数
         */
        explicit CoroutinePool(size_t blockSize = 256, size_t initialBlocks = 64);

        ~CoroutinePool();

        CoroutinePool(const CoroutinePool &)            = delete;
        CoroutinePool &operator=(const CoroutinePool &) = delete;

        /**
         * @brief 分配一块内存。
         * @param n 请求字节数（如需大于 blockSize 则退化到全局 new）
         * @return void* 内存指针
         */
        void *allocate(size_t n);

        /**
         * @brief 回收一块内存。
         * @param p 内存指针
         * @param n 原始请求字节数
         */
        void deallocate(void *p, size_t n);

        /**
         * @brief 获取当前线程的 CoroutinePool 实例（thread_local 单例）。
         */
        static CoroutinePool &instance();

        [[nodiscard]] size_t blockSize() const noexcept;
        [[nodiscard]] size_t allocatedCount() const noexcept;

    private:
        void expand(size_t count);

        size_t                   m_blockSize;
        size_t                   m_allocatedCount{0};
        std::vector<void *>      m_freeList;
        std::vector<std::byte *> m_chunks; // 底层的连续内存块
    };

}

#endif
