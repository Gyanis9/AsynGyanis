/**
 * @file MySqlResult.h
 * @brief MySQL 查询结果集实现
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_MYSQLRESULT_H
#define DATABASE_MYSQLRESULT_H

#include "DatabaseResult.h"

// 前向声明 MySQL C API 结构体（实际使用时需 #include <mysql/mysql.h>）
struct st_mysql_res;
using MYSQL_RES  = st_mysql_res;
struct st_mysql;
using MYSQL      = st_mysql;

namespace Database
{
    /**
     * @brief MySQL 查询结果集
     *
     * 封装 MySQL C API 的 MYSQL_RES，提供统一的 DatabaseResult 接口访问。
     * 内部持有 MYSQL_RES 指针的所有权，析构时自动释放。
     */
    class MySqlResult : public DatabaseResult
    {
    public:
        /**
         * @brief 使用 MySQL 结果集句柄构造
         * @param resultPointer MySQL C API 返回的 MYSQL_RES 指针
         * @param connection    MySQL 连接句柄（用于错误信息查询）
         */
        explicit MySqlResult(MYSQL_RES *resultPointer, MYSQL *connection = nullptr);

        /**
         * @brief 析构时自动释放 MYSQL_RES
         */
        ~MySqlResult() override;

        MySqlResult(const MySqlResult &)            = delete;
        MySqlResult &operator=(const MySqlResult &) = delete;
        MySqlResult(MySqlResult &&)                 = default;
        MySqlResult &operator=(MySqlResult &&)      = default;

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
         * @brief 获取底层的 MYSQL_RES 指针（仅供内部使用）
         */
        [[nodiscard]] MYSQL_RES *nativeHandle() const { return m_resultPointer; }

    private:
        /**
         * @brief 将 MySQL 字段值转换为统一的 DatabaseValue
         * @param row  当前行指针
         * @param index 列索引
         * @return 统一的数据库值
         */
        [[nodiscard]] DatabaseValue convertValue(char **row, size_t index) const;

        MYSQL_RES *m_resultPointer{nullptr}; ///< MySQL 结果集句柄
        MYSQL     *m_connection{nullptr};    ///< MySQL 连接句柄（用于错误信息）
        char     **m_currentRow{nullptr};    ///< 当前行数据
        size_t     m_rowCount{0};            ///< 缓存的行数
        size_t     m_columnCount{0};         ///< 缓存的列数
        bool       m_isEmpty{true};          ///< 是否为空结果集
    };

} // namespace Database

#endif // DATABASE_MYSQLRESULT_H
