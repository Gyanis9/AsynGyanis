// 覆盖场景（MySQL 真实服务端集成；与只测离线失败语义的 TestMySqlConnection.cpp 互补）：
// - 建连与服务端版本、错误口令的中文失败原因
// - 参数化执行（mysql_stmt_*）：影响行数、各类取值与 NULL/空串的往返、注入文本、参数个数与容器参数的拒绝面
// - 同一语句文本重复执行走连接的预处理语句缓存：必须读到最新数据，且失败一次后同一条文本仍可复用
// - ORM 端到端：CRUD、排序分页、批量插入分块、引用标识符（保留字/空格/反引号）、SchemaMigrator 建表与删表
// - 表存在性查询只认基表：同名视图不算「表已存在」，基表仍要算（TableExistsIgnoresViewsAndStillSeesBaseTables）
// - BIT 列在文本协议与预处理协议上都按整数读出（BitColumnsAreReadAsIntegersOnBothProtocolPaths）
// - 自增标识挂在写回执上：两条协议路径同口径、非插入语句与无自增列都回 0、宽不进 int64 时如实报 0 并写明原因
// - 自增主键端到端：SchemaMigrator 生成的 DDL 被 InnoDB 接受，单条与批量两条写入路径都省略主键、标识连着排成 1..N
// - 二进制列按 BLOB 存取而文本列仍按文本读回；异步读写链路与同步结果逐项一致；事务提交/回滚/析构与会话复位
// 门控：口令（ASYN_MYSQL_TEST_PASSWORD）没有默认值，未设置时整组 GTEST_SKIP，仓库零明文口令。
// 用例只碰自建专用库，表由各用例自建自清；表名必须按用例区分（并行执行时不得与其它用例共用同名表）。

#include "DatabaseTestSupport.h"

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Dialect/MySqlDialect.h"
#include "Database/MySql/MySqlConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include <gtest/gtest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{

    using TestSupport::readEnvironmentPortOrDefault;
    using TestSupport::readEnvironmentTextOrDefault;

    using TestSupport::containsLocalizedText;
    namespace
    {
#ifdef DATABASE_HAS_MYSQL
        /// 当前构建是否编译了真实的 libmysqlclient 驱动；桩构建里连不上任何服务端，整组跳过
        constexpr bool kMySqlDriverCompiled = true;
#else
        /// 桩构建：与「未提供口令即跳过」一起构成本文件在任何环境下都只跳过不失败的第二道保险
        constexpr bool kMySqlDriverCompiled = false;
#endif

        // ------------------------------------------------------------------------
        // 环境变量名与默认值（口令恒无默认值）
        // ------------------------------------------------------------------------

        constexpr const char *kHostVariableName     = "ASYN_MYSQL_TEST_HOST";
        constexpr const char *kPortVariableName     = "ASYN_MYSQL_TEST_PORT";
        constexpr const char *kUserVariableName     = "ASYN_MYSQL_TEST_USER";
        constexpr const char *kPasswordVariableName = "ASYN_MYSQL_TEST_PASSWORD";
        constexpr const char *kDatabaseVariableName = "ASYN_MYSQL_TEST_DATABASE";

        constexpr const char *kDefaultHost         = "127.0.0.1";
        constexpr std::uint16_t kDefaultPort       = 3306U;
        constexpr const char *kDefaultUserName     = "root";
        constexpr const char *kDefaultDatabaseName = "asyngyanis_test";

        // ------------------------------------------------------------------------
        // 测试用表：每个用例一张独立表，互不共用
        // ------------------------------------------------------------------------

        /// 参数化插入用例的表
        constexpr std::string_view kParameterInsertTableName = "Asyn_Mysql_ParamInsert";
        constexpr std::string_view kParameterInsertColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `amount` DOUBLE NOT NULL";

        /// 参数化取值往返用例的表（含 NULL 列、空串列与 TEXT 列）
        constexpr std::string_view kParameterSelectTableName = "Asyn_Mysql_ParamSelect";
        constexpr std::string_view kParameterSelectColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `amount` DOUBLE NOT NULL, "
            "`note` VARCHAR(191) NULL, `payload` TEXT NULL";

        /// 语句复用用例的表：同一文本要连跑多次，列故意少到能一眼看出读到的是哪一版
        constexpr std::string_view kReusedStatementTableName = "Asyn_Mysql_ReuseStmt";
        constexpr std::string_view kReusedStatementColumns = "`id` BIGINT PRIMARY KEY, `name` VARCHAR(64) NOT NULL";

        /// 注入证明用例的表
        constexpr std::string_view kInjectionTableName = "Asyn_Mysql_Injection";
        constexpr std::string_view kInjectionColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL";

        /// ORM CRUD 用例的表
        constexpr std::string_view kAccountTableName = "Asyn_Mysql_Account";
        constexpr std::string_view kAccountColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `balance` DOUBLE NOT NULL, "
            "`note` VARCHAR(191) NULL, `active` TINYINT(1) NOT NULL";

        /// ORM 批量插入分块用例的表（列数与分块换算直接相关，勿随意增减）
        constexpr std::string_view kBatchTableName = "Asyn_Mysql_Batch";
        constexpr std::string_view kBatchColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `score` DOUBLE NOT NULL, "
            "`note` VARCHAR(191) NULL, `active` TINYINT(1) NOT NULL";

        /// ORM 特殊标识符用例的表（表名含连字符，列名含保留字、空格与反引号）
        constexpr std::string_view kQuotedTableName = "Asyn_Mysql_Quote-Table";
        constexpr std::string_view kQuotedColumns =
            "`id` BIGINT PRIMARY KEY, "
            "`order` VARCHAR(191) NOT NULL, "
            "`group` VARCHAR(191) NOT NULL, "
            "`weird name` VARCHAR(191) NULL, "
            "`tick``column` VARCHAR(191) NULL";

        /// ORM 空结果集用例的表
        constexpr std::string_view kProbeTableName = "Asyn_Mysql_Empty";
        constexpr std::string_view kProbeColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL";

        /// 事务用例的四张表（提交可见 / 回滚不可见 / 析构自动回滚 / 异常穿越）
        constexpr std::string_view kTransactionCommitTableName    = "Asyn_Mysql_Tx_Commit";
        constexpr std::string_view kTransactionRollbackTableName  = "Asyn_Mysql_Tx_Rollback";

        // 会话复位用例刻意不与回滚用例共用表：ctest 每个用例是独立进程且并行执行，
        // 共用同一张表时两个用例会互相 DROP/撞主键（并行偶发、串行必过）
        constexpr std::string_view kTransactionResetTableName     = "Asyn_Mysql_Tx_Reset";
        constexpr std::string_view kTransactionDestructorTableName = "Asyn_Mysql_Tx_Destructor";
        constexpr std::string_view kTransactionExceptionTableName  = "Asyn_Mysql_Tx_Exception";
        constexpr std::string_view kTransactionColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `amount` DOUBLE NOT NULL";

        /// 建表迁移用例的表：由 SchemaMigrator 生成 DDL，表名必须是编译期常量（见下面的 TableSchema 特化）
        constexpr std::string_view kMigratedTableName = "Asyn_Mysql_Migrated";

        /// 无符号取值域用例的表：与建表迁移用例分开，各用例各持一张物理表（并行执行时互不干扰）
        constexpr std::string_view kUnsignedTableName = "Asyn_Mysql_Unsigned";

        /// 二进制列用例的表：同表内放一列 LONGBLOB 与一列 TEXT，用于验证字符集是二者在协议层的唯一区分
        constexpr std::string_view kBinaryTableName = "Asyn_Mysql_Binary";

        /// 异步读写链路用例的表（异步路径与同步路径在同一张表上对照）
        constexpr std::string_view kAsyncChainTableName = "Asyn_Mysql_AsyncChain";
        constexpr std::string_view kAsyncChainColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `note` VARCHAR(191) NULL";

        /// 异步批量插入用例的表
        constexpr std::string_view kAsyncBatchTableName = "Asyn_Mysql_AsyncBatch";
        constexpr std::string_view kAsyncBatchColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `note` VARCHAR(191) NULL";

        /// 异步错误路径用例的表名：本表刻意不创建，用于制造「表不存在」这条异常路径
        constexpr std::string_view kAsyncMissingTableName = "Asyn_Mysql_AsyncMissing";

        /// 自增标识用例的表：AUTO_INCREMENT 主键，验证写回执带出生成本条语句的标识
        constexpr std::string_view kAutoIncrementTableName = "Asyn_Mysql_AutoIncrement";
        constexpr std::string_view kAutoIncrementColumns =
            "`id` BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, `name` VARCHAR(191) NOT NULL";

        /// 空改动 UPDATE 用例的表：借用自增表的列定义，表名独立以免与其它用例并行时撞车
        constexpr std::string_view kNoOpUpdateTableName = "Asyn_Mysql_NoOpUpdate";

        /// 无自增列的对照表：插入之后自增标识必须是 0，不能凭空给出一个值
        constexpr std::string_view kPlainKeyTableName = "Asyn_Mysql_PlainKey";
        constexpr std::string_view kPlainKeyColumns =
            "`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL";

        /// 超宽自增标识用例的表：BIGINT UNSIGNED 的自增列可以播种到 int64 上界之外
        constexpr std::string_view kWideAutoIncrementTableName = "Asyn_Mysql_WideAutoIncrement";
        constexpr std::string_view kWideAutoIncrementColumns =
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, `name` VARCHAR(191) NOT NULL";

        /// 播种起点 2^63：恰在有符号 64 位能表达的最大值之外一格
        /// 自增主键 ORM 用例的表：由 SchemaMigrator 用方言生成的 DDL 建出来
        constexpr std::string_view kOrmAutoIncrementTableName = "Asyn_Mysql_OrmAutoInc";

        constexpr std::uint64_t kWideAutoIncrementSeed = 9223372036854775808ULL;

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
         * @brief 账号表结构体：覆盖整型、文本、浮点、可空列与布尔列
         */
        struct IntegrationAccountRow
        {
            std::int64_t               id;      ///< 主键
            std::string                name;    ///< 户名（含中文与单引号）
            double                     balance; ///< 余额（含负数）
            std::optional<std::string> note;    ///< 备注：NULL → 空 optional，空串 → 有值的空串
            bool                       active;  ///< 是否启用（TINYINT(1) 的 0/1）
        };

        /**
         * @brief 批量插入表结构体：五列，用于把 65535 的参数上限换算成每批行数
         */
        struct IntegrationBatchRow
        {
            std::int64_t               id;     ///< 主键
            std::string                name;   ///< 名称
            double                     score;  ///< 分值
            std::optional<std::string> note;   ///< 备注，可空
            bool                       active; ///< 是否启用
        };

        /**
         * @brief 特殊标识符表结构体：列名覆盖保留字、空格与反引号
         */
        struct IntegrationQuotedRow
        {
            std::int64_t id;           ///< 主键
            std::string  order;        ///< 列名是保留字 order
            std::string  group;        ///< 列名是保留字 group
            std::string  spacedColumn; ///< 列名含空格（"weird name"）
            std::string  tickColumn;   ///< 列名含反引号（"tick`column"），必须靠翻倍转义才能引用
        };

        /**
         * @brief 空结果集探针表结构体
         */
        struct IntegrationProbeRow
        {
            std::int64_t id;   ///< 主键
            std::string  name; ///< 名称
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
         * @brief 建表迁移用例的结构体：覆盖 SchemaMigrator 的全部类型映射分支
         *
         * @details 六列的成员类型分别对应 Int64 / Text / 可空 Text / Double / Bool / UInt64，
         *          因此建表语句会同时用到 BIGINT、TEXT、DOUBLE、TINYINT(1) 与 BIGINT UNSIGNED。
         */
        struct IntegrationMigratedRow
        {
            std::int64_t               id;       ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            std::string                name;     ///< 户名（TEXT NOT NULL）
            std::optional<std::string> note;     ///< 备注（TEXT，可空）
            double                     balance;  ///< 余额（DOUBLE NOT NULL）
            bool                       active;   ///< 是否启用（TINYINT(1) NOT NULL）
            std::uint64_t              sequence; ///< 序号（BIGINT UNSIGNED NOT NULL）
        };

        /**
         * @brief 无符号取值域用例的结构体：与建表迁移用例同构，但绑定独立的物理表
         *
         * @details SchemaMigrator 的表名只能来自编译期常量：与建表迁移用例共用结构体就等于共用同一张表，
         *          并行执行时会互相删表、互相撞主键，因此本用例单列一个绑定独立表名的结构体。
         */
        struct IntegrationUnsignedRow
        {
            std::int64_t               id;       ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            std::string                name;     ///< 名称（TEXT NOT NULL）
            std::optional<std::string> note;     ///< 备注（TEXT，可空）
            double                     balance;  ///< 余额（DOUBLE NOT NULL）
            bool                       active;   ///< 是否启用（TINYINT(1) NOT NULL）
            std::uint64_t              sequence; ///< 序号（BIGINT UNSIGNED NOT NULL）
        };

        /**
         * @brief 二进制列用例的结构体：一列二进制与一列文本同表共存
         *
         * @details 文本列的协议类型码与 BLOB 相同（都是 MYSQL_TYPE_BLOB），只有列的字符集不同，
         *          因此「两列必须各自读成正确的类型」这一条断言同时验证了字符集判定的两个方向。
         */
        struct IntegrationBinaryRow
        {
            std::int64_t id;      ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            BinaryBytes  payload; ///< 二进制列（LONGBLOB NOT NULL）
            std::string  label;   ///< 文本对照列（TEXT NOT NULL）：必须仍按文本读回
        };

        /**
         * @brief 构造一行账号数据
         * @param id 主键
         * @param name 户名
         * @param balance 余额
         * @param note 备注，可为空
         * @param active 是否启用
         * @return IntegrationAccountRow 结构体
         */
        [[nodiscard]] IntegrationAccountRow makeAccountRow(const std::int64_t id,
                                                           std::string name,
                                                           const double balance,
                                                           std::optional<std::string> note,
                                                           const bool active)
        {
            return IntegrationAccountRow{
                .id      = id,
                .name    = std::move(name),
                .balance = balance,
                .note    = std::move(note),
                .active  = active
            };
        }

        /**
         * @brief 异步读写链路用例的结构体：主键 + 文本 + 可空文本
         * @details 三列足够覆盖异步路径要验证的东西（写入、查询、更新、删除、可空列映射），
         *          类型映射的正确性已由同步用例覆盖，这里不再重复。
         */
        struct IntegrationAsyncRow
        {
            std::int64_t               id;   ///< 主键
            std::string                name; ///< 名称
            std::optional<std::string> note; ///< 备注，可空
        };

        /**
         * @brief 异步批量插入用例的结构体：列定义与链路用例相同，但绑定到独立的表
         */
        struct IntegrationAsyncBatchRow
        {
            std::int64_t               id;   ///< 主键
            std::string                name; ///< 名称
            std::optional<std::string> note; ///< 备注，可空
        };

        /**
         * @brief 异步错误路径用例的结构体：绑定到一张刻意不创建的表
         */
        struct IntegrationAsyncMissingRow
        {
            std::int64_t id; ///< 唯一一列
        };

    } // namespace

    // ========================================================================
    // TableSchema 特化：把结构体注册到各自的表
    // ========================================================================

    template<>
    struct Queryable::TableSchema<IntegrationAccountRow>
    {
        static constexpr std::string_view kTableName = kAccountTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationAccountRow::id, "id"),
            Column(&IntegrationAccountRow::name, "name"),
            Column(&IntegrationAccountRow::balance, "balance"),
            Column(&IntegrationAccountRow::note, "note"),
            Column(&IntegrationAccountRow::active, "active"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationBatchRow>
    {
        static constexpr std::string_view kTableName = kBatchTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationBatchRow::id, "id"),
            Column(&IntegrationBatchRow::name, "name"),
            Column(&IntegrationBatchRow::score, "score"),
            Column(&IntegrationBatchRow::note, "note"),
            Column(&IntegrationBatchRow::active, "active"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationQuotedRow>
    {
        static constexpr std::string_view kTableName = kQuotedTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationQuotedRow::id, "id"),
            Column(&IntegrationQuotedRow::order, "order"),
            Column(&IntegrationQuotedRow::group, "group"),
            Column(&IntegrationQuotedRow::spacedColumn, "weird name"),
            Column(&IntegrationQuotedRow::tickColumn, "tick`column"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationProbeRow>
    {
        static constexpr std::string_view kTableName = kProbeTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationProbeRow::id, "id"),
            Column(&IntegrationProbeRow::name, "name"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationTransactionRow>
    {
        // 表名只能是编译期常量，因此本结构体固定绑定到「提交可见」用例的表：
        // 其余事务用例外加的表名各不相同，一律走 insertTransactionRow() 的原始参数化语句写入
        static constexpr std::string_view kTableName = kTransactionCommitTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationTransactionRow::id, "id"),
            Column(&IntegrationTransactionRow::name, "name"),
            Column(&IntegrationTransactionRow::amount, "amount"),
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
            Column(&IntegrationMigratedRow::id,       "id"),
            Column(&IntegrationMigratedRow::name,     "name"),
            Column(&IntegrationMigratedRow::note,     "note"),
            Column(&IntegrationMigratedRow::balance,  "balance"),
            Column(&IntegrationMigratedRow::active,   "active"),
            Column(&IntegrationMigratedRow::sequence, "sequence"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationUnsignedRow>
    {
        // 表名固定为 kUnsignedTableName：与建表迁移用例的表分开，并行执行时互不干扰
        static constexpr std::string_view kTableName = kUnsignedTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationUnsignedRow::id,       "id"),
            Column(&IntegrationUnsignedRow::name,     "name"),
            Column(&IntegrationUnsignedRow::note,     "note"),
            Column(&IntegrationUnsignedRow::balance,  "balance"),
            Column(&IntegrationUnsignedRow::active,   "active"),
            Column(&IntegrationUnsignedRow::sequence, "sequence"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationBinaryRow>
    {
        static constexpr std::string_view kTableName = kBinaryTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationBinaryRow::id,      "id"),
            Column(&IntegrationBinaryRow::payload, "payload"),
            Column(&IntegrationBinaryRow::label,   "label"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationAsyncRow>
    {
        static constexpr std::string_view kTableName = kAsyncChainTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationAsyncRow::id,   "id"),
            Column(&IntegrationAsyncRow::name, "name"),
            Column(&IntegrationAsyncRow::note, "note"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationAsyncBatchRow>
    {
        static constexpr std::string_view kTableName = kAsyncBatchTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationAsyncBatchRow::id,   "id"),
            Column(&IntegrationAsyncBatchRow::name, "name"),
            Column(&IntegrationAsyncBatchRow::note, "note"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationAsyncMissingRow>
    {
        // 表名指向一张本文件从不创建的表：异步写失败这条链路靠它制造
        static constexpr std::string_view kTableName = kAsyncMissingTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationAsyncMissingRow::id, "id"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
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
         * @brief MySQL 真实服务端集成测试夹具
         *
         * @details SetUp 只做两件事：从环境变量装载连接配置（缺口令即 GTEST_SKIP），
         *          以及创建测试专用库。表由用例自己在用例体内按需创建（prepareTable），
         *          由其记录表名并在 TearDown 里删除——这样即使断言中途失败也不会留下残留。
         */
        class MySqlIntegrationTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                // 桩构建里没有任何客户端库可连：直接跳过，不让用例以「连不上」的名义失败
                if (!kMySqlDriverCompiled)
                {
                    GTEST_SKIP() << "当前构建未编译 MySQL 驱动（未定义 DATABASE_HAS_MYSQL），"
                                    "跳过真实服务端集成测试";
                }

                const std::string passwordText = readEnvironmentText(kPasswordVariableName);
                if (passwordText.empty())
                {
                    // 口令是唯一的必需项：没有它就无法建立任何会话，本组用例整体跳过而不是失败，
                    // 因此无服务端的 CI 与本地日常构建同样保持全绿
                    GTEST_SKIP() << "未设置环境变量 " << kPasswordVariableName
                                 << "，跳过真实 MySQL 集成测试（连接参数与环境变量名见文件头说明）";
                }

                m_configuration.host     = readEnvironmentTextOrDefault(kHostVariableName, kDefaultHost);
                m_configuration.port     = readEnvironmentPortOrDefault(kPortVariableName, kDefaultPort);
                m_configuration.userName = readEnvironmentTextOrDefault(kUserVariableName, kDefaultUserName);
                m_configuration.password = passwordText;
                m_configuration.database = readEnvironmentTextOrDefault(kDatabaseVariableName, kDefaultDatabaseName);

                // 专用库由本文件自己创建；除它以外不触碰任何既有库表
                if (!createTestDatabase())
                {
                    FAIL() << "无法创建测试专用库 " << m_configuration.database << "：" << m_lastSetupError;
                }
            }

            void TearDown() override
            {
                // 建表失败或本用例不需要表时无事可做；IF EXISTS 让残留表（上次崩溃剩下）也能被清掉
                if (!m_preparedTableName.empty())
                {
                    dropPreparedTable();
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
             * @brief 用生产方言引用一个标识符（测试不自己拼反引号，顺带验证引用实现）
             * @param identifier 待引用的标识符
             * @return std::string 形如 `identifier` 的文本
             */
            [[nodiscard]] static std::string quote(const std::string_view identifier)
            {
                const MySqlDialect dialect;
                return dialect.quoteIdentifier(identifier);
            }

            /**
             * @brief 新建一条独立连接（不经过连接池）
             * @return std::unique_ptr<MySqlConnection> 尚未 connect() 的连接
             */
            [[nodiscard]] std::unique_ptr<MySqlConnection> makeConnection() const
            {
                return std::make_unique<MySqlConnection>(m_configuration);
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
                        std::unique_ptr<DatabaseConnection> connection =
                            DatabaseFactory::createMySql(databaseConfiguration);
                        // 连接池的工厂契约要求交出已经 connect() 完成的连接
                        connection->connect();
                        return connection;
                    },
                    poolConfiguration);
            }

            /**
             * @brief 取得后台事件循环运行器，首次调用时才创建
             *
             * @details 运行器刻意延迟创建（放在 std::optional 里）：SetUp 在未编译驱动或未设置口令时会 GTEST_SKIP 并提前返回，
             *          直接做成夹具成员会让「构造即起线程」的对象在跳过的用例里也启停一轮。真正跑异步链路时才创建，
             *          事件循环线程活到 runToCompletion() 返回之后，协程帧的销毁时机由 EventLoopThread 的内部约定保证。
             *
             * @return TestSupport::EventLoopThread& 已启动且确认进入运行状态的事件循环运行器
             */
            [[nodiscard]] TestSupport::EventLoopThread &asyncLoop()
            {
                if (!m_asyncLoop.has_value())
                {
                    m_asyncLoop.emplace();
                    if (!m_asyncLoop->waitUntilRunning())
                    {
                        // 循环没跑起来时提交的协程永远不会被恢复；显式记一笔失败，
                        // 免得后面以「任务未在时限内完成」的表象掩盖真正的原因
                        ADD_FAILURE() << "后台事件循环未在时限内进入运行状态";
                    }
                }
                return m_asyncLoop.value();
            }

            /**
             * @brief 建立本用例专用的表
             * @details 先 DROP TABLE IF EXISTS 再 CREATE TABLE，因此上次运行留下的残留表
             *          （结构可能已不同）也会被清掉，本用例总是从一张空表开始。
             * @param tableName 表名
             * @param columnsDdl 建表语句括号内的列定义
             * @return true 表已建好；false 失败，原因见 m_lastSetupError
             */
            [[nodiscard]] bool prepareTable(const std::string_view tableName, const std::string_view columnsDdl)
            {
                MySqlConnection connection(m_configuration);
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

                const std::string createStatement =
                    "CREATE TABLE " + quotedTableName + " (" + std::string(columnsDdl) + ") CHARACTER SET utf8mb4";
                if (connection.execute(createStatement) == nullptr)
                {
                    m_lastSetupError = connection.lastError();
                    return false;
                }

                // 记下待清理的表名：TearDown 据此删除，用例中途失败也不会留下残留
                m_preparedTableName = std::string(tableName);
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

                const DatabaseValue countValue = result->getValue(0);
                const auto        *countedRows = std::get_if<std::int64_t>(&countValue);
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
            [[nodiscard]] static bool insertTransactionRow(DatabaseConnection &connection,
                                                           const std::string_view tableName,
                                                           const std::int64_t id,
                                                           std::string name,
                                                           const double amount)
            {
                const std::vector<DatabaseValue> parameters{id, std::move(name), amount};
                const std::unique_ptr<DatabaseResult> result = connection.execute(
                    "INSERT INTO " + quote(tableName) + " (`id`, `name`, `amount`) VALUES (?, ?, ?)", parameters);

                return result != nullptr && result->affectedRowCount() == 1;
            }

            /**
             * @brief 读取指定主键的行，返回其 name 列
             * @param connection 执行查询的连接
             * @param tableName 表名
             * @param id 主键
             * @return std::optional<std::string> 命中行的 name；无该行或执行失败时为空
             */
            [[nodiscard]] static std::optional<std::string> readTransactionRowName(DatabaseConnection &connection,
                                                                                  const std::string_view tableName,
                                                                                  const std::int64_t id)
            {
                const std::vector<DatabaseValue> parameters{id};
                const std::unique_ptr<DatabaseResult> result = connection.execute(
                    "SELECT `name` FROM " + quote(tableName) + " WHERE `id` = ?", parameters);
                if (result == nullptr || !result->next())
                {
                    return std::nullopt;
                }

                const DatabaseValue nameValue = result->getValue(0);
                const auto        *nameText  = std::get_if<std::string>(&nameValue);
                return nameText != nullptr ? std::optional<std::string>(*nameText) : std::nullopt;
            }

            // 以下成员必须放在 protected：用例体位于派生自本夹具的测试类里，
            // 私有成员对派生类不可见，断言就无法把建表失败的原因输出到失败信息中
            ConnectionConfig m_configuration;     ///< 从环境变量装载的连接配置
            std::string      m_preparedTableName; ///< 本用例建好的表名，供 TearDown 清理；空表示无需清理
            std::string      m_lastSetupError;    ///< 建库/建表失败的原因，供断言输出
            // 后台事件循环运行器：只在用到异步 API 的用例里才创建（见 asyncLoop()），
            // 因此被 GTEST_SKIP 的用例不会白起一个线程；它声明在最后，析构最先发生，
            // 内部的驱动协程帧也随它一起销毁（晚于循环线程 join）
            std::optional<TestSupport::EventLoopThread> m_asyncLoop;

        private:
            /**
             * @brief 创建测试专用库
             * @details 建库必须用一条「不指定默认库」的连接：目标库此刻还不存在，
             *          带着它去握手会在认证阶段就失败。IF NOT EXISTS 让重复执行也安全。
             * @return true 库已存在或已建好；false 失败，原因见 m_lastSetupError
             */
            [[nodiscard]] bool createTestDatabase()
            {
                ConnectionConfig bootstrapConfiguration = m_configuration;
                // 空库名 = 不断言任何默认库，这是 mysql_real_connect 的正规用法之一
                bootstrapConfiguration.database.clear();

                MySqlConnection connection(bootstrapConfiguration);
                if (!connection.connect())
                {
                    m_lastSetupError = connection.lastError();
                    return false;
                }

                const std::string createStatement = "CREATE DATABASE IF NOT EXISTS " + quote(m_configuration.database) +
                                                    " CHARACTER SET utf8mb4";
                if (connection.execute(createStatement) == nullptr)
                {
                    m_lastSetupError = connection.lastError();
                    return false;
                }

                return true;
            }

            /**
             * @brief 删除本用例建好的表，失败一律忽略（清理动作不该让用例结论变色）
             */
            void dropPreparedTable()
            {
                MySqlConnection connection(m_configuration);
                if (!connection.connect())
                {
                    return;
                }

                static_cast<void>(connection.execute("DROP TABLE IF EXISTS " + quote(m_preparedTableName)));
            }
        };

    } // namespace

    // ========================================================================
    // 连接
    // ========================================================================

    /**
     * @brief 验证用环境变量里的连接参数能真正连上服务端并读到服务端版本
     */
    TEST_F(MySqlIntegrationTest, ConnectsWithEnvironmentConfigurationAndReportsServerVersion)
    {
        MySqlConnection connection(configuration());

        ASSERT_TRUE(connection.connect()) << connection.lastError();
        EXPECT_TRUE(connection.isConnected());
        EXPECT_TRUE(connection.lastError().empty());

        // 服务端版本形如 "8.0.36" 或 "5.7.44-log"：非空且含数字是跨版本都成立的判据
        const std::string serverVersion = connection.serverVersion();
        EXPECT_FALSE(serverVersion.empty());
        EXPECT_NE(serverVersion.find_first_of("0123456789"), std::string::npos) << serverVersion;

        // 连接已建立时再次 connect() 必须是幂等的空操作（重复握手会丢掉会话状态）
        EXPECT_TRUE(connection.connect());
        EXPECT_EQ(connection.serverVersion(), serverVersion);

        connection.disconnect();
        EXPECT_FALSE(connection.isConnected());
    }

    /**
     * @brief 验证错误口令连不上，且失败原因是面向使用者的中文
     */
    TEST_F(MySqlIntegrationTest, ConnectWithWrongPasswordFailsWithLocalizedReason)
    {
        ConnectionConfig wrongConfiguration = configuration();
        // 在真实口令后追加一段固定后缀：口令本身恒从环境变量来，本文件不出现任何明文
        wrongConfiguration.password += "-definitely-wrong-on-purpose";

        MySqlConnection connection(wrongConfiguration);

        EXPECT_FALSE(connection.connect());
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);

        const std::string failureReason = connection.lastError();
        EXPECT_FALSE(failureReason.empty());
        // 中文文案 + 点明是 MySQL 驱动 + 带上客户端库原文与错误码
        EXPECT_TRUE(containsLocalizedText(failureReason)) << failureReason;
        EXPECT_NE(failureReason.find("MySQL"), std::string::npos) << failureReason;
        EXPECT_NE(failureReason.find("错误码"), std::string::npos) << failureReason;
    }

    // ========================================================================
    // 参数化执行（mysql_stmt_*）
    // ========================================================================

    /**
     * @brief 验证参数化 INSERT 成功回执携带真实的影响行数
     */
    TEST_F(MySqlIntegrationTest, ParameterizedInsertReportsSingleAffectedRow)
    {
        ASSERT_TRUE(prepareTable(kParameterInsertTableName, kParameterInsertColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const std::string insertStatement = "INSERT INTO " + quote(kParameterInsertTableName) +
                                            " (`id`, `name`, `amount`) VALUES (?, ?, ?)";
        const std::vector<DatabaseValue> parameters{std::int64_t{1}, std::string("张三"), 1234.5};

        const std::unique_ptr<DatabaseResult> insertResult = connection.execute(insertStatement, parameters);
        ASSERT_NE(insertResult, nullptr) << connection.lastError();
        // 写语句交出的是空回执：0 行 0 列，影响行数来自 mysql_stmt_affected_rows 的语句级快照
        EXPECT_EQ(insertResult->affectedRowCount(), 1);
        EXPECT_EQ(insertResult->rowCount(), 0U);
        EXPECT_TRUE(insertResult->isEmpty());

        // 用不带参数的路径独立复核：参数确实按顺序绑到了对应的列上
        const std::unique_ptr<DatabaseResult> selectResult = connection.execute(
            "SELECT `name`, `amount` FROM " + quote(kParameterInsertTableName) + " WHERE `id` = 1");
        ASSERT_NE(selectResult, nullptr) << connection.lastError();
        ASSERT_TRUE(selectResult->next());
        EXPECT_EQ(std::get<std::string>(selectResult->getValue("name")), "张三");
        EXPECT_DOUBLE_EQ(std::get<double>(selectResult->getValue("amount")), 1234.5);
        EXPECT_FALSE(selectResult->next());
    }

    /**
     * @brief 验证参数化 SELECT 的列名与各类取值都能原样往返，且 NULL 与空串不会混淆
     */
    TEST_F(MySqlIntegrationTest, ParameterizedSelectRoundTripsEveryValueKind)
    {
        ASSERT_TRUE(prepareTable(kParameterSelectTableName, kParameterSelectColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 长文本：刻意控制在 TEXT 的 65535 字节容量之内——越过容量时服务端会报
        // "Data too long for column" 并整条拒绝，那属于服务端容量语义而不是驱动的取值缺陷，
        // 因此这里只断言「容量内的长文本逐字节不丢」，不对截断行为做任何断言
        std::string longText;
        for (int index = 0; index < 3000; ++index)
        {
            longText += "中文";
            longText += std::to_string(index);
        }

        // 内嵌 '\0'（"前" + '\0' + "后"，共 7 字节）：驱动按「指针 + 长度」绑定与取值，
        // 因此 NUL 应当逐字节保留，与「空串」「NULL」三者互不相同
        const std::string embeddedNulText("前\0后", 7);

        const std::string insertStatement = "INSERT INTO " + quote(kParameterSelectTableName) +
                                            " (`id`, `name`, `amount`, `note`, `payload`) VALUES (?, ?, ?, ?, ?)";

        const std::vector<std::vector<DatabaseValue>> sampleRows{
            // 第 1 行：note 是 SQL NULL，payload 是空串——两者必须可区分
            {std::int64_t{1}, std::string("张三"), 1234.5, std::monostate{}, std::string("")},
            // 第 2 行：负数、中文备注、内嵌 '\0'
            {std::int64_t{2}, std::string("O'Brien"), -99.5, std::string("普通备注"), embeddedNulText},
            // 第 3 行：零值、空串备注、长文本
            {std::int64_t{3}, std::string("李四"), 0.0, std::string(""), longText}
        };

        for (const std::vector<DatabaseValue> &sampleRow: sampleRows)
        {
            const std::unique_ptr<DatabaseResult> insertResult = connection.execute(insertStatement, sampleRow);
            ASSERT_NE(insertResult, nullptr) << connection.lastError();
            EXPECT_EQ(insertResult->affectedRowCount(), 1);
        }

        const std::string selectStatement = "SELECT `id`, `name`, `amount`, `note`, `payload` FROM " +
                                            quote(kParameterSelectTableName) + " WHERE `id` = ?";
        const auto selectRowById = [&connection, &selectStatement](const std::int64_t identifier) -> std::unique_ptr<DatabaseResult>
        {
            return connection.execute(selectStatement, std::vector<DatabaseValue>{identifier});
        };

        // ---- 第 1 行：列名、中文、正常浮点、NULL 与空串的区分 ----
        const std::unique_ptr<DatabaseResult> firstRow = selectRowById(1);
        ASSERT_NE(firstRow, nullptr) << connection.lastError();
        EXPECT_EQ(firstRow->columnCount(), 5U);
        // 列名按 SELECT 列表原样交出，列序与列表一致
        EXPECT_EQ(firstRow->columnNames(), (std::vector<std::string>{"id", "name", "amount", "note", "payload"}));
        EXPECT_EQ(firstRow->columnIndex("payload").value_or(99U), 4U);

        ASSERT_TRUE(firstRow->next());
        EXPECT_EQ(std::get<std::int64_t>(firstRow->getValue("id")), 1);
        EXPECT_EQ(std::get<std::string>(firstRow->getValue("name")), "张三");
        EXPECT_DOUBLE_EQ(std::get<double>(firstRow->getValue("amount")), 1234.5);
        // note 是 SQL NULL → monostate；payload 是「有值且为空」的空串 → std::string
        EXPECT_TRUE(std::holds_alternative<std::monostate>(firstRow->getValue("note")));
        ASSERT_TRUE(std::holds_alternative<std::string>(firstRow->getValue("payload")));
        EXPECT_TRUE(std::get<std::string>(firstRow->getValue("payload")).empty());
        EXPECT_FALSE(firstRow->next());

        // ---- 第 2 行：单引号文本、负数、中文备注、内嵌 '\0' ----
        const std::unique_ptr<DatabaseResult> secondRow = selectRowById(2);
        ASSERT_NE(secondRow, nullptr) << connection.lastError();
        ASSERT_TRUE(secondRow->next());
        EXPECT_EQ(std::get<std::string>(secondRow->getValue("name")), "O'Brien");
        EXPECT_DOUBLE_EQ(std::get<double>(secondRow->getValue("amount")), -99.5);
        EXPECT_EQ(std::get<std::string>(secondRow->getValue("note")), "普通备注");

        // 内嵌 '\0' 逐字节保留：先比长度（截断一定改变长度），再比内容
        const std::string readBackNulText = std::get<std::string>(secondRow->getValue("payload"));
        EXPECT_EQ(readBackNulText.size(), embeddedNulText.size());
        EXPECT_EQ(readBackNulText, embeddedNulText);

        // ---- 第 3 行：零值、空串备注、长文本 ----
        const std::unique_ptr<DatabaseResult> thirdRow = selectRowById(3);
        ASSERT_NE(thirdRow, nullptr) << connection.lastError();
        ASSERT_TRUE(thirdRow->next());
        EXPECT_DOUBLE_EQ(std::get<double>(thirdRow->getValue("amount")), 0.0);
        EXPECT_TRUE(std::get<std::string>(thirdRow->getValue("note")).empty());

        const std::string readBackLongText = std::get<std::string>(thirdRow->getValue("payload"));
        EXPECT_EQ(readBackLongText.size(), longText.size());
        EXPECT_EQ(readBackLongText, longText);
    }

    /**
     * @brief 钉住被复用的预处理语句仍读到最新数据，且失败一次不会把这条语句用坏
     * @details 语句缓存带来的风险不是崩溃而是两件静默的事：复用的语句读出上一轮的旧结果，
     *          或者一次失败之后同一条文本再也执行不成功。前者用「改数据后再跑同一文本」钉住，
     *          后者用「本地拒绝 → 服务端拒绝 → 同文本继续正常」这一段钉住。
     */
    TEST_F(MySqlIntegrationTest, ReusedStatementServesFreshRowsAndRecoversFromFailure)
    {
        ASSERT_TRUE(prepareTable(kReusedStatementTableName, kReusedStatementColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const std::string insertStatement = "INSERT INTO " + quote(kReusedStatementTableName) + " (`id`, `name`) VALUES (?, ?)";
        const std::string selectStatement = "SELECT `name` FROM " + quote(kReusedStatementTableName) + " WHERE `id` = ?";
        const std::string updateStatement = "UPDATE " + quote(kReusedStatementTableName) + " SET `name` = ? WHERE `id` = ?";

        ASSERT_NE(connection.execute(insertStatement, std::vector<DatabaseValue>{std::int64_t{1}, std::string{"第一版"}}), nullptr)
            << connection.lastError();

        // 同一条文本被反复执行：第二次起走的就是缓存里那条已预编译的语句
        const auto readNameById = [&connection, &selectStatement](const std::int64_t identifier) -> std::string
        {
            const std::unique_ptr<DatabaseResult> result = connection.execute(selectStatement, std::vector<DatabaseValue>{identifier});
            EXPECT_NE(result, nullptr) << connection.lastError();
            if (result == nullptr || !result->next())
            {
                return {};
            }
            return std::get<std::string>(result->getValue(0));
        };

        EXPECT_EQ(readNameById(1), "第一版");

        ASSERT_NE(connection.execute(updateStatement, std::vector<DatabaseValue>{std::string{"第二版"}, std::int64_t{1}}), nullptr)
            << connection.lastError();
        // 复用的语句若把上一轮的结果留在客户端缓冲里交出来，这里就会读到「第一版」
        EXPECT_EQ(readNameById(1), "第二版");
        EXPECT_EQ(readNameById(1), "第二版");

        // 参数个数与占位符不符：驱动本地拒绝，同一条文本随后仍要正常工作
        EXPECT_EQ(connection.execute(selectStatement, std::vector<DatabaseValue>{}), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_EQ(readNameById(1), "第二版");

        // 服务端拒绝的一次（主键冲突）：这条语句被弃掉，同一条插入文本下一次换参数仍要成功
        EXPECT_EQ(connection.execute(insertStatement, std::vector<DatabaseValue>{std::int64_t{1}, std::string{"重复"}}), nullptr);
        EXPECT_NE(connection.execute(insertStatement, std::vector<DatabaseValue>{std::int64_t{2}, std::string{"新增"}}), nullptr)
            << connection.lastError();
        EXPECT_EQ(readNameById(2), "新增");
    }

    /**
     * @brief 验证含单引号与 "--" 的文本经参数绑定后原样回读，且表结构未被破坏
     *
     * @details 若取值是被拼进 SQL 文本而不是绑定送入，这段文本会提前闭合字符串字面量，
     *          后半段则变成注释与新的语句：轻则插入失败，重则表被删掉。这里既断言文本原样
     *          回读，也断言注入残留（DROP TABLE）没有生效。
     */
    TEST_F(MySqlIntegrationTest, ParameterizedTextWithQuotesAndCommentMarkersRoundTripsVerbatim)
    {
        ASSERT_TRUE(prepareTable(kInjectionTableName, kInjectionColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 恶意文本直接点名本用例的表，让「注入是否生效」可被观察
        const std::string hostileText = "O'Brien -- DROP TABLE " + quote(kInjectionTableName) + "; --";
        // 先自检恶意文本确实带齐了危险字符，否则本用例会变成一个空断言
        EXPECT_NE(hostileText.find('\''), std::string::npos);
        EXPECT_NE(hostileText.find("--"), std::string::npos);
        EXPECT_NE(hostileText.find("DROP TABLE"), std::string::npos);

        const std::vector<DatabaseValue> insertParameters{std::int64_t{1}, hostileText};
        const std::unique_ptr<DatabaseResult> insertResult = connection.execute(
            "INSERT INTO " + quote(kInjectionTableName) + " (`id`, `name`) VALUES (?, ?)", insertParameters);
        ASSERT_NE(insertResult, nullptr) << connection.lastError();
        ASSERT_EQ(insertResult->affectedRowCount(), 1);

        // 按值查询：WHERE 的取值同样走绑定，能精确命中说明存进去的就是原文
        const std::vector<DatabaseValue> selectParameters{hostileText};
        const std::unique_ptr<DatabaseResult> selectResult = connection.execute(
            "SELECT `name` FROM " + quote(kInjectionTableName) + " WHERE `name` = ?", selectParameters);
        ASSERT_NE(selectResult, nullptr) << connection.lastError();
        ASSERT_TRUE(selectResult->next());
        EXPECT_EQ(std::get<std::string>(selectResult->getValue("name")), hostileText);
        EXPECT_FALSE(selectResult->next());

        // 表仍在、恰好一行：证明 "--" 没有注释掉后续内容，分号也没有开启新语句
        EXPECT_EQ(countRows(connection, kInjectionTableName), 1);
    }

    /**
     * @brief 验证参数个数与占位符个数不匹配（少传 / 多传）都会被拒绝且错误为中文
     */
    TEST_F(MySqlIntegrationTest, ParameterizedStatementRejectsMismatchedParameterCount)
    {
        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // "SELECT ?" 恰好一个占位符且不需要任何表，把「个数不匹配」这一条单独隔离出来：
        // 语句本身能 prepare 成功，失败必然来自参数个数校验
        const std::unique_ptr<DatabaseResult> tooFewResult =
            connection.execute("SELECT ?", std::span<const DatabaseValue>{});
        EXPECT_EQ(tooFewResult, nullptr);
        const std::string tooFewReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(tooFewReason)) << tooFewReason;
        EXPECT_NE(tooFewReason.find("参数数量不匹配"), std::string::npos) << tooFewReason;
        // 原因里要同时给出「需要几个」与「实际给了几个」，否则调用方无从定位
        EXPECT_NE(tooFewReason.find("需要 1"), std::string::npos) << tooFewReason;
        EXPECT_NE(tooFewReason.find("实际提供 0"), std::string::npos) << tooFewReason;

        const std::vector<DatabaseValue> extraParameters{std::int64_t{1}, std::int64_t{2}};
        const std::unique_ptr<DatabaseResult> tooManyResult = connection.execute("SELECT ?", extraParameters);
        EXPECT_EQ(tooManyResult, nullptr);
        const std::string tooManyReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(tooManyReason)) << tooManyReason;
        EXPECT_NE(tooManyReason.find("参数数量不匹配"), std::string::npos) << tooManyReason;
        EXPECT_NE(tooManyReason.find("实际提供 2"), std::string::npos) << tooManyReason;
    }

    /**
     * @brief 验证容器类型参数被明确拒绝并给出中文原因
     *
     * @details DatabaseValue 允许承载 List / Hash（Redis 的返回形态），但把它们当成单个
     *          标量参数绑定给 SQL 占位符没有正确语义：正确用法是展开成多个标量参数
     *          （IN 列表由方言展开）。静默按 NULL 或空串执行会让调用方以为条件生效了。
     */
    TEST_F(MySqlIntegrationTest, ParameterizedStatementRejectsContainerParameter)
    {
        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 列表：对应 Redis List 形态
        const std::vector<DatabaseValue> listParameters{std::vector<std::string>{"甲", "乙"}};
        const std::unique_ptr<DatabaseResult> listResult = connection.execute("SELECT ?", listParameters);
        EXPECT_EQ(listResult, nullptr);
        const std::string listReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(listReason)) << listReason;
        EXPECT_NE(listReason.find("容器"), std::string::npos) << listReason;
        EXPECT_NE(listReason.find("List"), std::string::npos) << listReason;

        // 哈希：对应 Redis Hash 形态
        const std::vector<DatabaseValue> hashParameters{
            std::unordered_map<std::string, std::string>{{"键", "值"}}};
        const std::unique_ptr<DatabaseResult> hashResult = connection.execute("SELECT ?", hashParameters);
        EXPECT_EQ(hashResult, nullptr);
        const std::string hashReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(hashReason)) << hashReason;
        EXPECT_NE(hashReason.find("容器"), std::string::npos) << hashReason;
        EXPECT_NE(hashReason.find("Hash"), std::string::npos) << hashReason;

        // 容器参数被拒后连接仍可继续使用：校验发生在任何服务端交互之前
        EXPECT_TRUE(connection.isConnected());
        EXPECT_NE(connection.execute("SELECT 1"), nullptr) << connection.lastError();
    }

    // ========================================================================
    // ORM 端到端（SQL 由 MySqlDialect 生成）
    // ========================================================================

    /**
     * @brief 验证 ORM 的插入、条件查询、排序分页、单行读取、计数、更新与删除在真实 MySQL 上成立
     */
    TEST_F(MySqlIntegrationTest, OrmCrudRoundTripOverWhereOrderByPagingAndPrimaryKey)
    {
        ASSERT_TRUE(prepareTable(kAccountTableName, kAccountColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);

        OrmQuery<IntegrationAccountRow> insertQuery(*pool);
        ASSERT_EQ(1, insertQuery.insert(makeAccountRow(1, "张三", 1234.56, std::string("普通备注"), true)));
        ASSERT_EQ(1, insertQuery.insert(makeAccountRow(2, "O'Brien", -99.5, std::nullopt, false)));
        // 第三行的备注是空串：与第 2 行的 NULL 必须能区分开
        ASSERT_EQ(1, insertQuery.insert(makeAccountRow(3, "李四", 0.0, std::string(""), true)));

        // ---- toList：WHERE + ORDER BY + LIMIT + OFFSET（分页走 MySQL 的 "LIMIT ? OFFSET ?"）----
        {
            OrmQuery<IntegrationAccountRow> query(*pool);
            const std::vector<IntegrationAccountRow> rows =
                query.orderBy(asc("id")).limit(2).offset(1).toList();

            ASSERT_EQ(rows.size(), 2U);
            EXPECT_EQ(rows[0].id, 2);
            EXPECT_EQ(rows[1].id, 3);
            EXPECT_EQ(rows[0].name, "O'Brien");
            EXPECT_DOUBLE_EQ(rows[0].balance, -99.5);
            EXPECT_FALSE(rows[0].active);
            // NULL → 空 optional；空串 → 有值的空 optional
            EXPECT_FALSE(rows[0].note.has_value());
            ASSERT_TRUE(rows[1].note.has_value());
            EXPECT_TRUE(rows[1].note->empty());
        }

        // ---- WHERE 取子集 ----
        {
            OrmQuery<IntegrationAccountRow> query(*pool);
            const std::vector<IntegrationAccountRow> rows =
                query.where(Column(&IntegrationAccountRow::id, "id") >= std::int64_t{2})
                     .orderBy(asc("id"))
                     .toList();

            ASSERT_EQ(rows.size(), 2U);
            EXPECT_EQ(rows[0].id, 2);
            EXPECT_EQ(rows[1].id, 3);
        }

        // ---- first：命中与不命中 ----
        {
            OrmQuery<IntegrationAccountRow> hitQuery(*pool);
            const std::optional<IntegrationAccountRow> hit =
                hitQuery.where(Column(&IntegrationAccountRow::id, "id") == std::int64_t{1}).first();

            ASSERT_TRUE(hit.has_value());
            EXPECT_EQ(hit->name, "张三");
            EXPECT_DOUBLE_EQ(hit->balance, 1234.56);
            ASSERT_TRUE(hit->note.has_value());
            EXPECT_EQ(hit->note.value(), "普通备注");
            EXPECT_TRUE(hit->active);

            OrmQuery<IntegrationAccountRow> missQuery(*pool);
            EXPECT_FALSE(missQuery.where(Column(&IntegrationAccountRow::id, "id") == std::int64_t{404}).first().has_value());
        }

        // ---- count：无条件与带条件 ----
        {
            OrmQuery<IntegrationAccountRow> countQuery(*pool);
            EXPECT_EQ(countQuery.count(), 3);

            OrmQuery<IntegrationAccountRow> negativeCountQuery(*pool);
            EXPECT_EQ(negativeCountQuery.where(Column(&IntegrationAccountRow::balance, "balance") < 0.0).count(), 1);
        }

        // ---- update：按主键只改目标行 ----
        {
            OrmQuery<IntegrationAccountRow> updateQuery(*pool);
            EXPECT_EQ(updateQuery.update(makeAccountRow(2, "王五", 888.25, std::string("已更新"), true)), 1);

            OrmQuery<IntegrationAccountRow> readQuery(*pool);
            const std::vector<IntegrationAccountRow> rows = readQuery.orderBy(asc("id")).toList();
            ASSERT_EQ(rows.size(), 3U);
            EXPECT_EQ(rows[1].id, 2);
            EXPECT_EQ(rows[1].name, "王五");
            EXPECT_DOUBLE_EQ(rows[1].balance, 888.25);
            ASSERT_TRUE(rows[1].note.has_value());
            EXPECT_EQ(rows[1].note.value(), "已更新");
            EXPECT_TRUE(rows[1].active);
            // 其它两行不受影响
            EXPECT_EQ(rows[0].name, "张三");
            EXPECT_EQ(rows[2].name, "李四");
        }

        // ---- executeNonQuery：带条件删除并回报受影响行数 ----
        {
            OrmQuery<IntegrationAccountRow> deleteQuery(*pool);
            EXPECT_EQ(deleteQuery.where(Column(&IntegrationAccountRow::id, "id") >= std::int64_t{2}).executeNonQuery(), 2);

            OrmQuery<IntegrationAccountRow> remainingQuery(*pool);
            EXPECT_EQ(remainingQuery.count(), 1);

            OrmQuery<IntegrationAccountRow> lastRowQuery(*pool);
            const std::vector<IntegrationAccountRow> remainingRows = lastRowQuery.orderBy(asc("id")).toList();
            ASSERT_EQ(remainingRows.size(), 1U);
            EXPECT_EQ(remainingRows[0].id, 1);
        }
    }

    /**
     * @brief 验证 insertBatch 在行数超过方言参数上限时自动分块，且分块边界不丢不多
     *
     * @details MySQL 单条预处理语句的参数上限是 65535（协议层 num_params 字段只有 2 字节），
     *          每行占用「列数」个参数，因此每批最多 65535 ÷ 列数 行。这里插入「每批行数 + 1」行，
     *          让第二批恰好只剩一行——分块最常见的缺陷（最后一块上界算错）正落在这个边界上。
     */
    TEST_F(MySqlIntegrationTest, OrmBatchInsertChunksRowsBeyondDialectParameterLimit)
    {
        ASSERT_TRUE(prepareTable(kBatchTableName, kBatchColumns)) << m_lastSetupError;

        // 每行的绑定参数个数就是列数，由 TableSchema 的列数在编译期给出；
        // 该常量必须写在此处（特化声明之后），否则会先实例化空列的主模板而算出 0
        constexpr std::size_t kBatchColumnCount =
            std::tuple_size_v<std::remove_cvref_t<decltype(Queryable::TableSchema<IntegrationBatchRow>::kColumns)>>;

        const MySqlDialect dialect;
        const std::size_t rowsPerStatement = dialect.maximumStatementParameters() / kBatchColumnCount;
        const std::size_t totalRowCount    = rowsPerStatement + 1U;
        ASSERT_GE(rowsPerStatement, 2U);

        std::vector<IntegrationBatchRow> rows;
        rows.reserve(totalRowCount);
        for (std::size_t rowIndex = 0; rowIndex < totalRowCount; ++rowIndex)
        {
            const std::int64_t identifier = static_cast<std::int64_t>(rowIndex) + 1;
            rows.push_back(IntegrationBatchRow{
                .id   = identifier,
                .name = "批量行" + std::to_string(identifier),
                .score = static_cast<double>(identifier),
                // 偶数行不给备注（NULL）、奇数行给备注：让分块边界两侧的取值形态也不同
                .note   = (identifier % 2 == 0) ? std::optional<std::string>{} : std::optional<std::string>{"奇数行备注"},
                .active = identifier % 2 != 0
            });
        }

        std::unique_ptr<ConnectionPool> pool = makePool(3);

        OrmQuery<IntegrationBatchRow> batchQuery(*pool);
        // 分块路径必须把所有块落进同一个事务，累计影响行数等于总行数
        EXPECT_EQ(batchQuery.insertBatch(rows), static_cast<std::int64_t>(totalRowCount));

        OrmQuery<IntegrationBatchRow> countQuery(*pool);
        EXPECT_EQ(countQuery.count(), static_cast<std::int64_t>(totalRowCount));

        const auto readBackRow = [&pool](const std::int64_t identifier) -> std::optional<IntegrationBatchRow>
        {
            OrmQuery<IntegrationBatchRow> query(*pool);
            return query.where(Column(&IntegrationBatchRow::id, "id") == identifier).first();
        };

        // 第一块的首行：各列取值与写入一致
        const std::optional<IntegrationBatchRow> headRow = readBackRow(1);
        ASSERT_TRUE(headRow.has_value());
        EXPECT_EQ(headRow->name, "批量行1");
        EXPECT_DOUBLE_EQ(headRow->score, 1.0);
        ASSERT_TRUE(headRow->note.has_value());
        EXPECT_EQ(headRow->note.value(), "奇数行备注");
        EXPECT_TRUE(headRow->active);

        // 第一块的末行：恰好落在「每批行数」这个分块上界上，最常见缺陷是这里被算丢
        const std::optional<IntegrationBatchRow> boundaryRow = readBackRow(static_cast<std::int64_t>(rowsPerStatement));
        ASSERT_TRUE(boundaryRow.has_value());
        EXPECT_EQ(boundaryRow->name, "批量行" + std::to_string(rowsPerStatement));

        // 第二块的唯一一行：证明溢出到第二批的行确实被写进去了
        const std::optional<IntegrationBatchRow> tailRow = readBackRow(static_cast<std::int64_t>(totalRowCount));
        ASSERT_TRUE(tailRow.has_value());
        EXPECT_EQ(tailRow->name, "批量行" + std::to_string(totalRowCount));
        EXPECT_DOUBLE_EQ(tailRow->score, static_cast<double>(totalRowCount));
        EXPECT_FALSE(tailRow->note.has_value());
        EXPECT_FALSE(tailRow->active);
    }

    /**
     * @brief 验证 MySqlDialect 生成的反引号引用、多行 VALUES 与 "LIMIT ? OFFSET ?" 在真实服务端可执行
     *
     * @details 表名含连字符，列名覆盖保留字（order / group）、空格与反引号：
     *          只有把标识符正确加反引号（并把内部反引号翻倍）才能建表并被 ORM 读写。
     *          若方言偷懒不加引用或转义写错，这里会直接收到服务端的语法错误。
     */
    TEST_F(MySqlIntegrationTest, OrmQuotedIdentifiersRemainExecutableOnRealServer)
    {
        ASSERT_TRUE(prepareTable(kQuotedTableName, kQuotedColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);

        // ---- 单行插入：走 "INSERT INTO `表` (`列`…) VALUES (?, …)" ----
        OrmQuery<IntegrationQuotedRow> insertQuery(*pool);
        EXPECT_EQ(1, insertQuery.insert(IntegrationQuotedRow{
                          .id           = 1,
                          .order        = "保留字order",
                          .group        = "保留字group",
                          .spacedColumn = "空格列值",
                          .tickColumn   = "反引号列值"
                      }));

        // ---- 多行 VALUES：走方言的 translateInsertBatch ----
        const std::vector<IntegrationQuotedRow> batchRows{
            IntegrationQuotedRow{.id = 2, .order = "第二行order", .group = "第二行group",
                                 .spacedColumn = std::string("第二行空格"), .tickColumn = std::string("第二行反引号")},
            IntegrationQuotedRow{.id = 3, .order = "第三行order", .group = "第三行group",
                                 .spacedColumn = std::string("第三行空格"), .tickColumn = std::string("第三行反引号")}
        };
        EXPECT_EQ(2, insertQuery.insertBatch(batchRows));

        // ---- WHERE 命中保留字列 + LIMIT ? OFFSET ? ----
        {
            OrmQuery<IntegrationQuotedRow> query(*pool);
            const std::vector<IntegrationQuotedRow> rows =
                query.where(Column(&IntegrationQuotedRow::order, "order") == "保留字order")
                     .orderBy(asc("id"))
                     .limit(1)
                     .offset(0)
                     .toList();

            ASSERT_EQ(rows.size(), 1U);
            EXPECT_EQ(rows[0].id, 1);
            EXPECT_EQ(rows[0].group, "保留字group");
            // 含空格的列名与含反引号的列名都被正确引用，取值能原样读回
            EXPECT_EQ(rows[0].spacedColumn, "空格列值");
            EXPECT_EQ(rows[0].tickColumn, "反引号列值");
        }

        // ---- 分页在同一批数据上取第二行：OFFSET 以绑定参数送出 ----
        {
            OrmQuery<IntegrationQuotedRow> query(*pool);
            const std::vector<IntegrationQuotedRow> rows = query.orderBy(asc("id")).limit(1).offset(1).toList();

            ASSERT_EQ(rows.size(), 1U);
            EXPECT_EQ(rows[0].id, 2);
            EXPECT_EQ(rows[0].tickColumn, "第二行反引号");
        }

        // ---- 计数、更新与删除同样要能穿过这些标识符 ----
        {
            OrmQuery<IntegrationQuotedRow> countQuery(*pool);
            EXPECT_EQ(countQuery.count(), 3);

            OrmQuery<IntegrationQuotedRow> updateQuery(*pool);
            EXPECT_EQ(updateQuery.update(IntegrationQuotedRow{.id = 3, .order = "改后order", .group = "改后group",
                                                             .spacedColumn = std::string("改后空格"),
                                                             .tickColumn = std::string("改后反引号")}),
                      1);

            OrmQuery<IntegrationQuotedRow> updatedQuery(*pool);
            const std::optional<IntegrationQuotedRow> updated =
                updatedQuery.where(Column(&IntegrationQuotedRow::id, "id") == std::int64_t{3}).first();
            ASSERT_TRUE(updated.has_value());
            EXPECT_EQ(updated->order, "改后order");
            EXPECT_EQ(updated->tickColumn, "改后反引号");

            OrmQuery<IntegrationQuotedRow> deleteQuery(*pool);
            // 上一段更新把第三行的 group 改成了 "改后group"，这里按当前取值删除才命中该行
            EXPECT_EQ(deleteQuery.where(Column(&IntegrationQuotedRow::group, "group") == "改后group").executeNonQuery(), 1);

            OrmQuery<IntegrationQuotedRow> remainingQuery(*pool);
            EXPECT_EQ(remainingQuery.count(), 2);
        }
    }

    /**
     * @brief 验证 SchemaMigrator 在真实服务端建表后能被 ORM 直接读写，并能查存在与删表
     *
     * @details 覆盖与 SQLite 端到端用例相同的链路，但走真实 MySQL：DDL 的物理类型名
     *          （BIGINT / BIGINT UNSIGNED / DOUBLE / TINYINT(1) / TEXT）必须在本服务端被接受，
     *          且建出来的表要能被 ORM 的绑定与行映射直接读写。表名独立，不与其它用例共用。
     */
    TEST_F(MySqlIntegrationTest, SchemaMigratorCreatesTableThenOrmRoundTripAndDrops)
    {
        // 记下待清理的表名：即使断言中途失败，TearDown 也会 DROP TABLE IF EXISTS 兜底
        m_preparedTableName = std::string(kMigratedTableName);

        std::string errorText;

        // 先清掉上次运行可能留下的残留表：建表用例必须从「表不存在」这个前提出发
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationMigratedRow>(*makePool(1), true, &errorText)) << errorText;

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        EXPECT_FALSE(SchemaMigrator::tableExists<IntegrationMigratedRow>(*pool, &errorText));
        // 表确实不存在时 errorText 必须留空：它只承载「查询失败」这类原因
        EXPECT_TRUE(errorText.empty()) << errorText;

        // ---- 建表：DDL 由 SchemaMigrator 从 TableSchema 生成 ----
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationMigratedRow>(*pool, true, &errorText)) << errorText;
        EXPECT_TRUE(SchemaMigrator::tableExists<IntegrationMigratedRow>(*pool, &errorText)) << errorText;

        // ---- 写入两行：第一行有备注，第二行备注为 NULL（TEXT 列可空） ----
        {
            OrmQuery<IntegrationMigratedRow> insertQuery(*pool);
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 1, .name = "张三", .note = std::string("首条"),
                                                                   .balance = 1234.5, .active = true, .sequence = 7U}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 2, .name = "Li Si", .note = std::nullopt,
                                                                   .balance = -0.25, .active = false, .sequence = 8U}));
        }

        // ---- 读回：六列逐一核对，证明 DDL 的类型与可空性恰好匹配 ORM 的映射规则 ----
        {
            OrmQuery<IntegrationMigratedRow> query(*pool);
            const std::vector<IntegrationMigratedRow> rows = query.orderBy(asc("id")).toList();

            ASSERT_EQ(rows.size(), 2U);
            EXPECT_EQ(rows[0].id, 1);
            EXPECT_EQ(rows[0].name, "张三");
            ASSERT_TRUE(rows[0].note.has_value());
            EXPECT_EQ(rows[0].note.value(), "首条");
            EXPECT_DOUBLE_EQ(rows[0].balance, 1234.5);
            EXPECT_TRUE(rows[0].active);
            EXPECT_EQ(rows[0].sequence, 7U);

            EXPECT_EQ(rows[1].name, "Li Si");
            EXPECT_FALSE(rows[1].note.has_value());
            EXPECT_DOUBLE_EQ(rows[1].balance, -0.25);
            EXPECT_FALSE(rows[1].active);
            EXPECT_EQ(rows[1].sequence, 8U);
        }

        // ---- 重复建表（IF NOT EXISTS）幂等：返回 true，已写入的数据不受影响 ----
        EXPECT_TRUE(SchemaMigrator::createTable<IntegrationMigratedRow>(*pool)) << errorText;
        {
            OrmQuery<IntegrationMigratedRow> countQuery(*pool);
            EXPECT_EQ(countQuery.count(), 2);
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
     * @brief 验证 BIGINT UNSIGNED 列的完整取值域都能往返，含 int64 装不下的那一段
     *
     * @details BIGINT UNSIGNED 的上界超出 int64，驱动只能以十进制文本返回这类取值，写方向同样降级为文本：
     *          读方向缺了文本支路就会「写得进、读不回」。本用例与建表迁移用例同构但绑定独立表名，
     *          各自持有自己的物理表，并行执行时不会互删对方正在使用的表。
     */
    TEST_F(MySqlIntegrationTest, UnsignedColumnRoundTripsAcrossInt64Boundary)
    {
        m_preparedTableName = std::string(kUnsignedTableName);

        std::string errorText;
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationUnsignedRow>(*makePool(1), true, &errorText)) << errorText;

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationUnsignedRow>(*pool, true, &errorText)) << errorText;

        /// 2^63：int64 表示不了、BIGINT UNSIGNED 表示得了的第一个取值
        constexpr std::uint64_t kTwoToTheSixtyThird   = 9223372036854775808ULL;
        /// 2^64-1：BIGINT UNSIGNED 的上界，也是 uint64 的上界
        constexpr std::uint64_t kMaximumUnsignedValue = std::numeric_limits<std::uint64_t>::max();
        const auto              maximumSignedValue =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

        // ---- 写入四个代表性取值 ----
        {
            OrmQuery<IntegrationUnsignedRow> insertQuery(*pool);
            EXPECT_EQ(1, insertQuery.insert(IntegrationUnsignedRow{.id = 1, .name = "零", .note = std::nullopt,
                                                                   .balance = 0.0, .active = true, .sequence = 0U}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationUnsignedRow{.id = 2, .name = "int64上界", .note = std::nullopt,
                                                                   .balance = 0.0, .active = true,
                                                                   .sequence = maximumSignedValue}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationUnsignedRow{.id = 3, .name = "2的63次方", .note = std::nullopt,
                                                                   .balance = 0.0, .active = true,
                                                                   .sequence = kTwoToTheSixtyThird}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationUnsignedRow{.id = 4, .name = "uint64上界", .note = std::nullopt,
                                                                   .balance = 0.0, .active = true,
                                                                   .sequence = kMaximumUnsignedValue}));
        }

        // ---- 读回：四个取值必须逐位相等，任何一个被降级成 double 都会在这里暴露 ----
        {
            OrmQuery<IntegrationUnsignedRow> query(*pool);
            const std::vector<IntegrationUnsignedRow> rows = query.orderBy(asc("id")).toList();

            ASSERT_EQ(rows.size(), 4U);
            EXPECT_EQ(rows[0].sequence, 0U);
            EXPECT_EQ(rows[1].sequence, maximumSignedValue);
            EXPECT_EQ(rows[2].sequence, kTwoToTheSixtyThird);
            EXPECT_EQ(rows[3].sequence, kMaximumUnsignedValue);
        }

        // ---- 服务端确实按无符号数值存：数值比较能命中，而文本比较不会 ——
        // BIGINT UNSIGNED 上的 '>' 走数值语义，因此这条条件能命中 id=3/4 两行；
        // 若取值被存成文本（例如列类型被误建为 TEXT），SQLite 那种字典序比较会给出不同结果
        {
            OrmQuery<IntegrationUnsignedRow> query(*pool);
            const std::int64_t aboveBoundaryCount =
                query.where(Column(&IntegrationUnsignedRow::sequence, "sequence") >
                            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                     .count();
            EXPECT_EQ(aboveBoundaryCount, 2);
        }

        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationUnsignedRow>(*pool, true, &errorText)) << errorText;
    }

    /**
     * @brief 验证二进制列在真实服务端上按 BLOB 存取，且同表的文本列不会被误判成二进制
     *
     * @details MySQL 协议层不区分 BLOB 与 TEXT（类型码同为 MYSQL_TYPE_BLOB），驱动只能靠列的字符集号
     *          （binary = 63）判断。载荷刻意含单独出现的 0xFF（不是合法 utf8mb4 序列）：按文本绑定会被
     *          服务端按连接字符集替换，因此只有按 BLOB 存取才能逐字节往返。
     */
    TEST_F(MySqlIntegrationTest, BinaryColumnRoundTripsAsBlobWhileTextColumnStaysText)
    {
        m_preparedTableName = std::string(kBinaryTableName);

        std::string errorText;
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationBinaryRow>(*makePool(1), true, &errorText)) << errorText;

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationBinaryRow>(*pool, true, &errorText)) << errorText;

        // 0x00 是内嵌 NUL、0xFF 单独出现不是合法 utf8mb4 序列：两者都是「按文本搬运会坏」的形态
        const BinaryBytes payload{0x00, 0x5C, 0xFF, 0x41, 0x00};

        {
            OrmQuery<IntegrationBinaryRow> insertQuery(*pool);
            EXPECT_EQ(1, insertQuery.insert(IntegrationBinaryRow{.id = 1, .payload = payload, .label = "文本对照"}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationBinaryRow{.id = 2, .payload = BinaryBytes{}, .label = "zero"}));
        }

        {
            OrmQuery<IntegrationBinaryRow> query(*pool);
            const std::vector<IntegrationBinaryRow> rows = query.orderBy(asc("id")).toList();

            ASSERT_EQ(rows.size(), 2U);
            // 逐字节相等：任何按字符集做过的转换都会在这里露出差异
            EXPECT_EQ(rows[0].payload, payload);
            EXPECT_TRUE(rows[1].payload.empty());
            // 文本列仍按文本读回——它与 payload 的类型码相同，只有字符集能把两者分开
            EXPECT_EQ(rows[0].label, "文本对照");
            EXPECT_EQ(rows[1].label, "zero");
        }

        // 按二进制列做条件查询：真机上验证 ParameterValue 的二进制支路（与写入路径是两套变体）
        {
            OrmQuery<IntegrationBinaryRow> query(*pool);
            const std::vector<IntegrationBinaryRow> matched =
                query.where(Column(&IntegrationBinaryRow::payload, "payload") == payload).toList();
            ASSERT_EQ(matched.size(), 1U);
            EXPECT_EQ(matched[0].id, 1);
            EXPECT_EQ(matched[0].payload, payload);
        }

        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationBinaryRow>(*pool, true, &errorText)) << errorText;
    }

    /**
     * @brief 验证空结果集上 toList() 为空、first() 为空、count() 为零
     */
    TEST_F(MySqlIntegrationTest, OrmEmptyResultSetYieldsEmptyListAndEmptyFirst)
    {
        ASSERT_TRUE(prepareTable(kProbeTableName, kProbeColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(2);

        // 整张表为空
        {
            OrmQuery<IntegrationProbeRow> query(*pool);
            EXPECT_TRUE(query.toList().empty());
            EXPECT_FALSE(query.first().has_value());
            EXPECT_EQ(query.count(), 0);
        }

        // 表里有数据但条件不命中
        {
            OrmQuery<IntegrationProbeRow> insertQuery(*pool);
            ASSERT_EQ(1, insertQuery.insert(IntegrationProbeRow{.id = 1, .name = "唯一一行"}));
        }

        {
            OrmQuery<IntegrationProbeRow> query(*pool);
            const std::vector<IntegrationProbeRow> rows =
                query.where(Column(&IntegrationProbeRow::id, "id") == std::int64_t{404}).toList();
            EXPECT_TRUE(rows.empty());

            OrmQuery<IntegrationProbeRow> firstQuery(*pool);
            EXPECT_FALSE(firstQuery.where(Column(&IntegrationProbeRow::id, "id") == std::int64_t{404}).first().has_value());

            OrmQuery<IntegrationProbeRow> countQuery(*pool);
            EXPECT_EQ(countQuery.where(Column(&IntegrationProbeRow::id, "id") == std::int64_t{404}).count(), 0);

            // 命中条件时三者都要给出数据，证明上面的「空」不是查询整体失败造成的
            OrmQuery<IntegrationProbeRow> hitQuery(*pool);
            EXPECT_EQ(hitQuery.where(Column(&IntegrationProbeRow::id, "id") == std::int64_t{1}).count(), 1);
        }
    }

    // ========================================================================
    // ORM 异步链路（门控：没有口令时整个夹具在 SetUp 里跳过）
    // ========================================================================

    /**
     * @brief 验证真实服务端上的异步读写全链路与同步版在同一张表上逐项一致
     *
     * @details 覆盖 insertAsync → toListAsync → countAsync → updateAsync → executeNonQueryAsync：每一步都与同一张表上的
     *          同步版本对照，异步写之后一律用**同步查询**读回——同步查询看到的才是服务端的真实数据，这是「写确实提交上去了」
     *          的权威证据。执行器沿用进程级共享实例（顺带覆盖零配置的默认路径），协程的恢复都发生在后台事件循环线程上。
     */
    TEST_F(MySqlIntegrationTest, AsyncReadWriteChainMatchesSyncResults)
    {
        ASSERT_TRUE(prepareTable(kAsyncChainTableName, kAsyncChainColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool        = makePool(3);
        TestSupport::EventLoopThread   &loopRunner  = asyncLoop();

        // ---- insertAsync：异步写一行，随后同步写一行作对照 ----
        {
            OrmQuery<IntegrationAsyncRow> asyncInsertQuery(*pool);
            const TestSupport::CompletedTask<std::int64_t> inserted = loopRunner.runToCompletion(
                asyncInsertQuery.insertAsync(IntegrationAsyncRow{.id = 1, .name = "异步写入", .note = std::string("首条")},
                                             loopRunner.loop()));

            ASSERT_TRUE(inserted.finished) << "异步写入未在时限内完成";
            ASSERT_EQ(inserted.error, nullptr);
            ASSERT_TRUE(inserted.value.has_value());
            EXPECT_EQ(inserted.value.value(), 1);

            OrmQuery<IntegrationAsyncRow> syncInsertQuery(*pool);
            const std::int64_t syncAffectedRows =
                syncInsertQuery.insert(IntegrationAsyncRow{.id = 2, .name = "同步写入", .note = std::nullopt});
            EXPECT_EQ(inserted.value.value(), syncAffectedRows);
        }

        // ---- toListAsync：与同步 toList 逐行逐列比对 ----
        {
            OrmQuery<IntegrationAsyncRow> syncQuery(*pool);
            const std::vector<IntegrationAsyncRow> syncRows = syncQuery.orderBy(asc("id")).toList();

            OrmQuery<IntegrationAsyncRow> asyncQuery(*pool);
            asyncQuery.orderBy(asc("id"));
            const TestSupport::CompletedTask<std::vector<IntegrationAsyncRow>> listed =
                loopRunner.runToCompletion(asyncQuery.toListAsync(loopRunner.loop()));

            ASSERT_TRUE(listed.finished) << "异步查询未在时限内完成";
            ASSERT_EQ(listed.error, nullptr);
            ASSERT_TRUE(listed.value.has_value());
            ASSERT_EQ(listed.value->size(), syncRows.size());
            ASSERT_EQ(listed.value->size(), 2U);
            for (std::size_t index = 0; index < syncRows.size(); ++index)
            {
                EXPECT_EQ((*listed.value)[index].id, syncRows[index].id);
                EXPECT_EQ((*listed.value)[index].name, syncRows[index].name);
                EXPECT_EQ((*listed.value)[index].note, syncRows[index].note);
            }

            // 异步写进去的那一行，被同步查询原样读到：字段与可空列的取值都对得上
            EXPECT_EQ(syncRows[0].name, "异步写入");
            ASSERT_TRUE(syncRows[0].note.has_value());
            EXPECT_EQ(syncRows[0].note.value(), "首条");
            // NULL 与空串在真实服务端上仍是两种形态：第 2 行的备注写的是 NULL
            EXPECT_FALSE(syncRows[1].note.has_value());
        }

        // ---- countAsync：与同步 count 相等 ----
        {
            OrmQuery<IntegrationAsyncRow> syncCountQuery(*pool);
            const std::int64_t syncCount = syncCountQuery.count();

            OrmQuery<IntegrationAsyncRow> asyncCountQuery(*pool);
            const TestSupport::CompletedTask<std::int64_t> counted =
                loopRunner.runToCompletion(asyncCountQuery.countAsync(loopRunner.loop()));

            ASSERT_TRUE(counted.finished);
            ASSERT_EQ(counted.error, nullptr);
            ASSERT_TRUE(counted.value.has_value());
            EXPECT_EQ(counted.value.value(), syncCount);
            EXPECT_EQ(counted.value.value(), 2);
        }

        // ---- updateAsync：异步改第 1 行（备注置为 NULL），同步改第 2 行作对照 ----
        {
            OrmQuery<IntegrationAsyncRow> asyncUpdateQuery(*pool);
            const TestSupport::CompletedTask<std::int64_t> updated = loopRunner.runToCompletion(
                asyncUpdateQuery.updateAsync(IntegrationAsyncRow{.id = 1, .name = "异步改后", .note = std::nullopt},
                                             loopRunner.loop()));
            ASSERT_TRUE(updated.finished);
            ASSERT_EQ(updated.error, nullptr);
            ASSERT_TRUE(updated.value.has_value());
            EXPECT_EQ(updated.value.value(), 1);

            OrmQuery<IntegrationAsyncRow> syncUpdateQuery(*pool);
            const std::int64_t syncAffectedRows = syncUpdateQuery.update(
                IntegrationAsyncRow{.id = 2, .name = "同步改后", .note = std::string("同步备注")});
            EXPECT_EQ(updated.value.value(), syncAffectedRows);

            // 同步读回：逐字段核对，并且第 1 行的备注从「有值」变成了 NULL
            OrmQuery<IntegrationAsyncRow> readQuery(*pool);
            const std::vector<IntegrationAsyncRow> rows = readQuery.orderBy(asc("id")).toList();
            ASSERT_EQ(rows.size(), 2U);
            EXPECT_EQ(rows[0].id, 1);
            EXPECT_EQ(rows[0].name, "异步改后");
            EXPECT_FALSE(rows[0].note.has_value());
            EXPECT_EQ(rows[1].name, "同步改后");
            ASSERT_TRUE(rows[1].note.has_value());
            EXPECT_EQ(rows[1].note.value(), "同步备注");
        }

        // ---- executeNonQueryAsync 与 executeNonQuery：同一张表上按条件删除，行数对照 ----
        {
            OrmQuery<IntegrationAsyncRow> asyncDeleteQuery(*pool);
            const TestSupport::CompletedTask<std::int64_t> deleted = loopRunner.runToCompletion(
                asyncDeleteQuery.where(Column(&IntegrationAsyncRow::id, "id") == std::int64_t{1})
                                .executeNonQueryAsync(loopRunner.loop()));
            ASSERT_TRUE(deleted.finished);
            ASSERT_EQ(deleted.error, nullptr);
            ASSERT_TRUE(deleted.value.has_value());
            EXPECT_EQ(deleted.value.value(), 1);

            OrmQuery<IntegrationAsyncRow> syncDeleteQuery(*pool);
            const std::int64_t syncDeletedRows =
                syncDeleteQuery.where(Column(&IntegrationAsyncRow::id, "id") == std::int64_t{2}).executeNonQuery();
            EXPECT_EQ(deleted.value.value(), syncDeletedRows);

            // 两条删除各命中一行，表最终被清空（异步写路径同样真的落到了服务端）
            OrmQuery<IntegrationAsyncRow> remainingQuery(*pool);
            EXPECT_EQ(remainingQuery.count(), 0);
        }
    }

    /**
     * @brief 验证 insertBatchAsync 在真实服务端上的行数与逐行内容都与同步 insertBatch 一致
     *
     * @details 行数远低于 MySQL 的参数上限，走一次生成多行 VALUES 的单语句分支，不与 SQLite 端的分块用例重复。
     *          对照方式：异步批量写完同步读回全部行，清空表再交给同步 insertBatch 写同一份数据，
     *          两次读回必须逐字段相等，顺带钉住「多行 VALUES 在真实服务端可接受（空串与 NULL 并存）」。
     */
    TEST_F(MySqlIntegrationTest, AsyncBatchInsertWritesRowsEqualToSyncInsertBatch)
    {
        ASSERT_TRUE(prepareTable(kAsyncBatchTableName, kAsyncBatchColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool       = makePool(3);
        TestSupport::EventLoopThread   &loopRunner = asyncLoop();

        const std::vector<IntegrationAsyncBatchRow> batchRows{
            IntegrationAsyncBatchRow{.id = 1, .name = "异步批量甲", .note = std::string("有备注")},
            IntegrationAsyncBatchRow{.id = 2, .name = "异步批量乙", .note = std::nullopt},
            // 空串与 NULL 必须能被区分开：两者在服务端上是不同的取值
            IntegrationAsyncBatchRow{.id = 3, .name = "异步批量丙", .note = std::string("")}
        };

        OrmQuery<IntegrationAsyncBatchRow> asyncBatchQuery(*pool);
        const TestSupport::CompletedTask<std::int64_t> inserted =
            loopRunner.runToCompletion(asyncBatchQuery.insertBatchAsync(batchRows, loopRunner.loop()));

        ASSERT_TRUE(inserted.finished) << "异步批量插入未在时限内完成";
        ASSERT_EQ(inserted.error, nullptr);
        ASSERT_TRUE(inserted.value.has_value());
        EXPECT_EQ(inserted.value.value(), 3);

        OrmQuery<IntegrationAsyncBatchRow> asyncReadQuery(*pool);
        const std::vector<IntegrationAsyncBatchRow> asyncRows = asyncReadQuery.orderBy(asc("id")).toList();
        ASSERT_EQ(asyncRows.size(), batchRows.size());
        for (std::size_t index = 0; index < batchRows.size(); ++index)
        {
            EXPECT_EQ(asyncRows[index].id, batchRows[index].id);
            EXPECT_EQ(asyncRows[index].name, batchRows[index].name);
            EXPECT_EQ(asyncRows[index].note, batchRows[index].note);
        }

        // ---- 清空后交给同步 insertBatch 写同一份数据：行数与读回内容都必须一致 ----
        OrmQuery<IntegrationAsyncBatchRow> clearQuery(*pool);
        EXPECT_EQ(clearQuery.executeNonQuery(), static_cast<std::int64_t>(batchRows.size()));

        OrmQuery<IntegrationAsyncBatchRow> syncBatchQuery(*pool);
        EXPECT_EQ(syncBatchQuery.insertBatch(batchRows), inserted.value.value());

        OrmQuery<IntegrationAsyncBatchRow> syncReadQuery(*pool);
        const std::vector<IntegrationAsyncBatchRow> syncRows = syncReadQuery.orderBy(asc("id")).toList();
        ASSERT_EQ(syncRows.size(), asyncRows.size());
        for (std::size_t index = 0; index < syncRows.size(); ++index)
        {
            EXPECT_EQ(syncRows[index].id, asyncRows[index].id);
            EXPECT_EQ(syncRows[index].name, asyncRows[index].name);
            EXPECT_EQ(syncRows[index].note, asyncRows[index].note);
        }
    }

    /**
     * @brief 验证异步写落在不存在的表上时，异常按原类型与原中文消息从协程里穿出
     *
     * @details 与 SQLite 端的同名前缀用例互补：这里的失败来自真实服务端（表确实不存在），
     *          中间要经过「工作线程 → 调度投递 → 协程恢复」三跳，因此消息必须与同步版一字不差，
     *          否则就是把服务端的真实原因在某一层被包装或截断了。
     */
    TEST_F(MySqlIntegrationTest, AsyncWriteOnMissingTableSurfacesOriginalException)
    {
        // 本表刻意不创建：错误只能来自服务端的执行失败，而不是任何本地前置校验
        std::unique_ptr<ConnectionPool> pool       = makePool(2);
        TestSupport::EventLoopThread   &loopRunner = asyncLoop();

        // 同步版先把失败原因固定下来
        OrmQuery<IntegrationAsyncMissingRow> syncQuery(*pool);
        std::string syncMessage;
        try
        {
            static_cast<void>(syncQuery.insert(IntegrationAsyncMissingRow{.id = 1}));
            FAIL() << "表不存在时同步插入应当抛出 std::runtime_error";
        }
        catch (const std::runtime_error &exception)
        {
            syncMessage = exception.what();
        }
        EXPECT_NE(syncMessage.find("语句执行失败"), std::string::npos) << syncMessage;
        EXPECT_TRUE(containsLocalizedText(syncMessage)) << syncMessage;

        OrmQuery<IntegrationAsyncMissingRow> asyncQuery(*pool);
        const TestSupport::CompletedTask<std::int64_t> inserted = loopRunner.runToCompletion(
            asyncQuery.insertAsync(IntegrationAsyncMissingRow{.id = 1}, loopRunner.loop()));

        ASSERT_TRUE(inserted.finished) << "写失败也必须完成（否则协程会被永久挂起）";
        ASSERT_NE(inserted.error, nullptr);
        EXPECT_FALSE(inserted.value.has_value());

        try
        {
            // rethrow_exception 是 [[noreturn]]：后面写 FAIL() 只会被判成不可达代码（/W4 C4702）。
            // 若抛出的类型与下面的 catch 不符，异常会继续外传，gtest 同样把这条测试判失败
            std::rethrow_exception(inserted.error);
        }
        catch (const std::runtime_error &exception)
        {
            // 类型与消息都与同步版相同：异常原样穿过工作线程与调度投递，没有被包装或降级
            EXPECT_EQ(std::string(exception.what()), syncMessage);
        }
    }

    // ========================================================================
    // 事务
    // ========================================================================

    /**
     * @brief 验证事务提交的行对另一条连接可见，提交前不可见
     */
    TEST_F(MySqlIntegrationTest, TransactionCommitIsVisibleToAnotherConnection)
    {
        ASSERT_TRUE(prepareTable(kTransactionCommitTableName, kTransactionColumns)) << m_lastSetupError;

        // 观察者是一条完全独立的连接：自动提交下每条 SELECT 各自取快照，
        // 因此「提交前看不见 / 提交后看得见」是确定性结论，不需要任何等待
        std::unique_ptr<MySqlConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionCommitTableName), 0);

        std::unique_ptr<ConnectionPool> pool = makePool(3);
        {
            Transaction transaction(*pool);
            EXPECT_TRUE(transaction.isActive());

            // 走事务连接写入：Queryable(Transaction&) 保证语句与 START TRANSACTION 落在同一条连接上，
            // 否则写语句会运行在自动提交模式，回滚只能回滚一个空事务
            OrmQuery<IntegrationTransactionRow> transactionalQuery(transaction);
            EXPECT_EQ(1, transactionalQuery.insert(IntegrationTransactionRow{.id = 1, .name = "事务提交", .amount = 12.5}));

            // 提交之前：写者自己的连接能读到，另一条连接读不到
            const std::optional<IntegrationTransactionRow> uncommittedRow =
                transactionalQuery.where(Column(&IntegrationTransactionRow::id, "id") == std::int64_t{1}).first();
            ASSERT_TRUE(uncommittedRow.has_value());
            EXPECT_EQ(uncommittedRow->name, "事务提交");
            EXPECT_EQ(countRows(*observer, kTransactionCommitTableName), 0);

            ASSERT_TRUE(transaction.commit()) << transaction.lastError();
            EXPECT_FALSE(transaction.isActive());
        }

        // 提交之后：另一条连接立刻可见，且字段值完整
        EXPECT_EQ(countRows(*observer, kTransactionCommitTableName), 1);
        const std::optional<std::string> committedName =
            readTransactionRowName(*observer, kTransactionCommitTableName, 1);
        ASSERT_TRUE(committedName.has_value());
        EXPECT_EQ(committedName.value(), "事务提交");
    }

    /**
     * @brief 验证事务回滚后任何连接都看不到那一行
     */
    TEST_F(MySqlIntegrationTest, TransactionRollbackLeavesNoRowBehind)
    {
        ASSERT_TRUE(prepareTable(kTransactionRollbackTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);
        {
            Transaction transaction(*pool);

            ASSERT_TRUE(insertTransactionRow(transaction.connection(), kTransactionRollbackTableName, 1, "事务回滚", 3.5))
                << transaction.connection().lastError();
            // 事务内可见：证明这一行确实被写进去过，回滚要撤销的是真实存在的数据
            EXPECT_EQ(countRows(transaction.connection(), kTransactionRollbackTableName), 1);

            ASSERT_TRUE(transaction.rollback()) << transaction.lastError();
            EXPECT_FALSE(transaction.isActive());
            // 回滚是幂等的：事务已结束时再提交/回滚都是空操作，不会误发 COMMIT 把数据救回来
            EXPECT_TRUE(transaction.commit());
            EXPECT_TRUE(transaction.rollback());
        }

        // 换一条全新的连接复核：库表里没有任何残留
        std::unique_ptr<MySqlConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionRollbackTableName), 0);
    }

    /**
     * @brief 验证归还连接池时（resetSessionState）把未提交的事务滚掉
     * @details 残留的事务会跟着连接串给下一个借用者：对方的语句悄悄并进上一笔事务，
     *          行锁一直被握到事务结束为止
     */
    TEST_F(MySqlIntegrationTest, ResetSessionStateRollsBackUncommittedTransaction)
    {
        ASSERT_TRUE(prepareTable(kTransactionResetTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<MySqlConnection> connection = makeConnection();
        ASSERT_TRUE(connection->connect()) << connection->lastError();

        ASSERT_TRUE(connection->beginTransaction()) << connection->lastError();
        ASSERT_TRUE(insertTransactionRow(*connection, kTransactionResetTableName, 1, "会话复位", 1.5))
                << connection->lastError();
        // 事务内可见：证明这一行确实被写进去过，复位要撤销的是真实存在的数据
        ASSERT_EQ(countRows(*connection, kTransactionResetTableName), 1);

        connection->resetSessionState();

        // 事务被滚掉：未提交的行随之消失，也就是没有串给下一个借用者
        EXPECT_EQ(countRows(*connection, kTransactionResetTableName), 0) << "归还时没有滚掉未提交的事务";
        // 幂等：没有活动事务时再调一次什么都不做
        EXPECT_NO_THROW(connection->resetSessionState());
    }

    /**
     * @brief 验证事务对象析构（未提交）时自动回滚
     */
    TEST_F(MySqlIntegrationTest, TransactionDestructorRollsBackUncommittedRows)
    {
        ASSERT_TRUE(prepareTable(kTransactionDestructorTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);
        {
            Transaction transaction(*pool);
            ASSERT_TRUE(insertTransactionRow(transaction.connection(), kTransactionDestructorTableName, 1, "析构未提交", 1.0))
                << transaction.connection().lastError();
            // 事务内可见，说明行已写入、只是没提交
            EXPECT_EQ(countRows(transaction.connection(), kTransactionDestructorTableName), 1);
            EXPECT_TRUE(transaction.isActive());

            // 刻意不调用 commit()/rollback()：交给析构函数收尾
        }

        std::unique_ptr<MySqlConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionDestructorTableName), 0);
    }

    /**
     * @brief 验证异常穿越事务作用域后，只留下事务之前已提交的数据
     */
    TEST_F(MySqlIntegrationTest, ExceptionUnwindingKeepsOnlyPreviouslyCommittedRows)
    {
        ASSERT_TRUE(prepareTable(kTransactionExceptionTableName, kTransactionColumns)) << m_lastSetupError;

        std::unique_ptr<ConnectionPool> pool = makePool(3);

        // 第一步：先提交一行，作为「事务之前已落库」的既有数据
        {
            Transaction committedTransaction(*pool);
            ASSERT_TRUE(insertTransactionRow(committedTransaction.connection(), kTransactionExceptionTableName, 1,
                                             "已提交", 1.0))
                << committedTransaction.connection().lastError();
            ASSERT_TRUE(committedTransaction.commit()) << committedTransaction.lastError();
        }

        // 第二步：事务中途抛异常，异常直接穿越作用域，显式 rollback() 来不及执行
        try
        {
            Transaction failingTransaction(*pool);
            ASSERT_TRUE(insertTransactionRow(failingTransaction.connection(), kTransactionExceptionTableName, 2,
                                             "异常未提交", 2.0))
                << failingTransaction.connection().lastError();

            throw std::runtime_error("模拟业务异常：事务应当被析构函数回滚");
        }
        catch (const std::runtime_error &)
        {
            // 异常已被事务对象在栈展开时处理（析构补一次 ROLLBACK），此处只负责继续断言
        }

        // 只剩第一步提交的那一行，且残留的确实是 id = 1
        std::unique_ptr<MySqlConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionExceptionTableName), 1);
        EXPECT_EQ(readTransactionRowName(*observer, kTransactionExceptionTableName, 1).value_or(""), "已提交");
        EXPECT_FALSE(readTransactionRowName(*observer, kTransactionExceptionTableName, 2).has_value());
    }

    /**
     * @brief 钉住表存在性查询只认基表：同名视图不能被算成「表已存在」
     * @details 视图与表在 MySQL 里共用一个名字空间。视图被算成表时，建表被当成多余而跳过，
     *          随后按表写数据的语句落在视图上，只换来一句「目标表不可插入」——问题要到第一次写入才暴露。
     * @note 对象名带进程级随机后缀并自建自清：并行执行时别的用例/进程不会撞上这个名字
     */
    TEST_F(MySqlIntegrationTest, TableExistsIgnoresViewsAndStillSeesBaseTables)
    {
        const std::string objectName = "asyngyanis_itg_view_" + std::to_string(std::random_device{}());
        const MySqlDialect dialect;

        std::unique_ptr<ConnectionPool> pool       = makePool(1);
        PooledConnection                connection = pool->acquire();
        ASSERT_TRUE(connection);

        // 计数只读元数据，不碰任何业务表：走方言自己给的语句与参数，判据与 SchemaMigrator 一致
        const auto countMetadataMatches = [&connection, &dialect](const std::string &name) -> std::int64_t
        {
            const SqlStatement            probeStatement = dialect.tableExistsStatement(name);
            const std::unique_ptr<DatabaseResult> result =
                connection->execute(std::string_view{probeStatement.sql}, probeStatement.parameters);
            EXPECT_NE(result, nullptr) << connection->lastError();
            if (result == nullptr || !result->next())
            {
                return -1;
            }
            return std::get<std::int64_t>(result->getValue(0));
        };

        ASSERT_TRUE(connection->execute("CREATE VIEW " + objectName + " AS SELECT 1 AS one") != nullptr)
            << connection->lastError();
        EXPECT_EQ(countMetadataMatches(objectName), 0) << "视图被算成了基表：建表会被跳过，写入却落在视图上";

        // 换名前的视图必须先撤掉才能建同名基表（名字空间共用），建好之后计数必须是 1
        ASSERT_TRUE(connection->execute("DROP VIEW " + objectName) != nullptr) << connection->lastError();
        ASSERT_TRUE(connection->execute("CREATE TABLE " + objectName + " (id INT PRIMARY KEY)") != nullptr)
            << connection->lastError();
        EXPECT_EQ(countMetadataMatches(objectName), 1) << "table_type 过滤把基表也一起排除了";

        EXPECT_TRUE(connection->execute("DROP TABLE " + objectName) != nullptr) << connection->lastError();
    }

    /**
     * @brief 钉住真机 BIT 列在两条读取路径上都按整数交出，而不是原样控制字节
     * @details 线格式实测为 ⌈M/8⌉ 字节的大端无符号整数（BIT(8) 的 200 就是单字节 0xC8），
     *          文本协议与 mysql_stmt_* 二进制协议都过同一份列转换，因此两条路都要钉：
     *          只测一条时，另一条的取值缓冲形状不同（二进制协议按长度前缀读），漏改不会被发现。
     * @note 表名带进程级随机后缀并自建自清，并行执行时不会与其它用例撞名
     */
    TEST_F(MySqlIntegrationTest, BitColumnsAreReadAsIntegersOnBothProtocolPaths)
    {
        const std::string tableName = "asyngyanis_itg_bit_" + std::to_string(std::random_device{}());

        std::unique_ptr<ConnectionPool> pool       = makePool(1);
        PooledConnection                connection = pool->acquire();
        ASSERT_TRUE(connection);

        ASSERT_TRUE(connection->execute("CREATE TABLE " + tableName
                                        + " (id INT PRIMARY KEY, flags BIT(8), one BIT(1), wide BIT(64))")
                    != nullptr)
            << connection->lastError();
        ASSERT_TRUE(connection->execute("INSERT INTO " + tableName
                                        + " VALUES (1, b'11001000', b'0', b'1111111111111111111111111111111111111111111111111111111111111111')")
                    != nullptr)
            << connection->lastError();

        // ---- 文本协议（mysql_store_result）----
        const std::unique_ptr<DatabaseResult> textResult = connection->execute("SELECT flags, one, wide FROM " + tableName);
        ASSERT_TRUE(textResult != nullptr) << connection->lastError();
        ASSERT_TRUE(textResult->next());
        EXPECT_EQ(std::get<std::int64_t>(textResult->getValue(0)), 200);
        EXPECT_EQ(std::get<std::int64_t>(textResult->getValue(1)), 0) << "清位的 BIT(1) 不能读成长度 1 的 NUL 文本";
        // BIT(64) 的全 1 装不进 int64：按十进制文本交出，与 BIGINT UNSIGNED 同一条约定
        EXPECT_EQ(std::get<std::string>(textResult->getValue(2)), "18446744073709551615");

        // ---- 二进制协议（mysql_stmt_* 预处理语句）----
        const std::unique_ptr<DatabaseResult> preparedResult =
            connection->execute("SELECT flags, one, wide FROM " + tableName + " WHERE id = ?",
                                std::vector<DatabaseValue>{std::int64_t{1}});
        ASSERT_TRUE(preparedResult != nullptr) << connection->lastError();
        ASSERT_TRUE(preparedResult->next());
        EXPECT_EQ(std::get<std::int64_t>(preparedResult->getValue(0)), 200);
        EXPECT_EQ(std::get<std::int64_t>(preparedResult->getValue(1)), 0);
        EXPECT_EQ(std::get<std::string>(preparedResult->getValue(2)), "18446744073709551615");

        EXPECT_TRUE(connection->execute("DROP TABLE " + tableName) != nullptr) << connection->lastError();
    }

    /**
     * @brief 验证自增标识挂在写回执上，两条协议路径同口径，且不会把上一条的值冒充过来
     */
    TEST_F(MySqlIntegrationTest, GeneratedIdRidesOnTheWriteReceiptOfBothProtocolPaths)
    {
        ASSERT_TRUE(prepareTable(kAutoIncrementTableName, kAutoIncrementColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const std::string insertPrefix = "INSERT INTO " + quote(kAutoIncrementTableName) + " (`name`) VALUES (";

        // ---- 预处理协议：只给 name，id 由服务端生成 ----
        const std::vector<DatabaseValue> parameters{std::string("甲")};
        const std::unique_ptr<DatabaseResult> firstReceipt = connection.execute(insertPrefix + "?)", parameters);
        ASSERT_NE(firstReceipt, nullptr) << connection.lastError();
        EXPECT_EQ(firstReceipt->affectedRowCount(), 1);
        // 刻意通过基类引用取值：这条通道对全部驱动开放，调用方不必向下转型到 MySqlResult
        const DatabaseResult &firstBase = *firstReceipt;
        EXPECT_EQ(firstBase.lastInsertRowId(), 1);

        // ---- 文本协议：同一条语句形状，走的是 mysql_insert_id 那一格 ----
        const std::unique_ptr<DatabaseResult> secondReceipt = connection.execute(insertPrefix + "'乙')");
        ASSERT_NE(secondReceipt, nullptr) << connection.lastError();
        EXPECT_EQ(secondReceipt->lastInsertRowId(), 2) << "两条协议路径的自增标识口径不一致";

        // 标识要指向真那一行：只比计数器查不出「计数器对、行没写进去」
        const std::unique_ptr<DatabaseResult> readBack = connection.execute(
            "SELECT `name` FROM " + quote(kAutoIncrementTableName) + " WHERE `id` = 2");
        ASSERT_NE(readBack, nullptr) << connection.lastError();
        ASSERT_TRUE(readBack->next());
        EXPECT_EQ(std::get<std::string>(readBack->getValue(std::size_t{0})), "乙");

        // 非插入的写语句要回 0：服务端每条 OK 包都带这个字段，未生成时给的是 0，
        // 而不是上一条 INSERT 的值——把残值交给调用方是最容易被当成主键用的那类错
        const std::unique_ptr<DatabaseResult> updateReceipt = connection.execute(
            "UPDATE " + quote(kAutoIncrementTableName) + " SET `name` = '丙' WHERE `id` = 1");
        ASSERT_NE(updateReceipt, nullptr) << connection.lastError();
        EXPECT_EQ(updateReceipt->affectedRowCount(), 1);
        EXPECT_EQ(updateReceipt->lastInsertRowId(), 0) << "写回执把上一条插入的自增标识冒充成了本条的结果";

        // 只读结果集同样不给值（构造时按约定传 0）
        const std::unique_ptr<DatabaseResult> selection = connection.execute("SELECT `id` FROM " + quote(kAutoIncrementTableName));
        ASSERT_NE(selection, nullptr) << connection.lastError();
        EXPECT_EQ(selection->lastInsertRowId(), 0);
    }

    /**
     * @brief 验证没有自增列的表上插入不给出自增标识，也不给上一条插入的残值
     */
    TEST_F(MySqlIntegrationTest, InsertIntoTableWithoutAutoIncrementGivesNoGeneratedId)
    {
        ASSERT_TRUE(prepareTable(kPlainKeyTableName, kPlainKeyColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const std::string insertStatement = "INSERT INTO " + quote(kPlainKeyTableName) + " (`id`, `name`) VALUES (?, ?)";
        for (const std::int64_t id: {std::int64_t{11}, std::int64_t{12}})
        {
            const std::vector<DatabaseValue> parameters{id, std::string("手填主键")};
            const std::unique_ptr<DatabaseResult> receipt = connection.execute(insertStatement, parameters);
            ASSERT_NE(receipt, nullptr) << connection.lastError();
            // 主键由调用方给定，服务端没有「生成」任何东西；显式给的值不该被当成生成结果
            EXPECT_EQ(receipt->affectedRowCount(), 1);
            EXPECT_EQ(receipt->lastInsertRowId(), 0) << "表上没有自增列，回执却给出了一个标识";
        }
    }

    /**
     * @brief 验证自增标识宽不进有符号 64 位时如实报 0 并写明原因，而不是回绕成负数
     */
    TEST_F(MySqlIntegrationTest, GeneratedIdBeyondSignedSixtyFourBitsReportsZeroWithAReason)
    {
        ASSERT_TRUE(prepareTable(kWideAutoIncrementTableName, kWideAutoIncrementColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 把自增起点播种到 2^63：下一条 INSERT 生成的值就落在 int64 能表达的最大值之外一格
        const std::string seedStatement = "ALTER TABLE " + quote(kWideAutoIncrementTableName) + " AUTO_INCREMENT = " +
                                          std::to_string(kWideAutoIncrementSeed);
        ASSERT_NE(connection.execute(seedStatement), nullptr) << connection.lastError();

        const std::vector<DatabaseValue> parameters{std::string("超宽")};
        const std::unique_ptr<DatabaseResult> receipt = connection.execute(
            "INSERT INTO " + quote(kWideAutoIncrementTableName) + " (`name`) VALUES (?)", parameters);
        ASSERT_NE(receipt, nullptr) << connection.lastError();

        // 行确实写进去了（起点被服务端接受），否则「报 0」可能只是插入没成功的假证据
        EXPECT_EQ(countRows(connection, kWideAutoIncrementTableName), 1);

        // 交不出这个值就如实说交不出：0 + lastError() 写明原因，比补码回绕成一个看似合理的负数安全
        EXPECT_EQ(receipt->lastInsertRowId(), 0);
        EXPECT_NE(receipt->lastError().find("超出有符号 64 位"), std::string::npos) << receipt->lastError();
    }

    /**
     * @brief 钉住「匹配到但没改动任何值」的 UPDATE 在本驱动上算几行（MySQL 报的是改动数）
     *
     * @details 这条不是在测驱动的实现细节，而是在测调用方能不能靠返回值区分「行不存在」与
     *          「行存在但值没变」：两种情况下本驱动都给 0，而 SQLite 给 1。ORM 的 update()
     *          返回受影响行数，调用方若按「0 就是没找到」来分支，换到 MySQL 上就会误判。
     *          旧行为没被测过，也没写进任何文档——这里把实测口径钉住并供文档引用。
     */
    TEST_F(MySqlIntegrationTest, UpdateThatChangesNoValueReportsZeroAffectedRows)
    {
        ASSERT_TRUE(prepareTable(kNoOpUpdateTableName, kAutoIncrementColumns)) << m_lastSetupError;

        MySqlConnection connection(configuration());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const std::vector<DatabaseValue> insertParameters{std::string("原值")};
        const std::unique_ptr<DatabaseResult> insertReceipt = connection.execute(
            "INSERT INTO " + quote(kNoOpUpdateTableName) + " (`name`) VALUES (?)", insertParameters);
        ASSERT_NE(insertReceipt, nullptr) << connection.lastError();
        const std::int64_t targetId = insertReceipt->lastInsertRowId();
        ASSERT_GT(targetId, 0);

        // 同样的值再写一次：行被匹配到了，但没有一格的值发生变化
        const std::vector<DatabaseValue> sameValueParameters{std::string("原值"), targetId};
        const std::unique_ptr<DatabaseResult> noOpPrepared = connection.execute(
            "UPDATE " + quote(kNoOpUpdateTableName) + " SET `name` = ? WHERE `id` = ?", sameValueParameters);
        ASSERT_NE(noOpPrepared, nullptr) << connection.lastError();

        const std::unique_ptr<DatabaseResult> noOpText = connection.execute(
            "UPDATE " + quote(kNoOpUpdateTableName) + " SET `name` = '原值' WHERE `id` = " + std::to_string(targetId));
        ASSERT_NE(noOpText, nullptr) << connection.lastError();

        // 两条协议路径必须同口径，否则调用方换个入口就会拿到不同的数
        EXPECT_EQ(noOpPrepared->affectedRowCount(), 0) << "预处理路径给的是匹配数而不是改动数";
        EXPECT_EQ(noOpText->affectedRowCount(), 0) << "文本路径与预处理路径口径不一致";

        // 真改了值才算一行：排除「本驱动压根不统计影响行数」这种误读
        const std::vector<DatabaseValue> changedParameters{std::string("新值"), targetId};
        const std::unique_ptr<DatabaseResult> changedReceipt = connection.execute(
            "UPDATE " + quote(kNoOpUpdateTableName) + " SET `name` = ? WHERE `id` = ?", changedParameters);
        ASSERT_NE(changedReceipt, nullptr) << connection.lastError();
        EXPECT_EQ(changedReceipt->affectedRowCount(), 1);

        // 行不存在同样是 0：与上面那一格无法区分，这正是需要在文档里写清的点
        const std::vector<DatabaseValue> missingParameters{std::string("新值"), targetId + 1000};
        const std::unique_ptr<DatabaseResult> missingReceipt = connection.execute(
            "UPDATE " + quote(kNoOpUpdateTableName) + " SET `name` = ? WHERE `id` = ?", missingParameters);
        ASSERT_NE(missingReceipt, nullptr) << connection.lastError();
        EXPECT_EQ(missingReceipt->affectedRowCount(), 0);
    }

    /**
     * @brief 自增主键行：id 由 InnoDB 生成，声明里把主键标成自增
     */
    struct IntegrationTicketRow
    {
        std::int64_t id   = 0;  ///< 数据库生成的自增主键
        std::string  name = ""; ///< 名称列
    };

    template<>
    struct Queryable::TableSchema<IntegrationTicketRow>
    {
        static constexpr std::string_view kTableName = kOrmAutoIncrementTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationTicketRow::id, "id"),
            Column(&IntegrationTicketRow::name, "name"),
        };
        static constexpr std::string_view kPrimaryKey                = "id";
        static constexpr bool             kIsAutoIncrementPrimaryKey = true;
    };

    /**
     * @brief 验证自增主键在真实 MySQL 上端到端成立：方言生成的 DDL 被服务端接受，省略主键的写入真生成标识
     *
     * @details 离线断言只能证明 DDL 文本长什么样，本用例证明 InnoDB 认它，并且单条与批量两条写入路径
     *          都没有把主键列写进 INSERT（写了就会存下那个显式值，读回来的 id 不会连着排成 1..5）。
     */
    TEST_F(MySqlIntegrationTest, OrmAutoIncrementPrimaryKeyWorksOnRealServer)
    {
        // 先登记清理：中途任何断言失败，TearDown 都会把这张表删掉，不留残留
        m_preparedTableName = std::string(kOrmAutoIncrementTableName);

        std::unique_ptr<ConnectionPool> pool = makePool(2);
        std::string                     errorText;

        // 上次崩溃可能留下的表先清掉，保证自增计数从 1 起
        ASSERT_TRUE(SchemaMigrator::dropTable<IntegrationTicketRow>(*pool, true, &errorText)) << errorText;
        ASSERT_TRUE(SchemaMigrator::createTable<IntegrationTicketRow>(*pool, false, &errorText)) << errorText;

        OrmQuery<IntegrationTicketRow> query(*pool);
        // 结构体里刻意填了主键值：声明为自增之后它必须被忽略
        EXPECT_EQ(query.insertAndGetGeneratedId(IntegrationTicketRow{999, "第一张"}), 1)
                << "自增主键仍被写进 INSERT：显式值占了号段";
        EXPECT_EQ(query.insertAndGetGeneratedId(IntegrationTicketRow{0, "第二张"}), 2);

        // 批量路径同样省略主键：三行的标识必须接着往下走
        const std::vector<IntegrationTicketRow> batchRows{
            IntegrationTicketRow{0, "第三张"}, IntegrationTicketRow{0, "第四张"}, IntegrationTicketRow{0, "第五张"}};
        ASSERT_EQ(query.insertBatch(batchRows), 3);

        const std::vector<IntegrationTicketRow> rows = query.orderBy(asc("id")).toList();
        ASSERT_EQ(rows.size(), 5U);
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            EXPECT_EQ(rows[index].id, static_cast<std::int64_t>(index) + 1) << "第 " << index << " 行的主键不是生成的";
        }
        EXPECT_EQ(rows[0].name, "第一张");
        EXPECT_EQ(rows[4].name, "第五张");
    }

} // namespace AsynGyanis::Database
