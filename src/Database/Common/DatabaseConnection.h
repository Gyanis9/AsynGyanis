/**
 * @file DatabaseConnection.h
 * @brief 数据库连接抽象基类
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库连接抽象基类
     *
     * @details 所有驱动（MySQL、Redis、SQLite）都继承本类，调用方只依赖本接口。
     *          生命周期：构造 → connect() → execute() → disconnect() → 析构。m_isConnected 是
     *          连接状态的唯一真值来源，派生类必须在 connect()/disconnect() 里同步维护它；
     *          isConnected() 必须是纯状态查询（各驱动均刻意不发网络探活），池在获取/归还热路径上调用它。
     */
    class DatabaseConnection
    {
    public:
        /**
         * @brief 默认构造函数
         */
        DatabaseConnection() = default;

        /**
         * @brief 虚析构函数，确保派生类经基类指针释放时正确析构
         */
        virtual ~DatabaseConnection() = default;

        DatabaseConnection(const DatabaseConnection &) = delete;

        DatabaseConnection &operator=(const DatabaseConnection &) = delete;

        DatabaseConnection(DatabaseConnection &&) = delete;

        DatabaseConnection &operator=(DatabaseConnection &&) = delete;

        /**
         * @brief 建立与数据库的连接
         * @return true 连接成功；false 失败，原因见 lastError()
         * @note 重复调用应当是幂等的：已连接时直接返回 true
         */
        virtual bool connect() = 0;

        /**
         * @brief 断开连接并释放底层句柄
         * @note 未连接时调用必须安全返回，不得崩溃
         */
        virtual void disconnect() = 0;

        /**
         * @brief 判断当前是否处于可用连接状态
         * @return true 已连接
         */
        [[nodiscard]] virtual bool isConnected() const = 0;

        /**
         * @brief 本连接最近一次建立成功的时刻
         * @details 连接池按它判「存活期是否已到」，因此这个时刻必须跟着连接本身走，而不是记在池
         *          的某张表里：连接被借出、归还、再借出的整段途中池拿不到一个稳定的载体。
         * @return std::chrono::steady_clock::time_point 由 markEstablishedAt() 定下的时刻；
         *         从未被定过则为构造时刻
         */
        [[nodiscard]] std::chrono::steady_clock::time_point establishedAt() const noexcept
        {
            return m_establishedAt;
        }

        /**
         * @brief 把「建立成功」的时刻记下来
         * @details 由连接池在 connect() 返回 true 之后调用一次。重连要刷新它，否则一条反复握手失败
         *          的连接会被按第一次成功的时刻判成过期。
         * @param moment 建立成功的时刻（通常紧挨着 connect() 返回时取一次 steady_clock）
         */
        void markEstablishedAt(const std::chrono::steady_clock::time_point moment) noexcept
        {
            m_establishedAt = moment;
        }

        /**
         * @brief 归还连接池时的会话状态复位（默认什么都不做）
         * @details 池在把连接放回空闲栈**或直接交给等待者**之前调用一次。会话级状态（未发送的
         *          管道缓冲、临时表、未提交的事务……）必须在这里清掉，否则会串给下一个借用者：
         *          Redis 的管道缓冲就是活例子——残留命令会被下一个借用者的 flushPipeline() 代发，
         *          回复按下标错位且毫无报错
         * @note 不得抛异常（池的归还路径不处理异常），实现必须是幂等的
         */
        virtual void resetSessionState() noexcept
        {
        }

        /**
         * @brief 执行数据库命令（SQL 语句或 Redis 命令）
         * @param command 命令文本
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] virtual std::unique_ptr<DatabaseResult> execute(std::string_view command) = 0;

        /**
         * @brief 执行带参数的数据库命令（参数按位置绑定）
         *
         * @details 取值以绑定方式送入数据库而不是拼进 SQL 文本，因此含单引号、"--"、分号的字符串
         *          只会被当作普通数据（见 SqlStatement.h）；parameters[i] 绑定到第 i 个占位符。
         *          默认实现把 lastError() 置为中文提示并返回 nullptr，刻意不退化调用不带参数的
         *          execute()：那样占位符会全部按 NULL 执行，是最难排查的一类静默错误。
         *
         * @param command    带占位符的命令文本
         * @param parameters 按占位符出现顺序排列的绑定参数
         * @return std::unique_ptr<DatabaseResult> 结果集；失败或驱动不支持时返回 nullptr，
         *         原因见 lastError()
         * @note 参数个数与占位符个数不一致时必须失败而不是按缺省值执行；
         *       实现方应拒绝无法安全绑定的参数类型（如容器类型）并给出中文错误
         */
        [[nodiscard]] virtual std::unique_ptr<DatabaseResult> execute(const std::string_view command, const std::span<const DatabaseValue> parameters)
        {
            // 两个入参都不使用：本实现只负责给出明确的中文错误，不执行任何命令
            static_cast<void>(command);
            static_cast<void>(parameters);

            m_lastError = std::string("该驱动暂不支持参数化查询（") + databaseTypeName(databaseType()) + "）：请改用不带参数的 execute()，或为该驱动实现参数绑定";
            return nullptr;
        }

        /**
         * @brief 获取数据库类型
         * @return DatabaseType 具体驱动对应的类型枚举
         */
        [[nodiscard]] virtual DatabaseType databaseType() const = 0;

        /**
         * @brief 获取最后一次错误信息
         * @return std::string 错误描述，无错误时为空串
         */
        [[nodiscard]] virtual std::string lastError() const
        {
            return m_lastError;
        }

        /**
         * @brief 获取连接配置的只读引用
         * @return const ConnectionConfig& 当前配置
         */
        [[nodiscard]] const ConnectionConfig &configuration() const noexcept
        {
            return m_configuration;
        }

        /**
         * @brief 设置连接超时时间
         * @param milliseconds 超时毫秒数，必须在 connect() 之前设置才会生效
         * @note 派生类若在 connect() 内读取该值配置底层句柄，重连时需要再次调用
         */
        void setConnectTimeout(const int milliseconds) noexcept
        {
            m_connectTimeout = milliseconds;
        }

        /**
         * @brief 设置单条命令的执行超时时间
         * @param milliseconds 超时毫秒数，0 与负数一律按「不设超时」处理
         * @details 写入后立即调用 applyQueryTimeoutNow()，因此改值不必重连：SQLite 在每条语句入口现读
         *          这个值，Redis 当场改已建立上下文的收发超时。MySQL 客户端库只在握手前读一次该选项，
         *          它的实现是空操作，改完仍需重连（见 MySqlConnection 的 @warning）。
         * @note 同一线程借用期间调用：连接对象不是线程安全的，本方法会与命令执行争用同一个底层句柄
         */
        void setQueryTimeout(const int milliseconds) noexcept
        {
            m_queryTimeout = milliseconds;
            applyQueryTimeoutNow();
        }

        /**
         * @brief 读取当前连接超时设置
         * @return int 超时毫秒数
         */
        [[nodiscard]] int connectTimeout() const noexcept
        {
            return m_connectTimeout;
        }

        /**
         * @brief 读取当前命令执行超时设置
         * @return int 超时毫秒数
         */
        [[nodiscard]] int queryTimeout() const noexcept
        {
            return m_queryTimeout;
        }

    protected:
        /**
         * @brief 把当前的 queryTimeout() 落到已建立的底层句柄上
         * @details 默认空实现：没有「存续期间可改」这一能力的驱动不必重写。重写它即表示本驱动能在连接
         *          已建立时改超时，调用方因此不必先断开再重连。
         * @note 未连接时必须是空操作——connect() 会按最新值配置句柄
         */
        virtual void applyQueryTimeoutNow() noexcept
        {
        }

        ConnectionConfig m_configuration;          ///< 连接配置
        std::string      m_lastError;              ///< 最后一次错误信息
        int              m_connectTimeout = 5000;  ///< 连接超时毫秒数
        int              m_queryTimeout   = 30000; ///< 单条命令执行超时毫秒数
        bool             m_isConnected    = false; ///< 连接状态，由派生类同步维护
        std::chrono::steady_clock::time_point m_establishedAt = std::chrono::steady_clock::now(); ///< 最近一次建立成功的时刻，池在 connect() 成功后改写
    };

} // namespace AsynGyanis::Database
