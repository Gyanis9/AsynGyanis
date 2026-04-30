/**
 * @file BufferPool.h
 * @brief 固定大小字节缓冲池
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_BUFFERPOOL_H
#define CORE_BUFFERPOOL_H

#include <cstddef>
#include <vector>

namespace Core
{
    /**
     * @brief 固定大小字节缓冲池。
     *
     * 预分配一块连续内存，划分为固定大小的多个缓冲区。提供获取/释放缓冲区的接口，
     * 索引管理。适用于高频分配/释放固定大小内存块的场景，避免频繁系统调用。
     */
    class BufferPool
    {
    public:
        /**
         * @brief 构造缓冲池。
         * @param bufferSize 每个缓冲区的大小（字节数）
         * @param bufferCount 缓冲区个数
         */
        BufferPool(size_t bufferSize, size_t bufferCount);

        /**
         * @brief 析构函数，释放内部内存。
         */
        ~BufferPool();

        BufferPool(const BufferPool &)            = delete;
        BufferPool &operator=(const BufferPool &) = delete;
        BufferPool(BufferPool &&)                 = delete;
        BufferPool &operator=(BufferPool &&)      = delete;

        /**
         * @brief 获取一个空闲缓冲区。
         * @return 缓冲区索引（从0开始），若没有空闲缓冲区则返回 -1
         */
        int acquire();

        /**
         * @brief 释放指定索引的缓冲区。
         * @param index 要释放的缓冲区索引（必须有效且处于占用状态）
         */
        void release(int index);

        /**
         * @brief 获取指定索引缓冲区的起始地址。
         * @param index 缓冲区索引
         * @return 缓冲区首字节指针，若索引无效则返回 nullptr
         */
        [[nodiscard]] void *data(int index);

        /**
         * @brief 获取每个缓冲区的大小。
         * @return 缓冲区大小（字节数）
         */
        [[nodiscard]] size_t bufferSize() const noexcept;

        /**
         * @brief 获取缓冲区总个数。
         * @return 缓冲区个数
         */
        [[nodiscard]] size_t bufferCount() const noexcept;

    private:
        size_t                 m_bufferSize;  ///< 每个缓冲区大小（字节）
        size_t                 m_bufferCount; ///< 缓冲区个数
        std::vector<std::byte> m_memory;      ///< 连续内存块，总大小为 bufferSize * bufferCount
        std::vector<int>       m_freeList;    ///< 空闲缓冲区索引栈，O(1) 获取
        size_t                 m_freeTop{0};  ///< 空闲栈顶位置
    };

}

#endif
