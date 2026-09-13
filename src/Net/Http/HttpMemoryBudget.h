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

        /**
         * @brief 一次占用会话：构造时绑定预算，补预留用 growTo()，析构时自动归还
         *
         * @details 正文在内存里的存在期就是额度的计费区间，因此把「持有」与「归还」绑在一个对象上：
         *          请求结束、连接收口、异常展开都只需要让这个对象析构，不必在每个出口上手工释放。
         *          HTTP/1.1 上与会话一一对应；HTTP/2 上每条流一个（挂在待服务记录上），
         *          记录被摘掉即归还，多路复用下的总量自然等于各流之和。
         * @note 默认构造出一个「未绑定预算」的空会话，一切操作都是空操作；绑定用 reset()
         */
        class Reservation
        {
        public:
            /// 构造一个未绑定预算的空会话
            Reservation() noexcept = default;

            /**
             * @brief 构造并绑定一份预算
             * @param budget 共享预算，空指针表示不做限制
             */
            explicit Reservation(HttpMemoryBudget *budget) noexcept :
                m_budget(budget)
            {
            }

            /// 析构时归还全部已占额度
            ~Reservation()
            {
                releaseAll();
            }

            Reservation(const Reservation &) = delete;

            Reservation &operator=(const Reservation &) = delete;

            /**
             * @brief 移交占用（源对象交出额度，不再负责归还）
             * @details 待服务记录随容器移动时必须能跟着走：额度属于「哪条流的正文」，
             *          移动后由新的持有者归还；被移动走的对象变成空会话，析构时无事可做
             */
            Reservation(Reservation &&other) noexcept :
                m_budget(other.m_budget), m_reservedBytes(other.m_reservedBytes)
            {
                other.m_budget        = nullptr;
                other.m_reservedBytes = 0;
            }

            /// 移交占用：先归还本对象已占的额度，再接管源对象的
            Reservation &operator=(Reservation &&other) noexcept
            {
                if (this != &other)
                {
                    releaseAll();
                    m_budget              = other.m_budget;
                    m_reservedBytes       = other.m_reservedBytes;
                    other.m_budget        = nullptr;
                    other.m_reservedBytes = 0;
                }
                return *this;
            }

            /**
             * @brief 重新绑定预算（先归还旧账）
             * @param budget 新的共享预算，空指针表示此后不做限制
             */
            void reset(HttpMemoryBudget *budget) noexcept
            {
                releaseAll();
                m_budget = budget;
            }

            /**
             * @brief 把已占额度补到指定字节数
             * @param totalBytes 需要占用的总字节数（已含此前预留的部分）
             * @return true 已满足（含未绑定预算、无需增加两种情况）
             * @return false 增量超出全局剩余额度，本次调用未占用任何额度，调用方应当收口
             */
            [[nodiscard]] bool growTo(const std::size_t totalBytes) noexcept
            {
                if (m_budget == nullptr || totalBytes <= m_reservedBytes)
                {
                    return true;
                }
                if (!m_budget->tryReserve(totalBytes - m_reservedBytes))
                {
                    return false;
                }
                m_reservedBytes = totalBytes;
                return true;
            }

            /// 归还全部已占额度
            void releaseAll() noexcept
            {
                if (m_budget != nullptr && m_reservedBytes != 0)
                {
                    m_budget->release(m_reservedBytes);
                }
                m_reservedBytes = 0;
            }

        private:
            HttpMemoryBudget *m_budget{nullptr};  ///< 共享预算（非拥有），空指针表示未绑定
            std::size_t       m_reservedBytes{0}; ///< 当前已占的字节数
        };

    private:
        const std::size_t        m_maximumTotalBytes; ///< 上限字节数，0 表示不限制
        std::atomic<std::size_t> m_reservedBytes{0};  ///< 已预留总量，跨线程原子累加
    };
} // namespace AsynGyanis::Net
