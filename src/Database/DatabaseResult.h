/**
 * @file DatabaseResult.h
 * @brief 数据库查询结果集抽象基类
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_DATABASERESULT_H
#define DATABASE_DATABASERESULT_H

#include "DatabaseType.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Database
{
    /**
     * @brief 数据库结果集抽象基类
     *
     * 封装不同数据库的查询结果，提供统一的遍历和数据访问接口。
     * 子类需实现具体的行遍历和字段访问逻辑。
     *
     * 使用方式：
     * @code
     *   auto result = connection->execute("SELECT * FROM users");
     *   while (result->next()) {
     *       auto id   = result->getValue(0);
     *       auto name = result->getValue("name");
     *   }
     * @endcode
     */
    class DatabaseResult
    {
    public:
        /**
         * @brief 默认构造函数
         */
        DatabaseResult() = default;

        /**
         * @brief 虚析构函数
         */
        virtual ~DatabaseResult() = default;

        // 禁止拷贝，允许移动
        DatabaseResult(const DatabaseResult &)            = delete;
        DatabaseResult &operator=(const DatabaseResult &) = delete;
        DatabaseResult(DatabaseResult &&)                 = default;
        DatabaseResult &operator=(DatabaseResult &&)      = default;

        /**
         * @brief 将游标移动到下一行
         * @return 成功移动到下一行返回 true，已无更多行返回 false
         */
        virtual bool next() = 0;

        /**
         * @brief 获取当前结果集的行数
         * @return 行数，若不支持则返回 0
         */
        [[nodiscard]] virtual size_t rowCount() const = 0;

        /**
         * @brief 获取当前结果集的列数
         * @return 列数
         */
        [[nodiscard]] virtual size_t columnCount() const = 0;

        /**
         * @brief 获取指定索引的列名称
         * @param index 列索引（从 0 开始）
         * @return 列名字符串，索引无效时返回空 optional
         */
        [[nodiscard]] virtual std::optional<std::string> columnName(size_t index) const = 0;

        /**
         * @brief 获取指定列名的列索引
         * @param name 列名
         * @return 列索引，列名不存在时返回空 optional
         */
        [[nodiscard]] virtual std::optional<size_t> columnIndex(std::string_view name) const = 0;

        /**
         * @brief 通过列索引获取当前行的值
         * @param index 列索引（从 0 开始）
         * @return 数据库统一值，索引无效时返回空 monostate
         */
        [[nodiscard]] virtual DatabaseValue getValue(size_t index) const = 0;

        /**
         * @brief 通过列名获取当前行的值
         * @param name 列名
         * @return 数据库统一值，列名无效时返回空 monostate
         */
        [[nodiscard]] virtual DatabaseValue getValue(std::string_view name) const = 0;

        /**
         * @brief 获取所有列名
         * @return 列名列表
         */
        [[nodiscard]] virtual std::vector<std::string> columnNames() const = 0;

        /**
         * @brief 将游标重置到第一行之前
         */
        virtual void reset() = 0;

        /**
         * @brief 判断结果集是否为空
         * @return 无数据返回 true
         */
        [[nodiscard]] virtual bool isEmpty() const = 0;

        /**
         * @brief 获取最后一条错误信息
         * @return 错误描述字符串
         */
        [[nodiscard]] virtual std::string lastError() const { return m_lastError; }

    protected:
        std::string m_lastError; ///< 最后一次错误信息
    };

} // namespace Database

#endif // DATABASE_DATABASERESULT_H
