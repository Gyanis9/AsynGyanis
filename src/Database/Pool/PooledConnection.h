/**
 * @file PooledConnection.h
 * @brief RAII 连接包装器 — 析构时自动归还连接池
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这是连接池向调用方交出连接的唯一接口形态。析构或 release() 时自动归还，移动后源对象
 *          为空、不会重复归还，重复 release() 也安全（由标志位防护）。
 *          上层（Queryable、事务）直接使用本类，因此接口在此冻结。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseConnection.h"
#include "Database/Pool/PoolLiveness.h"

#include <memory>

namespace AsynGyanis::Database
{

    class ConnectionPool;

    /**
     * @brief RAII 连接包装器
     *
     * @details 析构时自动归还连接池，手工调用 release() 可提前归还。公共路径（析构与 release）
     *          应尽可能短，归还后的过期检查与健康检查这类重活留给后台线程，不健康的连接被丢弃。
     */
    class ASYN_DATABASE_API PooledConnection
    {
    public:
        /**
         * @brief 构造一个空的 PooledConnection，不持有任何连接
         */
        PooledConnection() noexcept = default;

        /**
         * @brief 构造一个持有有效连接的 PooledConnection
         * @param connection 数据库连接的所有权
         * @param pool       归属的连接池指针，不可为空
         */
        explicit PooledConnection(std::unique_ptr<DatabaseConnection> connection, ConnectionPool *pool) noexcept;

        /**
         * @brief 析构函数，自动归还连接（若尚未 release）
         */
        ~PooledConnection();

        // 禁止拷贝：每个连接同时只能有一个 RAII 包装持有
        PooledConnection(const PooledConnection &) = delete;

        PooledConnection &operator=(const PooledConnection &) = delete;

        /**
         * @brief 移动构造函数
         * @param other 源对象；移动后源对象为空，析构不归还
         */
        PooledConnection(PooledConnection &&other) noexcept;

        /**
         * @brief 移动赋值运算符
         * @param other 源对象；移动后源对象为空，析构不归还
         * @return 当前对象的引用
         */
        PooledConnection &operator=(PooledConnection &&other) noexcept;

        /**
         * @brief 通过箭头运算符访问底层连接
         * @return DatabaseConnection* 底层连接指针；不持有时返回 nullptr
         */
        DatabaseConnection *operator->() const;

        /**
         * @brief 通过解引用运算符访问底层连接
         * @return DatabaseConnection& 底层连接引用
         * @warning 不持有时解引用导致未定义行为，调用前应检查 operator bool
         */
        DatabaseConnection &operator*() const;

        /**
         * @brief 检查是否持有有效连接
         * @return true 持有有效连接；false 为空
         */
        explicit operator bool() const noexcept;

        /**
         * @brief 提前归还连接至池
         *
         * @details 调用后连接被归还到 ConnectionPool，池会执行过期与健康检查。
         *          本对象被标记为空，后续析构不会重复归还。
         *          支持多次调用：第二次及以后的调用被忽略（double-release 防护）。
         */
        void release();

        /**
         * @brief 主动丢弃这条连接：关掉它、腾出借出名额，而不把它交回池复用
         *
         * @details 用在「这条连接的会话状态已经不可信」的场合：语句在驱动侧报错、事务读到一半失败、
         *          或业务自己判断该重开一条。此时 `release()` 会把这条连接原样放回池，下一个借用者
         *          拿到的就是同一份脏会话——而归还路径上的会话复位只对「池知道该复位的东西」有效，
         *          它不知道连接为什么被弄脏。丢弃没有这个风险：连接直接关掉。
         * @details 与 `release()` 一样是幂等的收口：调用后本对象为空，析构不会重复归还，也不会
         *          重复减出借出的名额（那个计数一旦多减，池就会长期超发连接）。
         * @note 丢弃的连接不做会话复位（没有下一个借用者要保护），因此不付那次额外的往返
         */
        void discard();

    private:
        /**
         * @brief 归还连接的内部实现
         * @details 由析构函数、release() 与 discard() 共用，确保只执行一次归还逻辑。
         *          归还后清空 m_connection 与 m_pool，防止重复归还。
         * @param isDiscard true = 丢弃这条连接而不是交回池复用
         */
        void doReturnToPool(const bool isDiscard = false);

        std::unique_ptr<DatabaseConnection> m_connection;     ///< 底层数据库连接的所有权
        ConnectionPool                     *m_pool = nullptr; ///< 归属的连接池，析构时据此归还

        std::shared_ptr<PoolLiveness>
                m_poolLiveness; ///< 池存活令牌（与池共享）：池已（或正在）析构时归还路径据此直接关闭连接；归还时会持令牌内的互斥量判活并调用池，因此与析构不会交错
    };

} // namespace AsynGyanis::Database
