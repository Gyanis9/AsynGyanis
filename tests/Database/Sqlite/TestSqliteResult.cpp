// SqliteResult 单元测试：游标与预扫描语义、列元数据、存储类到 DatabaseValue 的映射与写回执快照。
// 结果集只能由 execute() 交出，因此全部用例跑在真实内存库上（建表灌样本、经 execute() 取回），零外部服务。
// 钉住的契约：只有只读语句会被预扫描（rowCount() 对只读查询精确、对写语句与 INSERT ... RETURNING 返回 0）；列值必须
// next() 之后读取（未 next()、游标耗尽、reset() 后一律 std::monostate）；存储类映射 NULL→monostate、INTEGER→int64_t、
// FLOAT→double、TEXT→std::string、BLOB→BinaryBytes（零长非 NULL）；越界用无符号比较；RETURNING 需 SQLite 3.35+。
// takeValue()（交出所有权的读值通道）在上限两侧各钉一条与 getValue 逐字比对，并钉住「无当前行/越界仍返回 monostate」。

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Sqlite/SqliteConnection.h"
#include "Database/Sqlite/SqliteResult.h"

#include "DatabaseTestSupport.h"

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
    using TestSupport::asInteger;
    using TestSupport::asText;
    using TestSupport::executeRequired;

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
         * @brief 把 execute() 交出的基类结果集还原成 SQLite 派生类型
         * @details lastInsertRowId() / nativeHandle() 是 SQLite 专有接口，
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
         * @brief 安全取出二进制列值
         * @param value 待判定的数据库值
         * @return std::optional<BinaryBytes> 类型不符时返回空值
         */
        std::optional<BinaryBytes> asBytes(const DatabaseValue &value)
        {
            const auto *bytes = std::get_if<BinaryBytes>(&value);
            return bytes == nullptr ? std::nullopt : std::optional<BinaryBytes>(*bytes);
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

        /**
         * @brief 另建一张 bulkRows 表并灌入指定行数（id 从 1 起，name 形如 row-<id>）
         * @details 与夹具自带的四行样本分开，行数由用例给定，用来覆盖行值快照上限的两侧
         * @param rowCount 要写入的行数
         */
        void seedBulkRows(const size_t rowCount)
        {
            ASSERT_NE(executeRequired(connection(), "CREATE TABLE bulkRows (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr);

            const std::string seedStatement = "WITH RECURSIVE counter(rowId) AS (SELECT 1 UNION ALL SELECT rowId + 1 FROM counter WHERE rowId < " +
                                              std::to_string(rowCount) + ") INSERT INTO bulkRows (id, name) SELECT rowId, 'row-' || rowId FROM counter";
            ASSERT_NE(executeRequired(connection(), seedStatement), nullptr) << "批量样本写入失败：" << seedStatement;
        }

        /**
         * @brief 生成 bulkRows 的期望名字序列，供整表比对
         * @param rowCount 行数
         * @return std::vector<std::string> 依次为 row-1 .. row-<rowCount>
         */
        [[nodiscard]] static std::vector<std::string> expectedBulkNames(const size_t rowCount)
        {
            std::vector<std::string> names;
            names.reserve(rowCount);
            for (size_t row = 1; row <= rowCount; ++row)
            {
                names.push_back("row-" + std::to_string(row));
            }
            return names;
        }

        std::unique_ptr<SqliteConnection> m_connection; ///< 已连接并灌入样本数据的内存库连接
    };

    // ============================================================================
    // 写回执与列元数据：不依赖样本数据
    // ============================================================================

    /** @brief 钉住写回执的退化形态处处一致：无游标、零列、取值一律 monostate */
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

    /** @brief 钉住列数/列名表/双向索引互相对得上，且查询结果持有真实游标 */
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

        // execute() 交出的动态类型必须是 SqliteResult；查询结果若已把行整份物化，游标就在
        // 构造期被交还语句缓存，nativeHandle() 如实给出 nullptr（写回执同样是 nullptr）
        SqliteResult *queryResult = asSqliteResult(*result);
        ASSERT_NE(queryResult, nullptr);
        EXPECT_EQ(queryResult->nativeHandle(), nullptr) << "行已物化的快照结果不该再持有游标";
    }

    /** @brief 钉住 next() 之前没有当前行：两个取值重载都读不到残值 */
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

    /** @brief 钉住越界索引与未知列名一律按「没有值」返回，索引比较走无符号防回绕 */
    TEST(SqliteResult, OutOfRangeIndexAndUnknownNameReadAsMissingValue)
    {
        const std::unique_ptr<SqliteConnection> connection = openMemoryConnection();

        std::unique_ptr<DatabaseResult> result = executeRequired(*connection, "SELECT 1 AS oneValue");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // 索引必须按无符号比较：强转成 int 会让SIZE_MAX 会回绕成 -1 绕过边界检查
        EXPECT_TRUE(isMissingValue(result->getValue(std::size_t{1})));
        EXPECT_TRUE(isMissingValue(result->getValue(std::numeric_limits<std::size_t>::max())));
        EXPECT_TRUE(isMissingValue(result->getValue("absentColumn")));
        EXPECT_FALSE(result->columnName(std::numeric_limits<std::size_t>::max()).has_value());
    }

    /** @brief 钉住列名比对区分大小写，空名字一律视为不存在 */
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

    /** @brief 钉住同名列先到先得：按名取值命中第一列 */
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

    /** @brief 钉住无名表达式列补空串占位，下标与列序严格对齐 */
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

    /** @brief 钉住只读查询构造期完成预扫描：未 next() 也有精确行数与非空判定 */
    TEST_F(SqliteUserQuery, RowCountIsExactForReadOnlyQuery)
    {
        const std::unique_ptr<DatabaseResult> result = query(kSelectUsersInIdOrderSql);
        ASSERT_NE(result, nullptr);

        // 只读语句在构造时就完成预扫描：还没 next() 也能拿到精确行数与「非空」判定
        EXPECT_EQ(result->rowCount(), 4u);
        EXPECT_FALSE(result->isEmpty());
        EXPECT_EQ(result->columnCount(), 6u);
    }

    /**
     * @brief 钉住行数正好等于快照上限时，遍历交出的仍是完整且按序的全部行
     */
    TEST_F(SqliteUserQuery, FullIterationIsCompleteAtTheSnapshotRowLimit)
    {
        seedBulkRows(SqliteResult::kMaximumMaterializedRowCount);

        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->rowCount(), SqliteResult::kMaximumMaterializedRowCount);
        EXPECT_EQ(collectNames(*result), expectedBulkNames(SqliteResult::kMaximumMaterializedRowCount));
    }

    /**
     * @brief 钉住行数刚超过快照上限时退回游标遍历：行数仍精确，一行都不丢
     * @details 快照只存了前若干行，若把「存下的行数」当成结果集规模交出去，调用方会静默读到
     *          一份被截断的数据——这条用例正是为拦住那个错而存在，它同时也是「超限即作废快照」
     *          那条判据的证伪入口
     */
    TEST_F(SqliteUserQuery, FullIterationIsCompleteJustAboveTheSnapshotRowLimit)
    {
        const size_t rowCount = SqliteResult::kMaximumMaterializedRowCount + 1;
        seedBulkRows(rowCount);

        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->rowCount(), rowCount) << "超过快照上限的预扫描仍要给出精确行数";
        EXPECT_EQ(collectNames(*result), expectedBulkNames(rowCount));
    }

    /**
     * @brief 按「交出所有权」的读值通道整表取名（与 collectNames 唯一差别就是用 takeValue）
     * @param result 结果集，游标从头推进
     * @return std::vector<std::string> 依次为每行第 1 列的文本值
     */
    std::vector<std::string> collectNamesByTakeValue(DatabaseResult &result)
    {
        std::vector<std::string> names;
        while (result.next())
        {
            // 每格只取一次——这正是 takeValue 允许搬空源的前提，也是 ORM 逐列映射的实际读法
            names.push_back(std::get<std::string>(result.takeValue(1)));
        }
        return names;
    }

    /**
     * @brief 验证快照行与游标行两侧都交出同样的取值
     *
     * @details takeValue() 有两条出口：物化行搬快照里的缓冲、非物化行退回当场构造的取值。
     *          行距公式或回退条件写错时只有其中一侧会错，因此上限两侧各钉一条，并与既有的
     *          getValue 路径比对同一份期望序列（整表逐行，不只看条数）。
     */
    TEST_F(SqliteUserQuery, TakeValueServesEveryRowOfAMaterializedSnapshot)
    {
        seedBulkRows(SqliteResult::kMaximumMaterializedRowCount);

        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(collectNamesByTakeValue(*result), expectedBulkNames(SqliteResult::kMaximumMaterializedRowCount))
                << "物化快照路径搬错了格子（行距或列距）";
    }

    /**
     * @brief 验证超出快照上限、退回游标遍历的行，takeValue 与 getValue 逐字一致
     * @details 这条钉的是「回退分支」：漏掉它会让超限结果的 takeValue 拿 (游标 - 1) 去索引一份
     *          根本不存在的快照。
     */
    TEST_F(SqliteUserQuery, TakeValueFallsBackToTheCursorBeyondTheSnapshotLimit)
    {
        const size_t cursorRowCount = SqliteResult::kMaximumMaterializedRowCount + 1;
        seedBulkRows(cursorRowCount);

        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(collectNamesByTakeValue(*result), expectedBulkNames(cursorRowCount))
                << "退回游标遍历的那一侧必须与快照路径逐字一致";
    }

    /**
     * @brief 验证 takeValue 的边界行为与 getValue 一致：无当前行与越界下标都读成「无值」
     *
     * @details 搬空只发生在快照路径的正常取值上；两条护栏（游标未就位、下标越界）一旦在重写里被
     *          漏掉，就会拿 (游标 - 1) 去索引负偏移的快照，属于越界读而不是「返回空」。
     */
    TEST_F(SqliteUserQuery, TakeValueKeepsTheMissingValueBehaviorOutsideACurrentRow)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM users ORDER BY id");
        ASSERT_NE(result, nullptr);

        // 还没 next()：游标不在任何行上，SQLite 的列读取接口此时属于未定义行为
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->takeValue(0)));

        ASSERT_TRUE(result->next());
        // 越界下标按「无值」返回：不能把 size_t 下标强转成 int 再比较，那会在 SIZE_MAX 上回绕绕过检查
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->takeValue(result->columnCount())));
        // 同一条语句里的正常取值仍然给得出值（说明上面的拒绝没有连带打断游标路径）
        EXPECT_FALSE(std::holds_alternative<std::monostate>(result->takeValue(0)));
    }

    /**
     * @brief 钉住快照结果集 reset() 之后能再完整遍历一遍，两遍逐行一致
     */
    TEST_F(SqliteUserQuery, ResetAllowsSecondFullScanOfSnapshotRows)
    {
        seedBulkRows(SqliteResult::kMaximumMaterializedRowCount);

        const std::unique_ptr<DatabaseResult> result = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(result, nullptr);

        const std::vector<std::string> firstScan = collectNames(*result);
        ASSERT_EQ(firstScan, expectedBulkNames(SqliteResult::kMaximumMaterializedRowCount));

        result->reset();
        // 第二遍与第一遍逐行相同：重遍历不重新执行查询，也不会从中间某行接着读
        EXPECT_EQ(collectNames(*result), firstScan);
    }

    /**
     * @brief 钉住同一条查询重复执行时，交还缓存的游标被再次用上且结果逐行不变
     * @details 交还与复用之间游标经历了「reset 回表 → 再借出 → 再跑一遍」，中间任何一次
     *          收尾没做对都会在第二遍上显形（少行、残值或直接被 ASan 抓到释放后使用）
     */
    TEST_F(SqliteUserQuery, RepeatingTheSameQueryReusesTheReturnedCursor)
    {
        seedBulkRows(SqliteResult::kMaximumMaterializedRowCount);

        const std::unique_ptr<DatabaseResult> firstResult = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(firstResult, nullptr);
        const std::vector<std::string> firstScan = collectNames(*firstResult);

        // 第一份结果还活着（游标已交还缓存），第二条同样的查询要能从表里拿到那条可重跑的游标
        const std::unique_ptr<DatabaseResult> secondResult = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(secondResult, nullptr) << connection().lastError();
        EXPECT_EQ(collectNames(*secondResult), firstScan);

        // 交还之后第一份结果仍能重遍历：它读的是快照，与游标此刻在谁手里无关
        firstResult->reset();
        EXPECT_EQ(collectNames(*firstResult), firstScan);
    }

    /**
     * @brief 钉住行数超过快照上限时，两份同时打开的结果集各握一条游标、互不借用
     * @details 这种结果集要 finalize 自己那条游标，因此它绝不能与缓存里同文本的那条混用，
     *          否则先析构的一份会把另一份正在用的游标一起释放掉
     */
    TEST_F(SqliteUserQuery, TwoOpenOverLimitResultsEachKeepTheirOwnCursor)
    {
        const size_t rowCount = SqliteResult::kMaximumMaterializedRowCount + 1;
        seedBulkRows(rowCount);

        const std::unique_ptr<DatabaseResult> firstResult = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(firstResult, nullptr);
        const std::unique_ptr<DatabaseResult> secondResult = query("SELECT id, name FROM bulkRows ORDER BY id");
        ASSERT_NE(secondResult, nullptr) << connection().lastError();

        // 两份都要交出完整 257 行：共享游标的话第二份会从头重跑、第一份读到错位或已释放的行
        const std::vector<std::string> expected = expectedBulkNames(rowCount);
        EXPECT_EQ(collectNames(*firstResult), expected);
        EXPECT_EQ(collectNames(*secondResult), expected);
    }

    /** @brief 钉住空集仍有列元数据，游标一步都迈不出去 */
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

    /** @brief 钉住列数与行数是构造期快照，不随游标推进或耗尽改变 */
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

    /** @brief 钉住 reset 支持完整重扫，逐行内容与首次一致 */
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

    /** @brief 钉住 reset 同时清掉「当前行有效」标志，两个取值路径都失效 */
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

    /** @brief 钉住无游标的写回执上 reset 是安全空操作 */
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

    /** @brief 钉住游标耗尽后读值一律 monostate，且 next() 不会重新开始 */
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

    /** @brief 钉住只读访问路径不改写错误状态，正常耗尽也不算失败 */
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

    /** @brief 钉住结果集提前销毁会 finalize，读锁与语句资源归还连接 */
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

    /** @brief 钉住 INTEGER 落 int64 不做收窄，含边界负值与 0 */
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

    /** @brief 钉住 FLOAT 存储类落 double，不退化成整数或文本 */
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

    /** @brief 钉住 TEXT 存储类落 std::string */
    TEST_F(SqliteUserQuery, TextColumnMapsToString)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT name FROM users WHERE id = 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        const std::optional<std::string> name = asText(result->getValue(std::size_t{0}));
        ASSERT_TRUE(name.has_value()) << databaseValueTypeName(result->getValue(std::size_t{0}));
        EXPECT_EQ(name.value(), "Alice");
    }

    /** @brief 钉住 UTF-8 文本按字节长度拷贝，中文不被截断 */
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

    /** @brief 钉住 SQL NULL 落 monostate */
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

    /** @brief 钉住空串与零长 BLOB 是「有值为空」，不塌成 monostate */
    TEST_F(SqliteUserQuery, EmptyTextAndEmptyBlobAreNotReportedAsNull)
    {
        // 第 4 行的 name 是空串、payload 是零长 BLOB：两者都是「有值且值为空」，不能塌成 monostate。
        // 空文本落到 String、零长二进制落到 Bytes，两条支路各自给出「空」而不是「没有值」
        const std::unique_ptr<DatabaseResult> result = query("SELECT name, payload FROM users WHERE id = 4");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        const DatabaseValue emptyName   = result->getValue(std::size_t{0});
        const DatabaseValue emptyPayload = result->getValue(std::size_t{1});

        ASSERT_TRUE(std::holds_alternative<std::string>(emptyName)) << databaseValueTypeName(emptyName);
        EXPECT_TRUE(std::get<std::string>(emptyName).empty());
        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(emptyPayload)) << databaseValueTypeName(emptyPayload);
        EXPECT_TRUE(std::get<BinaryBytes>(emptyPayload).empty());
    }

    /** @brief 钉住二进制按长度拷贝，内嵌空字节不丢失 */
    TEST_F(SqliteUserQuery, BlobKeepsEmbeddedNullByte)
    {
        const std::unique_ptr<DatabaseResult> result = query("SELECT payload FROM users WHERE id = 1");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // x'5c0041' 是 0x5C、0x00、0x41 三个字节：按长度拷贝才保得住中间那个 '\0'
        const std::optional<BinaryBytes> payload = asBytes(result->getValue(std::size_t{0}));
        ASSERT_TRUE(payload.has_value()) << databaseValueTypeName(result->getValue(std::size_t{0}));
        ASSERT_EQ(payload->size(), 3u);
        EXPECT_EQ((*payload)[0], 0x5C);
        EXPECT_EQ((*payload)[1], 0x00);
        EXPECT_EQ((*payload)[2], 0x41);
    }

    /** @brief 钉住只有 BLOB 存储类才走二进制，TEXT 列仍是字符串（挡住按声明类型一刀切） */
    TEST_F(SqliteUserQuery, TextColumnStillReadsAsStringNotBytes)
    {
        // 二进制现在是独立备选，但只有 BLOB 存储类才走它：TEXT 列必须仍是 std::string。
        // 这条对照用例挡住「按列声明类型一刀切判成二进制」这类过度识别
        const std::unique_ptr<DatabaseResult> result = query("SELECT name FROM users WHERE id = 2");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        const DatabaseValue name = result->getValue(std::size_t{0});
        ASSERT_TRUE(std::holds_alternative<std::string>(name)) << databaseValueTypeName(name);
        EXPECT_EQ(std::get<std::string>(name), "Bob");
        EXPECT_FALSE(std::holds_alternative<BinaryBytes>(name)) << databaseValueTypeName(name);
    }

    /** @brief 钉住 0/1 位落 int64 而不是 bool，收窄交给调用方 */
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

    /** @brief 钉住声明类型落不到四种存储类时按文本交出 */
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

    /** @brief 钉住按名与按下标两条取值路径共用同一份边界判定，结果完全一致 */
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

    /** @brief 钉住影响行数跟随写语句：INSERT 计 1、UPDATE 计全部匹配行、IGNORE 计 0 */
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

    /** @brief 钉住影响行数在构造这一刻快照，不被后来的写入改写 */
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

    /** @brief 钉住结果集快照与连接级计数器语义不同：快照不随新写入前进 */
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

    /** @brief 钉住查询结果上的计数来自连接上一条写语句（SQLite 无语句级历史） */
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

    /** @brief 钉住带写副作用的 RETURNING 绝不预扫描：rowCount 为 0，首次 next() 才真正执行 */
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

    /** @brief 钉住影响行数经基类虚接口即可取得，不必按驱动向下转型 */
    TEST_F(SqliteUserQuery, AffectedRowCountIsReachableThroughTheBaseInterface)
    {
        const std::unique_ptr<DatabaseResult> receipt =
            executeRequired(connection(), "UPDATE users SET age = age + 1");
        ASSERT_NE(receipt, nullptr);

        // 影响行数现在由 DatabaseResult 基类提供虚接口，这里刻意通过基类引用取值：
        // affectedRowCount() 在基类上即可取得，不必按 DatabaseType 向下转型，其它驱动一律得 0
        // 样本共四行，UPDATE 统计所有匹配并被写入的行，与值是否真的改变无关
        const DatabaseResult &baseResult = *receipt;
        EXPECT_EQ(baseResult.affectedRowCount(), 4);
    }

} // namespace AsynGyanis::Database
