/**
 * @file SqliteConnection.h
 * @brief SQLite 数据库连接实现
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_SQLITECONNECTION_H
#define DATABASE_SQLITECONNECTION_H

#include "DatabaseConnection.h"
#include "DatabaseResult.h"
#include "DatabaseType.h"

// SQLite C API 前向声明
struct sqlite3;

namespace Database
{
    /**
     * @brief SQLite 数据库连接
     *
     * 封装 SQLite C API（sqlite3），实现 DatabaseConnection 抽象接口。
     * SQLite 是嵌入式数据库，无需独立的服务进程，数据库即单个文件。
     *
     * 支持：
     * - 内存数据库（":memory:"）
     * - 文件数据库
     * - 事务控制（BEGIN/COMMIT/ROLLBACK）
     * - 预编译语句缓存
     *
     * 使用方式：
     * @code
     *   ConnectionConfig config;
     *   config.database = ":memory:";  // 内存数据库
     *   SqliteConnection connection(config);
     *   if (connection.connect()) {
     *       connection.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
     *       connection.execute("INSERT INTO users VALUES (1, 'Alice')");
     *       auto result = connection.execute("SELECT * FROM users");
     *   }
     * @endcode
     */
    class SqliteConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 SQLite 连接
         * @param config 连接配置（config.database 为数据库文件路径或 ":memory:"）
         */
        explicit SqliteConnection(const ConnectionConfig &config);

        /**
         * @brief 析构时自动关闭数据库
         */
        ~SqliteConnection() override;

        SqliteConnection(const SqliteConnection &)            = delete;
        SqliteConnection &operator=(const SqliteConnection &) = delete;
        SqliteConnection(SqliteConnection &&)                 = default;
        SqliteConnection &operator=(SqliteConnection &&)      = default;

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
         * @brief 获取 SQLite 版本信息
         * @return 版本字符串
         */
        [[nodiscard]] std::string serverVersion() const;

        /**
         * @brief 获取最近一次 INSERT 操作生成的行 ID
         * @return 行 ID
         */
        [[nodiscard]] int64_t lastInsertRowId() const;

        /**
         * @brief 获取底层 sqlite3 句柄（仅供内部或高级用途）
         * @return sqlite3 指针
         */
        [[nodiscard]] sqlite3 *nativeHandle() const { return m_database; }

    private:
        /**
         * @brief 解析 SQLite 错误信息并存入 m_lastError
         */
        void captureError();

        sqlite3 *m_database{nullptr}; ///< SQLite C API 数据库句柄
    };

} // namespace Database

#endif // DATABASE_SQLITECONNECTION_H
