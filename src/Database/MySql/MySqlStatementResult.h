/**
 * @file MySqlStatementResult.h
 * @brief MySQL 预处理语句的结果集（已完整预读进内存的只读快照）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 参数化执行（mysql_stmt_*）走的是二进制协议，结果没法包成 MYSQL_RES，因此连接在
 *          mysql_stmt_store_result() 之后把全部行读进内存再交给本类，语句随即被 mysql_stmt_close
 *          释放：结果集不引用任何句柄，可以比连接活得更久，代价是大结果集等额占内存
 *          （需要流式读取的场景应改用游标型语句，本驱动不提供）。
 *
 * @note 游标没停在有效行上（构造后、reset() 后、遍历结束后）时取值一律返回 std::monostate，
 *       与 MySqlResult 的约定一致；isEmpty() 描述结果集本身有没有行，不随游标推进改变。
 *       本类不覆盖 affectedRowCount()：写语句的影响行数由 MySqlConnection 用
 *       mysql_stmt_affected_rows() 取到后交给 MySqlResult 的写回执形态承载。
 */
#pragma once

#include "Database/Common/DatabaseResult.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief MySQL 预处理语句结果集（内存中的只读快照）
     *
     * @details 与 MySqlResult 的关系：两者是同一驱动里两条协议路径的结果集实现，
     *          接口语义完全对齐（列名/列序/取值映射/NULL 处理），差别只在数据来源——
     *          MySqlResult 持有 MYSQL_RES，本类持有已经转换好的 DatabaseValue 行。
     *          本类不接触任何 MySQL C API，因此可以在没有服务端的情况下直接构造并测试。
     */
    class MySqlStatementResult final : public DatabaseResult
    {
    public:
        /**
         * @brief 用预读好的列名与行构造结果集
         * @details 构造阶段不做检查：行的列数由连接在预读时保证（逐列读取元数据、
         *          逐行按列序取值），因此传进来的行与列名天然对齐；取值时仍做一次下标判界。
         * @param columnNames 按列序排列的列名，长度即列数
         * @param rows 已按 (指针, 长度) 转换好的行数据，每行的元素个数应与列名个数一致
         */
        MySqlStatementResult(std::vector<std::string> columnNames, std::vector<std::vector<DatabaseValue>> rows);

        /**
         * @brief 析构函数，释放行缓冲
         */
        ~MySqlStatementResult() override;

        // 结果集持有唯一一份行数据：拷贝没有语义（调用方本就从 unique_ptr 拿到它），
        // 基类同样已删除拷贝与移动，这里显式写清意图
        MySqlStatementResult(const MySqlStatementResult &) = delete;

        MySqlStatementResult &operator=(const MySqlStatementResult &) = delete;

        MySqlStatementResult(MySqlStatementResult &&) = delete;

        MySqlStatementResult &operator=(MySqlStatementResult &&) = delete;

        /**
         * @brief 将游标移动到下一行
         * @details 重写 DatabaseResult::next()：只推进内存下标，返回 false 只可能是「已到末尾」，不存在读取出错
         *          这一分支；按基类契约属只读路径，不会改写 m_lastError。其余与基类一致。
         * @return true 游标停在有效行上，可以读取列值
         * @return false 已无更多行
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：预读快照的行数恒为精确值。
         * @return size_t 行数
         */
        [[nodiscard]] size_t rowCount() const override;

        /**
         * @brief 获取结果集列数
         * @details 重写 DatabaseResult::columnCount()：等于构造时给出的列名个数。
         * @return size_t 列数
         */
        [[nodiscard]] size_t columnCount() const override;

        /**
         * @brief 按列索引取列名
         * @details 重写 DatabaseResult::columnName()：索引越界返回空值。
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 列名；索引越界时返回空值
         */
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;

        /**
         * @brief 按列名取列索引
         * @details 重写 DatabaseResult::columnIndex()：逐字节精确匹配（区分大小写），
         *          与 MySqlResult 的约定一致——列标识符的大小写敏感性由服务端排序规则决定，
         *          客户端不复制这份规则；同名列先到先得。
         * @param name 列名；空串一律视为不存在
         * @return std::optional<size_t> 列索引；列不存在时返回空值
         */
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;

        /**
         * @brief 按列索引读取当前行的值
         * @details 重写 DatabaseResult::getValue()：游标没停在有效行上（未 next()、已遍历完或刚 reset()）
         *          或列值为 SQL NULL 时都返回 std::monostate——两者在 DatabaseValue 里本就只有一种表达，
         *          调用方按「无值」统一处理即可。本方法是 const 读取路径，绝不改写 m_lastError。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；无当前行或索引越界时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;

        /**
         * @brief 读出指定列并把该格载荷的所有权交给调用方
         * @details 重写 DatabaseResult::takeValue()：本类的行是预读进内存的 DatabaseValue 快照，
         *          getValue() 每格都要复制一份 variant 载荷（文本、字节序列与嵌套列表都是堆缓冲，
         *          每复制一次就是一次分配），这里改成把格子里的载荷直接移动出去，源留成一个已搬空的
         *          同类型值。ORM 逐列映射器正是「每格只读一次」的用法，因此这一搬走是净收益。
         *          判定条件（无当前行、按列数名判界、行内长度判界）与 getValue() 逐条一致。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 该格的值；无当前行或索引越界时返回 std::monostate，载荷缓冲已被搬空
         */
        [[nodiscard]] DatabaseValue takeValue(size_t index) override;

        /**
         * @brief 按列名读取当前行的值
         * @details 重写 DatabaseResult::getValue()：先把列名解析成索引，再走索引重载，
         *          保证两条路径的判定完全一致。
         * @param name 列名（区分大小写，判定规则见 columnIndex()）
         * @return DatabaseValue 列值；列不存在、无当前行或值为 SQL NULL 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;

        /**
         * @brief 获取全部列名
         * @details 重写 DatabaseResult::columnNames()：直接返回构造时快照的副本，
         *          长度恒等于 columnCount() 且下标与列序严格对齐。
         * @return std::vector<std::string> 按列顺序排列的列名
         */
        [[nodiscard]] std::vector<std::string> columnNames() const override;

        /**
         * @brief 重置游标到首行之前，使结果集可重新遍历
         * @details 重写 DatabaseResult::reset()：数据全在内存里，复位即下标归零，并清掉「当前行有效」标志
         *          （否则 getValue() 会继续读最后一行）与上一轮遗留的错误文本。其余与基类一致。
         */
        void reset() override;

        /**
         * @brief 判断结果集是否为空
         * @details 重写 DatabaseResult::isEmpty()：直接由行数判定，不随游标推进改变。
         * @return true 没有任何数据行
         */
        [[nodiscard]] bool isEmpty() const override;

    private:
        std::vector<std::string>                m_columnNames;          ///< 按列序排列的列名，长度即列数
        std::vector<std::vector<DatabaseValue>> m_rows;                 ///< 预读好的全部行，行内按列序排列
        size_t                                  m_nextRowIndex{0};      ///< 下一次 next() 要交出的行下标
        bool                                    m_hasCurrentRow{false}; ///< 游标是否停在有效行上（getValue 的前置条件）
    };

} // namespace AsynGyanis::Database
