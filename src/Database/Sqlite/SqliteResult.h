/**
 * @file SqliteResult.h
 * @brief SQLite 查询结果集实现
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_SQLITERESULT_H
#define DATABASE_SQLITERESULT_H

#include "DatabaseResult.h"

// SQLite C API 前向声明
struct sqlite3;
struct sqlite3_stmt;

namespace Database
{
    /**
     * @brief SQLite 查询结果集
     *
     * 封装 SQLite C API 的 sqlite3_stmt，提供统一的 DatabaseResult 接口。
     * SQLite 使用预编译语句（prepared statement）模型，因此结果集绑定到具体的 stmt。
     */
    class SqliteResult : public DatabaseResult
    {
    public:
        /**
         * @brief 使用 SQLite 预编译语句构造
         * @param statement 已执行 step 的 SQLite 语句句柄
         * @param database  SQLite 数据库连接（用于错误信息）
         */
        explicit SqliteResult(sqlite3_stmt *statement, sqlite3 *database = nullptr);

        /**
         * @brief 析构时自动释放语句
         */
        ~SqliteResult() override;

        SqliteResult(const SqliteResult &)            = delete;
        SqliteResult &operator=(const SqliteResult &) = delete;
        SqliteResult(SqliteResult &&)                 = default;
        SqliteResult &operator=(SqliteResult &&)      = default;

        bool next() override;
        [[nodiscard]] size_t rowCount() const override;
        [[nodiscard]] size_t columnCount() const override;
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;
        [[nodiscard]] std::vector<std::string> columnNames() const override;
        void reset() override;
        [[nodiscard]] bool isEmpty() const override;

        /**
         * @brief 获取底层 SQLite 语句句柄（仅供内部使用）
         */
        [[nodiscard]] sqlite3_stmt *nativeHandle() const { return m_statement; }

        /**
         * @brief 获取最近一次 INSERT 操作生成的行 ID
         * @return 行 ID
         */
        [[nodiscard]] int64_t lastInsertRowId() const { return m_lastInsertRowId; }

        /**
         * @brief 获取受影响的行数
         * @return 行数
         */
        [[nodiscard]] int affectedRowCount() const { return m_affectedRows; }

    private:
        /**
         * @brief 将 SQLite 列值转换为统一的 DatabaseValue
         * @param index 列索引
         * @return 统一的数据库值
         */
        [[nodiscard]] DatabaseValue convertValue(int index) const;

        /**
         * @brief 遍历所有行以计算行数
         */
        void countRows();

        sqlite3_stmt *m_statement{nullptr};   ///< SQLite 预编译语句句柄
        sqlite3      *m_database{nullptr};    ///< SQLite 数据库连接
        int64_t       m_lastInsertRowId{0};    ///< 最后插入的行 ID
        int           m_affectedRows{0};       ///< 受影响行数
        size_t        m_rowCount{0};           ///< 行数缓存
        size_t        m_columnCount{0};        ///< 列数缓存
        bool          m_isEmpty{true};         ///< 是否为空
        bool          m_hasRow{false};         ///< 是否有当前行
    };

} // namespace Database

#endif // DATABASE_SQLITERESULT_H
