/**
 * @file DatabaseConnection.h
 * @brief 数据库连接抽象基类
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_DATABASECONNECTION_H
#define DATABASE_DATABASECONNECTION_H

#include "DatabaseResult.h"
#include "DatabaseType.h"

#include <memory>
#include <string>
#include <string_view>

namespace Database
{
    /**
     * @brief 数据库连接抽象基类
     *
     * 定义数据库连接的标准生命周期和操作接口，所有数据库实现（MySQL、Redis 等）
     * 均需继承此类并实现纯虚函数。外部调用者通过本接口操作数据库，无需关心底层实现。
     *
     * 生命周期：
     * 1. 构造 → 调用 connect() → 执行 SQL/命令 → 调用 disconnect() → 析构
     *
     * @code
     *   auto connection = DatabaseFactory::createMySql(config);
     *   if (connection->connect()) {
     *       auto result = connection->execute("SELECT 1");
     *       // ...
     *       connection->disconnect();
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
         * @brief 虚析构函数，确保子类正确析构
         */
        virtual ~DatabaseConnection() = default;

        // 禁止拷贝，允许移动
        DatabaseConnection(const DatabaseConnection &)            = delete;
        DatabaseConnection &operator=(const DatabaseConnection &) = delete;
        DatabaseConnection(DatabaseConnection &&)                 = default;
        DatabaseConnection &operator=(DatabaseConnection &&)      = default;

        /**
         * @brief 建立与数据库的连接
         * @return 连接成功返回 true，失败返回 false
         */
        virtual bool connect() = 0;

        /**
         * @brief 断开与数据库的连接
         */
        virtual void disconnect() = 0;

        /**
         * @brief 判断当前是否处于连接状态
         * @return 已连接返回 true
         */
        [[nodiscard]] virtual bool isConnected() const = 0;

        /**
         * @brief 执行数据库命令（SQL 语句或 Redis 命令）
         * @param command 命令字符串
         * @return 结果集智能指针，失败时返回 nullptr
         */
        virtual std::unique_ptr<DatabaseResult> execute(std::string_view command) = 0;

        /**
         * @brief 获取数据库类型
         * @return 数据库类型枚举
         */
        [[nodiscard]] virtual DatabaseType databaseType() const = 0;

        /**
         * @brief 获取最后一条错误信息
         * @return 错误描述字符串
         */
        [[nodiscard]] virtual std::string lastError() const { return m_lastError; }

        /**
         * @brief 获取连接配置的只读引用
         * @return 连接配置
         */
        [[nodiscard]] const ConnectionConfig &configuration() const { return m_configuration; }

        /**
         * @brief 设置连接超时时间（毫秒）
         * @param milliseconds 超时时间
         */
        virtual void setConnectTimeout(int milliseconds) { m_connectTimeout = milliseconds; }

        /**
         * @brief 设置命令执行超时时间（毫秒）
         * @param milliseconds 超时时间
         */
        virtual void setQueryTimeout(int milliseconds) { m_queryTimeout = milliseconds; }

    protected:
        ConnectionConfig m_configuration;        ///< 连接配置
        std::string      m_lastError;            ///< 最后一次错误信息
        int              m_connectTimeout{5000};  ///< 连接超时（毫秒）
        int              m_queryTimeout{30000};   ///< 查询超时（毫秒）
        bool             m_isConnected{false};    ///< 连接状态标志
    };

} // namespace Database

#endif // DATABASE_DATABASECONNECTION_H
