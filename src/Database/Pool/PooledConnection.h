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

#include <chrono>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>

namespace AsynGyanis::Database
{

    class ConnectionPool;

    /**
     * @brief 空手借出失败的原因
     * @details 四种等待出口的处置不同：超时可退避重试，停摆要停止请求，队列满要扩容或降级，
     *          分配失败要释放内存后再来。全部折成一个空连接时调用方只能猜。
     */
    enum class PoolAcquireFailure
    {
        None,             ///< 没有失败：持有连接，或尚未发生一次借出
        TimedOut,         ///< 等到截止时刻仍没有可用连接
        ShuttingDown,     ///< 池已停摆，等待者被就地唤醒
        WaitingQueueFull, ///< 等待名额已达上限，本次没有入表
        AllocationFailed, ///< 入表或恢复票据的分配失败
        Cancelled         ///< 调用方在等待期间请求停止
    };

    /**
     * @brief RAII 连接包装器
     *
     * @details 析构时自动归还连接池，手工调用 release() 可提前归还。公共路径（析构与 release）
     *          只做会话复位与状态判定；断开与周期驱逐留给锁外与后台线程，不健康的连接被丢弃。
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
         * @brief 空手借出的原因
         * @return PoolAcquireFailure 持有连接或尚未失败时为 None；空手时给出具体出口
         */
        [[nodiscard]] PoolAcquireFailure failureReason() const noexcept;

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

        /**
         * @brief 在本连接上执行一条语句，并把连接租给结果集
         * @details 结果集存活期间连接不会归还池：SQLite 超过物化上限的游标档、MySQL 的流式档都
         *          直接引用驱动句柄，连接被复用或关闭后再 next()/getValue() 读的就是已释放资源。
         *          本方法因此消耗包装本身：想继续用这条连接执行别的语句，就走 operator->() 的
         *          既有路径，并自行保证结果集先于连接销毁。
         * @param command SQL 语句或命令文本
         * @param stopToken 协作取消令牌；进入时已请求停止则不执行，执行期间或结果集存活期间
         *        请求停止都由驱动中断当前语句（含游标式结果集的后续 next()）
         * @return std::unique_ptr<DatabaseResult> 结果集；执行失败时返回空值（原因见连接的
         *         lastError()），此时连接仍由调用方持有的包装管着。因取消而失败时包装已被清空
         *         （连接丢弃），调用方需重新借出
         * @note 只有右值可以调用：租出连接是语义的一部分，调用点必须写出 std::move
         * @note 取消由驱动自己的 DatabaseConnection::requestQueryCancel() 兑现：驱动不支持时语句会自然
         *       跑完，令牌停止后仍拿到空值，但写副作用可能已经发生
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command, std::stop_token stopToken = std::stop_token{}) &&;

        /**
         * @brief 在本连接上执行带参数的语句，并把连接租给结果集
         * @details 语义与不带参数的重载相同：参数按位置绑定，失败时返回空值且连接不离开包装。
         * @param command 带占位符的语句文本
         * @param parameters 按占位符出现顺序排列的绑定参数
         * @param stopToken 协作取消令牌；语义与不带参数的重载相同
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回空值
         * @note 只有右值可以调用
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command, std::span<const DatabaseValue> parameters,
                                                              std::stop_token stopToken = std::stop_token{}) &&;

        /**
         * @brief 在本连接上执行流式查询，并把连接租给结果集
         * @details 流式档最需要这条租约：行留在服务端，结果集每推进一步都要走同一条连接。
         *          驱动未实现流式取数时按 DatabaseConnection::executeStreaming 的契约返回空值。
         * @param command 查询文本
         * @param stopToken 协作取消令牌；语义与 execute() 相同
         * @return std::unique_ptr<DatabaseResult> 流式结果集；失败或驱动不支持时返回空值
         * @note 只有右值可以调用
         * @note 令牌在结果集存活期间保持生效：驱动支持中断时，后续 next() 期间请求停止会打断
         *       当前步进；被取消过的连接在结果集析构时按「会话不可信」丢弃
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> executeStreaming(std::string_view command, std::stop_token stopToken = std::stop_token{}) &&;

        /**
         * @brief 在本连接上执行带参数的流式查询，并把连接租给结果集
         * @param command 带占位符的查询文本
         * @param parameters 按占位符出现顺序排列的绑定参数
         * @param stopToken 协作取消令牌；语义与 execute() 相同
         * @return std::unique_ptr<DatabaseResult> 流式结果集；失败或驱动不支持时返回空值
         * @note 只有右值可以调用
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> executeStreaming(std::string_view command, std::span<const DatabaseValue> parameters,
                                                                       std::stop_token stopToken = std::stop_token{}) &&;

        /**
         * @brief 拿到这条连接的时刻（steady 时钟）
         * @return 借出时刻；空包装返回 clock 的纪元值，不可当作真实借出时刻
         * @note 用途是把「超时该报哪一条」这类排查从猜变成读数：配合 heldDuration() 就能在
         *       业务日志里写明这条连接被持有了多久，而不必另拿一个计时器自己记
         */
        [[nodiscard]] std::chrono::steady_clock::time_point borrowedAt() const noexcept;

        /**
         * @brief 这条连接已被本包装持有了多久
         * @return 持有期；空包装返回 0
         * @note 同一连接被归还后再借出会重新起算（持有期属于「这次借出」而不是「这条连接」）
         */
        [[nodiscard]] std::chrono::milliseconds heldDuration() const noexcept;

    private:
        /**
         * @brief 构造一个带失败原因的空包装
         * @param failure 空手原因
         */
        explicit PooledConnection(PoolAcquireFailure failure) noexcept;

        /**
         * @brief 归还连接的内部实现
         * @details 由析构函数、release() 与 discard() 共用，确保只执行一次归还逻辑。
         *          归还后清空 m_connection 与 m_pool，防止重复归还。
         * @param isDiscard true = 丢弃这条连接而不是交回池复用
         */
        void doReturnToPool(const bool isDiscard = false);

        friend class ConnectionPool;

        std::unique_ptr<DatabaseConnection> m_connection;     ///< 底层数据库连接的所有权
        ConnectionPool                     *m_pool = nullptr; ///< 归属的连接池，析构时据此归还

        std::shared_ptr<PoolLiveness>
                m_poolLiveness; ///< 池存活令牌（与池共享）：池已（或正在）析构时归还路径据此直接关闭连接；归还时会持令牌内的互斥量判活并调用池，因此与析构不会交错

        std::size_t                           m_borrowSlot{PoolLiveness::kNoBorrowSlot}; ///< 借出账本槽位（登记在令牌上，见 PoolLiveness::borrowStartNanos）；空包装/未登记时是哨兵
        std::chrono::steady_clock::time_point m_borrowedAt{};                            ///< 本次借出的起始时刻（steady）；空包装是纪元值
        PoolAcquireFailure                    m_failure{PoolAcquireFailure::None};       ///< 空手借出的原因；持有连接时恒为 None
    };

} // namespace AsynGyanis::Database
