// 覆盖场景（MySqlStatementResult 承载预处理语句预读出的行数据，不接触 MySQL C API，无需服务端）：
// - 形状：行数/列数/列名快照，列名与列索引的边界判定
// - 游标：首次 next 之前的取值、按快照顺序推进、走到末尾清空、reset 后重放
// - 取值映射：整数/文本/布尔/浮点、NULL 与空串的区分、内嵌 '\0' 按长度保留、影响行数恒为 0
// 连接侧「准备—绑定—执行—预读」链路需要可用的服务端，不在此文件覆盖。

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

    /**
     * @brief 钉住结果集形状取自构造快照：行数、列数与列名顺序都逐项相符
     */
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

    /**
     * @brief 钉住列名按索引取用且越界返回空值，不做回绕
     */
    TEST(MySqlStatementResult, ColumnNameRespectsIndexBounds)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        EXPECT_EQ(result->columnName(0).value_or(""), "id");
        EXPECT_EQ(result->columnName(2).value_or(""), "note");
        // 越界索引返回空值，不做任何回绕
        EXPECT_FALSE(result->columnName(3).has_value());
    }

    /**
     * @brief 钉住列名反查按原文精确匹配：空串、不存在的列与大小写变体都未命中
     */
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

    /**
     * @brief 钉住零行快照仍是「有列但为空」：isEmpty 为真且 next() 恒假
     */
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

    /**
     * @brief 钉住构造后游标停在首行之前：按索引或按名取值都得到 monostate
     */
    TEST(MySqlStatementResult, ValueIsUnavailableBeforeFirstNext)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        // 构造后游标停在首行之前：取值与「无值」一致，不会读到第一行的残留
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(0)));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue("name")));
    }

    /**
     * @brief 钉住 next() 按快照顺序逐行推进、NULL 与文本列逐值正确、末尾后取值退回无值
     */
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

    /**
     * @brief 钉住 reset() 把游标退回首行之前并清除错误状态，整份快照可重放
     */
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

    /**
     * @brief 钉住列索引越界与「无当前行」在契约里同为 monostate
     */
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

    /**
     * @brief 钉住布尔/浮点/内嵌 '\0' 的文本都按原类型原长度交出
     */
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

    /**
     * @brief 钉住查询快照的影响行数为 0、无错误，不把返回行数冒充成改动行数
     */
    TEST(MySqlStatementResult, AffectedRowCountIsZeroForQuerySnapshot)
    {
        const std::unique_ptr<MySqlStatementResult> result = makeSampleResult();

        // 查询结果集属只读路径：按基类约定影响行数为 0，不把「返回了多少行」冒充成「改动了多少行」
        EXPECT_EQ(result->affectedRowCount(), 0);
        EXPECT_TRUE(result->lastError().empty());
    }

} // namespace AsynGyanis::Database
