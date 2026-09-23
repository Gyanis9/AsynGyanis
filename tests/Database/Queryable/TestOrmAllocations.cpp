// ORM 读写的分配台账 —— 用 AllocationProbe 把「一个形状每次操作碰几次堆」钉成读数。
// Net 侧早有同口径的台账（每请求分配数），Database 侧此前只有耗时读数：耗时能看出「变慢了」，
// 看不出「慢在哪一次分配上」，而 ORM 的每条语句都要经过查询树、方言渲染、结果映射三段合成，
// 段数一多就必须靠分配次数与字节分布才能定位。
// 覆盖形状：first() 按主键取一行、toList() 取二十行、count()、update() 按主键改一行。
// 两条自检先行：计数件要看得见一次普通堆分配，空窗口要量出零次——否则下面所有读数都不可信。
// 稳态读数（语句缓存与分配器空闲链已预热，每次操作）：
//   first() 取一行     GCC 14 次 / 1964 字节，MSVC 15 次
//   toList() 取二十行  GCC 37 次 / 23736 字节（每行不到两次），MSVC 86 次
//   count()            两侧都是 6 次 / 451 字节
//   update() 改一行    GCC 16 次 / 1812 字节，MSVC 20 次
// 两侧读数不同不是代码差异，而是 STL 的 vector 扩容系数与 string 分档不同（直方图实测：
// libstdc++ 把 21 字节的户名记在 16..31 档、MSVC 记在 32..47 档，且 MSVC 的扩容链更长）。
// 因此预算按平台各自取实测加一档，判的是量级而不是台次。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/TableSchema.h"

#include "AllocationProbe.h"
#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::TestSupport::TemporaryDatabaseFile;
    using AsynGyanis::TestSupport::AllocationProfile;
    using AsynGyanis::TestSupport::kMeasurementIterations;
    using AsynGyanis::TestSupport::measurePerOperation;

#if defined(_MSC_VER)
    /// MSVC 的 STL 分档与扩容系数不同，同一形状的读数比 libstdc++ 高一截，各自按实测收紧
    inline constexpr std::uint64_t kFirstRowAllocationBudget = 17U;
    inline constexpr std::uint64_t kListAllocationBudget = 96U;
    inline constexpr std::uint64_t kUpdateAllocationBudget = 24U;
#else
    /// libstdc++ 侧的实测读数（容器 GCC 13）加一档
    inline constexpr std::uint64_t kFirstRowAllocationBudget = 16U;
    inline constexpr std::uint64_t kListAllocationBudget = 44U;
    inline constexpr std::uint64_t kUpdateAllocationBudget = 18U;
#endif

    /// 台账用的行数：二十行足够让「每行成本」与「每次调用成本」分得开，又不让单条用例跑太久
    constexpr std::int64_t kLedgerRowCount = 20;

    /**
     * @brief 台账行：五列覆盖整型、文本、浮点、可空文本与布尔，与 ORM 的全部标量映射分支对齐
     */
    struct LedgerRow
    {
        std::int64_t               id;      ///< 主键
        std::string                name;    ///< 名称
        double                     amount;  ///< 金额
        std::optional<std::string> note;    ///< 备注，可空
        bool                       active;  ///< 是否启用
    };

} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<LedgerRow>
{
    static constexpr std::string_view kTableName = "ledger";
    static constexpr auto kColumns = std::tuple{
        Column(&LedgerRow::id,     "id"),
        Column(&LedgerRow::name,   "name"),
        Column(&LedgerRow::amount, "amount"),
        Column(&LedgerRow::note,   "note"),
        Column(&LedgerRow::active, "active"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

namespace
{
    /**
     * @brief 台账夹具：文件库 + 连接池，预置二十行
     *
     * @details 用文件库而不是内存库：内存库不跨连接共享，而台账要量的形状里有两条连接（一条跑语句、
     *          一条兜住池的并发借用），文件库的形态也更接近真实部署。
     */
    class OrmAllocationLedger : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            PoolConfig poolConfiguration;
            poolConfiguration.maximumPoolSize            = 2;
            poolConfiguration.acquireTimeoutMilliseconds = 3000;

            m_pool = std::make_unique<ConnectionPool>(
                [this]()
                {
                    auto connection = DatabaseFactory::createSqlite(
                        ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
                    // 池的工厂契约要求交出「已经 connect() 完成」的连接
                    static_cast<void>(connection->connect());
                    return connection;
                },
                poolConfiguration);

            const PooledConnection connection = m_pool->acquire();
            ASSERT_TRUE(connection);
            ASSERT_TRUE(connection->execute(
                            "CREATE TABLE ledger ("
                            "id INTEGER PRIMARY KEY, "
                            "name TEXT NOT NULL, "
                            "amount REAL, "
                            "note TEXT, "
                            "active INTEGER NOT NULL)") != nullptr)
                    << connection->lastError();

            for (std::int64_t index = 1; index <= kLedgerRowCount; ++index)
            {
                Queryable<LedgerRow> query(*m_pool);
                ASSERT_EQ(query.insert(makeLedgerRow(index)), 1);
            }
        }

        /**
         * @brief 造一行台账数据：名字刻意超过短串内联长度，让每行的文本列必须碰一次堆
         * @param id 主键
         * @return LedgerRow 结构体
         */
        [[nodiscard]] static LedgerRow makeLedgerRow(const std::int64_t id)
        {
            return LedgerRow{
                .id     = id,
                .name   = "ledger-account-name-" + std::to_string(id),
                .amount = 1.5,
                .note   = std::nullopt,
                .active = true
            };
        }

        /// 临时库文件必须先于池声明、后于池析构：Windows 上打开着的文件删不掉
        TemporaryDatabaseFile m_databaseFile{"OrmAlloc"};
        std::unique_ptr<ConnectionPool> m_pool; ///< 夹具独占的连接池
    };

    /**
     * @brief 计数件要证明自己看得见一次普通堆分配
     * @details 少了这条，哪天 operator new 的替换件被链接顺序顶掉，所有读数都会是 0，
     *          而 0 看着像「零分配的漂亮实现」
     */
    TEST(OrmAllocations, CountingHookSeesAPlainHeapAllocation)
    {
        const AllocationProfile profile = measurePerOperation(
                []
                {
                    const std::string allocated(64, 'x');
                    return allocated.size();
                });
        EXPECT_GE(profile.allocationsPerOperation, 1U) << "operator new 的替换件没生效，本文件所有读数都不可信";
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * 64U);
    }

    /**
     * @brief 测量窗自身的本底：什么都不做的循环必须量出零次分配
     */
    TEST(OrmAllocations, MeasurementWindowHasNoBackgroundAllocations)
    {
        const AllocationProfile profile = measurePerOperation([] { return std::uint64_t{1}; });
        EXPECT_EQ(profile.totalAllocations, 0U);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations);
    }

    /**
     * @brief 一次按主键取行的 ORM 分配台账
     * @details 形状是「建查询对象 → 挂条件 → first() → 映射一行」，量的是调用方每写一次这行代码
     *          碰几次堆；预热放在窗口之外，语句缓存与分配器空闲链要到位才算稳态。
     */
    TEST_F(OrmAllocationLedger, FirstByPrimaryKeyAllocationLedger)
    {
        for (int warmUp = 0; warmUp < 20; ++warmUp)
        {
            Queryable<LedgerRow> query(*m_pool);
            static_cast<void>(query.where(Column(&LedgerRow::id, "id") == std::int64_t{7}).first());
        }

        const AllocationProfile profile = measurePerOperation(
                [this]
                {
                    Queryable<LedgerRow> query(*m_pool);
                    const std::optional<LedgerRow> row =
                            query.where(Column(&LedgerRow::id, "id") == std::int64_t{7}).first();
                    return row.has_value() ? static_cast<std::uint64_t>(row->name.size()) : 0U;
                });

        EXPECT_EQ(profile.resultSum, kMeasurementIterations * 21U) << "有几次迭代没取到行，读数不能算稳态";
        // 一次按主键取行：GCC 实测 14 次 / 1964 字节，MSVC 实测 15 次。
        // 上界按各自的实测留一档，别把两台编译器的分配器差异算成回归
        EXPECT_LE(profile.allocationsPerOperation, kFirstRowAllocationBudget)
                << "first() 分配次数涨了，实测=" << profile.allocationsPerOperation;
    }

    /**
     * @brief 一次取二十行的 ORM 分配台账：每次调用的读数与每行的读数都要看
     */
    TEST_F(OrmAllocationLedger, ListOfTwentyRowsAllocationLedger)
    {
        for (int warmUp = 0; warmUp < 20; ++warmUp)
        {
            Queryable<LedgerRow> query(*m_pool);
            static_cast<void>(query.orderBy(AsynGyanis::Database::Queryable::asc("id")).toList());
        }

        const AllocationProfile profile = measurePerOperation(
                [this]
                {
                    Queryable<LedgerRow> query(*m_pool);
                    const std::vector<LedgerRow> rows =
                            query.orderBy(AsynGyanis::Database::Queryable::asc("id")).toList();
                    std::uint64_t nameBytes = 0;
                    for (const LedgerRow &row: rows)
                    {
                        nameBytes += row.name.size();
                    }
                    return nameBytes;
                });

        // 二十行整表读回：GCC 实测 37 次 / 23736 字节（每行不到两次），MSVC 实测 86 次——
        // 差在 vector 扩容系数与 string 的分档上，与我们的代码无关，所以两侧各自收紧。
        // 这条的门禁意义是「别退回成每格一次分配」：那一格是 20 行 x 5 列 = 100 次起
        EXPECT_LE(profile.allocationsPerOperation, kListAllocationBudget)
                << "每行的分配次数退化了，实测=" << profile.allocationsPerOperation;
    }

    /**
     * @brief 一次 count() 与一次 update() 的分配台账
     */
    TEST_F(OrmAllocationLedger, CountAndUpdateAllocationLedger)
    {
        const AllocationProfile countProfile = measurePerOperation(
                [this]
                {
                    Queryable<LedgerRow> query(*m_pool);
                    return static_cast<std::uint64_t>(query.count());
                });
        // count() 只要一个标量：两台编译器都实测 6 次 / 451 字节
        EXPECT_LE(countProfile.allocationsPerOperation, 8U)
                << "count() 分配次数涨了，实测=" << countProfile.allocationsPerOperation;

        const AllocationProfile updateProfile = measurePerOperation(
                [this]
                {
                    Queryable<LedgerRow> query(*m_pool);
                    return static_cast<std::uint64_t>(
                            query.update(makeLedgerRow(1)));
                });
        // update() 走写方向的完整翻译与参数收集：GCC 实测 16 次 / 1812 字节，MSVC 实测 20 次
        EXPECT_LE(updateProfile.allocationsPerOperation, kUpdateAllocationBudget)
                << "update() 分配次数涨了，实测=" << updateProfile.allocationsPerOperation;
    }

} // namespace
