// 版本化迁移（台账 3.2）的判据：迁移记录表、升序应用、幂等、可回退，以及「一条一笔事务」。
//
// 全部用 SQLite 内存库（池上限 1：内存库不跨连接共享）。真机那一侧另外有 MySQL 版本
// （TestMySqlIntegration.cpp：应用 + 幂等 + 回退 + 再应用；那侧的 DDL 隐式提交，回滚语义不可测）。

#include "Base/Exception/LogicException.h"

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
 *          表上加列），那么第一条语句建出来的表必须**不在**——若实现是「一句一句直接发」，
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
