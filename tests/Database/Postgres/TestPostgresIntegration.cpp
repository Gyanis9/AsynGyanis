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
 *          - Transaction 的提交可见、回滚不可见、析构自动回滚。
 *
 * ## 口令绝不进仓库（本文件的第一条硬规矩）
 * 连接参数一律从环境变量读取，本文件不出现任何明文口令：
 * - ASYN_POSTGRES_TEST_HOST      主机，默认 127.0.0.1
 * - ASYN_POSTGRES_TEST_PORT      端口，默认 5432
 * - ASYN_POSTGRES_TEST_USER      用户名，默认 postgres
 * - ASYN_POSTGRES_TEST_PASSWORD  口令，**没有默认值**；未设置时全部用例 GTEST_SKIP（不是失败）
 * - ASYN_POSTGRES_TEST_DATABASE  库名，默认 postgres（初始化集群时必然存在的维护库）
 * 口令缺失即跳过，因此无服务端的 CI 与本地日常构建同样保持全绿；未编译 PostgreSQL 驱动的桩
 * 构建也走跳过路径（见 kPostgresDriverCompiled）。
 *
 * ## 为什么不新建测试专用库
 * MySQL 侧可以随手 CREATE DATABASE，PostgreSQL 不行：CREATE DATABASE 不能在事务块里执行，
 * 且普通角色未必有建库权限。默认库取初始化集群时必然存在的 postgres，用例只操作**自己建的表**：
 * 每个用例使用独立表名，建表前先 DROP TABLE IF EXISTS，收尾在 TearDown 里再 DROP TABLE IF EXISTS，
 * 跑完不留任何残留。表名固定而非随机，是因为 TableSchema<T>::kTableName 必须是编译期常量；
 * 用例之间不共用表名，因此并行运行（ctest -j）也不会互相干扰。
 *
 * ## 与 MySQL 集成测试的两处有意差异
 * - **不做内嵌 '\0' 的往返断言**：PostgreSQL 的文本类型不允许出现零字节
 *   （服务端会以 "invalid byte sequence for encoding UTF8: 0x00" 整条拒绝），
 *   把 0 字节写进 TEXT 列不是驱动能做到的事。要承载任意二进制得改用 BYTEA，而 BYTEA 的
 *   文本协议输入需要十六进制或转义编码（当前驱动不做这层编码），因此这里如实不测；
 *   「按指针 + 长度取字节、不依赖零终止符」这件事由不受该限制的取值路径（长文本逐字节相等）
 *   与离线侧的空串/NULL 判定覆盖。
 * - **不写死 sleep**：与 MySQL 侧一致，需要「另一条连接看到什么」的地方一律新建一条连接后
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

#include <gtest/gtest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
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

        constexpr const char *kDefaultHost         = "127.0.0.1";
        constexpr std::uint16_t kDefaultPort       = 5432U;
        constexpr const char *kDefaultUserName     = "postgres";
        constexpr const char *kDefaultDatabaseName = "postgres";

        // ------------------------------------------------------------------------
        // 测试用表：每个用例一张独立表，互不共用
        // ------------------------------------------------------------------------

        /// 参数化插入用例的表
        constexpr std::string_view kParameterInsertTableName = "Asyn_Pg_ParamInsert";
        constexpr std::string_view kParameterInsertColumns =
            "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"amount\" DOUBLE PRECISION NOT NULL";

        /// 参数化取值往返用例的表（含 NULL 列、空串列与长文本列）
        constexpr std::string_view kParameterSelectTableName = "Asyn_Pg_ParamSelect";
        constexpr std::string_view kParameterSelectColumns =
            "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"amount\" DOUBLE PRECISION NOT NULL, "
            "\"note\" TEXT NULL, \"payload\" TEXT NULL";

        /// 注入证明用例的表
        constexpr std::string_view kInjectionTableName = "Asyn_Pg_Injection";
        constexpr std::string_view kInjectionColumns =
            "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL";

        /// 事务用例的三张表（提交可见 / 回滚不可见 / 析构自动回滚）
        constexpr std::string_view kTransactionCommitTableName     = "Asyn_Pg_Tx_Commit";
        constexpr std::string_view kTransactionRollbackTableName   = "Asyn_Pg_Tx_Rollback";
        constexpr std::string_view kTransactionDestructorTableName = "Asyn_Pg_Tx_Destructor";
        constexpr std::string_view kTransactionColumns =
            "\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL, \"amount\" DOUBLE PRECISION NOT NULL";

        /// 建表迁移用例的表：由 SchemaMigrator 生成 DDL，表名必须是编译期常量（见下面的 TableSchema 特化）
        constexpr std::string_view kMigratedTableName = "Asyn_Pg_Migrated";

        /// NUMERIC(20) 专用表：单独一个用例，验证无符号 64 位的承载类型被服务端接受
        constexpr std::string_view kNumericTableName = "Asyn_Pg_Numeric";

        /**
         * @brief 读取一个环境变量
         * @details 口令只经由本函数进入测试，源码里不出现任何明文。std::getenv 返回的指针
         *          在下一次改动环境前有效，这里立即拷贝成 std::string，不保留该指针。
         * @param variableName 环境变量名
         * @return std::string 变量值；未设置时为空串
         */
        [[nodiscard]] std::string readEnvironmentText(const char *variableName)
        {
            const char *rawValue = std::getenv(variableName);
            return rawValue != nullptr ? std::string(rawValue) : std::string{};
        }

        /**
         * @brief 读取文本型环境变量并在缺失时回落到默认值
         * @param variableName 环境变量名
         * @param fallback 未设置（或为空串）时使用的默认值
         * @return std::string 生效取值
         */
        [[nodiscard]] std::string readEnvironmentTextOrDefault(const char *variableName, const std::string_view fallback)
        {
            std::string variableValue = readEnvironmentText(variableName);
            // 空串与「未设置」在这里等价：两种情况都使用默认值，避免拼出一个空主机名
            return variableValue.empty() ? std::string(fallback) : variableValue;
        }

        /**
         * @brief 读取端口型环境变量
         * @details 用 std::from_chars 而不是 std::stoi：后者靠异常报错且接受 "5432abc" 这类
         *          带余文的输入。非法取值一律回落默认端口，不因为环境写错就让整组用例失败。
         * @param variableName 环境变量名
         * @param fallback 未设置或取值非法时使用的默认端口
         * @return std::uint16_t 生效端口
         */
        [[nodiscard]] std::uint16_t readEnvironmentPortOrDefault(const char *variableName, const std::uint16_t fallback)
        {
            const std::string portText = readEnvironmentText(variableName);
            if (portText.empty())
            {
                return fallback;
            }

            int parsedPort = 0;
            const auto [remainderBegin, parseError] =
                std::from_chars(portText.data(), portText.data() + portText.size(), parsedPort);

            // 三种非法情形一律回落：解析失败、尾部有余文、超出 1..65535 的端口范围
            if (parseError != std::errc{} || remainderBegin != portText.data() + portText.size() ||
                parsedPort <= 0 || parsedPort > 65535)
            {
                return fallback;
            }

            return static_cast<std::uint16_t>(parsedPort);
        }

        /**
         * @brief 判断文本是否含非 ASCII 字节，用作「面向使用者的中文文案」的稳定判据
         * @param text 待判定的文本
         * @return true 至少有一个字节的最高位被置起（UTF-8 多字节序列的特征）
         */
        [[nodiscard]] bool containsLocalizedText(const std::string &text)
        {
            for (const char character: text)
            {
                if (static_cast<unsigned char>(character) >= 0x80U)
                {
                    return true;
                }
            }
            return false;
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
        struct IntegrationMigratedRow
        {
            std::int64_t               id;      ///< 主键（BIGINT NOT NULL PRIMARY KEY）
            std::string                name;    ///< 户名（TEXT NOT NULL）
            std::optional<std::string> note;    ///< 备注（TEXT，可空）
            double                     balance; ///< 余额（DOUBLE PRECISION NOT NULL）
            bool                       active;  ///< 是否启用（BOOLEAN NOT NULL）
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

    } // namespace

    // ========================================================================
    // TableSchema 特化：把结构体注册到各自的表
    // ========================================================================

    template<>
    struct Queryable::TableSchema<IntegrationMigratedRow>
    {
        // 表名固定为 kMigratedTableName：SchemaMigrator 的表名只能来自编译期常量
        static constexpr std::string_view kTableName = kMigratedTableName;
        // 列名与成员一一对应，覆盖 SchemaMigrator 需要处理的全部类型映射分支
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationMigratedRow::id,      "id"),
            Column(&IntegrationMigratedRow::name,    "name"),
            Column(&IntegrationMigratedRow::note,    "note"),
            Column(&IntegrationMigratedRow::balance, "balance"),
            Column(&IntegrationMigratedRow::active,  "active"),
        };
        static constexpr std::string_view kPrimaryKey = "id";
    };

    template<>
    struct Queryable::TableSchema<IntegrationNumericRow>
    {
        static constexpr std::string_view kTableName = kNumericTableName;
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationNumericRow::id,       "id"),
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
        static constexpr auto kColumns = std::tuple{
            Column(&IntegrationTransactionRow::id,     "id"),
            Column(&IntegrationTransactionRow::name,   "name"),
            Column(&IntegrationTransactionRow::amount, "amount"),
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
                    GTEST_SKIP() << "未设置环境变量 " << kPasswordVariableName
                                 << "，跳过真实 PostgreSQL 集成测试（连接参数与环境变量名见文件头说明）";
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
                        std::unique_ptr<DatabaseConnection> connection =
                            DatabaseFactory::createPostgres(databaseConfiguration);
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
                const auto        *countedRows  = std::get_if<std::int64_t>(&countValue);
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
                    "INSERT INTO " + quote(tableName) + " (\"id\", \"name\", \"amount\") VALUES ($1, $2, $3)", parameters);

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
                    "SELECT \"name\" FROM " + quote(tableName) + " WHERE \"id\" = $1", parameters);
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
            ConnectionConfig              m_configuration;      ///< 从环境变量装载的连接配置
            std::vector<std::string>      m_preparedTableNames; ///< 本用例建好的表名，供 TearDown 清理
            std::string                   m_lastSetupError;     ///< 建表失败的原因，供断言输出

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

        const std::string insertStatement = "INSERT INTO " + quote(kParameterInsertTableName) +
                                            " (\"id\", \"name\", \"amount\") VALUES ($1, $2, $3)";
        const std::vector<DatabaseValue> parameters{std::int64_t{1}, std::string("张三"), 1234.5};

        const std::unique_ptr<DatabaseResult> insertResult = connection.execute(insertStatement, parameters);
        ASSERT_NE(insertResult, nullptr) << connection.lastError();
        // 写语句交出的是空回执：0 行 0 列，影响行数来自 PQcmdTuples 的语句级快照
        EXPECT_EQ(insertResult->affectedRowCount(), 1);
        EXPECT_EQ(insertResult->rowCount(), 0U);
        EXPECT_TRUE(insertResult->isEmpty());

        // 用不带参数的路径独立复核：参数确实按顺序绑到了对应的列上
        const std::unique_ptr<DatabaseResult> selectResult = connection.execute(
            "SELECT \"name\", \"amount\" FROM " + quote(kParameterInsertTableName) + " WHERE \"id\" = 1");
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

        const std::string insertStatement = "INSERT INTO " + quote(kParameterSelectTableName) +
                                            " (\"id\", \"name\", \"amount\", \"note\", \"payload\") VALUES ($1, $2, $3, $4, $5)";

        const std::vector<std::vector<DatabaseValue>> sampleRows{
            // 第 1 行：note 是 SQL NULL，payload 是空串——两者必须可区分
            {std::int64_t{1}, std::string("张三"), 1234.5, std::monostate{}, std::string("")},
            // 第 2 行：负数、单引号文本、中文备注
            {std::int64_t{2}, std::string("O'Brien"), -99.5, std::string("普通备注"), std::string("前前后后")},
            // 第 3 行：零值、空串备注、长文本
            {std::int64_t{3}, std::string("李四"), 0.0, std::string(""), longText}
        };

        for (const std::vector<DatabaseValue> &sampleRow: sampleRows)
        {
            const std::unique_ptr<DatabaseResult> insertResult = connection.execute(insertStatement, sampleRow);
            ASSERT_NE(insertResult, nullptr) << connection.lastError();
            EXPECT_EQ(insertResult->affectedRowCount(), 1);
        }

        const std::string selectStatement = "SELECT \"id\", \"name\", \"amount\", \"note\", \"payload\" FROM " +
                                            quote(kParameterSelectTableName) + " WHERE \"id\" = $1";
        const auto selectRowById = [&connection, &selectStatement](const std::int64_t identifier) -> std::unique_ptr<DatabaseResult>
        {
            return connection.execute(selectStatement, std::vector<DatabaseValue>{identifier});
        };

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

        const std::vector<DatabaseValue> insertParameters{std::int64_t{1}, hostileText};
        const std::unique_ptr<DatabaseResult> insertResult = connection.execute(
            "INSERT INTO " + quote(kInjectionTableName) + " (\"id\", \"name\") VALUES ($1, $2)", insertParameters);
        ASSERT_NE(insertResult, nullptr) << connection.lastError();
        ASSERT_EQ(insertResult->affectedRowCount(), 1);

        // 按值查询：WHERE 的取值同样走绑定，能精确命中说明存进去的就是原文
        const std::vector<DatabaseValue> selectParameters{hostileText};
        const std::unique_ptr<DatabaseResult> selectResult = connection.execute(
            "SELECT \"name\" FROM " + quote(kInjectionTableName) + " WHERE \"name\" = $1", selectParameters);
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
        const std::unique_ptr<DatabaseResult> tooFewResult =
            connection.execute("SELECT $1", std::span<const DatabaseValue>{});
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
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 1, .name = "张三", .note = std::string("首条"),
                                                                   .balance = 1234.5, .active = true}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 2, .name = "Li Si", .note = std::nullopt,
                                                                   .balance = -0.25, .active = false}));
            EXPECT_EQ(1, insertQuery.insert(IntegrationMigratedRow{.id = 3, .name = "王五", .note = std::string(""),
                                                                   .balance = 0.0, .active = true}));
        }

        // ---- 读回：五列逐一核对，证明 DDL 的类型与可空性恰好匹配 ORM 的映射规则 ----
        {
            OrmQuery<IntegrationMigratedRow> query(*pool);
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
            OrmQuery<IntegrationMigratedRow> query(*pool);
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

            const std::unique_ptr<DatabaseResult> result = connection.execute(
                "SELECT \"sequence\" FROM " + quote(kNumericTableName) + " WHERE \"id\" = $1",
                std::vector<DatabaseValue>{std::int64_t{1}});
            ASSERT_NE(result, nullptr) << connection.lastError();
            ASSERT_TRUE(result->next());

            const DatabaseValue sequenceValue = result->getValue(0);
            ASSERT_TRUE(std::holds_alternative<std::string>(sequenceValue))
                << "NUMERIC 应映射成精确十进制文本，实际类型为 " << databaseValueTypeName(sequenceValue)
                << "（值 " << (std::holds_alternative<std::int64_t>(sequenceValue)
                                  ? std::to_string(std::get<std::int64_t>(sequenceValue))
                                  : std::string("非整数备选")) << "）";
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
    TEST_F(PostgresIntegrationTest, TransactionRollbackLeavesNoRowBehind)
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
            ASSERT_TRUE(insertTransactionRow(transaction.connection(), kTransactionDestructorTableName, 1, "析构未提交", 1.0))
                << transaction.connection().lastError();
            // 事务内可见，说明行已写入、只是没提交
            EXPECT_EQ(countRows(transaction.connection(), kTransactionDestructorTableName), 1);
            EXPECT_TRUE(transaction.isActive());

            // 刻意不调用 commit()/rollback()：交给析构函数收尾
        }

        std::unique_ptr<PostgresConnection> observer = makeConnection();
        ASSERT_TRUE(observer->connect()) << observer->lastError();
        EXPECT_EQ(countRows(*observer, kTransactionDestructorTableName), 0);
    }

} // namespace AsynGyanis::Database
