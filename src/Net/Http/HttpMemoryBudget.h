/**
 * @file HttpMemoryBudget.h
 * @brief 服务进程级的在途正文字节预算：把「单条报文的上限」补成「全局的上限」
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <cstddef>

namespace AsynGyanis::Net
{
    /**
     * @brief 在途正文字节的全局预算，多线程共享一份
     *
     * @details 解决的是「单条报文的上限挡不住很多条连接」：`HttpParserLimits::maximumBodySize` 限的是
     *          **一条**请求的正文，N 条连接各压着一条大正文时总占用是它的 N 倍且与连接数同增。
     *          本类给出一条跨连接的账：会话在正文边收边攒的过程中按增量预留，请求结束后归还，
     *          预留失败即表示「此刻收下这条正文就会超预算」，由会话回 503 收口。
     * @note 预算按服务器实例的共享指针持有：同一条服务器上的所有连接共用一份账，因此
     *       （与按 IP 限额同理）不要给每个监听器各建一份，否则上限会乘以监听器数量。
     * @note 上限为 0 表示不限制（仍会记账，`reservedByteCount()` 可观测）。预留与归还都是原子操作，
     *       不阻塞、不加锁，可在事件循环线程上直接用。
     * @note 只约束**请求正文**这一类可预期增长的内存：每连接的接收窗口是固定的 8 KiB 且已被
     *       `TcpServer::setMaxConnections` 兜住，响应正文由业务自己决定，两者都不在本预算之内。
     * @see HttpServer::setMemoryBudget()
     */
    class HttpMemoryBudget
    {
    public:
        /**
         * @brief 构造预算
         * @param maximumTotalBytes 允许同时占用的字节上限，0 表示不限制（仍记账）
         */
        explicit HttpMemoryBudget(std::size_t maximumTotalBytes) noexcept;

        /**
         * @brief 尝试预留一段字节
         * @details 采用「读取当前值 → 比较 → CAS」的循环：竞争失败只重试而不阻塞，
         *          因此多个事件循环线程可以同时预留。
         * @param byteCount 本次要预留的字节数
         * @return true 预留成功，调用方随后必须原数归还
         * @return false 预留后超过上限，本次未占用任何额度
         */
        [[nodiscard]] bool tryReserve(std::size_t byteCount) noexcept;

        /**
         * @brief 归还先前预留的字节
         * @param byteCount 要归还的字节数，必须与成功预留的总量对应
         * @note 归还多于预留会让账目变小（不会阻塞、不会抛异常）：调用方按其预留总量归还即可，
         *       账目偏小只影响本预算的严格性，不影响内存安全
         */
        void release(std::size_t byteCount) noexcept;

        /**
         * @brief 取当前已预留的字节数
         * @return std::size_t 已预留总量
         */
        [[nodiscard]] std::size_t reservedByteCount() const noexcept;

        /**
         * @brief 取上限字节数
         * @return std::size_t 上限，0 表示不限制
         */
        [[nodiscard]] std::size_t maximumTotalBytes() const noexcept;

    private:
        const std::size_t        m_maximumTotalBytes; ///< 上限字节数，0 表示不限制
        std::atomic<std::size_t> m_reservedBytes{0};  ///< 已预留总量，跨线程原子累加
    };
} // namespace AsynGyanis::Net
