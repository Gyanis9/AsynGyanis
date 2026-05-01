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
     *
     * 每个线程拥有独立的内存池实例（通过 instance() 获取线程局部单例），
     * 分配时若请求大小不超过 blockSize，则从池中分配；否则退化为全局 ::operator new。
     */
    class CoroutinePool
    {
    public:
        /**
         * @brief 构造内存池。
         * @param blockSize 每个内存块大小（字节），默认 256（典型协程帧大小）
         * @param initialBlocks 初始预分配块数，池子启动时一次性分配这些块
         */
        explicit CoroutinePool(size_t blockSize = 256, size_t initialBlocks = 64);

        /**
         * @brief 析构函数，释放所有预分配的内存块。
         */
        ~CoroutinePool();

        CoroutinePool(const CoroutinePool &)            = delete;
        CoroutinePool &operator=(const CoroutinePool &) = delete;

        /**
         * @brief 从池中分配一块内存。
         *
         * 如果请求大小 n 不超过 m_blockSize，则从空闲列表中取出一块返回；
         * 否则调用全局 ::operator new(n) 分配。
         *
         * @param n 请求的字节数
         * @return 指向分配内存的指针，若失败（空闲列表为空且无法扩展）则返回 nullptr
         */
        void *allocate(size_t n);

        /**
         * @brief 回收先前分配的内存。
         *
         * 如果原始请求大小 n 不超过 m_blockSize，则将内存块放回空闲列表；
         * 否则调用全局 ::operator delete(p) 释放。
         *
         * @param p 要释放的内存指针
         * @param n 原始分配时的请求大小
         */
        void deallocate(void *p, size_t n);

        /**
         * @brief 检查指针 p 是否属于当前池管理的内存块。
         *
         * 用于跨线程安全：若 Task 在非本线程析构，通过此检查可回退到全局 delete。
         *
         * @param p 待检查的指针
         * @return true 指针属于本池，可安全回收
         */
        [[nodiscard]] bool owns(const void *p) const noexcept;

        /**
         * @brief 获取当前线程的 CoroutinePool 实例（thread_local 单例）。
         * @return 当前线程的内存池引用
         */
        static CoroutinePool &instance();

        /**
         * @brief 获取池中每个内存块的大小（分配单元）。
         * @return 块大小（字节）
         */
        [[nodiscard]] size_t blockSize() const noexcept;

        /**
         * @brief 获取当前已分配的块数量（包括空闲和已占用的块）。
         * @return 已分配的块总数
         */
        [[nodiscard]] size_t allocatedCount() const noexcept;

    private:
        /**
         * @brief 扩展内存池，新增 count 个内存块。
         * @param count 需要增加的块数量
         */
        void expand(size_t count);

        size_t                   m_blockSize;      ///< 每个固定块的大小（字节）
        size_t                   m_allocatedCount; ///< 当前已分配（预分配）的块总数
        std::vector<void *>      m_freeList;       ///< 空闲块指针列表（栈式）
        std::vector<std::byte *> m_chunks;         ///< 底层连续内存块（每块内存由多个 blockSize 单元组成）
    };

}

#endif
