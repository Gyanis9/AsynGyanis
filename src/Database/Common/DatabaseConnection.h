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

#include <memory>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库连接抽象基类
     *
     * @details 定义连接的生命周期与操作接口，所有驱动（MySQL、Redis、SQLite）
     *          都继承本类。调用方只依赖本接口，不需要知道底层引擎。
     *
     * 生命周期：构造 → connect() → execute() → disconnect() → 析构。
     *          m_isConnected 是连接状态的唯一真值来源，派生类必须在
     *          connect()/disconnect() 中同步维护它；isConnected() 可以在此基础上
     *          再叠加一次活性探测（如 mysql_ping）。
     *
     * @code
     *   auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault());
     *   if (connection->connect())
     *   {
     *       auto result = connection->execute("SELECT 1");
     *   }
     * @endcode
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

        DatabaseConnection(const DatabaseConnection &)            = delete;
        DatabaseConnection &operator=(const DatabaseConnection &) = delete;
        DatabaseConnection(DatabaseConnection &&)                 = delete;
        DatabaseConnection &operator=(DatabaseConnection &&)      = delete;

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
         * @brief 执行数据库命令（SQL 语句或 Redis 命令）
         * @param command 命令文本
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] virtual std::unique_ptr<DatabaseResult> execute(std::string_view command) = 0;

        /**
         * @brief 获取数据库类型
         * @return DatabaseType 具体驱动对应的类型枚举
         */
        [[nodiscard]] virtual DatabaseType databaseType() const = 0;

        /**
         * @brief 获取最后一次错误信息
         * @return std::string 错误描述，无错误时为空串
         */
        [[nodiscard]] virtual std::string lastError() const { return m_lastError; }

        /**
         * @brief 获取连接配置的只读引用
         * @return const ConnectionConfig& 当前配置
         */
        [[nodiscard]] const ConnectionConfig &configuration() const noexcept { return m_configuration; }

        /**
         * @brief 设置连接超时时间
         * @param milliseconds 超时毫秒数，必须在 connect() 之前设置才会生效
         * @note 派生类若在 connect() 内读取该值配置底层句柄，重连时需要再次调用
         */
        void setConnectTimeout(int milliseconds) noexcept { m_connectTimeout = milliseconds; }

        /**
         * @brief 设置单条命令的执行超时时间
         * @param milliseconds 超时毫秒数
         */
        void setQueryTimeout(int milliseconds) noexcept { m_queryTimeout = milliseconds; }

        /**
         * @brief 读取当前连接超时设置
         * @return int 超时毫秒数
         */
        [[nodiscard]] int connectTimeout() const noexcept { return m_connectTimeout; }

        /**
         * @brief 读取当前命令执行超时设置
         * @return int 超时毫秒数
         */
        [[nodiscard]] int queryTimeout() const noexcept { return m_queryTimeout; }

    protected:
        ConnectionConfig m_configuration; ///< 连接配置
        std::string      m_lastError;     ///< 最后一次错误信息
        int              m_connectTimeout = 5000;  ///< 连接超时毫秒数
        int              m_queryTimeout   = 30000; ///< 单条命令执行超时毫秒数
        bool             m_isConnected    = false; ///< 连接状态，由派生类同步维护
    };

} // namespace AsynGyanis::Database
