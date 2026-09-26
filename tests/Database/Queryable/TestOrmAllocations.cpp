// ORM 读写的分配台账 —— 用 AllocationProbe 把「一个形状每次操作碰几次堆」钉成读数。
// Net 侧早有同口径的台账（每请求分配数），Database 侧此前只有耗时读数：耗时能看出「变慢了」，
// 看不出「慢在哪一次分配上」，而 ORM 的每条语句都要经过查询树、方言渲染、结果映射三段合成，
// 段数一多就必须靠分配次数与字节分布才能定位。
// 覆盖形状：first() 按主键取一行、toList() 取二十行、count()、update() 按主键改一行、
// insertBatch() 的两条形状（20 行单条语句、1000 行分块）。
// 两条自检先行：计数件要看得见一次普通堆分配，空窗口要量出零次——否则下面所有读数都不可信。
// 稳态读数（语句缓存与分配器空闲链已预热，每次操作；下面是 **Release/NDEBUG** 那一档）：
//   first() 取一行     GCC 13 次，MSVC 14 次
//   toList() 取二十行  GCC 37 次 / 23736 字节（每行不到两次），MSVC 86 次
//   count()            两侧都是 6 次 / 451 字节
//   update() 改一行    GCC 16 次 / 1812 字节，MSVC 20 次
//   批量 20 行         两侧都是 88 次（每次 4.4 次/行），MSVC 9960 / GCC 8729 字节
//   批量 1000 行       两侧都是 4022 次（每次约 4 次/行），MSVC 473954 / GCC 415451 字节
// 两侧读数不同不是代码差异，而是 STL 的 vector 扩容系数与 string 分档不同（直方图实测：
// libstdc++ 把 21 字节的户名记在 16..31 档、MSVC 记在 32..47 档，且 MSVC 的扩容链更长）。
// **配置也是口径**：MSVC 的 Debug 打开 _ITERATOR_DEBUG_LEVEL=2，每个 STL 容器对象多挂一份代理
// 分配，同形状的实测读数变成 first() 75、toList 318、count 31、update 72、批量 20 行 216 次、
// 批量 1000 行 9077 次——把钉下上面那批数字的提交单独编出来重跑，Debug 下照样是这个数，所以
// 这不是代码涨了分配。门禁跑的是 Debug+ASan，因此 Debug 那一档必须自己钉一套数（见下面的常量）。
// 预算一般取实测加一档；first() 这一格已收到实测值本身——它走的是「一行都不必经向量」的通道，
// 再多一次分配就是回归，留着余量反而看不住。

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
    using AsynGyanis::TestSupport::measureOperations;
    using AsynGyanis::TestSupport::measurePerOperation;

    // 台账钉的是「一次操作碰几次堆」，而这个数同时取决于**编译器**与**配置**：MSVC 的 Debug 会打开
    // _ITERATOR_DEBUG_LEVEL=2，每个 STL 容器对象多挂一份代理分配，同一段代码的读数比 Release 高好几倍
    // （实测 first() 从 14 涨到 75、20 行列表从 96 涨到 318）。这不是代码涨了分配——把钉下这批数字的
    // 那个提交单独编出来重跑，Debug 下照样 75。所以三档各按自己的实测钉，别把某一档的数套到另一档；
    // 换编译器、换配置、或换掉链接的 SQLite（它的 prepare/step 也走 malloc，一样计入），都要重测再钉。
#if defined(_MSC_VER) && !defined(NDEBUG)
    /// MSVC Debug（迭代器检查开满，门禁这一路还带 ASan）的实测读数加一档
    inline constexpr std::uint64_t kFirstRowAllocationBudget = 80U;         ///< 实测 75
    inline constexpr std::uint64_t kListAllocationBudget = 340U;            ///< 实测 318
    inline constexpr std::uint64_t kCountAllocationBudget = 34U;            ///< 实测 31
    inline constexpr std::uint64_t kUpdateAllocationBudget = 78U;           ///< 实测 72
    inline constexpr std::uint64_t kSmallBatchAllocationBudget = 232U;      ///< 实测 216
    inline constexpr std::uint64_t kSmallBatchBytesBudget = 14848U;        ///< 实测 13816
    inline constexpr std::uint64_t kChunkedBatchAllocationBudget = 9600U;   ///< 实测 9077
    inline constexpr std::uint64_t kChunkedBatchBytesBudget = 696320U;     ///< 实测 643028
#elif defined(_MSC_VER)
    /// MSVC Release 的实测读数加一档：STL 分档与扩容系数与 libstdc++ 不同，同一形状高一截
    inline constexpr std::uint64_t kFirstRowAllocationBudget = 14U;         ///< 实测 14，已收到实测值本身
    inline constexpr std::uint64_t kListAllocationBudget = 96U;             ///< 实测 86
    inline constexpr std::uint64_t kCountAllocationBudget = 8U;             ///< 实测 6
    inline constexpr std::uint64_t kUpdateAllocationBudget = 24U;           ///< 实测 20
    inline constexpr std::uint64_t kSmallBatchAllocationBudget = 100U;      ///< 实测 88
    inline constexpr std::uint64_t kSmallBatchBytesBudget = 12U * 1024U;    ///< 实测 9960
    inline constexpr std::uint64_t kChunkedBatchAllocationBudget = 4300U;   ///< 实测 4022
    inline constexpr std::uint64_t kChunkedBatchBytesBudget = 512U * 1024U; ///< 实测 473954
#else
    /// libstdc++（容器 GCC 13，门禁那一路带 ASan）的实测读数加一档
    inline constexpr std::uint64_t kFirstRowAllocationBudget = 13U;         ///< 实测 13，已收到实测值
    inline constexpr std::uint64_t kListAllocationBudget = 44U;             ///< 实测 37
    inline constexpr std::uint64_t kCountAllocationBudget = 8U;             ///< 实测 6
    inline constexpr std::uint64_t kUpdateAllocationBudget = 18U;           ///< 实测 16
    inline constexpr std::uint64_t kSmallBatchAllocationBudget = 100U;      ///< 实测 88，与 MSVC 同数
    inline constexpr std::uint64_t kSmallBatchBytesBudget = 12U * 1024U;    ///< 实测 8729
    inline constexpr std::uint64_t kChunkedBatchAllocationBudget = 4300U;   ///< 实测 4022，与 MSVC 同数
    inline constexpr std::uint64_t kChunkedBatchBytesBudget = 512U * 1024U; ///< 实测 415451
#endif

    /// 台账用的行数：二十行足够让「每行成本」与「每次调用成本」分得开，又不让单条用例跑太久
    constexpr std::int64_t kLedgerRowCount = 20;

    /// 批量插入台账的行数：两列写入下 SQLite 的 999 参数上限折成 499 行一条语句，20 行仍是单条
    constexpr std::size_t kSmallBatchRowCount = 20U;
    /// 分块格的行数：1000 行两列 = 2000 个参数，必然拆成三条语句并落进一个本地事务
    constexpr std::size_t kChunkedBatchRowCount = 1000U;
    /// 分块格的连跑次数：每次一千行，二十次就够摊平；再多只会把库撑大而量不出新东西
    constexpr std::uint64_t kChunkedBatchIterations = 20U;
    /// 单条格的连跑次数：文件库上每一批都是一次独立提交，容器 overlayfs 的提交成本撑不起默认的一千轮
    constexpr std::uint64_t kSmallBatchIterations = 200U;

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

    /**
     * @brief 批量插入台账用的行：两列文本/整型，表不带主键约束
     * @details 不带主键才能连着跑一千轮而不撞约束——撞了约束的那条路径分配形状完全不同，
     *          量出来的不是稳态。列数刻意保持两列，让「单条」与「分块」两格只差在行数上。
     */
    struct BatchLedgerRow
    {
        std::int64_t id;    ///< 序号，无约束
        std::string  name;  ///< 名称，刻意超过短串内联长度
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

template<>
struct AsynGyanis::Database::Queryable::TableSchema<BatchLedgerRow>
{
    static constexpr std::string_view kTableName = "ledger_batch";
    static constexpr auto kColumns = std::tuple{
        Column(&BatchLedgerRow::id,   "id"),
        Column(&BatchLedgerRow::name, "name"),
    };
    /// 无主键：本表只做批量写入的台账，按主键更新/删除的形状另有 ledger 表覆盖
    static constexpr std::string_view kPrimaryKey = "";
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

            // 批量写入格用的表：无主键、无约束，因此同一批行可以连着写一千轮而不撞约束
            ASSERT_TRUE(connection->execute(
                            "CREATE TABLE ledger_batch (id INTEGER, name TEXT)") != nullptr)
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
        // count() 只要一个标量：Release 下两台编译器都实测 6 次 / 451 字节，MSVC Debug 实测 31 次
        EXPECT_LE(countProfile.allocationsPerOperation, kCountAllocationBudget)
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

    /**
     * @brief 造一批批量插入用的行
     * @details 刻意在测量窗口之外构造：入参的行向量本身不是被测形状，被测的是「拿着这批行写一次」
     *          要碰几次堆。
     * @param rowCount 行数
     * @return std::vector<BatchLedgerRow> 批次内容
     */
    [[nodiscard]] static std::vector<BatchLedgerRow> makeBatchRows(const std::size_t rowCount)
    {
        std::vector<BatchLedgerRow> rows;
        rows.reserve(rowCount);
        for (std::size_t index = 0; index < rowCount; ++index)
        {
            rows.push_back(BatchLedgerRow{
                .id   = static_cast<std::int64_t>(index),
                .name = "batch-account-name-" + std::to_string(index)
            });
        }
        return rows;
    }

    /**
     * @brief 单条多行 INSERT 的分配台账（行数在方言的参数上限之内，不触发分块）
     * @details 实测两行/列的形状：20 行一批 = 88 次 / MSVC 9960 字节 / GCC 8729 字节，两侧次数一致、
     *          只差在分配器的分档上。4 次/行来自两处双重存放：`insertValuesOf` 先给每行建一个
     *          值向量，`translateInsertBatch` 再把每个值拷进语句的参数表（文本列因此各拷一遍）。
     *          要压掉这两个方向得改 `SqlDialect::translateInsertBatch` 的形参（换成一段扁平值 +
     *          列数并按值取走），那是对外部方言实现者的破坏性变更，先只做记录不动契约。
     */
    TEST_F(OrmAllocationLedger, SingleStatementBatchInsertAllocationLedger)
    {
        const std::vector<BatchLedgerRow> batch = makeBatchRows(kSmallBatchRowCount);

        for (int warmUp = 0; warmUp < 10; ++warmUp)
        {
            Queryable<BatchLedgerRow> query(*m_pool);
            static_cast<void>(query.insertBatch(batch));
        }

        // 次数刻意低于其它格：文件库上每批都是一次独立提交，容器 overlayfs 的提交成本会把用例拖到秒级
        const AllocationProfile profile = measureOperations(kSmallBatchIterations,
                [this, &batch]
                {
                    Queryable<BatchLedgerRow> query(*m_pool);
                    return static_cast<std::uint64_t>(query.insertBatch(batch));
                });

        EXPECT_EQ(profile.resultSum, kSmallBatchIterations * kSmallBatchRowCount)
            << "有整批没写进去，读数不能算稳态";
        EXPECT_LE(profile.allocationsPerOperation, kSmallBatchAllocationBudget)
            << "20 行一批的分配次数涨了，实测=" << profile.allocationsPerOperation;
        EXPECT_LE(profile.bytesPerOperation, kSmallBatchBytesBudget)
            << "20 行一批的申请字节涨了，实测=" << profile.bytesPerOperation;
    }

    /**
     * @brief 分块批量插入的分配台账：参数总数超上限，必然拆成多条语句并落进一个本地事务
     * @details 实测 1000 行 = 4022 次 / MSVC 473954 字节 / GCC 415451 字节，即每次约 4 次、
     *          与单条那格同一形状（分块只多了一次事务控制语句与三条语句的骨架，按行摊已看不见）。
     *          这一格守的是「分块不要退化成每行一次往返」，那一退化会是几百倍的读数。
     */
    TEST_F(OrmAllocationLedger, ChunkedBatchInsertAllocationLedger)
    {
        const std::vector<BatchLedgerRow> batch = makeBatchRows(kChunkedBatchRowCount);

        for (int warmUp = 0; warmUp < 2; ++warmUp)
        {
            Queryable<BatchLedgerRow> query(*m_pool);
            static_cast<void>(query.insertBatch(batch));
        }

        const AllocationProfile profile = measureOperations(kChunkedBatchIterations,
                [this, &batch]
                {
                    Queryable<BatchLedgerRow> query(*m_pool);
                    return static_cast<std::uint64_t>(query.insertBatch(batch));
                });

        EXPECT_EQ(profile.resultSum, kChunkedBatchIterations * kChunkedBatchRowCount)
            << "有整批没写进去，读数不能算稳态";
        EXPECT_LE(profile.allocationsPerOperation, kChunkedBatchAllocationBudget)
            << "1000 行一批的分配次数涨了，实测=" << profile.allocationsPerOperation;
        EXPECT_LE(profile.bytesPerOperation, kChunkedBatchBytesBudget)
            << "1000 行一批的申请字节涨了，实测=" << profile.bytesPerOperation;
    }

} // namespace
