/**
 * @file TestSqliteResult.cpp
 * @brief SqliteResult 单元测试：游标与预扫描语义、列元数据、存储类到 DatabaseValue 的映射与写回执快照
 * @details 结果集只能由 execute() 交出，因此本文件全部用例都跑在真实的内存库上：
 *          先建表灌样本数据，再经 execute() 取回结果集，零外部服务、零伪造内部状态。
 *          affectedRowCount() / lastInsertRowId() / nativeHandle() 只存在于 SqliteResult 上，
 *          用例统一经 dynamic_cast 取回派生类型——转换失败本身就是「驱动交出错类型」的缺陷。
 *          钉住的实现契约（重构时刻意定下的语义，破坏即视为回归）：
 *          1、只有只读语句会被预扫描：rowCount() 对只读查询给出精确行数，对写语句与带副作用的
 *             语句（INSERT ... RETURNING）返回 0；isEmpty() 对只读结果集准确，对写回执恒为 true；
 *          2、列值必须 next() 之后读取：未 next()、游标耗尽、reset() 之后一律 std::monostate；
 *          3、存储类映射：NULL→monostate、INTEGER→int64_t、FLOAT→double、TEXT/BLOB→std::string
 *             （BLOB 原样按字节、内嵌 '\0' 不丢失，零长 TEXT/BLOB 是空串而不是 NULL；
 *             声明为 DATE/NUMERIC 却存了非数值文本的列按 TEXT 存储类落到 std::string）；
 *          4、columnNames() 长度恒等于 columnCount()，表达式列的空名以空串占位，不丢下标；
 *          5、越界判定用无符号比较，SIZE_MAX 这类输入不得绕过边界；
 *          6、next()/getValue() 等读取路径不改写 lastError()；reset() 属写路径，会先清掉历史错误。
 *          确认无法安全覆盖、因此不做断言的行为：
 *          1、未知存储类落到 default 分支返回 monostate——SQLite 现存取值只有上面五种，公开接口造不出来；
 *          2、语句在析构时被 finalize 这一事实无法从公开接口直接观测（没有语句计数器），
 *             本文件只验「遍历中途销毁结果集后连接照常可用」这一可观察面；
 *          3、sqlite3_reset 返回 SQLITE_BUSY 时 reset() 写入错误文本的分支——需要另一个连接正持有
 *             该语句读到的快照，构造成本高且依赖 WAL 检查点时机，属易碎用例；等锁失败的可观察面已由
 *             TestSqliteConnection.cpp 的排他锁用例覆盖，本文件不再重复制造；
 *          4、countRows() 中途出错的分支——只读语句在内存库里不会失败，无法稳定注入故障。
 *          依赖说明：INSERT ... RETURNING 需要 SQLite 3.35 及以上，本工程由 Conan 锁定 3.51.x。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Sqlite/SqliteConnection.h"
#include "Database/Sqlite/SqliteResult.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        /// 建表样板：覆盖整数、文本、浮点、整型布尔位与二进制五种列，另留两列给 NULL 与空值场景
        constexpr const char *kCreateUsersTableSql =
                "CREATE TABLE users ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                " name TEXT NOT NULL,"
                " age INTEGER,"
                " score REAL,"
                " active INTEGER NOT NULL DEFAULT 1,"
                " payload BLOB)";

        /// 第二张表：用声明类型（DATE / NUMERIC）而非存储类，验证「落不到四种存储类时按文本返回」
        constexpr const char *kCreateTypedTableSql =
                "CREATE TABLE typedValues ("
                " id INTEGER PRIMARY KEY,"
                " asDate DATE,"
                " asNumeric NUMERIC)";

        /// 样本数据：四行的 rowid 依次为 1..4，第 4 行刻意给出空串文本与零长二进制
        constexpr std::array<const char *, 4> kSeedUsersSql = {
                "INSERT INTO users (name, age, score, active, payload) VALUES ('Alice', 30, 95.5, 1, x'5c0041')",
                "INSERT INTO users (name, age, score, active) VALUES ('Bob', 25, 60.25, 0)",
                "INSERT INTO users (name) VALUES ('Carol')",
                "INSERT INTO users (name, age, score, payload) VALUES ('', 40, 0.0, x'')",
        };

        /// 稳定遍历顺序：所有逐行断言都按 id 排序，避免依赖 SQLite 的默认返回次序
        constexpr const char *kSelectUsersInIdOrderSql = "SELECT id, name, age, score, active, payload FROM users ORDER BY id";

        /// 用于「按字节拷贝不截断」的 UTF-8 中文名，插入后原样读回比对
        constexpr const char *kUnicodeName = "郭佳";

        /**
         * @brief 打开一个内存库连接，供不需要样本数据的用例使用
         * @return std::unique_ptr<SqliteConnection> 已连接的内存库；连接失败时用例即刻失败
         */
        std::unique_ptr<SqliteConnection> openMemoryConnection()
        {
            std::unique_ptr<SqliteConnection> connection = std::make_unique<SqliteConnection>(ConnectionConfig::sqliteDefault());
            EXPECT_TRUE(connection->connect()) << connection->lastError();
            return connection;
        }

        /**
         * @brief 执行一条按契约应当成功的命令
         * @details 只用 EXPECT 记录失败，返回的指针仍可能为空，调用方需自行 ASSERT_NE 决定是否中止。
         * @param connection 已连接的数据库连接
         * @param command SQL 文本
         * @return std::unique_ptr<DatabaseResult> 结果集，失败时为空
         */
        std::unique_ptr<DatabaseResult> executeRequired(DatabaseConnection &connection, const std::string_view command)
        {
            std::unique_ptr<DatabaseResult> result = connection.execute(command);
            EXPECT_NE(result, nullptr) << "命令本应执行成功：" << command << "，原因：" << connection.lastError();
            return result;
        }

        /**
         * @brief 把 execute() 交出的基类结果集还原成 SQLite 派生类型
         * @details affectedRowCount() / lastInsertRowId() / nativeHandle() 是 SQLite 专有接口，
         *          只能向下转换后读取；转换失败即「驱动交出错类型」，用例据此失败。
         * @param result execute() 交出的结果集
         * @return SqliteResult * 派生类型指针，转换失败时为空
         */
        SqliteResult *asSqliteResult(DatabaseResult &result)
        {
            return dynamic_cast<SqliteResult *>(&result);
        }

        /**
         * @brief 判断值是否为「没有值」，即 std::monostate
         * @param value 数据库统一值
         * @return true 该位置没有值（NULL、越界或游标无效共用这一表现）
         */
        bool isMissingValue(const DatabaseValue &value)
        {
            return std::holds_alternative<std::monostate>(value);
        }

        /**
         * @brief 安全取出整型列值
         * @param value 待判定的数据库值
         * @return std::optional<std::int64_t> 类型不符时返回空值而不是抛异常
         */
        std::optional<std::int64_t> asInteger(const DatabaseValue &value)
        {
            const auto *integer = std::get_if<std::int64_t>(&value);
            return integer == nullptr ? std::nullopt : std::optional<std::int64_t>(*integer);
        }

        /**
         * @brief 安全取出浮点列值
         * @param value 待判定的数据库值
         * @return std::optional<double> 类型不符时返回空值
         */
        std::optional<double> asReal(const DatabaseValue &value)
        {
            const auto *realValue = std::get_if<double>(&value);
            return realValue == nullptr ? std::nullopt : std::optional<double>(*realValue);
        }

        /**
         * @brief 安全取出文本或二进制列值
         * @param value 待判定的数据库值
         * @return std::optional<std::string> 类型不符时返回空值
         */
        std::optional<std::string> asText(const DatabaseValue &value)
        {
            const auto *text = std::get_if<std::string>(&value);
            return text == nullptr ? std::nullopt : std::optional<std::string>(*text);
        }
    } // namespace

    // ============================================================================
    // 夹具
    // ============================================================================

    /**
     * @brief 结果集夹具：内存库里已建 users / typedValues 两张表并灌入四行样本
     *
     * @details 建表与插入样板集中在 SetUp；用例只写「取结果集 + 断言那一件事」。
     *          样本行固定四行且 id 连续，凡涉及 rowid 的断言都以 4 为基准。
     */
    class SqliteUserQuery : public ::testing::Test
    {
    protected:
        /**
         * @brief 连接内存库、建表并写入样本数据
         */
        void SetUp() override
        {
            m_connection = std::make_unique<SqliteConnection>(ConnectionConfig::sqliteDefault());
            ASSERT_TRUE(m_connection->connect()) << m_connection->lastError();

            ASSERT_NE(executeRequired(*m_connection, kCreateUsersTableSql), nullptr);
            ASSERT_NE(executeRequired(*m_connection, kCreateTypedTableSql), nullptr);
            ASSERT_NE(executeRequired(*m_connection, "INSERT INTO typedValues (asDate, asNumeric) VALUES ('2026-09-12', 'n/a')"),
                      nullptr);

            for (const char *seedCommand: kSeedUsersSql)
            {
                ASSERT_NE(executeRequired(*m_connection, seedCommand), nullptr) << "样本数据初始化失败：" << seedCommand;
            }
        }

        /**
         * @brief 先销毁结果集再销毁连接，遵守「结果集严格早于连接」的生命周期约束
         */
        void TearDown() override
        {
            m_connection.reset();
        }

        /**
         * @brief 取夹具持有的连接引用
         * @return SqliteConnection & 已灌入样本数据的连接
         */
        [[nodiscard]] SqliteConnection &connection() const
        {
            return *m_connection;
        }

        /**
         * @brief 执行一条查询并把结果集交给用例
         * @param command SQL 文本
         * @return std::unique_ptr<DatabaseResult> 结果集
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> query(const std::string_view command) const
        {
            return executeRequired(*m_connection, command);
        }

        /**
         * @brief 按 id 顺序读出 users.name 一列，供遍历与重扫类用例复用
         * @param result 已按 id 排序的结果集，函数返回时游标已被走完
         * @return std::vector<std::string> 每行的名字，NULL 以空串占位
         */
        [[nodiscard]] static std::vector<std::string> collectNames(DatabaseResult &result)
        {
            std::vector<std::string> names;
            while (result.next())
            {
                const std::optional<std::string> name = asText(result.getValue(std::size_t{1}));
                names.push_back(name.value_or(std::string{}));
            }
            return names;
        }

        std::unique_ptr<SqliteConnection> m_connection; ///< 已连接并灌入样本数据的内存库连接
    };

    // ============================================================================
    // 写回执与列元数据：不依赖样本数据
    // ============================================================================

    TEST(SqliteResult, WriteReceiptCarriesNoCursorOrColumnMetadata)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "CREATE TABLE notes (id INTEGER PRIMARY KEY, title TEXT)");
        ASSERT_NE(result, nullptr);

        // 写语句没有返回列：驱动交出的是「执行成功但为空」的回执，退化形态必须处处一致
        SqliteResult *receipt = asSqliteResult(*result);
        ASSERT_NE(receipt, nullptr);
        EXPECT_EQ(receipt->nativeHandle(), nullptr);
        EXPECT_EQ(receipt->columnCount(), 0u);
        EXPECT_EQ(receipt->rowCount(), 0u);
        EXPECT_TRUE(receipt->isEmpty());
        EXPECT_TRUE(receipt->columnNames().empty());
        EXPECT_FALSE(receipt->columnName(std::size_t{0}).has_value());
        EXPECT_FALSE(receipt->columnIndex("id").has_value());
        EXPECT_FALSE(receipt->next());
        EXPECT_TRUE(isMissingValue(receipt->getValue(std::size_t{0})));
    }

    TEST(SqliteResult, QueryMetadataIsSelfConsistent)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 AS oneValue, 2 AS twoValue");
        ASSERT_NE(result, nullptr);

        // 列数、列名表与两个方向的索引查询必须互相对得上
        EXPECT_EQ(result->columnCount(), 2u);
        EXPECT_EQ(result->columnNames(), std::vector<std::string>({"oneValue", "twoValue"}));
        EXPECT_EQ(result->columnName(std::size_t{0}), std::optional<std::string>("oneValue"));
        EXPECT_EQ(result->columnName(std::size_t{1}), std::optional<std::string>("twoValue"));
        EXPECT_EQ(result->columnIndex("twoValue"), std::optional<std::size_t>(1));
        EXPECT_FALSE(result->columnName(std::size_t{2}).has_value()) << "columnCount 处即越界";

        // execute() 交出的动态类型必须是 SqliteResult，且查询结果持有真实游标（与写回执相对）
        SqliteResult *queryResult = asSqliteResult(*result);
        ASSERT_NE(queryResult, nullptr);
        EXPECT_NE(queryResult->nativeHandle(), nullptr);
    }

    TEST(SqliteResult, ValueReadBeforeFirstStepIsMissing)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 AS oneValue");
        ASSERT_NE(result, nullptr);

        // 预扫描已把游标退回首行之前：没有 next() 就没有当前行，两个重载都不许读到残值
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{0})));
        EXPECT_TRUE(isMissingValue(result->getValue("oneValue")));

        ASSERT_TRUE(result->next());
        EXPECT_EQ(asInteger(result->getValue(std::size_t{0})), std::optional<std::int64_t>(1));
    }

    TEST(SqliteResult, OutOfRangeIndexAndUnknownNameReadAsMissingValue)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 AS oneValue");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // 旧实现把 size_t 索引强转成 int 再比较，SIZE_MAX 会回绕成 -1 绕过边界检查
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{1})));
        EXPECT_TRUE(isMissingValue(result->getValue(std::numeric_limits<std::size_t>::max())));
        EXPECT_TRUE(isMissingValue(result->getValue("absentColumn")));
        EXPECT_FALSE(result->columnName(std::numeric_limits<std::size_t>::max()).has_value());
    }

    TEST(SqliteResult, ColumnIndexMatchingIsExactAndRejectsEmptyName)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 AS oneValue, 2 AS twoValue");
        ASSERT_NE(result, nullptr);

        // 列名比对区分大小写（与 SQLite 给出的别名原文一致），空名字一律视为不存在
        EXPECT_EQ(result->columnIndex("oneValue"), std::optional<std::size_t>(0));
        EXPECT_FALSE(result->columnIndex("ONEVALUE").has_value());
        EXPECT_FALSE(result->columnIndex("onevalu").has_value());
        EXPECT_FALSE(result->columnIndex("").has_value());
    }

    TEST(SqliteResult, DuplicateColumnNamesResolveToFirstIndex)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 AS amount, 2 AS amount");
        ASSERT_NE(result, nullptr);

        // 同名列先到先得：按名取值与 SQLite 自身一致，拿到的永远是第一列
        EXPECT_EQ(result->columnNames(), std::vector<std::string>({"amount", "amount"}));
        EXPECT_EQ(result->columnIndex("amount"), std::optional<std::size_t>(0));
        ASSERT_TRUE(result->next());
        EXPECT_EQ(asInteger(result->getValue("amount")), std::optional<std::int64_t>(1));
        EXPECT_EQ(asInteger(result->getValue(std::size_t{1})), std::optional<std::int64_t>(2));
    }

    TEST(SqliteResult, ExpressionColumnKeepsItsSlotInColumnNames)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 + 1");
        ASSERT_NE(result, nullptr);

        // 表达式列可能没有名字，实现选择补空串而不是丢弃，下标才与列序严格对齐
        EXPECT_EQ(result->columnCount(), 1u);
        ASSERT_EQ(result->columnNames().size(), 1u);
        ASSERT_TRUE(result->next());
        EXPECT_EQ(asInteger(result->getValue(std::size_t{0})), std::optional<std::int64_t>(2));
    }

    // ============================================================================
    // 游标推进、预扫描与 reset
    // ============================================================================

    TEST_F(SqliteUserQuery, RowCountIsExactForReadOnlyQuery)
    {
        const std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        // 只读语句在构造时就完成预扫描：还没 next() 也能拿到精确行数与「非空」判定
        EXPECT_EQ(result->rowCount(), 4u);
        EXPECT_FALSE(result->isEmpty());
        EXPECT_EQ(result->columnCount(), 6u);
    }

    TEST_F(SqliteUserQuery, EmptyResultHasColumnsButNoRows)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM users WHERE 1 = 0");
        ASSERT_NE(result, nullptr);

        // 空集不等于「没有列」：元数据照常，游标一步都迈不出去
        EXPECT_TRUE(result->isEmpty());
        EXPECT_EQ(result->rowCount(), 0u);
        EXPECT_EQ(result->columnCount(), 2u);
        EXPECT_FALSE(result->next());
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{0})));
    }

    TEST_F(SqliteUserQuery, MetadataSnapshotSurvivesFullScan)
    {
        std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        static_cast<void>(collectNames(*result));

        // 列数与行数都是构造期快照，不随游标推进或耗尽而改变
        EXPECT_EQ(result->rowCount(), 4u);
        EXPECT_EQ(result->columnCount(), 6u);
        EXPECT_FALSE(result->isEmpty());
        EXPECT_FALSE(result->next());
        EXPECT_EQ(result->rowCount(), 4u);
    }

    TEST_F(SqliteUserQuery, ResetAllowsSecondFullScan)
    {
        std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        const std::vector<std::string> firstScan = collectNames(*result);
        ASSERT_EQ(firstScan.size(), 4u);

        result->reset();
        const std::vector<std::string> secondScan = collectNames(*result);

        // 重扫得到逐行相同的内容，说明 reset 既退回首行之前也没有丢掉任何行
        EXPECT_EQ(secondScan, firstScan);
    }

    TEST_F(SqliteUserQuery, ResetInvalidatesCurrentRow)
    {
        std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        ASSERT_TRUE(result->next());
        ASSERT_FALSE(isMissingValue(result->getValue(std::size_t{1})));

        result->reset();

        // 游标退回首行之前，「当前行有效」标志必须一起清掉，否则会读到已失效的列值
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{1})));
        EXPECT_TRUE(isMissingValue(result->getValue("name")));

        ASSERT_TRUE(result->next());
        EXPECT_EQ(asText(result->getValue("name")), std::optional<std::string>("Alice"));
    }

    TEST_F(SqliteUserQuery, ResetOnWriteReceiptIsSafeNoOp)
    {
        std::unique_ptr<DatabaseResult> result = query("CREATE TABLE scratch (id INTEGER)");
        ASSERT_NE(result, nullptr);

        // 没有游标的回执 reset() 什么都不做，也不该报错
        result->reset();
        result->reset();

        EXPECT_FALSE(result->next());
        EXPECT_TRUE(result->isEmpty());
        EXPECT_TRUE(result->lastError().empty()) << result->lastError();
    }

    TEST_F(SqliteUserQuery, ExhaustedCursorReadsAsMissingValue)
    {
        std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        while (result->next())
        {
            static_cast<void>(result->getValue(std::size_t{0}));
        }

        // 耗尽后再读一律 monostate，且继续 next() 也不会重新从头开始
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{0})));
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{5})));
        EXPECT_FALSE(result->next());
    }

    TEST_F(SqliteUserQuery, ReadOnlyAccessorsNeverRecordAnError)
    {
        std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        // 基类契约：next / getValue / 元数据查询都属只读路径，不得改写错误状态
        static_cast<void>(result->next());
        static_cast<void>(result->getValue(std::size_t{0}));
        static_cast<void>(result->getValue("absentColumn"));
        static_cast<void>(result->columnName(std::size_t{99}));
        static_cast<void>(result->columnIndex("absentColumn"));
        static_cast<void>(result->columnNames());
        static_cast<void>(result->isEmpty());
        static_cast<void>(result->rowCount());
        // 一路走到耗尽：next() 返回 false 属正常结束，同样不得写错误文本
        while (result->next())
        {
        }

        EXPECT_TRUE(result->lastError().empty()) << result->lastError();
    }

    TEST_F(SqliteUserQuery, ResultDestroyedMidScanLeavesConnectionUsable)
    {
        {
            std::unique_ptr<DatabaseResult> abandoned = query(kSelectUsersInIdOrderSql);
            ASSERT_NE(abandoned, nullptr);
            ASSERT_TRUE(abandoned->next());
            // 只走了一行就离开作用域：析构必须 finalize，把读锁与语句资源交还连接
        }

        EXPECT_TRUE(connection().isConnected());
        const std::unique_ptr<DatabaseResult> secondChance = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(secondChance, nullptr);
        EXPECT_EQ(secondChance->rowCount(), 4u);
        EXPECT_TRUE(connection().lastError().empty()) << connection().lastError();
    }

    // ============================================================================
    // 存储类到 DatabaseValue 的映射
    // ============================================================================

    TEST_F(SqliteUserQuery, IntegerValuesKeepFullSignedRange)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT age FROM users WHERE id = 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // INTEGER 一律收进 64 位有符号整数，不做任何收窄
        EXPECT_EQ(asInteger(result->getValue(std::size_t{0})), std::optional<std::int64_t>(30));

        const std::unique_ptr<DatabaseResult> boundary = query("SELECT -9223372036854775807 AS negativeValue, 0 AS zeroValue");
        ASSERT_NE(boundary, nullptr);
        ASSERT_TRUE(boundary->next());
        EXPECT_EQ(asInteger(boundary->getValue("negativeValue")), std::optional<std::int64_t>(-9223372036854775807LL));
        EXPECT_EQ(asInteger(boundary->getValue("zeroValue")), std::optional<std::int64_t>(0));
    }

    TEST_F(SqliteUserQuery, RealColumnMapsToDouble)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT score FROM users WHERE id = 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // FLOAT 存储类落到 double，不能退化成整数或文本
        const std::optional<double> score = asReal(result->getValue(std::size_t{0}));
        ASSERT_TRUE(score.has_value()) << databaseValueTypeName(result->getValue(std::size_t{0}));
        EXPECT_DOUBLE_EQ(score.value(), 95.5);
    }

    TEST_F(SqliteUserQuery, TextColumnMapsToString)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT name FROM users WHERE id = 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        const std::optional<std::string> name = asText(result->getValue(std::size_t{0}));
        ASSERT_TRUE(name.has_value()) << databaseValueTypeName(result->getValue(std::size_t{0}));
        EXPECT_EQ(name.value(), "Alice");
    }

    TEST_F(SqliteUserQuery, UnicodeTextKeepsEveryByte)
    {
        const std::string insertStatement = std::string("INSERT INTO users (name) VALUES ('") + kUnicodeName + "')";
        ASSERT_NE(executeRequired(connection(), insertStatement), nullptr);

        const std::unique_ptr<DatabaseResult> result = query("SELECT name FROM users ORDER BY id DESC LIMIT 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // UTF-8 中文每字三字节，驱动按 sqlite3_column_bytes 的长度拷贝才不会截断
        const DatabaseValue stored = result->getValue(std::size_t{0});
        ASSERT_TRUE(std::holds_alternative<std::string>(stored)) << databaseValueTypeName(stored);
        EXPECT_EQ(std::get<std::string>(stored), std::string(kUnicodeName));
    }

    TEST_F(SqliteUserQuery, NullColumnMapsToMissingValue)
    {
        // Carol 只写了 name，age / score / payload 三列都是 NULL
        const std::unique_ptr<DatabaseResult> result = query("SELECT age, score, payload FROM users WHERE id = 3");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        for (const std::size_t columnIndex: {std::size_t{0}, std::size_t{1}, std::size_t{2}})
        {
            EXPECT_TRUE(isMissingValue(result->getValue(columnIndex))) << "列 " << columnIndex << " 应为 NULL";
        }
    }

    TEST_F(SqliteUserQuery, EmptyTextAndEmptyBlobAreNotReportedAsNull)
    {
        // 第 4 行的 name 是空串、payload 是零长 BLOB：两者都是「有值且值为空」，不能塌成 monostate
        const std::unique_ptr<DatabaseResult> result = query("SELECT name, payload FROM users WHERE id = 4");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        const DatabaseValue emptyName   = result->getValue(std::size_t{0});
        const DatabaseValue emptyPayload = result->getValue(std::size_t{1});

        ASSERT_TRUE(std::holds_alternative<std::string>(emptyName)) << databaseValueTypeName(emptyName);
        EXPECT_TRUE(std::get<std::string>(emptyName).empty());
        ASSERT_TRUE(std::holds_alternative<std::string>(emptyPayload)) << databaseValueTypeName(emptyPayload);
        EXPECT_TRUE(std::get<std::string>(emptyPayload).empty());
    }

    TEST_F(SqliteUserQuery, BlobKeepsEmbeddedNullByte)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT payload FROM users WHERE id = 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // x'5c0041' 是 0x5C、0x00、0x41 三个字节：按长度拷贝才保得住中间那个 '\0'
        const std::optional<std::string> payload = asText(result->getValue(std::size_t{0}));
        ASSERT_TRUE(payload.has_value()) << databaseValueTypeName(result->getValue(std::size_t{0}));
        ASSERT_EQ(payload->size(), 3u);
        EXPECT_EQ(static_cast<unsigned char>((*payload)[0]), 0x5C);
        EXPECT_EQ(static_cast<unsigned char>((*payload)[1]), 0x00);
        EXPECT_EQ(static_cast<unsigned char>((*payload)[2]), 0x41);
    }

    TEST_F(SqliteUserQuery, BooleanFlagReadsAsIntegerNotBool)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT id, active FROM users ORDER BY id");
        ASSERT_NE(result, nullptr);

        ASSERT_TRUE(result->next());
        EXPECT_EQ(asInteger(result->getValue(std::size_t{1})), std::optional<std::int64_t>(1));
        ASSERT_TRUE(result->next());
        EXPECT_EQ(asInteger(result->getValue(std::size_t{1})), std::optional<std::int64_t>(0));

        // SQLite 没有布尔存储类，0/1 只能是 int64_t，由调用方自行收窄
        EXPECT_FALSE(std::holds_alternative<bool>(result->getValue(std::size_t{1})));
    }

    TEST_F(SqliteUserQuery, NumericAndDateColumnsReadAsText)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT asDate, asNumeric FROM typedValues");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // 声明类型是 DATE / NUMERIC，但存进去的是无法无损转换的文本，SQLite 给的存储类就是 TEXT
        const std::optional<std::string> dateText = asText(result->getValue(std::size_t{0}));
        ASSERT_TRUE(dateText.has_value()) << databaseValueTypeName(result->getValue(std::size_t{0}));
        EXPECT_EQ(dateText.value(), "2026-09-12");

        const std::optional<std::string> numericText = asText(result->getValue(std::size_t{1}));
        ASSERT_TRUE(numericText.has_value()) << databaseValueTypeName(result->getValue(std::size_t{1}));
        EXPECT_EQ(numericText.value(), "n/a");
    }

    TEST_F(SqliteUserQuery, ValueByIndexMatchesValueByName)
    {
        const std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // 两条取值路径共用同一份边界与「无当前行」判定，逐列比对必须完全一致
        const std::vector<std::string> names = result->columnNames();
        ASSERT_EQ(names.size(), result->columnCount());
        for (std::size_t columnIndex = 0; columnIndex < names.size(); ++columnIndex)
        {
            const DatabaseValue byIndex = result->getValue(columnIndex);
            const DatabaseValue byName  = result->getValue(names[columnIndex]);
            EXPECT_EQ(std::holds_alternative<std::monostate>(byIndex), std::holds_alternative<std::monostate>(byName))
                    << "第 " << columnIndex << " 列 " << names[columnIndex];
            EXPECT_EQ(asText(byIndex), asText(byName)) << names[columnIndex];
            EXPECT_EQ(asInteger(byIndex), asInteger(byName)) << names[columnIndex];
            EXPECT_EQ(asReal(byIndex), asReal(byName)) << names[columnIndex];
        }

        EXPECT_EQ(asInteger(result->getValue(std::size_t{0})), std::optional<std::int64_t>(1));
        EXPECT_EQ(asInteger(result->getValue("id")), std::optional<std::int64_t>(1));
    }

    // ============================================================================
    // 影响行数与 rowid 快照
    // ============================================================================

    TEST_F(SqliteUserQuery, AffectedRowCountFollowsTheStatementThatWroteRows)
    {
        std::unique_ptr<DatabaseResult> singleInsert = query("INSERT INTO users (name) VALUES ('Dan')");
        ASSERT_NE(singleInsert, nullptr);
        const SqliteResult *singleInsertReceipt = asSqliteResult(*singleInsert);
        ASSERT_NE(singleInsertReceipt, nullptr);
        EXPECT_EQ(singleInsertReceipt->affectedRowCount(), 1);

        std::unique_ptr<DatabaseResult> updateAll = query("UPDATE users SET age = age + 1");
        ASSERT_NE(updateAll, nullptr);
        const SqliteResult *updateReceipt = asSqliteResult(*updateAll);
        ASSERT_NE(updateReceipt, nullptr);
        // UPDATE 统计所有匹配并被写入的行，与值是否真的改变无关：五名用户全部被改写
        EXPECT_EQ(updateReceipt->affectedRowCount(), 5);

        std::unique_ptr<DatabaseResult> ignoredInsert = query("INSERT OR IGNORE INTO users (id, name) VALUES (1, 'Duplicated')");
        ASSERT_NE(ignoredInsert, nullptr);
        const SqliteResult *ignoredReceipt = asSqliteResult(*ignoredInsert);
        ASSERT_NE(ignoredReceipt, nullptr);
        // 被 IGNORE 放弃的写入没有影响任何行，计数必须归零
        EXPECT_EQ(ignoredReceipt->affectedRowCount(), 0);
    }

    TEST_F(SqliteUserQuery, WriteReceiptSnapshotsAffectedRowsOfItsOwnStatement)
    {
        // 删除全部四行：影响行数必须在构造这一刻快照，之后的写入不会回来改写它
        const std::unique_ptr<DatabaseResult> deletion = query("DELETE FROM users");
        ASSERT_NE(deletion, nullptr);
        const SqliteResult *deletionReceipt = asSqliteResult(*deletion);
        ASSERT_NE(deletionReceipt, nullptr);
        EXPECT_EQ(deletionReceipt->affectedRowCount(), 4);

        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Later')"), nullptr);
        EXPECT_EQ(deletionReceipt->affectedRowCount(), 4) << "结果集快照不该反映后来的写入";
    }

    TEST_F(SqliteUserQuery, LastInsertRowIdSnapshotBelongsToItsOwnStatement)
    {
        const std::unique_ptr<DatabaseResult> firstInsert = query("INSERT INTO users (name) VALUES ('Eve')");
        ASSERT_NE(firstInsert, nullptr);
        const SqliteResult *firstReceipt = asSqliteResult(*firstInsert);
        ASSERT_NE(firstReceipt, nullptr);

        // 样本四行占了 1..4，本次插入是 5
        EXPECT_EQ(firstReceipt->lastInsertRowId(), 5);
        EXPECT_EQ(connection().lastInsertRowId(), 5);

        const std::unique_ptr<DatabaseResult> secondInsert = query("INSERT INTO users (name) VALUES ('Fay')");
        ASSERT_NE(secondInsert, nullptr);

        // 连接级计数器继续前进，但先前的结果集快照不跟着变——两者语义不同，不能混用
        EXPECT_EQ(firstReceipt->lastInsertRowId(), 5);
        EXPECT_EQ(asSqliteResult(*secondInsert)->lastInsertRowId(), 6);
        EXPECT_EQ(connection().lastInsertRowId(), 6);
    }

    TEST_F(SqliteUserQuery, QueryResultCountersComeFromThePreviousWrite)
    {
        ASSERT_NE(executeRequired(connection(), "UPDATE users SET age = age + 1"), nullptr);
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Gus')"), nullptr);

        const std::unique_ptr<DatabaseResult> selection = query("SELECT id FROM users");
        ASSERT_NE(selection, nullptr);

        // SQLite 没有语句级历史，查询结果上的两个计数就是「连接上一条写语句」的计数
        const SqliteResult *selectionResult = asSqliteResult(*selection);
        ASSERT_NE(selectionResult, nullptr);
        EXPECT_EQ(selectionResult->affectedRowCount(), 1);
        EXPECT_EQ(selectionResult->lastInsertRowId(), 5);
    }

    TEST_F(SqliteUserQuery, InsertReturningRowIsNotPrescanned)
    {
        ASSERT_NE(executeRequired(connection(), "UPDATE users SET active = 1"), nullptr);

        const std::unique_ptr<DatabaseResult> result = query("INSERT INTO users (name) VALUES ('Hank') RETURNING id, name");
        ASSERT_NE(result, nullptr);

        // 带写副作用的语句绝不重复执行：预扫描会把数据改两遍，因此 rowCount() 只能是 0
        EXPECT_EQ(result->rowCount(), 0u);
        EXPECT_FALSE(result->isEmpty()) << "未预扫描时按「可能有行」处理，真实有没有行由 next() 决定";
        EXPECT_EQ(result->columnCount(), 2u);

        // 首次 next() 才是语句真正执行的那一刻，此时连接级计数器才被刷新
        const SqliteResult *returningResult = asSqliteResult(*result);
        ASSERT_NE(returningResult, nullptr);
        EXPECT_EQ(returningResult->affectedRowCount(), 4) << "构造时语句尚未 step，快照的是上一条写入";

        ASSERT_TRUE(result->next());
        EXPECT_EQ(asText(result->getValue("name")), std::optional<std::string>("Hank"));
        EXPECT_EQ(asInteger(result->getValue("id")), std::optional<std::int64_t>(connection().lastInsertRowId()));
        EXPECT_FALSE(result->next());
    }

} // namespace AsynGyanis::Database
