/**
 * @file TestMySqlStatementResult.cpp
 * @brief MySqlStatementResult 单元测试：参数化执行路径的结果集快照语义（不需要数据库）
 * @details MySqlStatementResult 承载预处理语句（mysql_stmt_*）预读出来的行数据，不接触任何 MySQL C API，因此可以在
 *          没有服务端的情况下直接构造并验证全部接口：列名/列序/取值映射、游标推进与复位、NULL 与空串的区分、越界判定。
 *          连接侧真正的「准备—绑定—执行—预读」链路需要可用的服务端，本文件不覆盖。
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/MySql/MySqlStatementResult.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        /**
         * @brief 构造一份两行三列的结果集快照
         * @details 列类型覆盖整数、文本与 NULL，行数固定为 2，供各用例复用
         * @return std::unique_ptr<MySqlStatementResult> 结果集
         */
        std::unique_ptr<MySqlStatementResult> makeSampleResult()
        {
            std::vector<std::string> columnNames{"id", "name", "note"};
            std::vector<std::vector<DatabaseValue>> rows{
                {std::int64_t{1}, std::string("张三"), std::monostate{}},
                {std::int64_t{2}, std::string("O'Brien -- DROP"), std::string("普通备注")}
            };

            return std::make_unique<MySqlStatementResult>(std::move(columnNames), std::move(rows));
        }
    } // namespace

    // ------------------------------------------------------------------------
    // 形状：行数、列数、列名
    // ------------------------------------------------------------------------

    TEST(MySqlStatementResult, ReportsShapeFromSnapshot)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        EXPECT_EQ(result->rowCount(), 2U);
        EXPECT_EQ(result->columnCount(), 3U);
        EXPECT_FALSE(result->isEmpty());

        // 列名按列序快照，长度恒等于列数
        const std::vector<std::string> names = result->columnNames();
        ASSERT_EQ(names.size(), 3U);
        EXPECT_EQ(names[0], "id");
        EXPECT_EQ(names[1], "name");
        EXPECT_EQ(names[2], "note");
    }

    TEST(MySqlStatementResult, ColumnNameRespectsIndexBounds)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        EXPECT_EQ(result->columnName(0).value_or(""), "id");
        EXPECT_EQ(result->columnName(2).value_or(""), "note");
        // 越界索引返回空值，不做任何回绕
        EXPECT_FALSE(result->columnName(3).has_value());
    }

    TEST(MySqlStatementResult, ColumnIndexMatchesNameExactly)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        ASSERT_TRUE(result->columnIndex("name").has_value());
        EXPECT_EQ(result->columnIndex("name").value(), 1U);
        // 空串与不存在的列一律视为未命中
        EXPECT_FALSE(result->columnIndex("").has_value());
        EXPECT_FALSE(result->columnIndex("missing").has_value());
        // 列标识符按原文精确匹配（区分大小写），大小写敏感性由服务端排序规则决定
        EXPECT_FALSE(result->columnIndex("NAME").has_value());
    }

    TEST(MySqlStatementResult, EmptySnapshotIsEmptyAndIteratesNothing)
    {
        MySqlStatementResult result(std::vector<std::string>{"id"}, std::vector<std::vector<DatabaseValue>>{});

        EXPECT_TRUE(result.isEmpty());
        EXPECT_EQ(result.rowCount(), 0U);
        EXPECT_EQ(result.columnCount(), 1U);
        EXPECT_FALSE(result.next());
    }

    // ------------------------------------------------------------------------
    // 游标：推进、取值、复位
    // ------------------------------------------------------------------------

    TEST(MySqlStatementResult, ValueIsUnavailableBeforeFirstNext)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        // 构造后游标停在首行之前：取值与「无值」一致，不会读到第一行的残留
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(0)));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue("name")));
    }

    TEST(MySqlStatementResult, NextWalksRowsInSnapshotOrder)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        ASSERT_TRUE(result->next());
        EXPECT_EQ(std::get<std::int64_t>(result->getValue("id")), 1);
        EXPECT_EQ(std::get<std::string>(result->getValue("name")), "张三");
        // 第一行的 note 是 SQL NULL
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue("note")));

        ASSERT_TRUE(result->next());
        EXPECT_EQ(std::get<std::int64_t>(result->getValue(0)), 2);
        EXPECT_EQ(std::get<std::string>(result->getValue(1)), "O'Brien -- DROP");
        EXPECT_EQ(std::get<std::string>(result->getValue(2)), "普通备注");

        // 走到末尾：行数据已清空，取值退回「无值」
        EXPECT_FALSE(result->next());
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(0)));
        EXPECT_FALSE(result->next());
    }

    TEST(MySqlStatementResult, ResetRewindsCursorAndClearsError)
    {
        std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        ASSERT_TRUE(result->next());
        ASSERT_TRUE(result->next());
        ASSERT_FALSE(result->next());

        result->reset();

        // 复位后游标回到首行之前，可以完整重放一遍
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(0)));
        ASSERT_TRUE(result->next());
        EXPECT_EQ(std::get<std::int64_t>(result->getValue("id")), 1);
        ASSERT_TRUE(result->next());
        EXPECT_EQ(std::get<std::int64_t>(result->getValue("id")), 2);
        EXPECT_FALSE(result->next());
    }

    TEST(MySqlStatementResult, OutOfRangeAccessReturnsNoValue)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        ASSERT_TRUE(result->next());
        // 列索引越界与「无当前行」在契约里同为 monostate
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(3)));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue("missing")));
    }

    // ------------------------------------------------------------------------
    // 取值映射与影响行数
    // ------------------------------------------------------------------------

    TEST(MySqlStatementResult, PreservesValueTypesAndEmbeddedNul)
    {
        std::vector<std::string> columnNames{"flag", "score", "payload"};
        std::vector<std::vector<DatabaseValue>> rows{
            {true, 1.5, std::string("a\0b", 3)}
        };
        MySqlStatementResult result(std::move(columnNames), std::move(rows));

        ASSERT_TRUE(result.next());
        ASSERT_TRUE(std::holds_alternative<bool>(result.getValue("flag")));
        EXPECT_TRUE(std::get<bool>(result.getValue("flag")));
        EXPECT_DOUBLE_EQ(std::get<double>(result.getValue("score")), 1.5);

        // std::string 按长度保存，内嵌 '\0' 不丢
        const std::string payload = std::get<std::string>(result.getValue("payload"));
        ASSERT_EQ(payload.size(), 3U);
        EXPECT_EQ(payload, std::string("a\0b", 3));
    }

    TEST(MySqlStatementResult, AffectedRowCountIsZeroForQuerySnapshot)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        // 查询结果集属只读路径：按基类约定影响行数为 0，不把「返回了多少行」冒充成「改动了多少行」
        EXPECT_EQ(result->affectedRowCount(), 0);
        EXPECT_TRUE(result->lastError().empty());
    }

} // namespace AsynGyanis::Database
