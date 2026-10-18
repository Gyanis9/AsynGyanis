/**
 * @file TestPostgresIntegration.cpp
 * @brief PostgreSQL 真实服务端集成测试 —— 连接、参数化执行、ORM 端到端、事务与边界
 * @details 与同目录的 TestPostgresConnection.cpp（不依赖服务端的失败语义）互补：本文件只在
 *          真的能连上一个 PostgreSQL 服务端时才跑断言，覆盖只有真实服务端才能验证的部分——
 *          - 建连成功、isConnected()、serverVersion()、错误口令的中文失败原因；
 *          - PQexecParams 参数化执行的写与读：影响行数、列名与取值往返（中文 / 负数 / 浮点 /
 *            NULL 与空串可区分 / 长文本）、含单引号与 "--" 的文本原样回读、
 *            参数个数与语句里的 $n 不一致时由服务端拒绝；
 *          - ORM 端到端（SQL 由 PostgreSql 方言生成）：SchemaMigrator 建表 → insert →
 *            toList（WHERE + ORDER BY + LIMIT + OFFSET）→ count → tableExists → dropTable，
 *            以及 NUMERIC(20) 列被服务端接受、取值以精确十进制文本交回；
 *          - Transaction 的提交可见、回滚不可见、析构自动回滚；
 *          - 带参流式（executeStreaming(command, parameters)）：绑定参数 + 单行模式下的行序、
 *            NULL 与长文本、流式档两条哨兵，以及「在途流封锁连接」与析构收干。
 *
 * ## 口令绝不进仓库（本文件的第一条硬规矩）
 * 连接参数一律从环境变量读取，本文件不出现任何明文口令：
 * - ASYN_POSTGRES_TEST_HOST      主机，默认 127.0.0.1
 * - ASYN_POSTGRES_TEST_PORT      端口，默认 5432
 * - ASYN_POSTGRES_TEST_USER      用户名，默认 postgres
 * - ASYN_POSTGRES_TEST_PASSWORD  口令，没有默认值；未设置时全部用例 GTEST_SKIP（不是失败）
 * - ASYN_POSTGRES_TEST_DATABASE  库名，默认 postgres（初始化集群时必然存在的维护库）
 * 口令缺失即跳过，因此无服务端的 CI 与本地日常构建同样保持全绿；未编译 PostgreSQL 驱动的桩
 * 构建也走跳过路径（见 kPostgresDriverCompiled）。
 *
 * ## 为什么不新建测试专用库
 * MySQL 侧可以随手 CREATE DATABASE，PostgreSQL 不行：CREATE DATABASE 不能在事务块里执行，
 * 且普通角色未必有建库权限。默认库取初始化集群时必然存在的 postgres，用例只操作自己建的表：
 * 每个用例使用独立表名，建表前先 DROP TABLE IF EXISTS，收尾在 TearDown 里再 DROP TABLE IF EXISTS，
 * 跑完不留任何残留。表名固定而非随机，是因为 TableSchema<T>::kTableName 必须是编译期常量；
 * 用例之间不共用表名，因此并行运行（ctest -j）也不会互相干扰。
 *
 * ## 与 MySQL 集成测试的两处有意差异
 * - 内嵌 '\0' 的往返只在 BYTEA 列上断言，不在 TEXT 列上断言：PostgreSQL 的文本类型不允许零字节
 *   （服务端会以 "invalid byte sequence for encoding UTF8: 0x00" 整条拒绝），把 0 字节写进 TEXT 列
 *   不是驱动能做到的事。任意字节走 BYTEA：绑定层自己编成 `\x…`（见 toPostgresByteaText），
 *   读侧解回 BinaryBytes（见 parsePostgresByteaText），因此这一家与其余两家的差别只剩协议层。
 * - 不写死 sleep：与 MySQL 侧一致，需要「另一条连接看到什么」的地方一律新建一条连接后
 *   立即查询——PostgreSQL 默认隔离级别是 READ COMMITTED，每条 SELECT 各自取一次快照，
 *   结论确定，不需要任何等待。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Common/Decimal.h"
#include "Database/Dialect/PostgresDialect.h"
#include "Database/Postgres/PostgresConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
    using TestSupport::containsLocalizedText;
    using TestSupport::readEnvironmentPortOrDefault;
    using TestSupport::readEnvironmentTextOrDefault;

    namespace
    {
#ifdef DATABASE_HAS_POSTGRES
        /// 当前构建是否编译了真实的 libpq 驱动；桩构建里连不上任何服务端，整组跳过
        constexpr bool kPostgresDriverCompiled = true;
#else
        /// 桩构建：与「未提供口令即跳过」一起构成本文件在任何环境下都只跳过不失败的第二道保险
        constexpr bool kPostgresDriverCompiled = false;
#endif

        // ------------------------------------------------------------------------
        // 环境变量名与默认值（口令恒无默认值）
        // ------------------------------------------------------------------------

        constexpr const char *kHostVariableName     = "ASYN_POSTGRES_TEST_HOST";
        constexpr const char *kPortVariableName     = "ASYN_POSTGRES_TEST_PORT";
        constexpr const char *kUserVariableName     = "ASYN_POSTGRES_TEST_USER";
        constexpr const char *kPasswordVariableName = "ASYN_POSTGRES_TEST_PASSWORD";
        constexpr const char *kDatabaseVariableName = "ASYN_POSTGRES_TEST_DATABASE";

        constexpr const char   *kDefaultHost         = "127.0.0.1";
        constexpr std::uint16_t kDefaultPort         = 5432U;
        constexpr const char   *kDefaultUserName     = "postgres";
        constexpr const char   *kDefaultDatabaseName = "postgres";

        // ------------------------------------------------------------------------
        // 测试用表：每个用例一张独立表，互不共用
        // ------------------------------------------------------------------------

        /// 参数化插入用例的表
        constexpr std::string_view kParameterInsertTableName = "Asyn_Pg_ParamInsert";
        constexpr std::string_view kParameterInsertColumns   = "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"amount\" DOUBLE PRECISION NOT NULL";

        /// 参数化取值往返用例的表（含 NULL 列、空串列与长文本列）
        constexpr std::string_view kParameterSelectTableName = "Asyn_Pg_ParamSelect";
        constexpr std::string_view kParameterSelectColumns   = "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"amount\" DOUBLE PRECISION NOT NULL, "
                                                               "\"note\" TEXT NULL, \"payload\" TEXT NULL";

        /// 注入证明用例的表
        constexpr std::string_view kInjectionTableName = "Asyn_Pg_Injection";
        constexpr std::string_view kInjectionColumns   = "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL";

        /// 事务用例的表（提交可见 / 回滚不可见 / 析构自动回滚 / 时限透传读数）
        constexpr std::string_view kTransactionCommitTableName     = "Asyn_Pg_Tx_Commit";
        constexpr std::string_view kTransactionRollbackTableName   = "Asyn_Pg_Tx_Rollback";
        constexpr std::string_view kTransactionDestructorTableName = "Asyn_Pg_Tx_Destructor";
        constexpr std::string_view kTransactionBudgetTableName     = "Asyn_Pg_Tx_Budget";
        constexpr std::string_view kTransactionColumns             = "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"amount\" DOUBLE PRECISION NOT NULL";

        /// 建表迁移用例的表：由 SchemaMigrator 生成 DDL，表名必须是编译期常量（见下面的 TableSchema 特化）
        constexpr std::string_view kMigratedTableName = "Asyn_Pg_Migrated";
        /// 日期时间用例的表名：一个瞬时列（timestamptz）加一个墙上时间列（timestamp(6)）
        constexpr std::string_view kTemporalTableName = "Asyn_Pg_Temporal";
        /// 十进制金额用例的表名：与其余用例分开，并行执行时互不干扰
        constexpr std::string_view kDecimalTableName = "Asyn_Pg_Decimal";
        /// BYTEA 用例的表名：一列 BYTEA 加一列 TEXT，用来证明「二进制按字节、文本按字符」在这一家也分得开
        constexpr std::string_view kBinaryTableName = "Asyn_Pg_Binary";

        /// NUMERIC(20) 专用表：单独一个用例，验证无符号 64 位的承载类型被服务端接受
        constexpr std::string_view kNumericTableName = "Asyn_Pg_Numeric";

        /// 版本化迁移往返用例的表：由迁移步骤自己建、回退时撤，表名必须是编译期常量
        constexpr std::string_view kVersionedTableName          = "Asyn_Pg_Versioned";
        constexpr std::string_view kConcurrentMigrationTableOne = "Asyn_Pg_MigConcurrent_A";
        constexpr std::string_view kConcurrentMigrationTableTwo = "Asyn_Pg_MigConcurrent_B";

        /// 「UP 到一半失败」用例的探针表：第一步建它、第二步故意写坏，事务回滚后它必须不存在
        constexpr std::string_view kHalfFailedMigrationTableName = "Asyn_Pg_MigHalf";

        /// 种子幂等用例的表：由迁移建出来，表名独立以免与并行用例共用
        constexpr std::string_view kSeedTableName = "Asyn_Pg_Seed";

        /// 乐观锁用例的表：带版本列，两个会话同时改同一行
        constexpr std::string_view kVersionedLockTableName = "Asyn_Pg_VersionedLock";
        constexpr std::string_view kVersionedLockColumns   = "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"version\" BIGINT NOT NULL";

        /// 关系装载用例的三张表：由用例自建自清，表名先 grep 过整份文件确认零命中
        constexpr std::string_view kRelationAuthorTableName  = "Asyn_Pg_RelAuthors";
        constexpr std::string_view kRelationPostTableName    = "Asyn_Pg_RelPosts";
        constexpr std::string_view kRelationCommentTableName = "Asyn_Pg_RelComments";

        /**
         * @brief 读取一个环境变量
         * @details 口令只经由本函数进入测试，源码里不出现任何明文。底层实现按平台取环境变量
         *          并立即拷贝成 std::string（MSVC /W4 下 std::getenv 是弃用告警）
         * @param variableName 环境变量名
         * @return std::string 变量值；未设置时为空串
         */
        [[nodiscard]] std::string readEnvironmentText(const char *variableName)
        {
            return TestSupport::readEnvironmentVariableText(variableName);
        }

    } // namespace

    // ========================================================================
    // 测试用数据结构
    // ========================================================================

    namespace
    {
        /**
         * @brief 建表迁移用例的结构体：覆盖 SchemaMigrator 的整型、文本、可空文本、浮点与布尔分支
         *
         * @details 五列的成员类型分别对应 Int64 / Text / 可空 Text / Double / Bool，
         *          因此建表语句会同时用到 BIGINT、TEXT、DOUBLE PRECISION 与 BOOLEAN
         *          （PostgreSQL 是三个引擎里唯一有真布尔类型的，布尔列的文本形态是 't'/'f'）。
         */
        /// 十进制金额用例的行：表由 SchemaMigrator 按本映射建（PostgreSQL 落不带限定的 NUMERIC）
        struct IntegrationDecimalRow
        {
            std::int64_t                  id;     ///< 主键
            AsynGyanis::Database::Decimal amount; ///< 金额列：走 Decimal 一档
        };

        /// 日期时间用例的行：表由 SchemaMigrator 按本映射建（TIMESTAMPTZ + TIMESTAMP(6)）
        struct IntegrationTemporalRow
        {
            std::int64_t                        id;       ///< 主键
            AsynGyanis::Database::Timestamp     bookedAt; ///< UTC 瞬时
            AsynGyanis::Database::LocalDateTime localAt;  ///< 墙上时间
        };

        struct IntegrationMigratedRow
        {
            std::int64_t               id;      ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            std::string                name;    ///< 户名（TEXT NOT NULL）
            std::optional<std::string> note;    ///< 备注（TEXT，可空）
            double                     balance; ///< 余额（DOUBLE PRECISION NOT NULL）
            bool                       active;  ///< 是否启用（BOOLEAN NOT NULL）
        };

        /**
         * @brief BYTEA 用例的结构体
         *
         * @details 与 MySQL 侧的 IntegrationBinaryRow 同形：一列二进制、一列文本对照。
         *          三家共用同一条载荷（见下面那条用例里的 kSharedBinaryPayload 注释），
         *          「换引擎就换语义」这件事只有把三条用例钉成同一个形状才看得见。
         */
        struct IntegrationPgBinaryRow
        {
            std::int64_t id;      ///< 主键
            BinaryBytes  payload; ///< 任意字节（BYTEA）
            std::string  label;   ///< 文本对照列（TEXT）：证明两列在这一家也是按类型分开的
        };

        /**
         * @brief NUMERIC(20) 用例的结构体
         *
         * @details 无符号 64 位在 PostgreSQL 上没有对应的整数类型（服务端没有 unsigned），
         *          方言把它映射成 NUMERIC(20)（20 位十进制正好覆盖 18446744073709551615）。
         *          本用例只验证 DDL 被服务端接受、写入参数能落库、取值以精确十进制文本交回——
         *          注意 NUMERIC 在驱动侧的映射是 std::string（见 PostgresValueConversion.h），
         *          而 ORM 的整型成员只接受 int64_t 备选，因此这里刻意不通过 ORM 读回该列。
         */
        struct IntegrationNumericRow
        {
            std::int64_t  id;       ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            std::uint64_t sequence; ///< 序号（NUMERIC(20) NOT NULL）
        };

        /**
         * @brief 事务用例表结构体：只经由 Queryable(Transaction&) 使用
         */
        struct IntegrationTransactionRow
        {
            std::int64_t id;     ///< 主键
            std::string  name;   ///< 名称
            double       amount; ///< 金额
        };

        /**
         * @brief 时限透传用例的行型：与事务列同形，但 TableSchema 绑定到本用例专属的表
         * @details 预算透传只在经 Queryable 的语句上发生，因此这条用例必须走 ORM 而不是
         *          insertTransactionRow() 的直连通道；TableSchema 的表名是编译期常量、
         *          一个行型只能绑一张表，于是给它单起一个行型（并行用例不得共表）
         */
        struct IntegrationBudgetRow
        {
            std::int64_t id;     ///< 主键
            std::string  name;   ///< 名称
            double       amount; ///< 金额
        };

        /**
         * @brief 版本化迁移往返用例的行型：只含主键与文本列，绑定本用例专属的表
         * @details 迁移表只能来自编译期常量，一个行型只能绑一张表；与其它迁移用例分开
         *          既可避免并行时互相删表，也让「回退后重新应用」的判据落在自己的表上
         */
        struct IntegrationVersionedMigrationRow
        {
            std::int64_t id;   ///< 主键
            std::string  name; ///< 名称
        };

        /**
         * @brief 「UP 到一半失败」用例的探针行型：第一步建表用的就是它
         */
        struct IntegrationHalfFailedMigrationRow
        {
            std::int64_t id;   ///< 主键
            std::string  name; ///< 名称
        };

        /**
         * @brief 种子幂等用例的行型：显式给主键（幂等键就是它），表由迁移建出来
         */
        struct IntegrationSeedRow
        {
            std::int64_t id;   ///< 主键（种子行的幂等键）
            std::string  name; ///< 名称
        };

        /**
         * @brief 乐观锁用例的行：版本列声明进 TableSchema 之后，update() 的 WHERE 会带上它
         */
        struct IntegrationVersionedRow
        {
            std::int64_t id;      ///< 主键
            std::string  name;    ///< 业务列
            std::int64_t version; ///< 版本列（每次成功更新由服务端加一）
        };

        /// @brief 关系装载用例：作者（多对一的「一」）
        struct IntegrationRelationAuthor
        {
            std::int64_t id;   ///< 主键
            std::string  name; ///< 姓名
        };

        /// @brief 关系装载用例：评论（一对多的「多」，外键在子表这一侧）
        struct IntegrationRelationComment
        {
            std::int64_t id;     ///< 主键
            std::int64_t postId; ///< 所属帖子（外键）
            std::string  text;   ///< 正文
        };

        /**
         * @brief 关系装载用例：帖子（两个关系成员不进 kColumns，只进 kRelations）
         */
        struct IntegrationRelationPost
        {
            std::int64_t                             id;       ///< 主键
            std::string                              title;    ///< 标题
            std::int64_t                             authorId; ///< 外键列
            std::optional<IntegrationRelationAuthor> author;   ///< 装载结果：作者
            std::vector<IntegrationRelationComment>  comments; ///< 装载结果：评论
        };

    } // namespace

    // ========================================================================
    // TableSchema 特化：把结构体注册到各自的表
    // ========================================================================

    template<>
    struct Queryable::TableSchema<IntegrationDecimalRow>
    {
        static constexpr std::string_view kTableName = kDecimalTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationDecimalRow::id, "id"),
                Column(&IntegrationDecimalRow::amount, "amount"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationTemporalRow>
    {
        static constexpr std::string_view kTableName = kTemporalTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationTemporalRow::id, "id"),
                Column(&IntegrationTemporalRow::bookedAt, "booked_at"),
                Column(&IntegrationTemporalRow::localAt, "local_at"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationPgBinaryRow>
    {
        static constexpr std::string_view kTableName = kBinaryTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationPgBinaryRow::id, "id"),
                Column(&IntegrationPgBinaryRow::payload, "payload"),
                Column(&IntegrationPgBinaryRow::label, "label"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationMigratedRow>
    {
        // 表名固定为 kMigratedTableName：SchemaMigrator 的表名只能来自编译期常量
        static constexpr std::string_view kTableName = kMigratedTableName;
        // 列名与成员一一对应，覆盖 SchemaMigrator 需要处理的全部类型映射分支
        static constexpr auto kColumns = std::tuple{
                Column(&IntegrationMigratedRow::id, "id"),           Column(&IntegrationMigratedRow::name, "name"),     Column(&IntegrationMigratedRow::note, "note"),
                Column(&IntegrationMigratedRow::balance, "balance"), Column(&IntegrationMigratedRow::active, "active"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationNumericRow>
    {
        static constexpr std::string_view kTableName = kNumericTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationNumericRow::id, "id"),
                Column(&IntegrationNumericRow::sequence, "sequence"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationTransactionRow>
    {
        // 表名只能是编译期常量，因此本结构体固定绑定到「提交可见」用例的表：
        // 其余事务用例外加的表名各不相同，一律走 insertTransactionRow() 的原始参数化语句写入
        static constexpr std::string_view kTableName = kTransactionCommitTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationTransactionRow::id, "id"),
                Column(&IntegrationTransactionRow::name, "name"),
                Column(&IntegrationTransactionRow::amount, "amount"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationBudgetRow>
    {
        // 预算用例必须经 Queryable 才有透传，因此不能像其他事务用例那样走直连通道：
        // 单起一个行型把它绑到本用例专属的表上（并行执行时不得与别的用例共用表名）
        static constexpr std::string_view kTableName = kTransactionBudgetTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationBudgetRow::id, "id"),
                Column(&IntegrationBudgetRow::name, "name"),
                Column(&IntegrationBudgetRow::amount, "amount"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationVersionedMigrationRow>
    {
        static constexpr std::string_view kTableName = kVersionedTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationVersionedMigrationRow::id, "id"),
                Column(&IntegrationVersionedMigrationRow::name, "name"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationHalfFailedMigrationRow>
    {
        static constexpr std::string_view kTableName = kHalfFailedMigrationTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationHalfFailedMigrationRow::id, "id"),
                Column(&IntegrationHalfFailedMigrationRow::name, "name"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationSeedRow>
    {
        static constexpr std::string_view kTableName = kSeedTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationSeedRow::id, "id"),
                Column(&IntegrationSeedRow::name, "name"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationVersionedRow>
    {
        static constexpr std::string_view kTableName = kVersionedLockTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationVersionedRow::id, "id"),
                Column(&IntegrationVersionedRow::name, "name"),
                Column(&IntegrationVersionedRow::version, "version"),
        };
        static constexpr std::string_view kPrimaryKey    = "id";
        static constexpr std::string_view kVersionColumn = "version";
    };

    template<>
    struct Queryable::TableSchema<IntegrationRelationAuthor>
    {
        static constexpr std::string_view kTableName = kRelationAuthorTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationRelationAuthor::id, "id"),
                Column(&IntegrationRelationAuthor::name, "name"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationRelationComment>
    {
        static constexpr std::string_view kTableName = kRelationCommentTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationRelationComment::id, "id"),
                Column(&IntegrationRelationComment::postId, "post_id"),
                Column(&IntegrationRelationComment::text, "text"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationRelationPost>
    {
        static constexpr std::string_view kTableName = kRelationPostTableName;
        static constexpr auto             kColumns   = std::tuple{
                Column(&IntegrationRelationPost::id, "id"),
                Column(&IntegrationRelationPost::title, "title"),
                Column(&IntegrationRelationPost::authorId, "author_id"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
        static constexpr auto             kRelations  = std::tuple{
                AsynGyanis::Database::Queryable::Relation(&IntegrationRelationPost::author, "author_id"),
                AsynGyanis::Database::Queryable::Relation(&IntegrationRelationPost::comments, "post_id"),
        };
    };

    // ========================================================================
    // 夹具
    // ========================================================================

    namespace
    {
        using Queryable::asc;
        using Queryable::Column;
        using Queryable::SchemaMigrator;

        /**
         * @brief ORM 查询构建器模板的本地别名
         * @details 这里刻意不用 `using Queryable::Queryable;`：该 using 声明会把类模板名
         *          `Queryable` 注入匿名命名空间，而匿名命名空间的成员在 AsynGyanis::Database
         *          里可见，于是与同名的 `AsynGyanis::Database::Queryable` 命名空间构成歧义
         *          （C2872），此后每一处 `Queryable<T>` 都会编译失败。换一个别名即可绕开。
         * @tparam RowType 表数据结构类型
         */
        template<typename RowType>
        using OrmQuery = Queryable::Queryable<RowType>;

        /**
         * @brief PostgreSQL 真实服务端集成测试夹具
         *
         * @details SetUp 只做一件事：从环境变量装载连接配置（缺口令即 GTEST_SKIP）。
         *          测试库不新建（理由见文件头），表由用例自己在用例体内按需创建（prepareTable），
         *          由其记录表名并在 TearDown 里删除——这样即使断言中途失败也不会留下残留。
         */
        class PostgresIntegrationTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                // 桩构建里没有任何客户端库可连：直接跳过，不让用例以「连不上」的名义失败
                if (!kPostgresDriverCompiled)
                {
                    GTEST_SKIP() << "当前构建未编译 PostgreSQL 驱动（未定义 DATABASE_HAS_POSTGRES），"
                                    "跳过真实服务端集成测试";
                }

                const std::string passwordText = readEnvironmentText(kPasswordVariableName);
                if (passwordText.empty())
                {
                    // 口令是唯一的必需项：没有它就无法建立任何会话，本组用例整体跳过而不是失败，
                    // 因此无服务端的 CI 与本地日常构建同样保持全绿
                    GTEST_SKIP() << "未设置环境变量 " << kPasswordVariableName << "，跳过真实 PostgreSQL 集成测试（连接参数与环境变量名见文件头说明）";
                }

                m_configuration.host     = readEnvironmentTextOrDefault(kHostVariableName, kDefaultHost);
                m_configuration.port     = readEnvironmentPortOrDefault(kPortVariableName, kDefaultPort);
                m_configuration.userName = readEnvironmentTextOrDefault(kUserVariableName, kDefaultUserName);
                m_configuration.password = passwordText;
                m_configuration.database = readEnvironmentTextOrDefault(kDatabaseVariableName, kDefaultDatabaseName);
            }

            void TearDown() override
            {
                // 建表失败或本用例不需要表时列表为空；IF EXISTS 让残留表（上次崩溃剩下）也能被清掉
                for (const std::string &preparedTableName: m_preparedTableNames)
                {
                    dropTableIfExists(preparedTableName);
                }
            }

            /**
             * @brief 取得本用例的连接配置
             * @return const ConnectionConfig& 从环境变量装载的配置
             */
            [[nodiscard]] const ConnectionConfig &configuration() const noexcept
            {
                return m_configuration;
            }

            /**
             * @brief 用生产方言引用一个标识符（测试不自己拼双引号，顺带验证引用实现）
             * @param identifier 待引用的标识符
             * @return std::string 形如 "identifier" 的文本
             */
            [[nodiscard]] static std::string quote(const std::string_view identifier)
            {
                const PostgresDialect dialect;
                return dialect.quoteIdentifier(identifier);
            }

            /**
             * @brief 新建一条独立连接（不经过连接池）
             * @return std::unique_ptr<PostgresConnection> 尚未 connect() 的连接
             */
            [[nodiscard]] std::unique_ptr<PostgresConnection> makeConnection() const
            {
                return std::make_unique<PostgresConnection>(m_configuration);
            }

            /**
             * @brief 新建一个指向真实服务端的连接池
             * @param maximumPoolSize 连接数上限
             * @return std::unique_ptr<ConnectionPool> 连接池
             */
            [[nodiscard]] std::unique_ptr<ConnectionPool> makePool(const std::size_t maximumPoolSize) const
            {
                PoolConfig poolConfiguration;
                poolConfiguration.maximumPoolSize = maximumPoolSize;

                return std::make_unique<ConnectionPool>(
                        [databaseConfiguration = m_configuration]() -> std::unique_ptr<DatabaseConnection>
                        {
                            std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::createPostgres(databaseConfiguration);
                            // 连接池的工厂契约要求交出已经 connect() 完成的连接
                            connection->connect();
                            return connection;
                        },
                        poolConfiguration);
            }

            /**
             * @brief 建立本用例专用的表
             * @details 先 DROP TABLE IF EXISTS 再 CREATE TABLE，因此上次运行留下的残留表
             *          （结构可能已不同）也会被清掉，本用例总是从一张空表开始。
             *          DDL 里不写字符集子句：PostgreSQL 的库编码在 CREATE DATABASE 时就定死了，
             *          没有 MySQL 那种逐表 CHARACTER SET 的写法。
             * @param tableName 表名
             * @param columnsDdl 建表语句括号内的列定义
             * @return true 表已建好；false 失败，原因见 m_lastSetupError
             */
            [[nodiscard]] bool prepareTable(const std::string_view tableName, const std::string_view columnsDdl)
            {
                PostgresConnection connection(m_configuration);
                if (!connection.connect())
                {
                    m_lastSetupError = connection.lastError();
                    return false;
                }

                const std::string quotedTableName = quote(tableName);
                if (connection.execute("DROP TABLE IF EXISTS " + quotedTableName) == nullptr)
                {
                    m_lastSetupError = connection.lastError();
                    return false;
                }

                if (connection.execute("CREATE TABLE " + quotedTableName + " (" + std::string(columnsDdl) + ")") == nullptr)
                {
                    m_lastSetupError = connection.lastError();
                    return false;
                }

                // 记下待清理的表名：TearDown 据此删除，用例中途失败也不会留下残留
                m_preparedTableNames.emplace_back(tableName);
                return true;
            }

            /**
             * @brief 统计一张表的行数
             * @param connection 执行查询的连接
             * @param tableName 表名
             * @return std::int64_t 行数；查询失败时返回 -1（让断言直接暴露失败而不是误判成 0 行）
             */
            [[nodiscard]] static std::int64_t countRows(DatabaseConnection &connection, const std::string_view tableName)
            {
                const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT COUNT(*) FROM " + quote(tableName));
                if (result == nullptr || !result->next())
                {
                    return -1;
                }

                // PostgreSQL 的 COUNT(*) 返回 INT8（BIGINT），映射成 int64_t
                const DatabaseValue countValue  = result->getValue(0);
                const auto         *countedRows = std::get_if<std::int64_t>(&countValue);
                return countedRows != nullptr ? *countedRows : -1;
            }

            /**
             * @brief 在给定连接上按参数绑定插入一行事务用数据
             * @param connection 执行语句的连接
             * @param tableName 表名
             * @param id 主键
             * @param name 名称
             * @param amount 金额
             * @return true 语句执行成功且恰好改动一行
             */
            [[nodiscard]] static bool insertTransactionRow(DatabaseConnection &connection, const std::string_view tableName, const std::int64_t id, std::string name,
                                                           const double amount)
            {
                const std::vector<DatabaseValue>      parameters{id, std::move(name), amount};
                const std::unique_ptr<DatabaseResult> result =
                        connection.execute("INSERT INTO " + quote(tableName) + " (\"id\", \"name\", \"amount\") VALUES ($1, $2, $3)", parameters);

                return result != nullptr && result->affectedRowCount() == 1;
            }

            /**
             * @brief 读取指定主键的行，返回其 name 列
             * @param connection 执行查询的连接
             * @param tableName 表名
             * @param id 主键
             * @return std::optional<std::string> 命中行的 name；无该行或执行失败时为空
             */
            [[nodiscard]] static std::optional<std::string> readTransactionRowName(DatabaseConnection &connection, const std::string_view tableName, const std::int64_t id)
            {
                const std::vector<DatabaseValue>      parameters{id};
                const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT \"name\" FROM " + quote(tableName) + " WHERE \"id\" = $1", parameters);
                if (result == nullptr || !result->next())
                {
                    return std::nullopt;
                }

                const DatabaseValue nameValue = result->getValue(0);
                const auto         *nameText  = std::get_if<std::string>(&nameValue);
                return nameText != nullptr ? std::optional<std::string>(*nameText) : std::nullopt;
            }

            // 以下成员必须放在 protected：用例体位于派生自本夹具的测试类里，
            // 私有成员对派生类不可见，断言就无法把建表失败的原因输出到失败信息中
            ConnectionConfig         m_configuration;      ///< 从环境变量装载的连接配置
            std::vector<std::string> m_preparedTableNames; ///< 本用例建好的表名，供 TearDown 清理
            std::string              m_lastSetupError;     ///< 建表失败的原因，供断言输出

        private:
            /**
             * @brief 删除一张表，失败一律忽略（清理动作不该让用例结论变色）
             * @param tableName 待删除的表名
             */
            void dropTableIfExists(const std::string &tableName)
            {
                PostgresConnection connection(m_configuration);
                if (!connection.connect())
                {
                    return;
                }

                static_cast<void>(connection.execute("DROP TABLE IF EXISTS " + quote(tableName)));
            }
        };

    } // namespace

    // ========================================================================
    // 连接
    // ========================================================================

    /**
     * @brief 验证用环境变量里的连接参数能真正连上服务端并读到服务端版本
     */
    TEST_F(PostgresIntegrationTest, ConnectsWithEnvironmentConfigurationAndReportsServerVersion)
    {
        PostgresConnection connection(configuration());

        ASSERT_TRUE(connection.connect()) << connection.lastError();
        EXPECT_TRUE(connection.isConnected());
        EXPECT_TRUE(connection.lastError().empty());
        EXPECT_NE(connection.nativeHandle(), nullptr);

        // serverVersion() 返回的是 server_version 的版本号部分（形如 "17.11"）：非空且含数字是跨版本都成立的判据
        const std::string serverVersion = connection.serverVersion();
        EXPECT_FALSE(serverVersion.empty());
        EXPECT_NE(serverVersion.find_first_of("0123456789"), std::string::npos) << serverVersion;
        // 版本号里不该出现发行版的括号补充信息（那是被打包信息，已被裁掉）
        EXPECT_EQ(serverVersion.find('('), std::string::npos) << serverVersion;

        // 连接已建立时再次 connect() 必须是幂等的空操作（重复握手会丢掉会话状态）
        EXPECT_TRUE(connection.connect());
        EXPECT_EQ(connection.serverVersion(), serverVersion);

        connection.disconnect();
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    /**
     * @brief 验证错误口令连不上，且失败原因是面向使用者的中文
     */
    TEST_F(PostgresIntegrationTest, ConnectWithWrongPasswordFailsWithLocalizedReason)
    {
        ConnectionConfig wrongConfiguration = configuration();
        // 在真实口令后追加一段固定后缀：口令本身恒从环境变量来，本文件不出现任何明文
        wrongConfiguration.password += "-definitely-wrong-on-purpose";

        PostgresConnection connection(wrongConfiguration);

        EXPECT_FALSE(connection.connect());
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);

        const std::string failureReason = connection.lastError();
        EXPECT_FALSE(failureReason.empty());
        // 中文文案 + 点明是 PostgreSQL 驱动 + 点明失败动作。
        // 与 MySQL 侧的差异：libpq 的连接级错误不含数字错误码（SQLSTATE 只在结果集上给出），
        // 因此这里不断言「错误码」
        EXPECT_TRUE(containsLocalizedText(failureReason)) << failureReason;
        EXPECT_NE(failureReason.find("PostgreSQL"), std::string::npos) << failureReason;
        EXPECT_NE(failureReason.find("连接失败"), std::string::npos) << failureReason;
    }

    // ========================================================================
    // 参数化执行（PQexecParams）
    // ========================================================================

    /**
     * @brief 验证参数化 INSERT 成功回执携带真实的影响行数
     */
    TEST_F(PostgresIntegrationTest, ParameterizedInsertReportsSingleAffectedRow)
    {
        ASSERT_TRUE(prepareTable(kParameterInsertTableName, kParameterInsertColumns)) << m_lastSetupError;

        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const std::string                insertStatement = "INSERT INTO " + quote(kParameterInsertTableName) + " (\"id\", \"name\", \"amount\") VALUES ($1, $2, $3)";
        const std::vector<DatabaseValue> parameters{std::int64_t{1}, std::string("张三"), 1234.5};

        const std::unique_ptr<DatabaseResult> insertResult = connection.execute(insertStatement, parameters);
        ASSERT_NE(insertResult, nullptr) << connection.lastError();
        // 写语句交出的是空回执：0 行 0 列，影响行数来自 PQcmdTuples 的语句级快照
        EXPECT_EQ(insertResult->affectedRowCount(), 1);
        EXPECT_EQ(insertResult->rowCount(), 0U);
        EXPECT_TRUE(insertResult->isEmpty());

        // 用不带参数的路径独立复核：参数确实按顺序绑到了对应的列上
        const std::unique_ptr<DatabaseResult> selectResult = connection.execute("SELECT \"name\", \"amount\" FROM " + quote(kParameterInsertTableName) + " WHERE \"id\" = 1");
        ASSERT_NE(selectResult, nullptr) << connection.lastError();
        ASSERT_TRUE(selectResult->next());
        EXPECT_EQ(std::get<std::string>(selectResult->getValue("name")), "张三");
        EXPECT_DOUBLE_EQ(std::get<double>(selectResult->getValue("amount")), 1234.5);
        EXPECT_FALSE(selectResult->next());
    }

    /**
     * @brief 验证参数化 SELECT 的列名与各类取值都能原样往返，且 NULL 与空串不会混淆
     *
     * @details 本用例刻意不含内嵌 '\0' 的取值：PostgreSQL 的文本类型不允许零字节，服务端会整条拒绝
     *          （理由见文件头「与 MySQL 集成测试的两处有意差异」）。
     */
    TEST_F(PostgresIntegrationTest, ParameterizedSelectRoundTripsEveryValueKind)
    {
        ASSERT_TRUE(prepareTable(kParameterSelectTableName, kParameterSelectColumns)) << m_lastSetupError;

        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 长文本：TEXT 的上限约 1 GiB，这里给一段足够长、且含中文与数字的文本，
        // 断言「逐字节不丢」（长度相等 + 内容相等）
        std::string longText;
        for (int index = 0; index < 3000; ++index)
        {
            longText += "中文";
            longText += std::to_string(index);
        }

        const std::string insertStatement =
                "INSERT INTO " + quote(kParameterSelectTableName) + " (\"id\", \"name\", \"amount\", \"note\", \"payload\") VALUES ($1, $2, $3, $4, $5)";

        const std::vector<std::vector<DatabaseValue>> sampleRows{// 第 1 行：note 是 SQL NULL，payload 是空串——两者必须可区分
                                                                 {std::int64_t{1}, std::string("张三"), 1234.5, std::monostate{}, std::string("")},
                                                                 // 第 2 行：负数、单引号文本、中文备注
                                                                 {std::int64_t{2}, std::string("O'Brien"), -99.5, std::string("普通备注"), std::string("前前后后")},
                                                                 // 第 3 行：零值、空串备注、长文本
                                                                 {std::int64_t{3}, std::string("李四"), 0.0, std::string(""), longText}};

        for (const std::vector<DatabaseValue> &sampleRow: sampleRows)
        {
            const std::unique_ptr<DatabaseResult> insertResult = connection.execute(insertStatement, sampleRow);
            ASSERT_NE(insertResult, nullptr) << connection.lastError();
            EXPECT_EQ(insertResult->affectedRowCount(), 1);
        }

        const std::string selectStatement = "SELECT \"id\", \"name\", \"amount\", \"note\", \"payload\" FROM " + quote(kParameterSelectTableName) + " WHERE \"id\" = $1";
        const auto        selectRowById   = [&connection, &selectStatement](const std::int64_t identifier) -> std::unique_ptr<DatabaseResult>
        { return connection.execute(selectStatement, std::vector<DatabaseValue>{identifier}); };

        // ---- 第 1 行：列名、中文、正常浮点、NULL 与空串的区分 ----
        const std::unique_ptr<DatabaseResult> firstRow = selectRowById(1);
        ASSERT_NE(firstRow, nullptr) << connection.lastError();
        EXPECT_EQ(firstRow->columnCount(), 5U);
        // 列名按 SELECT 列表原样交出（方言一律给标识符加双引号，元数据里保留的是不带引号的列名）
        EXPECT_EQ(firstRow->columnNames(), (std::vector<std::string>{"id", "name", "amount", "note", "payload"}));
        EXPECT_EQ(firstRow->columnIndex("payload").value_or(99U), 4U);
        // 列名匹配区分大小写：PostgreSQL 会把未加引号的标识符折叠成小写，快照里的名字是服务端写出的原文
        EXPECT_FALSE(firstRow->columnIndex("PAYLOAD").has_value());

        ASSERT_TRUE(firstRow->next());
        // BIGINT → int64_t、TEXT → std::string、DOUBLE PRECISION → double
        EXPECT_EQ(std::get<std::int64_t>(firstRow->getValue("id")), 1);
        EXPECT_EQ(std::get<std::string>(firstRow->getValue("name")), "张三");
        EXPECT_DOUBLE_EQ(std::get<double>(firstRow->getValue("amount")), 1234.5);
        // note 是 SQL NULL → monostate；payload 是「有值且为空」的空串 → std::string
        EXPECT_TRUE(std::holds_alternative<std::monostate>(firstRow->getValue("note")));
        ASSERT_TRUE(std::holds_alternative<std::string>(firstRow->getValue("payload")));
        EXPECT_TRUE(std::get<std::string>(firstRow->getValue("payload")).empty());
        EXPECT_FALSE(firstRow->next());

        // ---- 第 2 行：单引号文本、负数、中文备注 ----
        const std::unique_ptr<DatabaseResult> secondRow = selectRowById(2);
        ASSERT_NE(secondRow, nullptr) << connection.lastError();
        ASSERT_TRUE(secondRow->next());
        EXPECT_EQ(std::get<std::string>(secondRow->getValue("name")), "O'Brien");
        EXPECT_DOUBLE_EQ(std::get<double>(secondRow->getValue("amount")), -99.5);
        EXPECT_EQ(std::get<std::string>(secondRow->getValue("note")), "普通备注");

        // ---- 第 3 行：零值、空串备注、长文本逐字节相等 ----
        const std::unique_ptr<DatabaseResult> thirdRow = selectRowById(3);
        ASSERT_NE(thirdRow, nullptr) << connection.lastError();
        ASSERT_TRUE(thirdRow->next());
        EXPECT_DOUBLE_EQ(std::get<double>(thirdRow->getValue("amount")), 0.0);
        EXPECT_TRUE(std::get<std::string>(thirdRow->getValue("note")).empty());

        const std::string readBackLongText = std::get<std::string>(thirdRow->getValue("payload"));
        EXPECT_EQ(readBackLongText.size(), longText.size());
        EXPECT_EQ(readBackLongText, longText);

        // 结果集可重新遍历：reset() 之后游标回到首行之前，行数等快照信息不变
        thirdRow->reset();
        EXPECT_EQ(thirdRow->rowCount(), 1U);
        EXPECT_TRUE(thirdRow->next());
        EXPECT_EQ(std::get<std::string>(thirdRow->getValue("name")), "李四");
    }

    /**
     * @brief 验证含单引号与 "--" 的文本经参数绑定后原样回读，且表结构未被破坏
     *
     * @details 若取值是被拼进 SQL 文本而不是绑定送入，这段文本会提前闭合字符串字面量，
     *          后半段则变成注释与新的语句：轻则插入失败，重则表被删掉。这里既断言文本原样
     *          回读，也断言注入残留（DROP TABLE）没有生效。PostgreSQL 的提示符与 MySQL 相同
     *          （单引号 + -- 行注释），因此本用例的结论在两个引擎上同义。
     */
    TEST_F(PostgresIntegrationTest, ParameterizedTextWithQuotesAndCommentMarkersRoundTripsVerbatim)
    {
        ASSERT_TRUE(prepareTable(kInjectionTableName, kInjectionColumns)) << m_lastSetupError;

        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 恶意文本直接点名本用例的表，让「注入是否生效」可被观察
        const std::string hostileText = "O'Brien -- DROP TABLE " + quote(kInjectionTableName) + "; --";
        // 先自检恶意文本确实带齐了危险字符，否则本用例会变成一个空断言
        EXPECT_NE(hostileText.find('\''), std::string::npos);
        EXPECT_NE(hostileText.find("--"), std::string::npos);
        EXPECT_NE(hostileText.find("DROP TABLE"), std::string::npos);

        const std::vector<DatabaseValue>      insertParameters{std::int64_t{1}, hostileText};
        const std::unique_ptr<DatabaseResult> insertResult =
                connection.execute("INSERT INTO " + quote(kInjectionTableName) + " (\"id\", \"name\") VALUES ($1, $2)", insertParameters);
        ASSERT_NE(insertResult, nullptr) << connection.lastError();
        ASSERT_EQ(insertResult->affectedRowCount(), 1);

        // 按值查询：WHERE 的取值同样走绑定，能精确命中说明存进去的就是原文
        const std::vector<DatabaseValue>      selectParameters{hostileText};
        const std::unique_ptr<DatabaseResult> selectResult = connection.execute("SELECT \"name\" FROM " + quote(kInjectionTableName) + " WHERE \"name\" = $1", selectParameters);
        ASSERT_NE(selectResult, nullptr) << connection.lastError();
        ASSERT_TRUE(selectResult->next());
        EXPECT_EQ(std::get<std::string>(selectResult->getValue("name")), hostileText);
        EXPECT_FALSE(selectResult->next());

        // 表仍在、恰好一行：证明 "--" 没有注释掉后续内容，分号也没有开启新语句
        EXPECT_EQ(countRows(connection, kInjectionTableName), 1);
    }

    /**
     * @brief 验证参数个数与语句里的 $n 不一致时由服务端拒绝，且原因被转成中文
     *
     * @details 与 MySQL 侧的差异：MySQL 在客户端（mysql_stmt_param_count）就能比出个数不符，
     *          PostgreSQL 的扩展查询协议把这件事交给服务端——PQexecParams 直接把参数个数与
     *          语句一起送去，个数不符时服务端回 "bind message supplies N parameters, but
     *          prepared statement ... requires M"，本驱动只负责把这段原文转成中文原因。
     */
    TEST_F(PostgresIntegrationTest, ParameterizedStatementRejectsMismatchedParameterCount)
    {
        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // "SELECT $1" 恰好一个占位符且不需要任何表，把「个数不匹配」这一条单独隔离出来
        const std::unique_ptr<DatabaseResult> tooFewResult = connection.execute("SELECT $1", std::span<const DatabaseValue>{});
        EXPECT_EQ(tooFewResult, nullptr);
        const std::string tooFewReason = connection.lastError();
        EXPECT_FALSE(tooFewReason.empty());
        EXPECT_TRUE(containsLocalizedText(tooFewReason)) << tooFewReason;
        EXPECT_NE(tooFewReason.find("PostgreSQL"), std::string::npos) << tooFewReason;
        // 服务端原文里带着 "parameters" 这一关键词，且原因里应带上可排查的 SQLSTATE
        EXPECT_NE(tooFewReason.find("SQLSTATE"), std::string::npos) << tooFewReason;

        // 多给一个参数同样失败，且失败之后连接仍然可用
        const std::vector<DatabaseValue> extraParameters{std::int64_t{1}, std::int64_t{2}};
        EXPECT_EQ(connection.execute("SELECT $1", extraParameters), nullptr);
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();
        EXPECT_TRUE(connection.isConnected());
        EXPECT_NE(connection.execute("SELECT 1"), nullptr) << connection.lastError();
    }

    /**
     * @brief 参数化数据语句走命名语句缓存：同文复用、条数有界，DISCARD ALL 后一起失效
     * @details 判据分三段：同一条 SELECT 第二次执行命中缓存（服务端不再 Parse）；灌满上限后
     *          条数稳定停在 64；resetSessionState() 之后缓存清零且下一次执行仍能重新准备成功
     */
    TEST_F(PostgresIntegrationTest, CachesPreparedStatementsAndClearsThemOnSessionReset)
    {
        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        EXPECT_EQ(connection.cachedStatementCount(), 0U);
        EXPECT_EQ(connection.statementCacheHitCount(), 0U);

        const auto runAddition = [&connection](const std::int64_t operand) { return connection.execute("SELECT $1::bigint + 1 AS answer", std::vector<DatabaseValue>{operand}); };

        {
            const std::unique_ptr<DatabaseResult> firstResult = runAddition(41);
            ASSERT_NE(firstResult, nullptr) << connection.lastError();
            ASSERT_TRUE(firstResult->next());
            EXPECT_EQ(std::get<std::int64_t>(firstResult->getValue("answer")), 42);
        }
        EXPECT_EQ(connection.cachedStatementCount(), 1U) << "首次执行没有把语句放进缓存";
        EXPECT_EQ(connection.statementCacheHitCount(), 0U);

        {
            const std::unique_ptr<DatabaseResult> secondResult = runAddition(1);
            ASSERT_NE(secondResult, nullptr) << connection.lastError();
            ASSERT_TRUE(secondResult->next());
            EXPECT_EQ(std::get<std::int64_t>(secondResult->getValue("answer")), 2);
        }
        EXPECT_EQ(connection.cachedStatementCount(), 1U) << "同一条语句重复执行长出了第二条缓存";
        EXPECT_EQ(connection.statementCacheHitCount(), 1U) << "第二次执行没有命中缓存";

        // 灌超上限：条数必须停在上界，且逐出之后仍能继续执行
        for (std::size_t index = 0; index < PostgresConnection::kMaximumCachedStatements + 8U; ++index)
        {
            ASSERT_NE(connection.execute("SELECT " + std::to_string(index) + "::bigint AS value", std::span<const DatabaseValue>{}), nullptr) << connection.lastError();
        }
        EXPECT_LE(connection.cachedStatementCount(), PostgresConnection::kMaximumCachedStatements) << "语句缓存没有上界";
        ASSERT_NE(runAddition(10), nullptr) << connection.lastError();

        // DISCARD ALL 把服务端命名语句全部收掉，本地表必须跟着失效，否则下一次命中会撞上
        // “prepared statement does not exist”
        ASSERT_TRUE(connection.resetSessionState()) << connection.lastError();
        EXPECT_EQ(connection.cachedStatementCount(), 0U) << "会话复位后本地缓存仍留着已失效的语句名";
        const std::unique_ptr<DatabaseResult> afterReset = runAddition(41);
        ASSERT_NE(afterReset, nullptr) << connection.lastError();
        ASSERT_TRUE(afterReset->next());
        EXPECT_EQ(std::get<std::int64_t>(afterReset->getValue("answer")), 42);
        EXPECT_EQ(connection.cachedStatementCount(), 1U);
    }

    // ========================================================================
    // ORM 端到端（SQL 由 PostgreSql 方言生成）
    // ========================================================================

    /**
     * @brief 验证 SchemaMigrator 在真实服务端建表后能被 ORM 直接读写，并能查存在与删表
     *
     * @details 覆盖与 MySQL 端到端用例相同的链路，但走真实 PostgreSQL：DDL 的物理类型名
     *          （BIGINT / TEXT / DOUBLE PRECISION / BOOLEAN）必须在本服务端被接受，
     *          且建出来的表要能被 ORM 的绑定与行映射直接读写。表名独立，不与其它用例共用。
     *          分页走方言的 "LIMIT $n OFFSET $n"，布尔条件走方言生成的 "= $n"——后者顺带验证
     *          bool 参数按 "true"/"false" 送出确实能被 BOOLEAN 列比较。
     */
    TEST_F(PostgresIntegrationTest, SchemaMigratorCreatesTableThenOrmRoundTripAndDrops)
    {
        // 记下待清理的表名：即使断言中途失败，TearDown 也会 DROP TABLE IF EXISTS 兜底
        m_preparedTableNames.emplace_back(kMigratedTableName);

        std::string errorText;

        // 先清掉上次运行可能留下的残留表：建表用例必须从「表不存在」这个前提出发
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationMigratedRow>(*makePool(1), true, &errorText)) << errorText;

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        EXPECT_FALSE(SchemaMigrator::tableExists<IntegrationMigratedRow>(*pool, &errorText));
        // 表确实不存在时 errorText 必须留空：它只承载「查询失败」这类原因
        EXPECT_TRUE(errorText.empty()) << errorText;

        // ---- 建表：DDL 由 SchemaMigrator 从 TableSchema 生成，类型名由 PostgreSql 方言给出 ----
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationMigratedRow>(*pool, true, &errorText)) << errorText;
        EXPECT_TRUE(SchemaMigrator::tableExists<IntegrationMigratedRow>(*pool, &errorText)) << errorText;

        // ---- 写入三行：覆盖可空列、中文、负数、空串与布尔两种取值 ----
        {
            OrmQuery<IntegrationMigratedRow> insertQuery(*pool);
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 1, .name = "张三", .note = std::string("首条"), .balance = 1234.5, .active = true}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 2, .name = "Li Si", .note = std::nullopt, .balance = -0.25, .active = false}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 3, .name = "王五", .note = std::string(""), .balance = 0.0, .active = true}));
        }

        // ---- 读回：五列逐一核对，证明 DDL 的类型与可空性恰好匹配 ORM 的映射规则 ----
        {
            OrmQuery<IntegrationMigratedRow>          query(*pool);
            const std::vector<IntegrationMigratedRow> rows = query.orderBy(asc("id")).toList();

            ASSERT_EQ(rows.size(), 3U);
            EXPECT_EQ(rows[0].id, 1);
            EXPECT_EQ(rows[0].name, "张三");
            ASSERT_TRUE(rows[0].note.has_value());
            EXPECT_EQ(rows[0].note.value(), "首条");
            EXPECT_DOUBLE_EQ(rows[0].balance, 1234.5);
            // BOOLEAN 列的文本形态是 't'/'f'，驱动映射成 bool
            EXPECT_TRUE(rows[0].active);

            EXPECT_EQ(rows[1].name, "Li Si");
            // NULL → 空 optional（与第 3 行的空串区分开）
            EXPECT_FALSE(rows[1].note.has_value());
            EXPECT_DOUBLE_EQ(rows[1].balance, -0.25);
            EXPECT_FALSE(rows[1].active);

            EXPECT_EQ(rows[2].name, "王五");
            ASSERT_TRUE(rows[2].note.has_value());
            EXPECT_TRUE(rows[2].note->empty());
            EXPECT_TRUE(rows[2].active);
        }

        // ---- WHERE + ORDER BY + LIMIT $n OFFSET $n：分页参数以占位符送出 ----
        {
            OrmQuery<IntegrationMigratedRow>          query(*pool);
            const std::vector<IntegrationMigratedRow> rows = query.orderBy(asc("id")).limit(1).offset(1).toList();

            ASSERT_EQ(rows.size(), 1U);
            EXPECT_EQ(rows[0].id, 2);
            EXPECT_EQ(rows[0].name, "Li Si");
        }

        // ---- WHERE 用 bool 参数：驱动按 "true" 送出，BOOLEAN 列必须能直接比较 ----
        {
            OrmQuery<IntegrationMigratedRow> query(*pool);
            EXPECT_EQ(query.where(Column(&IntegrationMigratedRow::active, "active") == true).count(), 2);

            OrmQuery<IntegrationMigratedRow> integerQuery(*pool);
            EXPECT_EQ(integerQuery.where(Column(&IntegrationMigratedRow::id, "id") >= std::int64_t{2}).count(), 2);

            OrmQuery<IntegrationMigratedRow> emptyQuery(*pool);
            EXPECT_EQ(emptyQuery.where(Column(&IntegrationMigratedRow::id, "id") == std::int64_t{404}).count(), 0);
        }

        // ---- 重复建表（IF NOT EXISTS）幂等：返回 true，已写入的数据不受影响 ----
        EXPECT_TRUE(SchemaMigrator::createTable<IntegrationMigratedRow>(*pool)) << errorText;
        {
            OrmQuery<IntegrationMigratedRow> countQuery(*pool);
            EXPECT_EQ(countQuery.count(), 3);
        }

        // ---- 关掉 IF NOT EXISTS 后对已存在的表如实失败，并给出可读的中文原因 ----
        errorText.clear();
        EXPECT_FALSE(SchemaMigrator::createTable<IntegrationMigratedRow>(*pool, false, &errorText));
        EXPECT_FALSE(errorText.empty());
        EXPECT_NE(errorText.find("DDL 执行失败"), std::string::npos) << errorText;

        // ---- 删表：表不存在了，且 errorText 仍为空（false 表示确实不存在而不是查询失败） ----
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationMigratedRow>(*pool, true, &errorText)) << errorText;
        EXPECT_FALSE(SchemaMigrator::tableExists<IntegrationMigratedRow>(*pool, &errorText));
        EXPECT_TRUE(errorText.empty()) << errorText;
    }

    /**
     * @brief 钉住 Decimal 成员在真实 PostgreSQL 上逐字符往返：不带限定的 NUMERIC 按写入的标度存
     *
     * @details 这一条是判据 1 的 PostgreSQL 那一格。与 MySQL 那格的关键差别在列定义：
     *          本映射在 PostgreSQL 上给的是不带精度限定的 NUMERIC，服务端按写入时的标度存，
     *          因此 "0.1000" 读回来还是 "0.1000"（尾零留着），而 MySQL 的 DECIMAL(65,30) 会把小数
     *          补到 30 位。两格都不写成「同一个断言」，因为两家的引擎契约本来就不同 ——
     *          把差别抹平正是这条判据要避免的事。
     * @note 19 位整数的金额超出 double 能逐位还原的范围；"3.000000000000000000000000000001"
     *          更超出任何 64 位浮点。读回来逐字符相等才是「没经浮点」的可证形式。
     * @note 最后那一格钉的是数值语义：NUMERIC 上的 ">" 走数值比较，"> 0.2" 命中 3 与 5 两行。
     * @note 证伪：把驱动的参数文本化改成先折成 double，超出分辨率的两位立刻读不回原样，这条红。
     */
    TEST_F(PostgresIntegrationTest, DecimalColumnKeepsEveryDigitAndItsScale)
    {
        m_preparedTableNames.emplace_back(kDecimalTableName);

        std::string errorText;
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationDecimalRow>(*makePool(1), true, &errorText)) << errorText;
        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationDecimalRow>(*pool, true, &errorText)) << errorText;

        const std::vector<std::string> writtenTexts{
                std::string("0.1000"),
                std::string("1234567890123456789"),
                std::string("3.000000000000000000000000000001"),
        };
        {
            OrmQuery<IntegrationDecimalRow> insertQuery(*pool);
            for (std::size_t rowIndex = 0; rowIndex < writtenTexts.size(); ++rowIndex)
            {
                const auto parsed = AsynGyanis::Database::Decimal::tryParse(writtenTexts[rowIndex]);
                ASSERT_TRUE(parsed.has_value()) << writtenTexts[rowIndex];
                ASSERT_EQ(1, insertQuery.insert(IntegrationDecimalRow{.id = static_cast<std::int64_t>(rowIndex + 1), .amount = *parsed})) << "写不进去：" << writtenTexts[rowIndex];
            }
        }

        {
            OrmQuery<IntegrationDecimalRow>          query(*pool);
            const std::vector<IntegrationDecimalRow> rows = query.orderBy(asc("id")).toList();
            ASSERT_EQ(rows.size(), writtenTexts.size());
            for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
            {
                EXPECT_EQ(rows[rowIndex].amount.text(), writtenTexts[rowIndex]) << "第 " << (rowIndex + 1) << " 行的金额文本被改写过";
            }
        }

        {
            OrmQuery<IntegrationDecimalRow> query(*pool);
            EXPECT_EQ(2, query.where(Column(&IntegrationDecimalRow::amount, "amount") > std::string("0.2")).count());
        }

        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationDecimalRow>(*pool, true, &errorText)) << errorText;
    }

    /**
     * @brief 验证 NUMERIC(20) 列被服务端接受，且取值以精确十进制文本交回
     *
     * @details PostgreSQL 没有无符号整数类型，方言把 UInt64 映射成 NUMERIC(20)（能无损容纳
     *          0 .. 2^64-1）。本用例验证三件事：建表被接受、写入参数能落库、读回的是精确十进制
     *          文本而不是被折成 double 的近似值——NUMERIC 在驱动侧的映射刻意是 std::string
     *          （转换规则见 PostgresValueConversion.h），因此这里用原始参数化查询读回该列。
     */
    TEST_F(PostgresIntegrationTest, SchemaMigratorAcceptsNumericColumnAndReturnsExactDecimalText)
    {
        m_preparedTableNames.emplace_back(kNumericTableName);

        std::string errorText;
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationNumericRow>(*makePool(1), true, &errorText)) << errorText;

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationNumericRow>(*pool, true, &errorText)) << errorText;
        EXPECT_TRUE(SchemaMigrator::tableExists<IntegrationNumericRow>(*pool, &errorText)) << errorText;

        // 用 UInt64 的极大值附近取值验证「不丢精度」：这类值折成 double 会变成另一个数
        constexpr std::uint64_t kLargeSequence = 18446744073709551615ULL;
        {
            OrmQuery<IntegrationNumericRow> insertQuery(*pool);
            EXPECT_EQ(1, insertQuery.insert(IntegrationNumericRow{.id = 1, .sequence = kLargeSequence}));
        }

        // 原始参数化查询读回：NUMERIC 恒按精确十进制文本交出
        {
            PostgresConnection connection(configuration());
            ASSERT_TRUE(connection.connect()) << connection.lastError();

            const std::unique_ptr<DatabaseResult> result =
                    connection.execute("SELECT \"sequence\" FROM " + quote(kNumericTableName) + " WHERE \"id\" = $1", std::vector<DatabaseValue>{std::int64_t{1}});
            ASSERT_NE(result, nullptr) << connection.lastError();
            ASSERT_TRUE(result->next());

            const DatabaseValue sequenceValue = result->getValue(0);
            ASSERT_TRUE(std::holds_alternative<std::string>(sequenceValue))
                    << "NUMERIC 应映射成精确十进制文本，实际类型为 " << databaseValueTypeName(sequenceValue) << "（值 "
                    << (std::holds_alternative<std::int64_t>(sequenceValue) ? std::to_string(std::get<std::int64_t>(sequenceValue)) : std::string("非整数备选")) << "）";
            EXPECT_EQ(std::get<std::string>(sequenceValue), "18446744073709551615");
        }

        // 全链路收尾：删表后确实不存在，且失败原因不与「表不存在」混淆
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationNumericRow>(*pool, true, &errorText)) << errorText;
        EXPECT_FALSE(SchemaMigrator::tableExists<IntegrationNumericRow>(*pool, &errorText));
        EXPECT_TRUE(errorText.empty()) << errorText;
    }

    // ========================================================================
    // 事务
    // ========================================================================

    /**
     * @brief 验证事务提交的行对另一条连接可见，提交前不可见
     */
    TEST_F(PostgresIntegrationTest, TransactionCommitIsVisibleToAnotherConnection)
    {
        ASSERT_TRUE(prepareTable(kTransactionCommitTableName, kTransactionColumns)) << m_lastSetupError;

        // 观察者是一条完全独立的连接：默认隔离级别 READ COMMITTED 下每条 SELECT 各自取快照，
        // 因此「提交前看不见 / 提交后看得见」是确定性结论，不需要任何等待
        std::unique_ptr<PostgresConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionCommitTableName), 0);

        std::unique_ptr<ConnectionPool> pool = makePool(3);
        {
            Transaction transaction(*pool);
            EXPECT_TRUE(transaction.isActive());

            // 走事务连接写入：Queryable(Transaction&) 保证语句与 BEGIN 落在同一条连接上，
            // 否则写语句会运行在自动提交模式，回滚只能回滚一个空事务
            OrmQuery<IntegrationTransactionRow> transactionalQuery(transaction);
            EXPECT_EQ(1, transactionalQuery.insert(IntegrationTransactionRow{.id = 1, .name = "事务提交", .amount = 12.5}));

            // 提交之前：写者自己的连接能读到，另一条连接读不到
            const std::optional<IntegrationTransactionRow> uncommittedRow = transactionalQuery.where(Column(&IntegrationTransactionRow::id, "id") == std::int64_t{1}).first();
            ASSERT_TRUE(uncommittedRow.has_value());
            EXPECT_EQ(uncommittedRow->name, "事务提交");
            EXPECT_EQ(countRows(*observer, kTransactionCommitTableName), 0);

            ASSERT_TRUE(transaction.commit()) << transaction.lastError();
            EXPECT_FALSE(transaction.isActive());
        }

        // 提交之后：另一条连接立刻可见，且字段值完整
        EXPECT_EQ(countRows(*observer, kTransactionCommitTableName), 1);
        const std::optional<std::string> committedName = readTransactionRowName(*observer, kTransactionCommitTableName, 1);
        ASSERT_TRUE(committedName.has_value());
        EXPECT_EQ(committedName.value(), "事务提交");
    }

    /**
     * @brief 钉住：整笔事务的剩余预算透传到服务端的 statement_timeout
     *
     * @details 走事务的语句在下发前把「剩余预算与连接会话级超时的更严者」交给引擎，
     *          正在执行的语句因此能在预算耗尽时被服务端打断（打断能力本身由
     *          QueryTimeoutInterruptsSlowStatementOnEstablishedConnection 钉住，
     *          本条只钉「预算确实到了服务端」这一环）。读数取 pg_settings 的数字形式而不是
     *          SHOW 的 "10s" 文本，判据不依赖文本渲染。直连通道读这条变量不经 Queryable，
     *          不会再触发一次透传，读到的就是上一条语句下发的结果。
     *          证伪：拆掉 Queryable 取租约时的透传调用，statement_timeout 停在基线 30000，变红。
     */
    TEST_F(PostgresIntegrationTest, TransactionTimeBudgetReachesTheServerStatementTimeout)
    {
        ASSERT_TRUE(prepareTable(kTransactionBudgetTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(1);
        {
            Transaction transaction(*pool);
            transaction.setTimeLimit(std::chrono::seconds(10));

            OrmQuery<IntegrationBudgetRow> transactionalQuery(transaction);
            ASSERT_EQ(1, transactionalQuery.insert(IntegrationBudgetRow{.id = 1, .name = "预算透传", .amount = 1.0}));

            const std::unique_ptr<DatabaseResult> probe = transaction.connection().execute("SELECT setting::bigint FROM pg_settings WHERE name = 'statement_timeout'");
            ASSERT_NE(probe, nullptr) << transaction.connection().lastError();
            ASSERT_TRUE(probe->next());
            const DatabaseValue probeValue   = probe->getValue(0);
            const auto         *milliseconds = std::get_if<std::int64_t>(&probeValue);
            ASSERT_NE(milliseconds, nullptr) << "statement_timeout 的读数不是整数毫秒";
            EXPECT_LE(*milliseconds, 10000) << "预算没有收紧服务端的语句时限，读数 " << *milliseconds;
            EXPECT_GT(*milliseconds, 5000) << "读数 " << *milliseconds << " 不像 10 秒预算刚走掉几毫秒的样子";

            ASSERT_TRUE(transaction.commit()) << transaction.lastError();
        }
    }

    /**
     * @brief 验证事务回滚后任何连接都看不到那一行
     */
    TEST_F(PostgresIntegrationTest, TransactionRollbackLeavesNoRowBehind)
    {
        ASSERT_TRUE(prepareTable(kTransactionRollbackTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);
        {
            Transaction transaction(*pool);

            ASSERT_TRUE(insertTransactionRow(transaction.connection(), kTransactionRollbackTableName, 1, "事务回滚", 3.5)) << transaction.connection().lastError();
            // 事务内可见：证明这一行确实被写进去过，回滚要撤销的是真实存在的数据
            EXPECT_EQ(countRows(transaction.connection(), kTransactionRollbackTableName), 1);

            ASSERT_TRUE(transaction.rollback()) << transaction.lastError();
            EXPECT_FALSE(transaction.isActive());
            // 回滚是幂等的：事务已结束时再提交/回滚都是空操作，不会误发 COMMIT 把数据救回来
            EXPECT_TRUE(transaction.commit());
            EXPECT_TRUE(transaction.rollback());
        }

        // 换一条全新的连接复核：库表里没有任何残留
        std::unique_ptr<PostgresConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionRollbackTableName), 0);
    }

    /**
     * @brief 验证事务对象析构（未提交）时自动回滚
     */
    TEST_F(PostgresIntegrationTest, TransactionDestructorRollsBackUncommittedRows)
    {
        ASSERT_TRUE(prepareTable(kTransactionDestructorTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);
        {
            Transaction transaction(*pool);
            ASSERT_TRUE(insertTransactionRow(transaction.connection(), kTransactionDestructorTableName, 1, "析构未提交", 1.0)) << transaction.connection().lastError();
            // 事务内可见，说明行已写入、只是没提交
            EXPECT_EQ(countRows(transaction.connection(), kTransactionDestructorTableName), 1);
            EXPECT_TRUE(transaction.isActive());

            // 刻意不调用 commit()/rollback()：交给析构函数收尾
        }

        std::unique_ptr<PostgresConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionDestructorTableName), 0);
    }

    // ========================================================================
    // 本轮新增：多语句拒 / 冲突处置 / 流式 / 命令超时 / 池复位（真机）
    // ========================================================================

    namespace
    {
        /// 冲突处置用例的表
        constexpr std::string_view kConflictTableName = "Asyn_Pg_Conflict";

        /**
         * @brief 冲突处置用例的结构体：主键 + 业务列
         */
        struct IntegrationConflictRow
        {
            std::int64_t id;   ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            std::string  name; ///< 业务列（TEXT NOT NULL）
        };
    } // namespace

    template<>
    struct Queryable::TableSchema<IntegrationConflictRow>
    {
        static constexpr std::string_view kTableName  = kConflictTableName;
        static constexpr auto             kColumns    = std::tuple{Column(&IntegrationConflictRow::id, "id"), Column(&IntegrationConflictRow::name, "name")};
        static constexpr std::string_view kPrimaryKey = "id";
    };

    /**
     * @brief 钉住「带第二条语句的文本整次被拒，且第一条也不执行」——多语句注入那道防线要可证
     *
     * @details 两条 execute 都走扩展查询协议（PQexecParams），服务端在解析阶段拒绝多语句；
     *          首条是 DDL 时「首条没落库」由「随后查这张表失败」直接证明。与 MySQL 侧
     *          CLIENT_MULTI_STATEMENTS 的姿态对齐：半执行状态比整次报错危险得多。
     */
    TEST_F(PostgresIntegrationTest, MultipleStatementsAreRejectedWithoutRunningTheFirst)
    {
        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        static constexpr std::string_view kNeverCreatedTable = "Asyn_Pg_MultiStatementProbe";
        static_cast<void>(connection.execute("DROP TABLE IF EXISTS " + quote(kNeverCreatedTable)));

        const std::string script = "CREATE TABLE " + quote(kNeverCreatedTable) + " (id BIGINT); SELECT 1";
        EXPECT_EQ(connection.execute(script), nullptr) << "多语句文本没被拒绝（扩展协议应当在解析阶段整次拒）";
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();

        EXPECT_EQ(connection.execute("SELECT * FROM " + quote(kNeverCreatedTable)), nullptr) << "第二条被拒了但第一条已经执行：留下半执行状态";
        EXPECT_TRUE(connection.isConnected()) << "服务端拒绝一条语句不该把链路判断：" << connection.lastError();
    }

    /**
     * @brief 钉住 PG 的冲突处置真机语义：Ignore 跳过、Replace 覆盖、Fail 报 23505，且都只留一行
     *
     * @details 走 ORM → 方言的完整链路（translateInsert 的 ON CONFLICT 尾部子句），冲突键是主键。
     *          Fail 档顺带验证「原生码与文本成对写入」：SQLSTATE 23505 经
     *          postgresSqlStateNativeCode() 落到异常的 nativeErrorCode() 上。
     */
    TEST_F(PostgresIntegrationTest, InsertConflictUpsertsThroughDoNothingAndDoUpdate)
    {
        m_preparedTableNames.emplace_back(kConflictTableName);
        ASSERT_TRUE(prepareTable(kConflictTableName, "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL")) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(1);

        OrmQuery<IntegrationConflictRow> query(*pool);
        EXPECT_EQ(query.insert(IntegrationConflictRow{.id = 1, .name = "first"}), 1);

        // Ignore（ON CONFLICT DO NOTHING）：冲突行跳过，不覆盖旧值，影响行数是 0
        EXPECT_EQ(query.insert(IntegrationConflictRow{.id = 1, .name = "ignored"}, Queryable::InsertConflict::Ignore), 0);
        // Replace（ON CONFLICT DO UPDATE SET 列 = EXCLUDED.列）：按列清单覆盖，影响行数是 1
        EXPECT_EQ(query.insert(IntegrationConflictRow{.id = 1, .name = "replaced"}, Queryable::InsertConflict::Replace), 1);
        // Upsert：PG 上与 Replace 同一份 DO UPDATE；ORM 必须自动带上冲突目标，否则方言会拒绝翻译
        EXPECT_EQ(query.insert(IntegrationConflictRow{.id = 1, .name = "upserted"}, Queryable::InsertConflict::Upsert), 1);

        // Fail：主键冲突报错，且异常的 nativeErrorCode() 必须是 SQLSTATE 23505 的打包值
        bool nativeCodeMatched = false;
        try
        {
            static_cast<void>(query.insert(IntegrationConflictRow{.id = 1, .name = "boom"}));
            FAIL() << "主键冲突没有报错";
        } catch (const QueryExecutionException &error)
        {
            nativeCodeMatched = error.nativeErrorCode() == postgresSqlStateNativeCode("23505");
            EXPECT_TRUE(nativeCodeMatched) << "原生码不是 23505：" << error.what();
        }

        const std::vector<IntegrationConflictRow> rows = query.orderBy(asc("id")).toList();
        ASSERT_EQ(rows.size(), 1U) << "upsert 留下了多行";
        EXPECT_EQ(rows[0].name, "upserted");
    }

    /**
     * @brief 钉住流式读取：逐行取全、未读完即释放时连接仍可复用、在途期间拒发新语句、reset 拒重扫
     *
     * @details 单行模式（PQsetSingleRowMode）下每 next() 取一块一行；本用例把四条契约一次钉死——
     *          行数回 0 的含义是「未知」、isEmpty() 不宣称空、析构收干后连接恢复可用、
     *          在途未清时新语句在发送前被拒。
     */
    TEST_F(PostgresIntegrationTest, StreamingReadsEveryRowAndReleasesTheConnectionWhenDisposed)
    {
        static constexpr std::string_view kStreamingTableName = "Asyn_Pg_Streaming";
        m_preparedTableNames.emplace_back(kStreamingTableName);
        ASSERT_TRUE(prepareTable(kStreamingTableName, "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL")) << m_lastSetupError;

        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        for (std::int64_t rowIndex = 1; rowIndex <= 5; ++rowIndex)
        {
            ASSERT_NE(connection.execute("INSERT INTO " + quote(kStreamingTableName) + " (\"id\", \"name\") VALUES ($1, $2)",
                                         std::vector<DatabaseValue>{rowIndex, std::string("第" + std::to_string(rowIndex) + "行")}),
                      nullptr)
                    << connection.lastError();
        }

        // ---- 读完：行序、取值与两条流式口径 ----
        {
            const std::unique_ptr<DatabaseResult> stream = connection.executeStreaming("SELECT \"id\", \"name\" FROM " + quote(kStreamingTableName) + " ORDER BY \"id\"");
            ASSERT_NE(stream, nullptr) << connection.lastError();

            // 流式口径：rowCount() 回 0 的含义是「未知」，且不宣称自己空
            EXPECT_EQ(stream->rowCount(), 0U);
            EXPECT_FALSE(stream->isEmpty());

            std::int64_t expectedId = 1;
            while (stream->next())
            {
                EXPECT_EQ(std::get<std::int64_t>(stream->getValue("id")), expectedId);
                EXPECT_EQ(std::get<std::string>(stream->getValue("name")), "第" + std::to_string(expectedId) + "行");
                ++expectedId;
            }
            EXPECT_EQ(expectedId, 6) << "流没有读全";
        }

        // 读完之后连接恢复可用（在途计数已归位）
        {
            const std::unique_ptr<DatabaseResult> countResult = connection.execute("SELECT COUNT(*) FROM " + quote(kStreamingTableName));
            ASSERT_NE(countResult, nullptr) << "流读完后连接仍不可用：" << connection.lastError();
            ASSERT_TRUE(countResult->next());
            EXPECT_EQ(std::get<std::int64_t>(countResult->getValue(0)), 5);
        }

        // ---- 未读完即释放：析构把剩余块收干，连接同样恢复可用 ----
        {
            const std::unique_ptr<DatabaseResult> stream = connection.executeStreaming("SELECT \"id\" FROM " + quote(kStreamingTableName) + " ORDER BY \"id\"");
            ASSERT_NE(stream, nullptr) << connection.lastError();
            ASSERT_TRUE(stream->next()); // 只读一行就走
        } // 结果集在这里析构（收干剩余块）

        const std::unique_ptr<DatabaseResult> afterDrop = connection.execute("SELECT COUNT(*) FROM " + quote(kStreamingTableName));
        ASSERT_NE(afterDrop, nullptr) << "未读完的流析构后连接仍不可用：" << connection.lastError();

        // ---- 在途期间拒发新语句；reset 拒绝重扫 ----
        {
            const std::unique_ptr<DatabaseResult> stream = connection.executeStreaming("SELECT \"id\" FROM " + quote(kStreamingTableName));
            ASSERT_NE(stream, nullptr) << connection.lastError();

            EXPECT_EQ(connection.execute("SELECT 1"), nullptr) << "在途未清流时连接不该放行新语句";
            EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();

            stream->reset();
            EXPECT_NE(stream->lastError().find("重扫"), std::string::npos) << stream->lastError();
        }
    }

    /**
     * @brief 带参流式：参数绑定 + 单行模式——行序、长文本、NULL 与流式档两条哨兵都要对
     */
    TEST_F(PostgresIntegrationTest, ParameterizedStreamingReadsEveryRowWithBoundParameters)
    {
        static constexpr std::string_view kTableName = "Asyn_Pg_ParamStream";
        m_preparedTableNames.emplace_back(kTableName);
        ASSERT_TRUE(prepareTable(kTableName, "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"payload\" TEXT NULL")) << m_lastSetupError;

        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        std::string bigPayload;
        for (int index = 0; index < 2000; ++index)
        {
            bigPayload += "流式";
            bigPayload += std::to_string(index);
        }

        const std::string insertStatement = "INSERT INTO " + quote(kTableName) + " (\"id\", \"name\", \"payload\") VALUES ($1, $2, $3)";
        for (std::int64_t rowIndex = 1; rowIndex <= 8; ++rowIndex)
        {
            std::vector<DatabaseValue> row{rowIndex, std::string("第" + std::to_string(rowIndex) + "行"), std::string("small")};
            if (rowIndex == 1)
            {
                row[2] = bigPayload;
            } else if (rowIndex == 4)
            {
                row[2] = std::monostate{};
            }
            ASSERT_NE(connection.execute(insertStatement, row), nullptr) << connection.lastError();
        }

        // 与不带参的流式同一条契约：行数未知回 0、不宣称自己空；参数经 $1 真实绑定参与过滤
        const std::unique_ptr<DatabaseResult> stream = connection.executeStreaming(
                "SELECT \"id\", \"name\", \"payload\" FROM " + quote(kTableName) + " WHERE \"id\" >= $1 ORDER BY \"id\"", std::vector<DatabaseValue>{std::int64_t{1}});
        ASSERT_NE(stream, nullptr) << connection.lastError();
        EXPECT_EQ(stream->rowCount(), 0U);
        EXPECT_FALSE(stream->isEmpty());

        std::int64_t expectedId = 1;
        while (stream->next())
        {
            EXPECT_EQ(std::get<std::int64_t>(stream->getValue("id")), expectedId) << "行序错乱";
            EXPECT_EQ(std::get<std::string>(stream->getValue("name")), "第" + std::to_string(expectedId) + "行");
            if (expectedId == 1)
            {
                const std::string payload = std::get<std::string>(stream->getValue("payload"));
                EXPECT_EQ(payload.size(), bigPayload.size());
                EXPECT_EQ(payload, bigPayload);
            }
            if (expectedId == 4)
            {
                EXPECT_TRUE(std::holds_alternative<std::monostate>(stream->getValue("payload"))) << "NULL 被读成了别的值";
            }
            ++expectedId;
        }
        EXPECT_EQ(expectedId, 9) << "流没有读全";

        // 读完即收口：连接接着可用
        const std::unique_ptr<DatabaseResult> afterDrain = connection.execute("SELECT 1");
        ASSERT_NE(afterDrain, nullptr) << connection.lastError();
    }

    /**
     * @brief 带参流在途封锁连接：没读完之前执行入口都在发送之前拒，析构收干后恢复
     */
    TEST_F(PostgresIntegrationTest, ParameterizedStreamingBlocksTheConnectionUntilDisposed)
    {
        static constexpr std::string_view kTableName = "Asyn_Pg_ParamStreamHold";
        m_preparedTableNames.emplace_back(kTableName);
        ASSERT_TRUE(prepareTable(kTableName, "\"id\" BIGINT PRIMARY KEY")) << m_lastSetupError;

        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        for (std::int64_t rowIndex = 1; rowIndex <= 5; ++rowIndex)
        {
            ASSERT_NE(connection.execute("INSERT INTO " + quote(kTableName) + " (\"id\") VALUES ($1)", std::vector<DatabaseValue>{rowIndex}), nullptr) << connection.lastError();
        }

        {
            const std::unique_ptr<DatabaseResult> stream =
                    connection.executeStreaming("SELECT \"id\" FROM " + quote(kTableName) + " WHERE \"id\" >= $1 ORDER BY \"id\"", std::vector<DatabaseValue>{std::int64_t{1}});
            ASSERT_NE(stream, nullptr) << connection.lastError();
            ASSERT_TRUE(stream->next());

            EXPECT_EQ(connection.execute("SELECT 1"), nullptr) << "流没读完时又发了一条语句";
            EXPECT_NE(connection.lastError().find("未消费完"), std::string::npos) << connection.lastError();
        } // 析构把剩余单行块收干

        const std::unique_ptr<DatabaseResult> afterDisposal = connection.execute("SELECT 1");
        ASSERT_NE(afterDisposal, nullptr) << connection.lastError();
    }

    /**
     * @brief 钉住 setQueryTimeout 在已建立的连接上当场生效：慢语句被打断且原生码是 57014
     *
     * @details statement_timeout 是服务端会话变量（建连时下发、存续期可改）；超时打断的
     *          SQLSTATE 是 57014（query_canceled），本驱动把它成对写进原生码——本用例同时
     *          验证「查询被取消」与「码与文本配对」两件事，并确认打断只是语句级处置、
     *          解除时限后同一条连接照常可用。
     */
    TEST_F(PostgresIntegrationTest, QueryTimeoutInterruptsSlowStatementOnEstablishedConnection)
    {
        PostgresConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        connection.setQueryTimeout(200);

        const std::unique_ptr<DatabaseResult> interrupted = connection.execute("SELECT pg_sleep(5)");
        EXPECT_EQ(interrupted, nullptr) << "慢语句没有被语句时限打断";
        EXPECT_EQ(connection.lastNativeErrorCode(), postgresSqlStateNativeCode("57014")) << connection.lastError();

        // 解除时限后同一条连接仍然可用
        connection.setQueryTimeout(0);
        const std::unique_ptr<DatabaseResult> afterReset = connection.execute("SELECT 1");
        ASSERT_NE(afterReset, nullptr) << "时限打断后连接不可用：" << connection.lastError();
    }

    /**
     * @brief 钉住池归还时的会话复位：借用者留下的会话变量不会串给下一位
     *
     * @details 归还路径走本驱动的 resetSessionState（DISCARD ALL）；这里用 application_name
     *          作可观察的会话变量：借出者设定它，归还后下一位借用者读到的必须是复位后的默认值。
     */
    TEST_F(PostgresIntegrationTest, PooledReturnClearsSessionStateForNextBorrower)
    {
        std::unique_ptr<ConnectionPool> pool = makePool(1);

        {
            PooledConnection connection = pool->acquire();
            ASSERT_TRUE(connection);

            // application_name 是普通 GUC：借用者留下的值若不复位就会串给下一位
            ASSERT_NE(connection->execute("SET application_name = 'asyn-integration-borrower'"), nullptr) << connection->lastError();

            const std::unique_ptr<DatabaseResult> probe = connection->execute("SHOW application_name");
            ASSERT_NE(probe, nullptr) << connection->lastError();
            ASSERT_TRUE(probe->next());
            EXPECT_EQ(std::get<std::string>(probe->getValue(0)), "asyn-integration-borrower");
        } // 归还：池在这里调用 resetSessionState（DISCARD ALL）

        PooledConnection next = pool->acquire();
        ASSERT_TRUE(next);

        const std::unique_ptr<DatabaseResult> afterReturn = next->execute("SHOW application_name");
        ASSERT_NE(afterReturn, nullptr) << next->lastError();
        ASSERT_TRUE(afterReturn->next());
        EXPECT_NE(std::get<std::string>(afterReturn->getValue(0)), "asyn-integration-borrower") << "会话变量跟着连接串给了下一位借用者";
    }

    /**
     * @brief 真机上钉住乐观锁的「两个会话同时改同一行」：后写的拿到冲突，先写的改动没被抹掉
     *
     * @details 两个会话各自读出行（版本都是 0），会话一先写成功，会话二拿过期版本再写——
     *          必须抛 OptimisticLockException，且会话一写进去的值原样还在。这里没有任何唯一键
     *          冲突，唯一的失败来源就是版本条件；版本加一必须由服务端的 SET 完成。
     */
    TEST_F(PostgresIntegrationTest, OptimisticLockRejectsTheStaleWriteFromASecondSession)
    {
        ASSERT_TRUE(prepareTable(kVersionedLockTableName, kVersionedLockColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool>   pool = makePool(2);
        OrmQuery<IntegrationVersionedRow> firstSession(*pool);
        OrmQuery<IntegrationVersionedRow> secondSession(*pool);

        ASSERT_EQ(firstSession.insert(IntegrationVersionedRow{.id = 1, .name = "原值", .version = 0}), 1);

        const std::optional<IntegrationVersionedRow> firstView  = firstSession.where(Column(&IntegrationVersionedRow::id, "id") == std::int64_t{1}).first();
        const std::optional<IntegrationVersionedRow> secondView = secondSession.where(Column(&IntegrationVersionedRow::id, "id") == std::int64_t{1}).first();
        ASSERT_TRUE(firstView.has_value());
        ASSERT_TRUE(secondView.has_value());
        ASSERT_EQ(firstView->version, 0);
        ASSERT_EQ(secondView->version, 0);

        IntegrationVersionedRow firstRow = *firstView;
        firstRow.name                    = "会话一写的";
        EXPECT_EQ(firstSession.update(firstRow), 1) << "第一个写者不该失败";

        IntegrationVersionedRow secondRow = *secondView;
        secondRow.name                    = "会话二写的";
        try
        {
            static_cast<void>(secondSession.update(secondRow));
            FAIL() << "会话二用过期版本写成功了：会话一的改动已经被静默覆盖";
        } catch (const OptimisticLockException &error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find(kVersionedLockTableName), std::string::npos) << "冲突消息没点出表名：" << message;
            EXPECT_NE(message.find("version"), std::string::npos) << "冲突消息没点出版本列：" << message;
            EXPECT_NE(message.find("期望值 0"), std::string::npos) << "冲突消息没点出期望的版本值：" << message;
        }

        const std::optional<IntegrationVersionedRow> after = OrmQuery<IntegrationVersionedRow>(*pool).where(Column(&IntegrationVersionedRow::id, "id") == std::int64_t{1}).first();
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->name, "会话一写的") << "先写的那份数据被覆盖了";
        EXPECT_EQ(after->version, 1) << "成功的这次没有把版本加一";
    }

    /**
     * @brief 真机上钉住版本化迁移的四格：UP 到一半失败必须真回滚、应用、幂等、回退、再应用
     *
     * @details PostgreSQL 的 DDL 在事务里可回滚，这是 MySQL 侧（DDL 隐式提交）测不到的一面：
     *          第一条 up 语句把表建出来、第二条被服务端拒绝时，`runStep` 的那笔事务必须把
     *          第一句的影响一起撤掉，迁移记录也不许留下。把第二条故意写坏的语句去掉，本用例
     *          的 `ASSERT_FALSE(migrate(...))` 当场变红——这条判据因此不是「跑了一圈都成功」。
     *
     *          随后用一份正常的迁移走完整圈：应用 → 重复应用 0 条 → 回退（表与记账一起消失）
     *          → 再应用把结构建回来。
     */
    TEST_F(PostgresIntegrationTest, VersionedMigrationAppliesRollsBackAndReapplies)
    {
        {
            PostgresConnection cleanup(m_configuration);
            ASSERT_TRUE(cleanup.connect()) << cleanup.lastError();
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kVersionedTableName)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kHalfFailedMigrationTableName)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(SchemaMigrator::kMigrationLedgerTableName)));
        }
        // 迁移自己建表，因此只登记清理名（不要先 prepareTable()，那样测的就不是迁移了）
        m_preparedTableNames.emplace_back(kVersionedTableName);
        m_preparedTableNames.emplace_back(kHalfFailedMigrationTableName);
        m_preparedTableNames.emplace_back(SchemaMigrator::kMigrationLedgerTableName);

        std::unique_ptr<ConnectionPool> pool = makePool(1);

        const std::shared_ptr<SqlDialect> dialect = SchemaMigrator::dialectOf(*pool);
        ASSERT_NE(dialect, nullptr);

        // ---- UP 到一半失败：同一笔迁移的第一句建表成功、第二句被服务端拒，事务必须整体回滚 ----
        const std::vector<SchemaMigrator::MigrationStep> halfFailingSteps{
                SchemaMigrator::MigrationStep{
                        .name           = "0001-half-applied",
                        .upStatements   = {SchemaMigrator::createTableStatement<IntegrationHalfFailedMigrationRow>(*dialect),
                                           SqlStatement{.sql = "CREATE TABLE \"Asyn_Pg_MigHalf_Broken\" ("}},
                        .downStatements = {SchemaMigrator::dropTableStatement<IntegrationHalfFailedMigrationRow>(*dialect)},
                },
        };

        std::size_t appliedOnFailure = 0;
        std::string error;
        ASSERT_FALSE(SchemaMigrator::migrate(*pool, halfFailingSteps, &appliedOnFailure, &error)) << "第二条 up 语句故意写坏，迁移却成功了";
        EXPECT_EQ(appliedOnFailure, 0U);
        EXPECT_NE(error.find("已回滚"), std::string::npos) << "失败原因没说明本次已回滚：" << error;
        EXPECT_FALSE(SchemaMigrator::tableExists<IntegrationHalfFailedMigrationRow>(*pool, &error)) << "UP 的第二句失败后第一句建的表还在：DDL 没有跟着事务回滚";
        std::vector<std::string> appliedAfterFailure;
        ASSERT_TRUE(SchemaMigrator::appliedMigrations(*pool, &appliedAfterFailure, &error)) << error;
        EXPECT_TRUE(appliedAfterFailure.empty()) << "失败的迁移留下了记账";

        // ---- 正常往返：应用 → 幂等 → 回退 → 再应用 ----
        const std::vector<SchemaMigrator::MigrationStep> steps{
                SchemaMigrator::MigrationStep{
                        .name           = "0001-create-versioned",
                        .upStatements   = {SchemaMigrator::createTableStatement<IntegrationVersionedMigrationRow>(*dialect)},
                        .downStatements = {SchemaMigrator::dropTableStatement<IntegrationVersionedMigrationRow>(*dialect)},
                },
        };

        std::size_t applied = 0;
        ASSERT_TRUE(SchemaMigrator::migrate(*pool, steps, &applied, &error)) << error;
        EXPECT_EQ(applied, 1U);
        EXPECT_TRUE(SchemaMigrator::tableExists<IntegrationVersionedMigrationRow>(*pool, &error)) << "迁移没把表建出来";

        std::size_t appliedAgain = 0;
        ASSERT_TRUE(SchemaMigrator::migrate(*pool, steps, &appliedAgain, &error)) << error;
        EXPECT_EQ(appliedAgain, 0U) << "重复应用同一迁移";

        std::size_t rolledBack = 0;
        ASSERT_TRUE(SchemaMigrator::rollbackLast(*pool, steps, 1, &rolledBack, &error)) << error;
        EXPECT_EQ(rolledBack, 1U);
        EXPECT_FALSE(SchemaMigrator::tableExists<IntegrationVersionedMigrationRow>(*pool, &error)) << "回退没把表撤掉";
        std::vector<std::string> recorded;
        ASSERT_TRUE(SchemaMigrator::appliedMigrations(*pool, &recorded, &error)) << error;
        EXPECT_TRUE(recorded.empty()) << "记账没跟着回退一起删";

        std::size_t reapplied = 0;
        ASSERT_TRUE(SchemaMigrator::migrate(*pool, steps, &reapplied, &error)) << error;
        EXPECT_EQ(reapplied, 1U);
        EXPECT_TRUE(SchemaMigrator::tableExists<IntegrationVersionedMigrationRow>(*pool, &error)) << "回退之后重新应用没把结构建回来";
    }

    /**
     * @brief 真机上钉住迁移的跨实例互斥：两个连接池同时 migrate，每一步只应用一次
     *
     * @details 两个池各持一条连接（上限 1），在同一份步骤表上并发调用 `migrate()`。
     *          咨询锁让其中一个先跑完，另一个等锁后读到最新的记账、一步都不再应用。
     *          判据三条：两个调用都成功、两个实例的 appliedCount 之和等于步骤数、
     *          台账里每个名字只有一行。把取锁那一步去掉，两个实例会同时读到空台账并各应用
     *          一遍——第二个实例的记账 INSERT 撞主键失败（本用例当场红）。
     */
    TEST_F(PostgresIntegrationTest, ConcurrentMigrationsApplyEachStepExactlyOnce)
    {
        {
            PostgresConnection cleanup(m_configuration);
            ASSERT_TRUE(cleanup.connect()) << cleanup.lastError();
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kConcurrentMigrationTableOne)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kConcurrentMigrationTableTwo)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(SchemaMigrator::kMigrationLedgerTableName)));
        }
        m_preparedTableNames.emplace_back(kConcurrentMigrationTableOne);
        m_preparedTableNames.emplace_back(kConcurrentMigrationTableTwo);
        m_preparedTableNames.emplace_back(SchemaMigrator::kMigrationLedgerTableName);

        const std::unique_ptr<ConnectionPool> firstPool  = makePool(1);
        const std::unique_ptr<ConnectionPool> secondPool = makePool(1);

        const std::vector<SchemaMigrator::MigrationStep> steps{
                SchemaMigrator::MigrationStep{
                        .name           = "0001-concurrent-a",
                        .upStatements   = {SqlStatement{.sql = "CREATE TABLE " + quote(kConcurrentMigrationTableOne) + " (id BIGINT PRIMARY KEY)"}},
                        .downStatements = {SqlStatement{.sql = "DROP TABLE IF EXISTS " + quote(kConcurrentMigrationTableOne)}},
                },
                SchemaMigrator::MigrationStep{
                        .name           = "0002-concurrent-b",
                        .upStatements   = {SqlStatement{.sql = "CREATE TABLE " + quote(kConcurrentMigrationTableTwo) + " (id BIGINT PRIMARY KEY)"}},
                        .downStatements = {SqlStatement{.sql = "DROP TABLE IF EXISTS " + quote(kConcurrentMigrationTableTwo)}},
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
        EXPECT_EQ(std::count(recorded.begin(), recorded.end(), "0001-concurrent-a"), 1) << "第一步被记了不止一行";
        EXPECT_EQ(std::count(recorded.begin(), recorded.end(), "0002-concurrent-b"), 1) << "第二步被记了不止一行";

        // 记账之外再核一遍结构：两张表都真的建出来了
        PostgresConnection verification(m_configuration);
        ASSERT_TRUE(verification.connect()) << verification.lastError();
        for (const std::string_view tableName: {kConcurrentMigrationTableOne, kConcurrentMigrationTableTwo})
        {
            const std::unique_ptr<DatabaseResult> result = verification.execute("SELECT COUNT(*) FROM " + quote(tableName));
            EXPECT_NE(result, nullptr) << "表 " << tableName << " 不存在：" << verification.lastError();
        }
    }

    /**
     * @brief 真机上钉住种子数据的幂等：连跑两轮，第二轮一行都不新增
     *
     * @details 与 SQLite 那条同一份判据，换到真服务端——PostgreSQL 走 `ON CONFLICT DO NOTHING`，
     *          被跳过的行不计入影响行数；计数与表内容两条都要在真机上成立。
     * @note 池上限为 1：迁移与种子各自要借连接，握着不放就是死等
     */
    TEST_F(PostgresIntegrationTest, SeedingIsIdempotentOnTheRealServer)
    {
        {
            PostgresConnection cleanup(m_configuration);
            ASSERT_TRUE(cleanup.connect()) << cleanup.lastError();
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kSeedTableName)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(SchemaMigrator::kMigrationLedgerTableName)));
        }
        m_preparedTableNames.emplace_back(kSeedTableName);
        m_preparedTableNames.emplace_back(SchemaMigrator::kMigrationLedgerTableName);

        std::unique_ptr<ConnectionPool> pool = makePool(1);

        const std::shared_ptr<SqlDialect> dialect = SchemaMigrator::dialectOf(*pool);
        ASSERT_NE(dialect, nullptr);

        const std::vector<SchemaMigrator::MigrationStep> steps{
                SchemaMigrator::MigrationStep{
                        .name           = "0001-create-seed",
                        .upStatements   = {SchemaMigrator::createTableStatement<IntegrationSeedRow>(*dialect)},
                        .downStatements = {SchemaMigrator::dropTableStatement<IntegrationSeedRow>(*dialect)},
                },
        };
        std::string error;
        ASSERT_TRUE(SchemaMigrator::migrate(*pool, steps, nullptr, &error)) << error;

        const std::vector<IntegrationSeedRow> seedRows = {
                IntegrationSeedRow{.id = 1, .name = "真机种子一"},
                IntegrationSeedRow{.id = 2, .name = "真机种子二"},
        };

        std::size_t inserted = 0;
        ASSERT_TRUE(SchemaMigrator::seed(*pool, seedRows, &inserted, &error)) << error;
        EXPECT_EQ(inserted, 2U) << "第一轮的新增行数不对";

        std::size_t insertedAgain = 0;
        ASSERT_TRUE(SchemaMigrator::seed(*pool, seedRows, &insertedAgain, &error)) << error;
        EXPECT_EQ(insertedAgain, 0U) << "重复落种子数据产生了新行";

        const std::vector<IntegrationSeedRow> rows = OrmQuery<IntegrationSeedRow>(*pool).orderBy(asc("id")).toList();
        ASSERT_EQ(rows.size(), 2U) << "重复落种子数据让行数变了";
        EXPECT_EQ(rows[0].name, "真机种子一");
        EXPECT_EQ(rows[1].name, "真机种子二");
    }

    /**
     * @brief 真机上钉住关系装载的两个方向（多对一 + 一对多，一条 IN 查询各补取一次）
     *
     * @details 与 SQLite 那一组同一份判据，换到真服务端：`IN (…)` 的参数绑定、分组键的取值、
     *          以及「对端不存在时留空」在 PostgreSQL 的取值映射下同样成立。三张表都由本用例自建自清。
     * @note 池上限为 1：这顺带把「补取必须在外层连接释放之后」这条纪律在真机上也压了一遍
     */
    TEST_F(PostgresIntegrationTest, RelationLoadingWorksOnTheRealServer)
    {
        {
            PostgresConnection cleanup(m_configuration);
            ASSERT_TRUE(cleanup.connect()) << cleanup.lastError();
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kRelationCommentTableName)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kRelationPostTableName)));
            static_cast<void>(cleanup.execute("DROP TABLE IF EXISTS " + quote(kRelationAuthorTableName)));
        }
        m_preparedTableNames.emplace_back(kRelationAuthorTableName);
        m_preparedTableNames.emplace_back(kRelationPostTableName);
        m_preparedTableNames.emplace_back(kRelationCommentTableName);

        std::unique_ptr<ConnectionPool> pool = makePool(1);
        {
            PostgresConnection ddl(m_configuration);
            ASSERT_TRUE(ddl.connect()) << ddl.lastError();
            ASSERT_NE(ddl.execute("CREATE TABLE " + quote(kRelationAuthorTableName) + " (\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL)"), nullptr) << ddl.lastError();
            ASSERT_NE(ddl.execute("CREATE TABLE " + quote(kRelationPostTableName) + " (\"id\" BIGINT PRIMARY KEY, \"title\" TEXT NOT NULL, \"author_id\" BIGINT NOT NULL)"),
                      nullptr)
                    << ddl.lastError();
            ASSERT_NE(ddl.execute("CREATE TABLE " + quote(kRelationCommentTableName) + " (\"id\" BIGINT PRIMARY KEY, \"post_id\" BIGINT NOT NULL, \"text\" TEXT NOT NULL)"),
                      nullptr)
                    << ddl.lastError();
        }

        {
            OrmQuery<IntegrationRelationAuthor> authors(*pool);
            ASSERT_EQ(authors.insert(IntegrationRelationAuthor{.id = 1, .name = "真机作者一"}), 1);
            ASSERT_EQ(authors.insert(IntegrationRelationAuthor{.id = 2, .name = "真机作者二"}), 1);

            OrmQuery<IntegrationRelationPost> posts(*pool);
            ASSERT_EQ(posts.insert(IntegrationRelationPost{.id = 10, .title = "真机第一篇", .authorId = 1}), 1);
            ASSERT_EQ(posts.insert(IntegrationRelationPost{.id = 11, .title = "真机第二篇", .authorId = 2}), 1);
            ASSERT_EQ(posts.insert(IntegrationRelationPost{.id = 12, .title = "真机孤儿篇", .authorId = 999}), 1);

            OrmQuery<IntegrationRelationComment> comments(*pool);
            ASSERT_EQ(comments.insert(IntegrationRelationComment{.id = 100, .postId = 10, .text = "真机第一评"}), 1);
            ASSERT_EQ(comments.insert(IntegrationRelationComment{.id = 101, .postId = 10, .text = "真机第二评"}), 1);
        }

        const std::vector<IntegrationRelationPost> posts = OrmQuery<IntegrationRelationPost>(*pool).orderBy(asc("id")).then(&IntegrationRelationPost::author).toList();
        ASSERT_EQ(posts.size(), 3U);
        ASSERT_TRUE(posts[0].author.has_value());
        EXPECT_EQ(posts[0].author->name, "真机作者一") << "第一条帖子挂错了作者";
        ASSERT_TRUE(posts[1].author.has_value());
        EXPECT_EQ(posts[1].author->name, "真机作者二") << "两条帖子挂成了同一个作者";
        EXPECT_FALSE(posts[2].author.has_value()) << "对端不存在却挂上了一行";

        const std::vector<IntegrationRelationPost> withComments = OrmQuery<IntegrationRelationPost>(*pool).orderBy(asc("id")).then(&IntegrationRelationPost::comments).toList();
        ASSERT_EQ(withComments.size(), 3U);
        ASSERT_EQ(withComments[0].comments.size(), 2U) << "第一条帖子的评论数不对";
        EXPECT_EQ(withComments[0].comments[0].text, "真机第一评");
        ASSERT_EQ(withComments[1].comments.size(), 0U) << "第二条帖子没有评论（分组键错位会把别家的挂上来）";
        EXPECT_TRUE(withComments[2].comments.empty());
    }

    /**
     * @brief 钉住 timestamptz 在会话时区改变后仍是同一个瞬间，而 timestamp 仍是同一个历法读数
     *
     * @details 这是判据 3 的那一格，两条一起才算说清「时区语义没被抹平」：
     *          ① 会话从 UTC 挪到 Asia/Kolkata 之后，瞬时列的原始文本必须**换了偏移来写同一个瞬间**
     *            （读回来的成员值与写入逐微秒相等）；墙上时间列的文本必须**一字不变**（它没有偏移可言）。
     *          ② 中间夹一条 `SHOW TimeZone` 与一条对原始文本的断言：否则「值没变」可能只是因为
     *            会话压根没改成功，那条绿就是白给的。
     * @note 期望瞬间/读数按公历直接写（2026-10-18 是 1970-01-01 之后第 20744 天，
     *       2026-03-01 是第 20513 天），不复用被测解析器造期望。
     * @note 收尾把会话时区改回 UTC：本驱动的会话复位用的是 DISCARD ALL，改回去是为了即使在那一步
     *       之前用例就失败，归还的连接也不会带着别的时区给下一个借用者。
     */
    TEST_F(PostgresIntegrationTest, TimestamptzKeepsTheInstantAcrossSessionTimeZoneWhileTimestampKeepsTheLiteral)
    {
        m_preparedTableNames.emplace_back(kTemporalTableName);

        std::string errorText;
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationTemporalRow>(*makePool(1), true, &errorText)) << errorText;
        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationTemporalRow>(*pool, true, &errorText)) << errorText;

        const AsynGyanis::Database::Timestamp     bookedAt{std::chrono::days{20744} + std::chrono::hours{3} + std::chrono::minutes{50} + std::chrono::seconds{30} +
                                                           std::chrono::microseconds{123456}};
        const AsynGyanis::Database::LocalDateTime localAt{std::chrono::days{20513} + std::chrono::hours{2} + std::chrono::minutes{30} + std::chrono::microseconds{500000}};
        ASSERT_EQ(1, OrmQuery<IntegrationTemporalRow>(*pool).insert(IntegrationTemporalRow{.id = 1, .bookedAt = bookedAt, .localAt = localAt}));

        // 会话基线钉成 UTC，再挪到 +05:30：两步都读一次原始文本，比较才有对照
        {
            PooledConnection connection = pool->acquire();
            ASSERT_TRUE(connection);
            ASSERT_TRUE(connection->execute("SET TIME ZONE 'UTC'") != nullptr) << connection->lastError();
            const std::unique_ptr<DatabaseResult> utcText = connection->execute("SELECT booked_at::text, local_at::text FROM \"Asyn_Pg_Temporal\" WHERE id = 1");
            ASSERT_TRUE(utcText != nullptr) << connection->lastError();
            ASSERT_TRUE(utcText->next());
            EXPECT_EQ(std::get<std::string>(utcText->getValue(0)), "2026-10-18 03:50:30.123456+00");
            EXPECT_EQ(std::get<std::string>(utcText->getValue(1)), "2026-03-01 02:30:00.5");

            ASSERT_TRUE(connection->execute("SET TIME ZONE 'Asia/Kolkata'") != nullptr) << connection->lastError();
            const std::unique_ptr<DatabaseResult> zoneCheck = connection->execute("SHOW TimeZone");
            ASSERT_TRUE(zoneCheck != nullptr) << connection->lastError();
            ASSERT_TRUE(zoneCheck->next());
            EXPECT_EQ(std::get<std::string>(zoneCheck->getValue(0)), "Asia/Kolkata") << "会话没真的改时区，下面那条比较就什么都不是";

            const std::unique_ptr<DatabaseResult> shifted = connection->execute("SELECT booked_at::text, local_at::text FROM \"Asyn_Pg_Temporal\" WHERE id = 1");
            ASSERT_TRUE(shifted != nullptr) << connection->lastError();
            ASSERT_TRUE(shifted->next());
            EXPECT_EQ(std::get<std::string>(shifted->getValue(0)), "2026-10-18 09:20:30.123456+05:30") << "瞬时列没有按会话时区重新渲染";
            EXPECT_EQ(std::get<std::string>(shifted->getValue(1)), "2026-03-01 02:30:00.5") << "墙上时间列被会话时区改写了（它本没有时区可言）";

            static_cast<void>(connection->execute("SET TIME ZONE 'UTC'"));
        }

        // 换个连接（新会话）经 ORM 读回来：偏移折叠之后必须还是原来那个瞬间
        const std::optional<IntegrationTemporalRow> readBack = OrmQuery<IntegrationTemporalRow>(*pool).first();
        ASSERT_TRUE(readBack.has_value()) << "日期时间行读不回来";
        EXPECT_EQ(readBack->bookedAt, bookedAt) << "timestamptz 折回 UTC 之后不是写入的那个瞬间";
        EXPECT_EQ(readBack->localAt, localAt) << "timestamp 折回墙上时间之后不是写入的那个历法读数";

        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationTemporalRow>(*pool, true, &errorText)) << errorText;
    }

    /**
     * @brief BYTEA 列逐字节往返：内嵌 NUL、反斜杠、单独出现的 0xFF 与多字节 UTF-8 序列都原样回来
     * @details 这是「换引擎就换语义」那条判据在 PG 侧的落点。同一条载荷、同一组断言形状还在
     *          MySqlIntegrationTest.BinaryColumnRoundTripsAsBlobWhileTextColumnStaysText（MySQL 的
     *          LONGBLOB）与 SchemaMigrationsTest.BinaryMemberRoundTripsEveryByteIncludingNulOnSqlite
     *          （SQLite 的 BLOB）里各钉一次，三处任一家把二进制读成文本就红。
     *          载荷里的 0x5C 是这一家特有的关键格：BYTEA 的两种文本形态都以反斜杠起头，
     *          编码或解码错一位，读回来的是另一种东西而不是报错。
     * @note 证伪：删掉 toPostgresTextParameter 的 BinaryBytes 分支，写入这一步就报「容器参数被拒」，
     *       insert 回 0；删掉 convertColumnText 的 BYTEA 分支，读回来是十六进制文本，
     *       std::get 不到 BinaryBytes，两条断言各自红。
     */
    TEST_F(PostgresIntegrationTest, ByteaColumnRoundTripsEveryByteIncludingNul)
    {
        m_preparedTableNames.emplace_back(kBinaryTableName);

        std::string errorText;
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationPgBinaryRow>(*makePool(1), true, &errorText)) << errorText;

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationPgBinaryRow>(*pool, true, &errorText)) << errorText;

        // 三家共用的载荷：0x00 内嵌 NUL、0x5C 反斜杠、0xFF 高位、0x41 可打印、「你」的三字节
        // UTF-8 序列，收尾再来一个 0x00——按字符集搬运的写法会在第一格就断
        const BinaryBytes payload{0x00, 0x5C, 0xFF, 0x41, 0xE4, 0xBD, 0xA0, 0x00};

        {
            OrmQuery<IntegrationPgBinaryRow> insertQuery(*pool);
            EXPECT_EQ(1, insertQuery.insert(IntegrationPgBinaryRow{.id = 1, .payload = payload, .label = "文本对照"}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationPgBinaryRow{.id = 2, .payload = BinaryBytes{}, .label = "zero"}));
        }

        {
            OrmQuery<IntegrationPgBinaryRow>          query(*pool);
            const std::vector<IntegrationPgBinaryRow> rows = query.orderBy(asc("id")).toList();

            ASSERT_EQ(rows.size(), 2U);
            EXPECT_EQ(rows[0].payload, payload) << "BYTEA 读回来的不是写入的那串字节";
            EXPECT_TRUE(rows[1].payload.empty()) << "零长 BYTEA 与 SQL NULL 被混了";
            EXPECT_EQ(rows[0].label, "文本对照");
            EXPECT_EQ(rows[1].label, "zero");
        }

        // 库里那一格钉的是「存的确实是 BYTEA」：服务端按默认的 hex 输出时它长 \x 前缀。
        // 字节数用 octet_length 而不是 pg_column_size——后者量的是磁盘占用，短 BYTEA 还带一个
        // varlena 长度头（实测 8 字节的载荷报 9），拿它当「存进去几个字节」会读数错
        {
            PooledConnection connection = pool->acquire();
            ASSERT_TRUE(connection);
            const std::unique_ptr<DatabaseResult> raw =
                    connection->execute("SELECT payload::text, octet_length(payload) FROM \"" + std::string(kBinaryTableName) + "\" WHERE id = 1");
            ASSERT_TRUE(raw != nullptr) << connection->lastError();
            ASSERT_TRUE(raw->next());
            EXPECT_EQ(std::get<std::string>(raw->getValue(0)), "\\x005cff41e4bda000");
            EXPECT_EQ(std::get<std::int64_t>(raw->getValue(1)), 8);
        }

        // 按二进制列做等值条件：证明 WHERE 那一侧的参数也走了同一套编码
        {
            OrmQuery<IntegrationPgBinaryRow>          query(*pool);
            const std::vector<IntegrationPgBinaryRow> matched = query.where(Column(&IntegrationPgBinaryRow::payload, "payload") == payload).toList();
            ASSERT_EQ(matched.size(), 1U) << "按 BYTEA 等值查不回写入的那一行";
            EXPECT_EQ(matched[0].id, 1);
        }

        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationPgBinaryRow>(*pool, true, &errorText)) << errorText;
    }


    /**
     * @brief 悲观锁：A 会话带锁读到的行挡住 B 会话，直到 A 提交为止
     * @details 判据 1 的 PostgreSQL 那一格，形状与 MySQL 那条一致（同一条载荷、同一组断言）。
     *          B 的等锁上限用 `SET lock_timeout`，实现没生效时用例红而不是挂死。
     *          池上限取 2：两条连接必须同时在场，上限为 1 时第二条永远等不到名额。
     * @note 证伪：删掉 translate 里的行锁追加，B 的第一句会成功，这条红；
     *       把 count() 路径的行锁标志清掉那一行删了，PostgreSQL 会以「FOR UPDATE is not allowed
     *       with aggregate functions」拒绝整条 count()，同一用例里另一格红。
     */
    TEST_F(PostgresIntegrationTest, RowLockBlocksTheSecondSessionUntilTheHolderCommits)
    {
        m_preparedTableNames.emplace_back(kMigratedTableName);

        std::string                     errorText;
        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationMigratedRow>(*pool, true, &errorText)) << errorText;
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationMigratedRow>(*pool, true, &errorText)) << errorText;
        {
            OrmQuery<IntegrationMigratedRow> insertQuery(*pool);
            ASSERT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 1, .name = "锁靶", .note = std::nullopt, .balance = 0.0, .active = true}))
                    << "写不进锁靶行，后面的判据都无从谈起";
        }

        Transaction                               holder(*pool);
        const std::vector<IntegrationMigratedRow> held = OrmQuery<IntegrationMigratedRow>(holder).forUpdate().toList();
        ASSERT_EQ(held.size(), 1U) << "带行锁的 SELECT 没读到那一行";

        // 聚合路径不带锁：这一家的服务端明确拒绝「聚合 + FOR UPDATE」，标志没丢掉就在这里露出来
        EXPECT_EQ(OrmQuery<IntegrationMigratedRow>(holder).forUpdate().count(), 1);

        {
            PooledConnection challenger = pool->acquire();
            ASSERT_TRUE(challenger);
            ASSERT_TRUE(challenger->execute("SET lock_timeout = '1s'") != nullptr) << challenger->lastError();

            const std::unique_ptr<DatabaseResult> blocked = challenger->execute(R"(SELECT id FROM "Asyn_Pg_Migrated" WHERE id = 1 FOR UPDATE)");
            EXPECT_EQ(blocked, nullptr) << "A 还持着行锁，B 却读到了：那句 FOR UPDATE 没真的锁行";
            EXPECT_NE(challenger->lastError().find("lock timeout"), std::string::npos) << challenger->lastError();
        }

        ASSERT_TRUE(holder.commit());

        PooledConnection afterCommit = pool->acquire();
        ASSERT_TRUE(afterCommit);
        ASSERT_TRUE(afterCommit->execute("SET lock_timeout = '1s'") != nullptr) << afterCommit->lastError();
        const std::unique_ptr<DatabaseResult> granted = afterCommit->execute(R"(SELECT id FROM "Asyn_Pg_Migrated" WHERE id = 1 FOR UPDATE)");
        ASSERT_TRUE(granted != nullptr) << "A 已提交而 B 仍等不到锁：" << afterCommit->lastError();
        ASSERT_TRUE(granted->next());
    }

} // namespace AsynGyanis::Database
