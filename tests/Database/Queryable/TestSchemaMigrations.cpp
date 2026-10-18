// 版本化迁移（台账 3.2）与随迁移落的种子数据（台账 3.4）的判据：迁移记录表、升序应用、幂等、
// 可回退、「一条一笔事务」，以及种子数据「连跑两轮不新增行」。
//
// 全部用 SQLite 内存库（池上限 1：内存库不跨连接共享）。真机那一侧另外有 MySQL 版本
// （TestMySqlIntegration.cpp：应用 + 幂等 + 回退 + 再应用 + 种子往返；那侧的 DDL 隐式提交，回滚语义不可测）。

#include "Base/Exception/LogicException.h"

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include "Database/Common/Decimal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

// ========================================================================
// 测试用数据结构
// ========================================================================

namespace
{
    using AsynGyanis::Database::Queryable::Column;

    /// 迁移会建出来的表之一：只有一列
    struct MigrationNoteRow
    {
        std::int64_t id;   ///< 主键
        std::string  text; ///< 正文，v1 建表时就有
    };

    /// 另一张表：给「第二条迁移」当靶子（回退时整张表消失，判据好认）
    struct MigrationTagRow
    {
        std::int64_t id; ///< 主键
    };

    /// 种子数据的反例靶子：自增主键的表（`seed()` 必须拒）
    struct AutoIncrementSeedRow
    {
        std::int64_t id;   ///< 自增主键（不进 INSERT 的列清单）
        std::string  name; ///< 名称
    };
    /// 十进制金额用例的行：主键不自增，插入值由用例自己给，少一处引擎差异
    struct DecimalMoneyRow
    {
        std::int64_t                  id;     ///< 主键
        AsynGyanis::Database::Decimal amount; ///< 金额列：走本映射的 Decimal 一档
    };
} // namespace

// ========================================================================
// TableSchema 特化：迁移会建的两张表
// ========================================================================

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MigrationNoteRow>
{
    static constexpr std::string_view kTableName = "migration_notes";
    static constexpr auto             kColumns   = std::tuple{
            Column(&MigrationNoteRow::id, "id"),
            Column(&MigrationNoteRow::text, "text"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MigrationTagRow>
{
    static constexpr std::string_view kTableName = "migration_tags";
    static constexpr auto             kColumns   = std::tuple{
            Column(&MigrationTagRow::id, "id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AutoIncrementSeedRow>
{
    static constexpr std::string_view kTableName = "seed_auto_increment";
    static constexpr auto             kColumns   = std::tuple{
            Column(&AutoIncrementSeedRow::id, "id"),
            Column(&AutoIncrementSeedRow::name, "name"),
    };
    static constexpr std::string_view kPrimaryKey                = "id";
    static constexpr bool             kIsAutoIncrementPrimaryKey = true;
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<DecimalMoneyRow>
{
    static constexpr std::string_view kTableName = "decimal_money";
    static constexpr auto             kColumns   = std::tuple{
            Column(&DecimalMoneyRow::id, "id"),
            Column(&DecimalMoneyRow::amount, "amount"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::SqlDialect;
    using AsynGyanis::Database::SqlStatement;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::SchemaMigrator;
    using AsynGyanis::Database::Queryable::TableSchema;

    /// ORM 查询构建器的本地别名（理由同 TestMySqlIntegration.cpp：`using Queryable::Queryable;`
    /// 会与同名命名空间构成歧义 C2872）
    template<typename RowType>
    using OrmQuery = AsynGyanis::Database::Queryable::Queryable<RowType>;

    /**
     * @brief 迁移用例的夹具：一个内存库 + 两条现成的迁移
     * @details 两条迁移分别建两张表，因此「应用了几条」可以直接问 `tableExists`；
     *          回退一条就是一张表消失，比在列上做差分更不容易看错
     */
    class SchemaMigrationsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            PoolConfig poolConfiguration;
            poolConfiguration.maximumPoolSize = 1;
            m_pool                            = std::make_unique<ConnectionPool>(
                    []()
                    {
                        auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                        connection->connect();
                        return connection;
                    },
                    poolConfiguration);

            const std::shared_ptr<SqlDialect> dialect = SchemaMigrator::dialectOf(*m_pool);
            ASSERT_NE(dialect, nullptr) << "取不到方言，用例前置条件不成立";

            m_steps.push_back(SchemaMigrator::MigrationStep{
                    .name           = "0001-create-notes",
                    .upStatements   = {SchemaMigrator::createTableStatement<MigrationNoteRow>(*dialect)},
                    .downStatements = {SchemaMigrator::dropTableStatement<MigrationNoteRow>(*dialect)},
            });
            m_steps.push_back(SchemaMigrator::MigrationStep{
                    .name           = "0002-create-tags",
                    .upStatements   = {SchemaMigrator::createTableStatement<MigrationTagRow>(*dialect)},
                    .downStatements = {SchemaMigrator::dropTableStatement<MigrationTagRow>(*dialect)},
            });
        }

        /// 按名字取一条步骤（按值，方便改它的语句）
        [[nodiscard]] SchemaMigrator::MigrationStep stepNamed(const std::string_view name) const
        {
            for (const auto &step: m_steps)
            {
                if (step.name == name)
                {
                    return step;
                }
            }
            return {};
        }

        std::unique_ptr<ConnectionPool>            m_pool;  ///< 用例独占的连接池
        std::vector<SchemaMigrator::MigrationStep> m_steps; ///< 两条常规迁移
    };
} // namespace


// ========================================================================
// 应用与幂等
// ========================================================================

/**
 * @brief 钉住升序应用与幂等：第二条依赖第一条的效果落地，重复跑是 0
 * @details 「升序」在这条用例里的可证形式：两条迁移各建一张表，应用完之后两张都在；
 *          幂等则要求第二次调用一条都不应用（迁移表把它挡住了）
 * @note 证伪：把「记账」那一步删掉，第二次调用会重新应用 → 表已存在（IF NOT EXISTS）也许不报错，
 *       但 appliedCount 会是 2 而不是 0，这条当场红
 */
TEST_F(SchemaMigrationsTest, AppliesStepsInOrderAndIsIdempotent)
{
    std::size_t applied = 0;
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, &applied, &error)) << error;
    EXPECT_EQ(applied, 2U);
    EXPECT_TRUE(SchemaMigrator::tableExists<MigrationNoteRow>(*m_pool)) << "第一条迁移没生效";
    EXPECT_TRUE(SchemaMigrator::tableExists<MigrationTagRow>(*m_pool)) << "第二条迁移没生效";

    std::vector<std::string> recorded;
    ASSERT_TRUE(SchemaMigrator::appliedMigrations(*m_pool, &recorded, &error)) << error;
    EXPECT_EQ(recorded.size(), 2U) << "记账条数与应用条数不一致";

    // 幂等：同一份列表再来一遍，一条都不应用
    std::size_t appliedAgain = 0;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, &appliedAgain, &error)) << error;
    EXPECT_EQ(appliedAgain, 0U) << "重复应用同一迁移：这一次不该再动结构";
}

/**
 * @brief plan 只报待应用项、不改结构；改掉已应用迁移的 SQL 时 plan 与 migrate 都按漂移拒绝
 */
TEST_F(SchemaMigrationsTest, PlanReportsPendingStepsAndRejectsChecksumDrift)
{
    std::string              error;
    std::vector<std::string> pending;

    // 台账尚未建立：全部待应用，且 plan 不得顺手把台账/表建出来
    ASSERT_TRUE(SchemaMigrator::plan(*m_pool, m_steps, &pending, &error)) << error;
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[0], m_steps[0].name);
    EXPECT_FALSE(SchemaMigrator::tableExists<MigrationNoteRow>(*m_pool)) << "plan 执行了迁移语句";

    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, nullptr, &error)) << error;
    ASSERT_TRUE(SchemaMigrator::plan(*m_pool, m_steps, &pending, &error)) << error;
    EXPECT_TRUE(pending.empty()) << "全部已应用时仍有待应用项";

    // 追加一条：plan 只报它，migrate 只应用它
    std::vector<SchemaMigrator::MigrationStep> extended = m_steps;
    extended.push_back(SchemaMigrator::MigrationStep{.name = "003-marker", .upStatements = {}, .downStatements = {}});
    ASSERT_TRUE(SchemaMigrator::plan(*m_pool, extended, &pending, &error)) << error;
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0], "003-marker");
    std::size_t applied = 0;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, extended, &applied, &error)) << error;
    EXPECT_EQ(applied, 1U);

    // 改掉已应用那条的 SQL：校验和漂移，plan 与 migrate 都必须拒绝
    std::vector<SchemaMigrator::MigrationStep> drifted = extended;
    drifted[0].upStatements[0].sql += " ";
    EXPECT_FALSE(SchemaMigrator::plan(*m_pool, drifted, &pending, &error));
    EXPECT_NE(error.find("校验和"), std::string::npos) << error;
    EXPECT_FALSE(SchemaMigrator::migrate(*m_pool, drifted, nullptr, &error));
    EXPECT_NE(error.find("校验和"), std::string::npos) << error;
}

// ========================================================================
// 回退与往返
// ========================================================================

/**
 * @brief 钉住回退撤最后一条、并把记账一起撤掉（回退之后能重新应用）
 * @details 三步：退一条后那张表消失、记账只剩一条；再 migrate() 会把退掉的那条重新应用。
 *          只测「表消失」不够——记账没删的话重新应用会是 0 条，表永远回不来
 */
TEST_F(SchemaMigrationsTest, RollsBackTheLastStepAndCanReapplyIt)
{
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, nullptr, &error)) << error;

    std::size_t rolledBack = 0;
    ASSERT_TRUE(SchemaMigrator::rollbackLast(*m_pool, m_steps, 1, &rolledBack, &error)) << error;
    EXPECT_EQ(rolledBack, 1U);
    EXPECT_FALSE(SchemaMigrator::tableExists<MigrationTagRow>(*m_pool)) << "最后一条迁移没被撤掉";
    EXPECT_TRUE(SchemaMigrator::tableExists<MigrationNoteRow>(*m_pool)) << "回退撤多了：前一条的表也消失了";

    std::vector<std::string> recorded;
    ASSERT_TRUE(SchemaMigrator::appliedMigrations(*m_pool, &recorded, &error)) << error;
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(recorded.front(), "0001-create-notes") << "记账没跟着回退一起删";

    // 往返：重新应用退回的那一条
    std::size_t reapplied = 0;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, &reapplied, &error)) << error;
    EXPECT_EQ(reapplied, 1U);
    EXPECT_TRUE(SchemaMigrator::tableExists<MigrationTagRow>(*m_pool)) << "回退之后重新应用没把结构建回来";
}

/**
 * @brief 钉住回退条数的边界：0 条与超过已应用条数都被拒
 */
TEST_F(SchemaMigrationsTest, RefusesRollbackCountsOutsideTheAppliedRange)
{
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, nullptr, &error)) << error;

    EXPECT_FALSE(SchemaMigrator::rollbackLast(*m_pool, m_steps, 0, nullptr, &error));
    EXPECT_NE(error.find("回退条数为 0"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(SchemaMigrator::rollbackLast(*m_pool, m_steps, 3, nullptr, &error));
    EXPECT_NE(error.find("只应用了 2 条"), std::string::npos) << error;
    // 两条都在：被拒的回退不该动结构
    EXPECT_TRUE(SchemaMigrator::tableExists<MigrationTagRow>(*m_pool));
}

// ========================================================================
// 种子数据（台账 3.4：随迁移一起落的幂等数据集）
// ========================================================================

/**
 * @brief 钉住种子数据的幂等：连跑两轮，第二轮一行都不新增
 * @details 一个部署步骤的形状：`migrate()` 建表 → `seed()` 落数据 → 再来一轮（什么结构都不动、
 *          一行都不新增）。判据三层：新增行数（3 → 0）、表内总行数（始终 3）、行内容（与种子
 *          逐字一致）。第三层是必须的——「第二轮没新增」也可以靠「把内容覆盖成别的样子」做到
 * @note 证伪：把 `Ignore` 换成 `Fail`，第二轮直接撞唯一键（返回 false + 冲突原因）而不是 0；
 *       换成 `Replace`，第二轮返回 3（MySQL 侧会记成 6）且内容可能被覆盖，第一条断言就红
 */
TEST_F(SchemaMigrationsTest, SeedsOnlyTheMissingRowsAndIsIdempotent)
{
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, nullptr, &error)) << error;

    const std::vector<MigrationNoteRow> seedRows = {
            MigrationNoteRow{.id = 1, .text = "第一条"},
            MigrationNoteRow{.id = 2, .text = "第二条"},
            MigrationNoteRow{.id = 3, .text = "第三条"},
    };

    // 第一轮：三行都是新的
    std::size_t inserted = 0;
    ASSERT_TRUE(SchemaMigrator::seed(*m_pool, seedRows, &inserted, &error)) << error;
    EXPECT_EQ(inserted, 3U) << "第一轮的新增行数不对";

    // 第二轮：结构已应用、行也已存在，一行都不该新增
    std::size_t insertedAgain = 0;
    ASSERT_TRUE(SchemaMigrator::seed(*m_pool, seedRows, &insertedAgain, &error)) << "第二轮种子失败（幂等写入撞了唯一键？）：" << error;
    EXPECT_EQ(insertedAgain, 0U) << "重复落种子数据产生了新行";

    // 内容层：三行都在，文本与种子逐字一致（按集合比对，顺序由服务端定）
    const std::vector<MigrationNoteRow> rows = OrmQuery<MigrationNoteRow>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).toList();
    ASSERT_EQ(rows.size(), 3U) << "重复落种子数据让行数变了";
    EXPECT_EQ(rows[0].text, "第一条");
    EXPECT_EQ(rows[1].text, "第二条");
    EXPECT_EQ(rows[2].text, "第三条");

    // 只补缺的：删掉中间那一行再落一遍，新增数正好是 1（不是「全表重写」）
    static_cast<void>(OrmQuery<MigrationNoteRow>(*m_pool).where(Column(&MigrationNoteRow::id, "id") == std::int64_t{2}).executeNonQuery());
    std::size_t insertedAfterDelete = 0;
    ASSERT_TRUE(SchemaMigrator::seed(*m_pool, seedRows, &insertedAfterDelete, &error)) << error;
    EXPECT_EQ(insertedAfterDelete, 1U) << "缺的那一行没被补回来（或把别的行也一起动了）";
    EXPECT_EQ(OrmQuery<MigrationNoteRow>(*m_pool).count(), 3);
}

/**
 * @brief 钉住空数据集不发语句：连表都不存在也返回成功
 * @details 「空列表 = 什么都不做」这条由下一层的 `insertBatch` 承诺（空集合直接返回 0、不产生任何
 *          语句），本用例把它端到端钉住。用一张不存在的表来判：真发了语句就一定失败，
 *          返回成功只能是「没发」
 */
TEST_F(SchemaMigrationsTest, SeedsAnEmptyDataSetWithoutTouchingTheDatabase)
{
    std::string error;
    std::size_t inserted = 1U;
    const bool  seeded   = SchemaMigrator::seed<MigrationTagRow>(*m_pool, std::span<const MigrationTagRow>{}, &inserted, &error);
    EXPECT_TRUE(seeded) << error;
    EXPECT_EQ(inserted, 0U);
    EXPECT_FALSE(SchemaMigrator::tableExists<MigrationTagRow>(*m_pool)) << "空数据集不该碰数据库（这张表从未建过，被建出来说明发了 DDL/DML）";
}

/**
 * @brief 钉住自增主键的表被拒（种子数据在那张表上做不到幂等）
 * @details 自增主键不进 INSERT 的列清单，因此 `INSERT OR IGNORE` 撞不上任何唯一键——每跑一轮
 *          都会追加一份。这是「静默追加重复行」的形状，宁可当场拒
 * @note 证伪：把守卫整段删掉，这条会返回 true（表被建出来、行被插进去），断言立刻红
 */
TEST_F(SchemaMigrationsTest, RefusesSeedsOnAutoIncrementPrimaryKeyTables)
{
    std::string                             error;
    std::size_t                             inserted = 0;
    const std::vector<AutoIncrementSeedRow> rows     = {AutoIncrementSeedRow{.name = "甲"}};

    EXPECT_FALSE(SchemaMigrator::seed(*m_pool, rows, &inserted, &error)) << "自增主键的表被放行了";
    EXPECT_NE(error.find("自增主键"), std::string::npos) << "拒绝原因没点出自增主键：" << error;
    EXPECT_EQ(inserted, 0U);
    // 守卫在碰库之前就拒：这张表在用例里从未被创建过，被建出来说明走到了 SQL 那一层
    EXPECT_FALSE(SchemaMigrator::tableExists<AutoIncrementSeedRow>(*m_pool));
}

// ========================================================================
// 历史一致性
// ========================================================================

/**
 * @brief 钉住「已应用的必须是本次列表的前缀」：漏一条、换一条、顺序被改过都在应用之前拒掉
 * @details 这条防的是「拿错误的顺序改结构」：迁移表里记着 A、B，而这次给的列表是 [B]（漏了 A）
 *          或 [A, C]（换了一条）时，继续应用就是按一份与落库历史不一致的顺序动结构
 */
TEST_F(SchemaMigrationsTest, RefusesListsThatDoNotMatchTheAppliedHistory)
{
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, nullptr, &error)) << error;

    const std::vector<SchemaMigrator::MigrationStep> missingFirst{m_steps[1]};
    EXPECT_FALSE(SchemaMigrator::migrate(*m_pool, missingFirst, nullptr, &error));
    EXPECT_NE(error.find("前缀"), std::string::npos) << error;

    error.clear();
    const std::vector<SchemaMigrator::MigrationStep> swappedFirst{SchemaMigrator::MigrationStep{.name = "0003-other"}, m_steps[1]};
    EXPECT_FALSE(SchemaMigrator::migrate(*m_pool, swappedFirst, nullptr, &error));
    EXPECT_NE(error.find("前缀"), std::string::npos) << error;

    // 追加一条新迁移是正常推进：前缀一致，只应用新的那条
    error.clear();
    std::vector<SchemaMigrator::MigrationStep> extended = m_steps;
    extended.push_back(SchemaMigrator::MigrationStep{.name = "0003-marker-only"});
    std::size_t applied = 0;
    EXPECT_TRUE(SchemaMigrator::migrate(*m_pool, extended, &applied, &error)) << error;
    EXPECT_EQ(applied, 1U) << "只该应用新增的那一条";
}

/**
 * @brief 钉住空名与重名在动数据库之前就被拒
 */
TEST_F(SchemaMigrationsTest, RejectsEmptyAndDuplicateStepNames)
{
    const std::vector<SchemaMigrator::MigrationStep> withEmptyName{SchemaMigrator::MigrationStep{.name = ""}};
    EXPECT_THROW(static_cast<void>(SchemaMigrator::migrate(*m_pool, withEmptyName)), AsynGyanis::Base::LogicException);
    EXPECT_THROW(static_cast<void>(SchemaMigrator::rollbackLast(*m_pool, withEmptyName, 1)), AsynGyanis::Base::LogicException);

    const std::vector<SchemaMigrator::MigrationStep> duplicated{m_steps[0], m_steps[0]};
    EXPECT_THROW(static_cast<void>(SchemaMigrator::migrate(*m_pool, duplicated)), AsynGyanis::Base::LogicException);
}

// ========================================================================
// 一条一笔事务
// ========================================================================

/**
 * @brief 钉住「一条迁移失败时，它自己做过的结构改动也被撤回」，且不留记账
 * @details 这条是「结构改动与记账同一笔事务」的可证形式：让第二条语句注定失败（往一条不存在的
 *          表上加列），那么第一条语句建出来的表必须不在——若实现是「一句一句直接发」，
 *          第一句已经落库，表会留在那里，判据当场红。SQLite 与 PostgreSQL 的 DDL 可回滚，
 *          MySQL 不行（那一侧的例外写在 SchemaMigrator::migrate() 的 @warning 里）
 * @note 失败之后迁移表里不能有这条的记录：否则下次会跳过它，结构永远缺一块
 */
TEST_F(SchemaMigrationsTest, RollsBackTheWholeStepWhenOneStatementFails)
{
    const SchemaMigrator::MigrationStep failingStep{
            .name           = "0003-half-broken",
            .upStatements   = {SchemaMigrator::createTableStatement<MigrationTagRow>(*SchemaMigrator::dialectOf(*m_pool)),
                               SqlStatement{.sql = "ALTER TABLE \"no_such_table_for_migration_test\" ADD COLUMN \"x\" INTEGER", .parameters = {}}},
            .downStatements = {SchemaMigrator::dropTableStatement<MigrationTagRow>(*SchemaMigrator::dialectOf(*m_pool))},
    };
    const std::vector<SchemaMigrator::MigrationStep> steps{failingStep};

    std::size_t applied = 0;
    std::string error;
    EXPECT_FALSE(SchemaMigrator::migrate(*m_pool, steps, &applied, &error));
    EXPECT_EQ(applied, 0U) << "失败的那一条被算成已应用";
    EXPECT_NE(error.find("本次已回滚"), std::string::npos) << error;
    EXPECT_FALSE(SchemaMigrator::tableExists<MigrationTagRow>(*m_pool)) << "第一条语句建出来的表留下来了：一条迁移不是一笔事务";

    std::vector<std::string> recorded;
    ASSERT_TRUE(SchemaMigrator::appliedMigrations(*m_pool, &recorded, &error)) << error;
    EXPECT_TRUE(recorded.empty()) << "失败的那一条留了记账，下次会跳过它";
}

/**
 * @brief 钉住回退失败时记账也留着：那条迁移仍算「已应用」，重新应用不会重复
 */
TEST_F(SchemaMigrationsTest, KeepsTheRecordWhenRollbackItselfFails)
{
    const SchemaMigrator::MigrationStep brokenDown{
            .name           = "0001-locked-in",
            .upStatements   = {SchemaMigrator::createTableStatement<MigrationTagRow>(*SchemaMigrator::dialectOf(*m_pool))},
            .downStatements = {SqlStatement{.sql = "DROP TABLE \"no_such_table_for_migration_test\"", .parameters = {}}},
    };
    const std::vector<SchemaMigrator::MigrationStep> steps{brokenDown};
    std::string                                      error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, steps, nullptr, &error)) << error;

    EXPECT_FALSE(SchemaMigrator::rollbackLast(*m_pool, steps, 1, nullptr, &error));
    EXPECT_NE(error.find("回退迁移"), std::string::npos) << error;

    std::vector<std::string> recorded;
    ASSERT_TRUE(SchemaMigrator::appliedMigrations(*m_pool, &recorded, &error)) << error;
    ASSERT_EQ(recorded.size(), 1U) << "回退失败却把记账删了：这条迁移会变成「没应用过」而被重复应用";

    // 表还在（回退失败没撤掉结构），重新应用也是 0 条
    std::size_t reapplied = 0;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, steps, &reapplied, &error)) << error;
    EXPECT_EQ(reapplied, 0U);
}

// ========================================================================
// 迁移表本身
// ========================================================================

/**
 * @brief 钉住迁移表能被既有设施认出来：它是一张普通登记表，列增补在它上面是干净的 0
 * @details 迁移表由本类自己建，但它走的是同一份 TableSchema 登记——因此 `addMissingColumns`
 *          与 `tableExists` 对它同样成立。手写一份 DDL 就做不到这件事
 */
/**
 * @brief SQLite 的互斥等价物：两个实例同时对同一个文件库跑 migrate，每一步只应用一次
 *
 * @details SQLite 没有会话级咨询锁，实现用「整段一笔 BEGIN IMMEDIATE 写事务 + busy_timeout」
 *          当等价物。两个池各一条连接、同时起跑：先拿到写锁的那个读空台账并把两步都应用掉，
 *          另一个等锁后读到最新台账、一步都不再应用。把取锁那一段（BEGIN IMMEDIATE）去掉，
 *          两个实例会同时读到空台账并各应用一遍，第二个的记账 INSERT 撞主键失败——本用例红。
 */
TEST(SchemaMigrationsConcurrency, TwoInstancesApplyEachStepOnceOnSqlite)
{
    const std::filesystem::path databasePath =
            std::filesystem::temp_directory_path() / ("asyn-mig-concurrent-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");

    const auto makePool = [&databasePath]
    {
        PoolConfig poolConfiguration;
        poolConfiguration.maximumPoolSize = 1;
        return std::make_unique<ConnectionPool>(
                [databasePath]
                {
                    auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(databasePath.string()));
                    connection->connect();
                    return connection;
                },
                poolConfiguration);
    };

    std::unique_ptr<ConnectionPool> firstPool  = makePool();
    std::unique_ptr<ConnectionPool> secondPool = makePool();

    const std::shared_ptr<SqlDialect> dialect = SchemaMigrator::dialectOf(*firstPool);
    ASSERT_NE(dialect, nullptr);

    const std::vector<SchemaMigrator::MigrationStep> steps{
            SchemaMigrator::MigrationStep{
                    .name           = "0001-concurrent-notes",
                    .upStatements   = {SchemaMigrator::createTableStatement<MigrationNoteRow>(*dialect)},
                    .downStatements = {SchemaMigrator::dropTableStatement<MigrationNoteRow>(*dialect)},
            },
            SchemaMigrator::MigrationStep{
                    .name           = "0002-concurrent-tags",
                    .upStatements   = {SchemaMigrator::createTableStatement<MigrationTagRow>(*dialect)},
                    .downStatements = {SchemaMigrator::dropTableStatement<MigrationTagRow>(*dialect)},
            },
    };

    std::atomic<int>           readyCount{0};
    std::atomic<bool>          startGate{false};
    std::array<bool, 2>        succeeded{};
    std::array<std::string, 2> errors;
    std::array<std::size_t, 2> appliedCounts{};

    const auto runMigration = [&](const std::size_t index, ConnectionPool &pool)
    {
        readyCount.fetch_add(1, std::memory_order_release);
        while (!startGate.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        succeeded[index] = SchemaMigrator::migrate(pool, steps, &appliedCounts[index], &errors[index]);
    };

    std::thread firstThread(runMigration, std::size_t{0}, std::ref(*firstPool));
    std::thread secondThread(runMigration, std::size_t{1}, std::ref(*secondPool));
    while (readyCount.load(std::memory_order_acquire) < 2)
    {
        std::this_thread::yield();
    }
    startGate.store(true, std::memory_order_release);
    firstThread.join();
    secondThread.join();

    EXPECT_TRUE(succeeded[0]) << "实例一迁移失败：" << errors[0];
    EXPECT_TRUE(succeeded[1]) << "实例二迁移失败：" << errors[1];
    EXPECT_EQ(appliedCounts[0] + appliedCounts[1], 2U) << "同一步被两个实例各应用了一次";

    std::string              error;
    std::vector<std::string> recorded;
    ASSERT_TRUE(SchemaMigrator::appliedMigrations(*firstPool, &recorded, &error)) << error;
    ASSERT_EQ(recorded.size(), 2U) << "台账里的步数不对";
    EXPECT_EQ(std::count(recorded.begin(), recorded.end(), "0001-concurrent-notes"), 1) << "第一步被记了不止一行";
    EXPECT_EQ(std::count(recorded.begin(), recorded.end(), "0002-concurrent-tags"), 1) << "第二步被记了不止一行";

    // 连接还握着文件时 Windows 删不掉：先收池再删
    firstPool.reset();
    secondPool.reset();
    std::error_code removeError;
    std::filesystem::remove(databasePath, removeError);
}

/**
 * @brief 迁移失败之后锁必须被放掉：同一池上限 1 的部署里下一次 migrate 不能死等
 *
 * @details 失败路径（步骤语句被拒）走的是会话析构：事务回滚、连接归还。若锁/事务留在
 *          那条连接上，池上限 1 的下一次调用会在借用/开事务处挂住，本用例超时前就会红。
 */
TEST_F(SchemaMigrationsTest, FailedMigrationReleasesTheLockForTheNextRun)
{
    std::vector<SchemaMigrator::MigrationStep> brokenSteps = m_steps;
    brokenSteps.front().upStatements.push_back(SqlStatement{.sql = "CREATE TABLE"});

    std::string error;
    std::size_t applied = 0;
    EXPECT_FALSE(SchemaMigrator::migrate(*m_pool, brokenSteps, &applied, &error)) << "故意写坏的迁移却成功了";
    EXPECT_EQ(applied, 0U) << "失败的那一步不应计入已应用条数";

    std::size_t appliedAfterFailure = 0;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, &appliedAfterFailure, &error)) << "失败路径没有释放锁/事务：" << error;
    EXPECT_EQ(appliedAfterFailure, 2U) << "失败之后的正常迁移应用条数不对";
}

TEST_F(SchemaMigrationsTest, TheLedgerTableIsVisibleToTheExistingColumnRepair)
{
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, m_steps, nullptr, &error)) << error;

    EXPECT_TRUE(SchemaMigrator::tableExists<SchemaMigrator::MigrationLedgerRow>(*m_pool)) << "迁移表不在库里";
    std::size_t added = 99;
    ASSERT_TRUE(SchemaMigrator::addMissingColumns<SchemaMigrator::MigrationLedgerRow>(*m_pool, &added, &error)) << error;
    EXPECT_EQ(added, 0U) << "迁移表在自己的纠偏路径上被当成缺列了";

    // 单独调 ensureMigrationLedger() 也是幂等的（表在就不动它）
    EXPECT_TRUE(SchemaMigrator::ensureMigrationLedger(*m_pool, &error)) << error;
    std::vector<std::string> recorded;
    ASSERT_TRUE(SchemaMigrator::appliedMigrations(*m_pool, &recorded, &error)) << error;
    EXPECT_EQ(recorded.size(), 2U) << "确保迁移表这一步把已有记录清掉了";
}

// ========================================================================
// 十进制金额列（Decimal）
// ========================================================================

/**
 * @brief 钉住 Decimal 成员在 SQLite 上的逐字符往返：DDL 由映射给出，写入什么文本就读回什么文本
 * @details 这条是判据 1 的 SQLite 那一格，也是「不经二进制浮点」的可证形式：挑的三个取值都是
 *          二进制浮点装不出来的 —— "0.1000" 经 double 会丢掉尾零，
 *          "1234567890123456789" 超出 double 能逐位还原的范围（2^53 以上），
 *          "-0.000000000000000000001" 干脆落在它的分辨率之外。
 *          断言比的是逐字符而不是数值近似：数值近似正是这类缺陷能一路静默的原因。
 * @note 建表语句由 SchemaMigrator::createTableStatement 生成，因此这一条同时钉住
 *       「新列类型在迁移侧能建出表来」（SQLite 落 TEXT 亲和，理由见 SqliteDialect::columnTypeName）。
 * @note 证伪：把驱动的十进制绑定臂降级成 double，或把成员的 Decimal 换成 double，
 *       第一行会读回 "0.1"、第二行末位会错，这条当场红。
 */
TEST_F(SchemaMigrationsTest, DecimalMemberRoundTripsCharacterForCharacterOnSqlite)
{
    const std::shared_ptr<SqlDialect> dialect = SchemaMigrator::dialectOf(*m_pool);
    ASSERT_NE(dialect, nullptr) << "取不到方言，用例前置条件不成立";

    const std::vector<SchemaMigrator::MigrationStep> steps{
            SchemaMigrator::MigrationStep{
                    .name         = "0001-create-decimal-money",
                    .upStatements = {SchemaMigrator::createTableStatement<DecimalMoneyRow>(*dialect)},
            },
    };
    std::size_t applied = 0;
    std::string error;
    ASSERT_TRUE(SchemaMigrator::migrate(*m_pool, steps, &applied, &error)) << error;
    ASSERT_EQ(applied, 1U) << "金额表没建出来";

    const std::vector<std::string> writtenTexts{
            std::string("0.1000"),
            std::string("1234567890123456789"),
            std::string("-0.000000000000000000001"),
    };
    for (std::size_t rowIndex = 0; rowIndex < writtenTexts.size(); ++rowIndex)
    {
        const auto parsed = AsynGyanis::Database::Decimal::tryParse(writtenTexts[rowIndex]);
        ASSERT_TRUE(parsed.has_value()) << writtenTexts[rowIndex];
        const DecimalMoneyRow row{.id = static_cast<std::int64_t>(rowIndex + 1), .amount = *parsed};
        ASSERT_EQ(1, OrmQuery<DecimalMoneyRow>(*m_pool).insert(row)) << "金额列没能写进去：" << writtenTexts[rowIndex];
    }

    const std::vector<DecimalMoneyRow> readBack = OrmQuery<DecimalMoneyRow>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).toList();
    ASSERT_EQ(readBack.size(), writtenTexts.size());
    for (std::size_t rowIndex = 0; rowIndex < writtenTexts.size(); ++rowIndex)
    {
        EXPECT_EQ(readBack[rowIndex].amount.text(), writtenTexts[rowIndex]) << "第 " << (rowIndex + 1) << " 行的金额文本被改写过";
    }

    // 列的标度信息由承载文本带着：第三行是 21 位小数，读回来仍然数得出同样的位数
    EXPECT_EQ(readBack[2].amount.scale(), 21U);
}
