/**
 * @file DatabaseResult.h
 * @brief 数据库查询结果集抽象基类
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Database/Common/DatabaseValue.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库结果集抽象基类
     *
     * @details 封装不同数据库的查询结果，提供统一的游标遍历与列访问接口。
     *          实例一律由 DatabaseConnection::execute() 以 unique_ptr 交出，
     *          因此本类既禁止拷贝也禁止移动：移动一个持有数据库句柄的多态基类子对象
     *          极易留下悬垂引用，而收益为零。
     *
     * 使用方式：
     * @code
     *   auto result = connection->execute("SELECT * FROM users");
     *   while (result->next())
     *   {
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
         * @brief 虚析构函数，保证派生类经基类指针释放时正确析构
         */
        virtual ~DatabaseResult() = default;

        DatabaseResult(const DatabaseResult &)            = delete;
        DatabaseResult &operator=(const DatabaseResult &) = delete;
        DatabaseResult(DatabaseResult &&)                 = delete;
        DatabaseResult &operator=(DatabaseResult &&)      = delete;

        /**
         * @brief 将游标移动到下一行
         * @return true 成功移动到下一行；false 已无更多行
         */
        virtual bool next() = 0;

        /**
         * @brief 获取结果集行数
         * @return size_t 行数；驱动无法预先得知全部行时返回 0
         */
        [[nodiscard]] virtual size_t rowCount() const = 0;

        /**
         * @brief 获取结果集列数
         * @return size_t 列数
         */
        [[nodiscard]] virtual size_t columnCount() const = 0;

        /**
         * @brief 按列索引取列名
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 列名；索引越界返回空值
         */
        [[nodiscard]] virtual std::optional<std::string> columnName(size_t index) const = 0;

        /**
         * @brief 按列名取列索引
         * @param name 列名
         * @return std::optional<size_t> 列索引；列不存在返回空值
         */
        [[nodiscard]] virtual std::optional<size_t> columnIndex(std::string_view name) const = 0;

        /**
         * @brief 按列索引读取当前行的值
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；索引无效或值为 NULL 时返回 std::monostate
         */
        [[nodiscard]] virtual DatabaseValue getValue(size_t index) const = 0;

        /**
         * @brief 按列名读取当前行的值
         * @param name 列名
         * @return DatabaseValue 列值；列不存在或值为 NULL 时返回 std::monostate
         */
        [[nodiscard]] virtual DatabaseValue getValue(std::string_view name) const = 0;

        /**
         * @brief 获取全部列名
         * @return std::vector<std::string> 按列顺序排列的列名
         */
        [[nodiscard]] virtual std::vector<std::string> columnNames() const = 0;

        /**
         * @brief 将游标重置到首行之前，使结果集可重新遍历
         */
        virtual void reset() = 0;

        /**
         * @brief 判断结果集是否为空
         * @return true 没有任何数据行
         */
        [[nodiscard]] virtual bool isEmpty() const = 0;

        /**
         * @brief 获取最后一次错误信息
         * @details 派生类只允许在连接、执行等写路径上写入 m_lastError；
         *          getValue/next 等 const 读取路径不得改写错误状态。
         * @return std::string 错误描述，无错误时为空串
         */
        [[nodiscard]] virtual std::string lastError() const { return m_lastError; }

        /**
         * @brief 最近一次写语句影响的行数
         *
         * @details 本方法带默认实现（返回 0），是本次接口演进的追加项：
         *          以往只有具体驱动的结果集知道影响行数，上层不得不按 DatabaseType
         *          向下转型才能取到它，其它驱动一律拿不到。放到基类后，
         *          调用方只依赖 DatabaseResult 就能吃到该信息，未覆盖的驱动则保持 0。
         *
         *          语义约定：
         *          - 写语句（INSERT/UPDATE/DELETE）返回本条语句实际改动的行数；
         *          - 驱动不提供该信息（如 MySQL / Redis 驱动）返回 0，即「未知」，
         *            不会用一个虚假的非零值冒充统计结果；
         *          - 只读结果集返回 0，但具体驱动若只能读到连接级计数器（SQLite 即是如此，
         *            引擎未提供语句级历史），可以返回该计数器并自行在注释中说明。
         *
         * @return std::int64_t 影响行数；0 表示未知、只读结果集或没有行被改动
         * @note 本方法必须 noexcept：它是执行路径上的统计读取，不允许因取数失败而打断调用方
         */
        [[nodiscard]] virtual std::int64_t affectedRowCount() const noexcept { return 0; }

    protected:
        std::string m_lastError; ///< 最后一次错误信息
    };

} // namespace AsynGyanis::Database
