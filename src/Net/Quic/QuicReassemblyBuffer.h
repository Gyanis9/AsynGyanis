/**
 * @file QuicReassemblyBuffer.h
 * @brief 按偏移重组字节流的缓存（RFC 9000 §7.5）：重复与重叠的分片合并，只按序交付一次
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 对端换了分片大小再发（合法写法）时，只按「起始偏移」建索引的缓存会把一部分字节永远晾在
 *          外面：新段的起点落在早已缓存的那一段中间，排干时找不到恰好等于交付点的键就停住。本类把
 *          缓存按**互不重叠的覆盖区**维护，插入即合并，因此任何顺序、任何重叠程度的分片都能补齐。
 *
 * @note 交付点以下的字节直接丢弃，所以调用方不必自己先剪头部重叠。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 一段「按偏移可乱序到达」的字节流的重组状态
     * @warning 不是线程安全的：一个实例属于一条流（或一个包号空间的握手流），只在所属循环线程上驱动。
     */
    class QuicReassemblyBuffer
    {
    public:
        /**
         * @brief 交入一段字节，与已有缓存按覆盖区合并
         * @details 完全落在已交付前缀里的分片直接丢；与缓存重叠的分片拼接成一段，重复的字节以本次交入的为准
         *          （规范允许同一偏移的字节被重发，只要内容一致，§7.5）。
         * @param offset 本段在流上的起始偏移
         * @param bytes 本段字节，按「指针 + 长度」取
         */
        void insert(std::uint64_t offset, std::span<const std::uint8_t> bytes);

        /**
         * @brief 把从交付点起连续的字节接到输出末尾，交付点跟着后移
         * @param out 目标缓冲，二进制安全；本函数只追写，不预先清空
         * @return std::size_t 本次交付的字节数；0 表示下一个期望偏移还没到
         */
        std::size_t drain(std::vector<std::uint8_t> &out);

        /**
         * @brief 当前缓存的字节数（不含已交付的部分）
         * @return std::size_t 用于守住调用方自设的缓存上限
         */
        [[nodiscard]] std::size_t bufferedByteCount() const noexcept;

        /**
         * @brief 已交付的字节数，也就是下一个期望偏移
         * @return std::uint64_t 恒等于调用方眼里「已经排干到哪里」
         */
        [[nodiscard]] std::uint64_t deliveredOffset() const noexcept;

    private:
        std::map<std::uint64_t, std::vector<std::uint8_t>> m_fragments{};          ///< 覆盖区，按起始偏移索引且互不重叠、互不相接
        std::uint64_t                                      m_deliveredOffset{0};   ///< 已经交出去的字节数
        std::size_t                                        m_bufferedByteCount{0}; ///< 缓存字节数，随插入与排干增减
    };
} // namespace AsynGyanis::Net
