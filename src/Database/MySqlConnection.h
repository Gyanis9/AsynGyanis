/**
 * @file MySqlConnection.h
 * @brief MySQL 数据库连接实现
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_MYSQLCONNECTION_H
#define DATABASE_MYSQLCONNECTION_H

#include "DatabaseConnection.h"
#include "DatabaseResult.h"
#include "DatabaseType.h"

// 前向声明 MySQL C API 结构体（实际集成时需 #include <mysql/mysql.h>）
struct st_mysql;
using MYSQL = st_mysql;

#include <memory>
#include <string>
#include <string_view>

namespace Database
{
    /**
     * @brief MySQL 数据库连接
     *
     * 封装 MySQL C API，实现 DatabaseConnection 抽象接口。
     * 支持连接管理、SQL 执行和事务控制。
     *
     * 底层依赖 libmysqlclient，通过 Conan 或系统包管理安装。
     *
     * 使用方式：
     * @code
     *   auto config = ConnectionConfig::mySqlDefault();
     *   config.database = "mydb";
     *   MySqlConnection connection(config);
     *   if (connection.connect()) {
     *       auto result = connection.execute("SELECT * FROM users");
     *       while (result->next()) {
     *           auto id = result->getValue("id");
     *       }
     *   }
     * @endcode
     */
    class MySqlConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 MySQL 连接
         * @param config 连接配置
         */
        explicit MySqlConnection(const ConnectionConfig &config);

        /**
         * @brief 析构时自动断开连接
         */
        ~MySqlConnection() override;

        MySqlConnection(const MySqlConnection &)            = delete;
        MySqlConnection &operator=(const MySqlConnection &) = delete;
        MySqlConnection(MySqlConnection &&)                 = default;
        MySqlConnection &operator=(MySqlConnection &&)      = default;

        bool connect() override;
        void disconnect() override;
        [[nodiscard]] bool isConnected() const override;
        std::unique_ptr<DatabaseResult> execute(std::string_view command) override;
        [[nodiscard]] DatabaseType databaseType() const override;
        [[nodiscard]] std::string lastError() const override;

        /**
         * @brief 开始事务
         * @return 成功返回 true
         */
        bool beginTransaction();

        /**
         * @brief 提交事务
         * @return 成功返回 true
         */
        bool commit();

        /**
         * @brief 回滚事务
         * @return 成功返回 true
         */
        bool rollback();

        /**
         * @brief 获取 MySQL 客户端版本信息
         * @return 版本字符串
         */
        [[nodiscard]] std::string serverVersion() const;

        /**
         * @brief 获取底层 MYSQL 句柄（仅供内部或高级用途）
         * @return MYSQL 指针
         */
        [[nodiscard]] MYSQL *nativeHandle() const { return m_mysqlPointer; }

    private:
        /**
         * @brief 解析 MySQL 错误信息并存入 m_lastError
         */
        void captureError();

        MYSQL *m_mysqlPointer{nullptr}; ///< MySQL C API 连接句柄
    };

} // namespace Database

#endif // DATABASE_MYSQLCONNECTION_H
