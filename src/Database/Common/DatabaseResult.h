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

        DatabaseResult(const DatabaseResult &) = delete;

        DatabaseResult &operator=(const DatabaseResult &) = delete;

        DatabaseResult(DatabaseResult &&) = delete;

        DatabaseResult &operator=(DatabaseResult &&) = delete;

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
         * @brief 读出指定列并把该格载荷的**所有权**交给调用方
         * @details 给逐列映射用：结果集按行前进、每格只读一次，把已经存好的文本/二进制缓冲直接搬走
         *          就省掉「快照 → 返回值」那一次整串拷贝与它的堆分配。默认实现转交 getValue()——
         *          游标式取值本来就是当场构造的，没有可省的拷贝，因此只有物化了行的驱动值得重写。
         * @warning 取用过一次后不得再读同一格：重写方允许把源留成空值。同一行内每格只取一次是
         *          ORM 映射器的既有前提（列下标两两不同由 resolveColumnIndices 保证）。
         *          对物化快照型的实现，reset() 只复位游标、不重建数据：被取走过的格在第二轮遍历里
         *          读到的是空值，因此「先 takeValue 消费、再 reset() 重扫」不是受支持的用法。
         * @param index 列下标，越界或游标无效时按「无值」返回 monostate
         * @return DatabaseValue 该格的值，载荷缓冲可能已被搬空
         */
        [[nodiscard]] virtual DatabaseValue takeValue(const std::size_t index)
        {
            return getValue(index);
        }

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
        [[nodiscard]] virtual std::string lastError() const
        {
            return m_lastError;
        }

        /**
         * @brief 最近一次写语句影响的行数
         *
         * @details 本方法带默认实现（返回 0），不提供该信息的驱动可直接复用，调用方只依赖
         *          DatabaseResult 就能取到它。语义约定：写语句返回本条语句实际改动的行数；
         *          驱动不提供该信息（如 MySQL / Redis）或只读结果集返回 0，即「未知」，
         *          不用虚假的非零值冒充统计结果；只能读到连接级计数器的驱动（SQLite）按快照如实返回。
         *
         * @return std::int64_t 影响行数；0 表示未知、只读结果集或没有行被改动
         * @note 本方法必须 noexcept：它是执行路径上的统计读取，不允许因取数失败而打断调用方
         */
        [[nodiscard]] virtual std::int64_t affectedRowCount() const noexcept
        {
            return 0;
        }

        /**
         * @brief 本条语句所属连接上最近一次成功插入生成的自增标识
         *
         * @details 值在结果集构造时从驱动句柄就地快照：连接由池共享，等调用方想起来再查句柄，
         *          读到的可能已是别人那次插入。插入之后的非插入写语句上两驱动有差异——MySQL 回 0
         *          （服务端每条 OK 包都带该字段），SQLite 仍是上一条 INSERT 的 rowid（它只有连接级
         *          计数器，与 affectedRowCount() 同一处境）。
         *
         * @return std::int64_t 自增标识；0 表示没有产生（从未插入、表无自增列、驱动不提供该信息，
         *         或有符号 64 位装不下而如实报 0——后者会在 lastError() 里写明原因）
         * @note 本方法必须 noexcept：它是执行路径上的统计读取，不允许因取数失败而打断调用方
         */
        [[nodiscard]] virtual std::int64_t lastInsertRowId() const noexcept
        {
            return 0;
        }

    protected:
        std::string m_lastError; ///< 最后一次错误信息
    };

} // namespace AsynGyanis::Database
