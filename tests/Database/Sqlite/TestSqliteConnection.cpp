// SqliteConnection 单元测试：真实 SQLite 驱动的建库、单语句执行契约、事务与连接级计数器。
// SQLite 是进程内引擎，全部用例零外部服务（内存库 ":memory:"，文件库用 TestSupport::TemporaryDatabaseFile 的临时路径，
// 结束即连 -wal/-shm/-journal 残留一起删除）。
// 钉住的契约：execute() 一次只执行一条语句（分号后还有可执行语句就整次失败、一条都不执行）；queryTimeout() 走
// sqlite3_busy_timeout 并经 PRAGMA 直读验证；启动期两条 PRAGMA 失败不致命；刻意不测命令超 INT_MAX 与
// SQLITE_MISUSE（外部抢先关句柄）两条分支。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Sqlite/SqliteConnection.h"
#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace AsynGyanis::Database
{

    using TestSupport::containsLocalizedText;
    using TestSupport::asInteger;
    using TestSupport::asText;
    using TestSupport::executeRequired;
    namespace
    {
        /// 基类 DatabaseConnection 声明的单条命令执行超时默认毫秒数
        constexpr int kDefaultQueryTimeoutMilliseconds = 30000;

        /// 建表样板：一列主键 + 文本 + 整数 + 浮点 + 整型布尔位 + 二进制，覆盖全部映射分支
        constexpr const char *kCreateUsersTableSql =
                "CREATE TABLE users ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                " name TEXT NOT NULL,"
                " age INTEGER,"
                " score REAL,"
                " active INTEGER NOT NULL DEFAULT 1,"
                " payload BLOB)";

        /// 样本数据：三行的 rowid 依次为 1..3，第三条只写 name，其余列走默认值与 NULL
        constexpr const char *kInsertAliceSql =
                "INSERT INTO users (name, age, score, active, payload) VALUES ('Alice', 30, 95.5, 1, x'5c0041')";
        constexpr const char *kInsertBobSql =
                "INSERT INTO users (name, age, score, active) VALUES ('Bob', 25, 60.25, 0)";
        constexpr const char *kInsertCarolSql = "INSERT INTO users (name) VALUES ('Carol')";

        /**
         * @brief 判断文本里是否出现了指定子串
         * @param haystack 待检文本，实测里通常是中文错误说明
         * @param needle 关键子串，只取整句中稳定的那一段
         * @return true 命中
         */
        bool containsText(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /**
         * @brief 转出一份小写副本，供 PRAGMA 返回值做大小写无关比较
         * @details 日志模式名一类的取值由 SQLite 内部拼写，不同版本的大小写并不保证一致，
         *          比较前统一转小写，避免把用例的失败原因留给无关的大小写差异。
         * @param text 原始文本，按字节逐个转换（只对 ASCII 有效）
         * @return std::string 小写副本
         */
        std::string toLowerCopy(const std::string &text)
        {
            std::string lowered;
            lowered.reserve(text.size());
            for (const char character: text)
            {
                const bool isUpperAscii = character >= 'A' && character <= 'Z';
                lowered.push_back(isUpperAscii ? static_cast<char>(character - 'A' + 'a') : character);
            }
            return lowered;
        }

        /**
         * @brief 读取一个 PRAGMA 的标量返回值
         * @details 形如 PRAGMA busy_timeout / journal_mode 的单列单行结果，用例只关心那一个值。
         * @param connection 已连接的数据库连接
         * @param command PRAGMA 文本
         * @return std::optional<std::int64_t> 整数返回值；语句失败或无行时返回空值
         */
        std::optional<std::int64_t> readScalarInteger(DatabaseConnection &connection, const std::string_view command)
        {
            const std::unique_ptr<DatabaseResult> result = connection.execute(command);
            if (result == nullptr || !result->next())
            {
                return std::nullopt;
            }
            return asInteger(result->getValue(std::size_t{0}));
        }
    } // namespace

    // ============================================================================
    // 夹具
    // ============================================================================

    /**
     * @brief 内存库夹具：已连接、已建 users 表并写入三行样本数据
     *
     * @details 建表与插入的样板集中在 SetUp，用例只写与被测行为有关的那一条命令。
     *          三行的 rowid 依次是 1..3，凡涉及 rowid 与总行数的断言都以 3 为基准；
     *          内存库不落地，用例之间天然互不可见。
     */
    class SqliteConnectedMemoryDatabase : public ::testing::Test
    {
    protected:
        /**
         * @brief 连接内存库并灌入样本数据
         */
        void SetUp() override
        {
            m_connection = std::make_unique<SqliteConnection>(ConnectionConfig::sqliteDefault());
            ASSERT_TRUE(m_connection->connect()) << m_connection->lastError();

            constexpr std::array<const char *, 4> kSeedCommands = {
                    kCreateUsersTableSql, kInsertAliceSql, kInsertBobSql, kInsertCarolSql};
            for (const char *seedCommand: kSeedCommands)
            {
                const std::unique_ptr<DatabaseResult> seedResult = m_connection->execute(seedCommand);
                ASSERT_NE(seedResult, nullptr) << "样本数据初始化失败：" << seedCommand << "，原因：" << m_connection->lastError();
            }
        }

        /**
         * @brief 释放连接对象，其析构会无条件调用 disconnect 归还 sqlite3 句柄
         */
        void TearDown() override
        {
            m_connection.reset();
        }

        /**
         * @brief 取夹具持有的连接引用
         * @return SqliteConnection & 已完成初始化、可直接执行的连接
         */
        [[nodiscard]] SqliteConnection &connection() const
        {
            return *m_connection;
        }

        std::unique_ptr<SqliteConnection> m_connection; ///< 已连接并灌入样本数据的内存库连接
    };

    /**
     * @brief 临时文件库夹具：数据库文件由被测代码自己创建，析构时随 RAII 一起删除
     *
     * @details 成员声明顺序即析构顺序的反序：m_connection 后声明、先析构，
     *          连接句柄一定早于文件删除释放，Windows 上才不会因文件被占用而删不掉。
     */
    class SqliteTemporaryFileDatabase : public ::testing::Test
    {
    protected:
        /**
         * @brief 生成临时路径并连接文件库
         */
        void SetUp() override
        {
            m_configuration = ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path());
            m_connection    = std::make_unique<SqliteConnection>(m_configuration);
            ASSERT_TRUE(m_connection->connect()) << m_connection->lastError();
        }

        /**
         * @brief 先关连接再删文件
         */
        void TearDown() override
        {
            m_connection.reset();
        }

        /**
         * @brief 取夹具持有的连接引用
         * @return SqliteConnection & 已连接到临时文件的连接
         */
        [[nodiscard]] SqliteConnection &connection() const
        {
            return *m_connection;
        }

        /**
         * @brief 在临时文件上另开一个连接，供并发与落盘类用例使用
         * @return std::unique_ptr<SqliteConnection> 未连接的配置对等对象
         */
        [[nodiscard]] std::unique_ptr<SqliteConnection> openSecondConnection() const
        {
            return std::make_unique<SqliteConnection>(m_configuration);
        }

        TestSupport::TemporaryDatabaseFile m_databaseFile{"SqliteFile"}; ///< 临时数据库文件，负责清理残留
        ConnectionConfig                   m_configuration;              ///< 指向该文件的连接配置
        std::unique_ptr<SqliteConnection>  m_connection;                 ///< 夹具主连接
    };

    // ============================================================================
    // 未连接状态：构造即安全，各入口一致失败
    // ============================================================================

    /** @brief 钉住构造阶段不做任何 IO：无句柄、无错误文本，重复 disconnect 是安全空操作 */
    TEST(SqliteConnection, ConstructedConnectionHasNoHandleAndToleratesDisconnect)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // 构造阶段不做任何 IO：没有句柄、没有错误文本，重复 disconnect 是安全的空操作
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_TRUE(connection.lastError().empty());

        connection.disconnect();
        connection.disconnect();
        EXPECT_FALSE(connection.isConnected());
        EXPECT_TRUE(connection.lastError().empty()) << connection.lastError();
    }

    /** @brief 钉住未连接时 databaseType() 仍可读，可安全用于日志与分派 */
    TEST(SqliteConnection, DatabaseTypeIsSqliteWithoutConnection)
    {
        const SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // databaseType() 不查句柄，未连接时同样可用于日志与分派断言
        EXPECT_EQ(connection.databaseType(), DatabaseType::Sqlite);
        EXPECT_STREQ(databaseTypeName(connection.databaseType()), "Sqlite");
    }

    /** @brief 钉住版本取自链接进来的库：连接前后可读且不变 */
    TEST(SqliteConnection, ServerVersionDoesNotDependOnConnection)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // 内存里没有服务端进程，版本取自链接进来的 SQLite 库，断开后同样可读
        const std::string versionBeforeConnect = connection.serverVersion();
        ASSERT_FALSE(versionBeforeConnect.empty());
        EXPECT_NE(versionBeforeConnect.find_first_of("0123456789"), std::string::npos) << versionBeforeConnect;
        EXPECT_TRUE(containsText(versionBeforeConnect, ".")) << versionBeforeConnect;

        ASSERT_TRUE(connection.connect()) << connection.lastError();
        EXPECT_EQ(connection.serverVersion(), versionBeforeConnect);

        connection.disconnect();
        EXPECT_EQ(connection.serverVersion(), versionBeforeConnect);
    }

    /** @brief 钉住未连接时绝不把空句柄交给 SQLite：如实失败并给出中文前置条件说明 */
    TEST(SqliteConnection, ExecuteWithoutConnectionIsRejectedWithLocalizedReason)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT 1");

        // 未连接时绝不把空句柄交给 SQLite：直接失败并给出中文前置条件说明
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();
        EXPECT_TRUE(containsText(connection.lastError(), "未连接")) << connection.lastError();
    }

    /** @brief 钉住三个事务入口共用 execute() 的未连接前置检查，行为一致 */
    TEST(SqliteConnection, TransactionHelpersAllFailWithoutConnection)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // 三个事务入口共用 execute() 的前置检查，未连接时行为必须一致
        EXPECT_FALSE(connection.beginTransaction());
        EXPECT_TRUE(containsText(connection.lastError(), "未连接")) << connection.lastError();
        EXPECT_FALSE(connection.commit());
        EXPECT_FALSE(connection.rollback());
    }

    /** @brief 钉住 rowid 计数器随句柄走：无句柄或断开后归零，不残留上次会话的值 */
    TEST(SqliteConnection, LastInsertRowIdIsZeroWithoutOpenHandle)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // 从未连接、也就无从谈起「最近插入」：SQLite 的 rowid 从 1 起，0 是明确的空值
        EXPECT_EQ(connection.lastInsertRowId(), 0);

        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, kCreateUsersTableSql), nullptr);
        ASSERT_NE(executeRequired(connection, kInsertAliceSql), nullptr);
        EXPECT_NE(connection.lastInsertRowId(), 0);

        // 断开后句柄归零，计数器不能继续读到上一次会话的残值
        connection.disconnect();
        EXPECT_EQ(connection.lastInsertRowId(), 0);
    }

    /** @brief 钉住内存库建连暴露句柄，且启动期 PRAGMA 不留下错误文本 */
    TEST(SqliteConnection, ConnectsToInMemoryDatabaseAndExposesNativeHandle)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        ASSERT_TRUE(connection.connect()) << connection.lastError();

        EXPECT_TRUE(connection.isConnected());
        EXPECT_NE(connection.nativeHandle(), nullptr);
        // 内存库改不了 WAL 属正常场景，启动期 PRAGMA 不该留下任何错误文本
        EXPECT_TRUE(connection.lastError().empty()) << connection.lastError();
    }

    /** @brief 钉住重复 connect 幂等且复用同一句柄，不因重开造成旧句柄泄漏 */
    TEST(SqliteConnection, RepeatedConnectIsIdempotentAndKeepsSameHandle)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        sqlite3 *firstHandle = connection.nativeHandle();
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 幂等：第二次调用直接返回 true，不重开句柄，否则旧句柄就此泄漏
        EXPECT_EQ(connection.nativeHandle(), firstHandle);
        EXPECT_TRUE(connection.isConnected());
    }

    /** @brief 钉住 disconnect 释放句柄，之后的命令按未连接被拒 */
    TEST(SqliteConnection, DisconnectReleasesHandleAndRejectsFurtherCommands)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        connection.disconnect();

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);

        const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT 1");
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection.lastError(), "未连接")) << connection.lastError();
    }

    /** @brief 钉住内存库随句柄销毁一起清空：重连拿到全新空库 */
    TEST(SqliteConnection, ReconnectAfterDisconnectOpensFreshInMemoryDatabase)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, kCreateUsersTableSql), nullptr);
        ASSERT_NE(executeRequired(connection, kInsertAliceSql), nullptr);

        connection.disconnect();
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 内存库随句柄销毁一起清空：重连拿到的是全新空库，users 表不再存在
        EXPECT_NE(connection.nativeHandle(), nullptr);
        EXPECT_EQ(connection.execute("SELECT * FROM users"), nullptr);
        EXPECT_TRUE(containsText(connection.lastError(), "no such table")) << connection.lastError();
    }

    /** @brief 钉住空库路径兜底成内存库，且兜底后连接可正常执行 */
    TEST(SqliteConnection, EmptyDatabasePathFallsBackToInMemoryDatabase)
    {
        ConnectionConfig configuration;
        configuration.database = "";

        SqliteConnection connection(configuration);
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 构造函数注释承诺「database 为空串时按内存库处理」，这里钉住该兜底分支
        EXPECT_TRUE(connection.isConnected());
        EXPECT_NE(connection.nativeHandle(), nullptr);
        const std::unique_ptr<DatabaseResult> result = executeRequired(connection, "SELECT 1");
        ASSERT_NE(result, nullptr);
        EXPECT_TRUE(result->next());
        EXPECT_EQ(asInteger(result->getValue(std::size_t{0})), std::optional<std::int64_t>(1));
    }

    /** @brief 钉住嵌入式引擎忽略 host/port，不偷读无关网络字段 */
    TEST(SqliteConnection, HostAndPortAreIgnoredAndOnlyDatabasePathIsUsed)
    {
        TestSupport::TemporaryDatabaseFile databaseFile("SqliteIgnoresRemote");

        ConnectionConfig configuration;
        configuration.host     = "this.host.must.not.be.reached.invalid";
        configuration.port     = 65000;
        configuration.userName = "nobody";
        configuration.password = "not-a-password";
        configuration.database = databaseFile.utf8Path();

        SqliteConnection connection(configuration);

        // SQLite 没有网络往返：一个不存在的端口/主机也不该影响打开本地文件，否则说明驱动偷读了无关字段
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        EXPECT_TRUE(connection.isConnected());
        EXPECT_NE(executeRequired(connection, kCreateUsersTableSql), nullptr);
        EXPECT_TRUE(databaseFile.exists());
    }

    /** @brief 钉住打不开文件时如实失败、回收半开句柄，并把请求路径写进错误文本 */
    TEST(SqliteConnection, ConnectIntoMissingDirectoryFailsAndReportsRequestedPath)
    {
        // 父目录不存在且刻意不创建：SQLite 只能报「打不开文件」，这是无需权限即可稳定复现的失败路径
        const std::filesystem::path missingDirectoryPath = std::filesystem::temp_directory_path() /
                                                           TestSupport::makeUniqueDatabaseName("MissingDirectory") /
                                                           "nested" / "database.db";
        ASSERT_FALSE(std::filesystem::exists(missingDirectoryPath.parent_path()));

        ConnectionConfig configuration;
        configuration.database = TestSupport::toUtf8PathText(missingDirectoryPath);
        SqliteConnection connection(configuration);

        EXPECT_FALSE(connection.connect());
        EXPECT_FALSE(connection.isConnected());
        // 打开失败时 sqlite3_open 可能已分配半开句柄，实现必须关掉它，不能留下残柄
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsText(connection.lastError(), "打开")) << connection.lastError();
        EXPECT_TRUE(containsText(connection.lastError(), configuration.database)) << connection.lastError();

        // 失败可重复：第二次 connect() 仍走同一条错误路径，不会因为残留状态而误报成功
        EXPECT_FALSE(connection.connect());
        EXPECT_FALSE(connection.isConnected());
    }

    // ============================================================================
    // execute()：单语句约束与错误路径
    // ============================================================================

    /** @brief 钉住空命令在执行前就被拦下，不占用一次 prepare */
    TEST_F(SqliteConnectedMemoryDatabase, EmptyCommandIsRejected)
    {
        const std::unique_ptr<DatabaseResult> result = connection().execute("");

        // 空命令先被拦下，不占用一次 prepare
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "为空")) << connection().lastError();
    }

    /** @brief 钉住无语句输入（空白/分号/注释）如实失败，不把空操作报成执行成功 */
    TEST_F(SqliteConnectedMemoryDatabase, CommandWithoutExecutableStatementIsRejected)
    {
        // 四种输入都让 prepare 成功却不产出语句：只剩空白、多余分号、行注释与块注释
        constexpr std::array<const char *, 4> kCommandsWithoutStatement = {"   ", ";", "-- 只是一行注释", "/* 块注释 */"};

        for (const char *command: kCommandsWithoutStatement)
        {
            const std::unique_ptr<DatabaseResult> result = connection().execute(command);

            // 「什么都没执行」不能报成执行成功，否则调用方会把空操作当成生效
            EXPECT_EQ(result, nullptr) << command;
            EXPECT_TRUE(containsText(connection().lastError(), "没有可执行的 SQL 语句")) << command << " / " << connection().lastError();
        }
    }

    /** @brief 钉住尾部分号与注释不算第二条语句，首条语句照常执行 */
    TEST_F(SqliteConnectedMemoryDatabase, TrailingSemicolonAndCommentAfterStatementAreNotExtraStatements)
    {
        // 分号与注释之后的空白不算第二条语句：这种尾巴必须照常执行首条语句
        const std::unique_ptr<DatabaseResult> withSemicolon = executeRequired(connection(), "SELECT 1 AS oneValue;");
        ASSERT_NE(withSemicolon, nullptr);
        EXPECT_EQ(withSemicolon->columnCount(), 1u);

        const std::unique_ptr<DatabaseResult> withComment = executeRequired(connection(), "SELECT 2 AS twoValue /* 尾注 */");
        ASSERT_NE(withComment, nullptr);
        EXPECT_TRUE(withComment->next());
        EXPECT_EQ(asInteger(withComment->getValue(std::size_t{0})), std::optional<std::int64_t>(2));
    }

    /** @brief 钉住多语句脚本整次拒绝且首条也不执行，不留半执行状态 */
    TEST_F(SqliteConnectedMemoryDatabase, MultipleStatementsAreRejectedWithoutRunningTheFirst)
    {
        const std::string script = "CREATE TABLE pairs (leftValue INTEGER, rightValue INTEGER);"
                                   " INSERT INTO pairs VALUES (1, 2);";

        const std::unique_ptr<DatabaseResult> result = connection().execute(script);

        // 分号后仍有可执行语句就整次失败：执行前半段再静默丢掉后半段是不允许的
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsLocalizedText(connection().lastError())) << connection().lastError();
        // 只校验「中文说明 + 提到语句」：额外语句编译失败与整次拒绝两条分支措辞不同，不该钉死
        EXPECT_TRUE(containsText(connection().lastError(), "语句")) << connection().lastError();

        // 一条都不执行：首条建表语句没有生效
        EXPECT_EQ(connection().execute("SELECT * FROM pairs"), nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "no such table")) << connection().lastError();
    }

    /** @brief 钉住额外语句编译失败时归因正确、首条不执行 */
    TEST_F(SqliteConnectedMemoryDatabase, BrokenTrailingStatementIsRejectedWithoutRunningTheFirst)
    {
        const std::string script = "CREATE TABLE triplets (firstValue INTEGER); SELECT FROM;";

        const std::unique_ptr<DatabaseResult> result = connection().execute(script);

        // 后半段编译不过时，错误来自「额外语句编译失败」而不是首条语句，且首条依然不执行
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "额外的 SQL 语句")) << connection().lastError();
        EXPECT_TRUE(containsText(connection().lastError(), "错误码")) << connection().lastError();

        EXPECT_EQ(connection().execute("SELECT * FROM triplets"), nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "no such table")) << connection().lastError();
    }

    /** @brief 钉住错误文本三要素齐全：中文动作说明 + SQLite 原文 + 错误码 */
    TEST_F(SqliteConnectedMemoryDatabase, InvalidSqlIsRejectedWithUnderlyingReason)
    {
        const std::unique_ptr<DatabaseResult> result = connection().execute("SELECT * FROM missing_table");

        // 中文动作说明 + SQLite 原始英文原因 + 错误码，三者缺一不可
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "SQL 语句")) << connection().lastError();
        EXPECT_TRUE(containsText(connection().lastError(), "no such table")) << connection().lastError();
        EXPECT_TRUE(containsText(connection().lastError(), "错误码")) << connection().lastError();
    }

    /** @brief 钉住 step 阶段的约束违例如实报错，且失败的插入不产生新行 */
    TEST_F(SqliteConnectedMemoryDatabase, ConstraintViolationIsRejectedAtStepPhase)
    {
        // NOT NULL 违例在 step 阶段才暴露，编译期无从发现
        const std::unique_ptr<DatabaseResult> result = connection().execute("INSERT INTO users (name, age) VALUES (NULL, 1)");

        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "NOT NULL")) << connection().lastError();
        EXPECT_TRUE(containsText(connection().lastError(), "错误码")) << connection().lastError();

        // 失败的插入没有产生新行：样本行数仍是三条
        const std::optional<std::int64_t> rowCount = readScalarInteger(connection(), "SELECT COUNT(*) FROM users");
        EXPECT_EQ(rowCount, std::optional<std::int64_t>(3));
    }

    /** @brief 钉住每次调用先清错误：成功路径不拿上一轮失败冒充本次结果 */
    TEST_F(SqliteConnectedMemoryDatabase, SuccessfulCommandClearsPreviousFailure)
    {
        EXPECT_EQ(connection().execute("SELECT * FROM missing_table"), nullptr);
        EXPECT_FALSE(connection().lastError().empty());

        ASSERT_NE(executeRequired(connection(), "SELECT 1"), nullptr);

        // 每次调用开头清空 m_lastError：成功路径不能拿上一轮的失败冒充本次结果
        EXPECT_TRUE(connection().lastError().empty()) << connection().lastError();
    }

    // ============================================================================
    // 事务
    // ============================================================================

    /** @brief 钉住 BEGIN/COMMIT 收尾后写入生效，rowid 计数器随之前进 */
    TEST_F(SqliteConnectedMemoryDatabase, BeginAndCommitKeepInsertedRow)
    {
        ASSERT_TRUE(connection().beginTransaction()) << connection().lastError();
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Committed')"), nullptr);
        ASSERT_TRUE(connection().commit()) << connection().lastError();

        EXPECT_EQ(readScalarInteger(connection(), "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(4));
        EXPECT_EQ(connection().lastInsertRowId(), 4);
    }

    /** @brief 钉住 ROLLBACK 撤销本次写入，行数回到事务前的三条 */
    TEST_F(SqliteConnectedMemoryDatabase, RollbackDiscardsInsertedRow)
    {
        ASSERT_TRUE(connection().beginTransaction()) << connection().lastError();
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Discarded')"), nullptr);
        ASSERT_TRUE(connection().rollback()) << connection().lastError();

        // 回滚后计数回到事务前的三条
        EXPECT_EQ(readScalarInteger(connection(), "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(3));
    }

    /** @brief 钉住嵌套 BEGIN 被拒且不破坏已有事务，后续写入与提交照常 */
    TEST_F(SqliteConnectedMemoryDatabase, NestedBeginIsRejected)
    {
        ASSERT_TRUE(connection().beginTransaction()) << connection().lastError();

        EXPECT_FALSE(connection().beginTransaction());
        EXPECT_TRUE(containsText(connection().lastError(), "transaction")) << connection().lastError();

        // 被拒的嵌套 BEGIN 不能破坏已有事务：提交后本轮写入依然生效
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('StillInside')"), nullptr);
        ASSERT_TRUE(connection().commit()) << connection().lastError();
        EXPECT_EQ(readScalarInteger(connection(), "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(4));
    }

    /** @brief 钉住自动提交模式下无事务可收尾时如实失败，且连接保持可用 */
    TEST_F(SqliteConnectedMemoryDatabase, CommitOrRollbackWithoutActiveTransactionIsRejected)
    {
        // 自动提交模式下没有事务可收尾，两个入口都必须如实失败而不是返回「成功但什么都没做」
        EXPECT_FALSE(connection().commit());
        EXPECT_TRUE(containsLocalizedText(connection().lastError())) << connection().lastError();

        EXPECT_FALSE(connection().rollback());
        EXPECT_TRUE(containsLocalizedText(connection().lastError())) << connection().lastError();

        // 失败不影响连接本身：随后仍能正常查询
        EXPECT_NE(executeRequired(connection(), "SELECT 1"), nullptr);
        EXPECT_TRUE(connection().isConnected());
    }

    // ============================================================================
    // 连接级计数器与启动期配置
    // ============================================================================

    /** @brief 钉住计数器记的是「最近插入的 rowid」：显式 rowid 的插入同样刷新它 */
    TEST_F(SqliteConnectedMemoryDatabase, LastInsertRowIdFollowsEachSuccessfulInsert)
    {
        // AUTOINCREMENT 的 rowid 从 1 递增，样本数据已占到 3
        EXPECT_EQ(connection().lastInsertRowId(), 3);

        ASSERT_NE(executeRequired(connection(), kInsertCarolSql), nullptr);
        EXPECT_EQ(connection().lastInsertRowId(), 4);

        // 显式给出 rowid 的插入同样刷新这个计数器，它记的是「最近插入的 rowid」而不是「插入了几行」
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (id, name) VALUES (500, 'Explicit')"), nullptr);
        EXPECT_EQ(connection().lastInsertRowId(), 500);
    }

    /** @brief 钉住只有 INSERT 会改变 rowid 计数器，读与删除都不动它 */
    TEST_F(SqliteConnectedMemoryDatabase, LastInsertRowIdIgnoresReadAndDeleteStatements)
    {
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Dan')"), nullptr);
        const std::int64_t rowIdAfterInsert = connection().lastInsertRowId();
        ASSERT_NE(rowIdAfterInsert, 0);

        // 「该计数器只由 INSERT 改变」是 SQLite 的既有语义，读语句与删除都不该动它
        ASSERT_NE(executeRequired(connection(), "SELECT * FROM users"), nullptr);
        EXPECT_EQ(connection().lastInsertRowId(), rowIdAfterInsert);

        ASSERT_NE(executeRequired(connection(), "DELETE FROM users"), nullptr);
        EXPECT_EQ(connection().lastInsertRowId(), rowIdAfterInsert);
    }

    /** @brief 钉住启动期外键 PRAGMA 真正生效：孤儿行被拒、父行存在后可插入 */
    TEST_F(SqliteConnectedMemoryDatabase, StartupPragmaEnablesForeignKeys)
    {
        ASSERT_NE(executeRequired(connection(), "CREATE TABLE parents (id INTEGER PRIMARY KEY)"), nullptr);
        ASSERT_NE(executeRequired(connection(), "CREATE TABLE children (id INTEGER PRIMARY KEY, parentId INTEGER REFERENCES parents (id))"), nullptr);

        // connect() 已执行 PRAGMA foreign_keys=ON：孤儿子行必须在 step 阶段被拒
        const std::unique_ptr<DatabaseResult> orphanInsert = connection().execute("INSERT INTO children (parentId) VALUES (99)");
        EXPECT_EQ(orphanInsert, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "FOREIGN KEY")) << connection().lastError();

        // 父行存在后同一写法应当成功，排除「任何插入都被拒」的假阳性
        ASSERT_NE(executeRequired(connection(), "INSERT INTO parents VALUES (99)"), nullptr);
        ASSERT_NE(executeRequired(connection(), "INSERT INTO children (parentId) VALUES (99)"), nullptr);
    }

    /** @brief 钉住 queryTimeout 经 busy_timeout 零计时映射进 SQLite，含基类默认值 */
    TEST(SqliteConnection, QueryTimeoutBecomesBusyTimeoutOnConnect)
    {
        // PRAGMA busy_timeout 的读形式直接反映 sqlite3_busy_timeout 设进去的值，是零计时的映射验证；
        // 第一轮取的正是基类默认超时，顺带钉住「不设置也按 queryTimeout() 默认值配置」
        for (const int expectedTimeout: {kDefaultQueryTimeoutMilliseconds, 4321})
        {
            SqliteConnection connection(ConnectionConfig::sqliteDefault());
            connection.setQueryTimeout(expectedTimeout);
            ASSERT_TRUE(connection.connect()) << connection.lastError();

            EXPECT_EQ(readScalarInteger(connection, "PRAGMA busy_timeout"), std::optional<std::int64_t>(expectedTimeout));
        }
    }

    /** @brief 钉住非正超时统一夹成 0（不等锁，立刻 SQLITE_BUSY） */
    TEST(SqliteConnection, NonPositiveQueryTimeoutMapsToZeroBusyTimeout)
    {
        // 基类允许 0 与负值；负数交给 SQLite 属未定义用法，实现统一夹成 0（不等锁，立刻 SQLITE_BUSY）
        for (const int timeoutSetting: {0, -100})
        {
            SqliteConnection connection(ConnectionConfig::sqliteDefault());
            connection.setQueryTimeout(timeoutSetting);
            ASSERT_TRUE(connection.connect()) << connection.lastError();

            EXPECT_EQ(readScalarInteger(connection, "PRAGMA busy_timeout"), std::optional<std::int64_t>(0))
                    << "queryTimeout=" << timeoutSetting;
        }
    }

    /** @brief 钉住内存库改不成 WAL 不算失败：不报错且连接照常可用 */
    TEST_F(SqliteConnectedMemoryDatabase, InMemoryDatabaseKeepsItsOwnJournalModeAndStaysUsable)
    {
        // 内存库改不成 WAL 是预期行为，connect() 不许因此失败，也不许留下错误文本
        const std::unique_ptr<DatabaseResult> result = executeRequired(connection(), "PRAGMA journal_mode");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        const std::optional<std::string> mode = asText(result->getValue(std::size_t{0}));
        ASSERT_TRUE(mode.has_value());
        EXPECT_EQ(toLowerCopy(mode.value()), "memory");
        EXPECT_TRUE(connection().lastError().empty()) << connection().lastError();

        // 启动期 PRAGMA「未达成」不等于连接坏了：随后的查询照常返回结果
        EXPECT_NE(executeRequired(connection(), "SELECT 1"), nullptr);
    }

    // ============================================================================
    // 文件库：落盘、重开与锁竞争
    // ============================================================================

    /** @brief 钉住 connect 创建库文件，且文件只落在系统临时目录 */
    TEST_F(SqliteTemporaryFileDatabase, ConnectCreatesDatabaseFileOnDisk)
    {
        // 构造夹具时文件还不存在，是 connect() 把它创建出来的
        EXPECT_TRUE(m_databaseFile.exists());
        EXPECT_NE(connection().nativeHandle(), nullptr);

        // 库文件必须落在系统临时目录，而不是测试进程的当前工作目录——否则会往源码树里写残留。
        // Windows 的 temp_directory_path() 带结尾分隔符，两侧都先做路径归一化再比较
        EXPECT_EQ(std::filesystem::weakly_canonical(m_databaseFile.path().parent_path()),
                  std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()));

        const std::unique_ptr<DatabaseResult> result = executeRequired(connection(), "SELECT 1");
        ASSERT_NE(result, nullptr);
        EXPECT_TRUE(result->next());
    }

    /** @brief 钉住文件库的 WAL PRAGMA 真正生效（读回 wal） */
    TEST_F(SqliteTemporaryFileDatabase, FileDatabaseRunsInWriteAheadLogMode)
    {
        const std::unique_ptr<DatabaseResult> result = executeRequired(connection(), "PRAGMA journal_mode");
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->next());

        // 文件库是 WAL PRAGMA 真正生效的场景，读回来必须是 wal
        const std::optional<std::string> mode = asText(result->getValue(std::size_t{0}));
        ASSERT_TRUE(mode.has_value());
        EXPECT_EQ(mode.value(), "wal");
    }

    /** @brief 钉住断连时检查点回写与重连后的数据完整 */
    TEST_F(SqliteTemporaryFileDatabase, CommittedRowsAndFileContentSurviveReconnect)
    {
        ASSERT_NE(executeRequired(connection(), kCreateUsersTableSql), nullptr);
        ASSERT_NE(executeRequired(connection(), kInsertAliceSql), nullptr);

        connection().disconnect();

        // 最后一个连接关闭时 SQLite 会把 WAL 检查点回写主文件，此时磁盘上必须已有内容
        EXPECT_GT(m_databaseFile.fileSizeBytes(), 0u);

        ASSERT_TRUE(connection().connect()) << connection().lastError();
        EXPECT_EQ(readScalarInteger(connection(), "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(1));
    }

    /** @brief 钉住第二个连接对象能读到已提交内容，与是谁写入的无关 */
    TEST_F(SqliteTemporaryFileDatabase, SecondConnectionObjectReadsCommittedRows)
    {
        ASSERT_NE(executeRequired(connection(), kCreateUsersTableSql), nullptr);
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name, age) VALUES ('Writer', 20)"), nullptr);

        {
            // 换一个连接对象、走同一份配置：读到的是已提交内容，与是谁写入的无关
            const std::unique_ptr<SqliteConnection> reader = openSecondConnection();
            ASSERT_TRUE(reader->connect()) << reader->lastError();

            const std::unique_ptr<DatabaseResult> result = executeRequired(*reader, "SELECT name FROM users");
            ASSERT_NE(result, nullptr);
            ASSERT_TRUE(result->next());
            EXPECT_EQ(asText(result->getValue(std::size_t{0})), std::optional<std::string>("Writer"));
        }

        EXPECT_TRUE(connection().isConnected());
    }

    /** @brief 钉住等锁超时如实报错，解锁后同一写入成功 */
    TEST_F(SqliteTemporaryFileDatabase, WriteOnSecondConnectionFailsWhileFirstHoldsExclusiveLock)
    {
        ASSERT_NE(executeRequired(connection(), kCreateUsersTableSql), nullptr);
        ASSERT_NE(executeRequired(connection(), "BEGIN EXCLUSIVE"), nullptr);

        const std::unique_ptr<SqliteConnection> contender = openSecondConnection();
        // 锁等待上限很短：真正的等待时长已由 PRAGMA busy_timeout 用例证明，这里只验可观察的失败结果
        contender->setQueryTimeout(50);
        ASSERT_TRUE(contender->connect()) << contender->lastError();

        const std::unique_ptr<DatabaseResult> result = contender->execute("INSERT INTO users (name) VALUES ('Queued')");

        // 等锁超时后如实报错：中文说明 + SQLite 原始 busy 文本
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsLocalizedText(contender->lastError())) << contender->lastError();
        EXPECT_TRUE(containsText(contender->lastError(), "locked")) << contender->lastError();

        ASSERT_NE(executeRequired(connection(), "ROLLBACK"), nullptr);
        EXPECT_NE(executeRequired(*contender, "INSERT INTO users (name) VALUES ('Queued')"), nullptr);
    }

    /** @brief 钉住工厂交出的基类指针能对配置的文件读写 */
    TEST_F(SqliteTemporaryFileDatabase, FactoryCreatedConnectionWritesAndReadsConfiguredFile)
    {
        const std::unique_ptr<DatabaseConnection> connection =
                DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));

        // 工厂交出的是基类指针：SQLite 专有信息通过基类接口 + 类型枚举间接确认
        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
        ASSERT_TRUE(connection->connect()) << connection->lastError();

        ASSERT_NE(executeRequired(*connection, kCreateUsersTableSql), nullptr);
        ASSERT_NE(executeRequired(*connection, kInsertAliceSql), nullptr);
        EXPECT_EQ(readScalarInteger(*connection, "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(1));
        EXPECT_TRUE(m_databaseFile.exists());
    }

    /**
     * @brief 只读查询的预扫描失败要如实报错，而不是变成一个「0 行」的空结果
     * @details 查询在构造结果集时先被预扫描一遍数行；中途出错时结果集看起来只是「没有数据」。
     *          把这种结果当成功交出去，调用方拿到的是「查询没有返回任何行」，真原因只剩在
     *          结果集的 lastError() 里没人看——写路径一直是如实失败，两条路径口径必须一致
     */
    TEST(SqliteConnection, ReadOnlyQueryPreScanFailureSurfacesAsError)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // abs() 对最小整数报 "integer overflow"，且这发生在 step 阶段、语句本身是只读的
        // （减一写法是为了绕开字面量解析：9223372036854775808 会被当成浮点数）
        EXPECT_EQ(connection.execute("SELECT abs(-9223372036854775807 - 1)"), nullptr) << "预扫描失败被当成空结果返回了";
        EXPECT_FALSE(connection.lastError().empty()) << "报错时没有留下原因";
    }

    /**
     * @brief 归还连接池前把未提交的事务滚掉：下一个借用者不会继承上一笔事务
     */
    TEST(SqliteConnection, ResetSessionStateRollsBackUncommittedTransaction)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr);

        ASSERT_TRUE(connection.beginTransaction()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, "INSERT INTO t (id, name) VALUES (1, 'leftover')"), nullptr);

        // 复位前：事务仍开着，这条未提交的行在本连接里看得见
        ASSERT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t"), std::optional<std::int64_t>(1));

        connection.resetSessionState();

        // 事务被滚掉：未提交的行随之消失，也就是没有串给下一个借用者
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t"), std::optional<std::int64_t>(0))
                << "归还时没有滚掉未提交的事务：下一个借用者会继承上一笔事务";

        // 幂等：没有活动事务时再调一次什么都不做，也不留下错误文本
        EXPECT_NO_THROW(connection.resetSessionState());
        EXPECT_TRUE(connection.lastError().empty()) << connection.lastError();
    }

    /**
     * @brief 同一句参数化 UPDATE 反复执行时，每一轮的参数都真正生效
     * @details 第二次起走的是缓存里那条已编译游标，因此这条钉的是复用面的两件事：游标被 reset 回了
     *          可重跑状态（否则第二次起一行都不命中），以及上一次的绑定不会残留成这一次的取值
     *          （否则落库的名字与 id 会配错对）。断言核对的是最终落库结果，而不是返回码本身。
     */
    TEST(SqliteConnection, ReusedWriteStatementAppliesEveryRoundOfParameters)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr);
        ASSERT_NE(executeRequired(connection, "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')"), nullptr);

        constexpr std::size_t kRoundCount = 5;
        for (std::size_t roundIndex = 0; roundIndex < kRoundCount; ++roundIndex)
        {
            // 占位符顺序即绑定顺序：先 name 后 id
            const std::array<DatabaseValue, 2> parameters{
                    "round-" + std::to_string(roundIndex), static_cast<std::int64_t>((roundIndex % 3U) + 1U)};
            const std::unique_ptr<DatabaseResult> result = connection.execute(
                    "UPDATE t SET name = ? WHERE id = ?", std::span<const DatabaseValue>(parameters));
            ASSERT_NE(result, nullptr) << "第 " << roundIndex << " 轮失败：" << connection.lastError();
            EXPECT_EQ(result->affectedRowCount(), 1) << "第 " << roundIndex << " 轮一行都没改到：复用的游标没有回到可重跑状态";
        }

        // 轮 i 写 id = i % 3 + 1，故五轮下来 id1 最后被第 3 轮改写、id2 被第 4 轮、id3 被第 2 轮。
        // 逐行核对最终值：只要有一轮的绑定残留到了下一轮（或游标没被 reset），这里就对不上
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t WHERE id = 1 AND name = 'round-3'"),
                  std::optional<std::int64_t>(1)) << "id 1 的最终值不是第 3 轮写的：绑定在复用间串了";
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t WHERE id = 2 AND name = 'round-4'"),
                  std::optional<std::int64_t>(1)) << "id 2 的最终值不是第 4 轮写的：绑定在复用间串了";
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t WHERE id = 3 AND name = 'round-2'"),
                  std::optional<std::int64_t>(1)) << "id 3 的最终值不是第 2 轮写的：绑定在复用间串了";
    }

    /**
     * @brief 断开再重连之后，不能沿用上一个数据库句柄上编译出来的游标
     * @details 缓存的键只有 SQL 文本，而 sqlite3_stmt 属于具体的那个 sqlite3 句柄：disconnect() 把
     *          句柄关掉之后，表里那些游标全部是上一个数据库对象图里的东西。因此清表必须落在换句柄
     *          之前。实测撤掉清表时的表现不是崩溃也不是 ASan 报告（sqlite3_close_v2 会把还有未
     *          finalize 游标的连接标成 zombie，内存因此仍在），而是 step 以一个毫无意义的原因失败：
     *          「执行 SQL 语句失败：not an error（错误码 0）」——正是那种只能靠用例拦住的静默错误。
     *          内存库重连得到的是一个全新的空库，所以「建表能成功」这条断言只有在语句按新句柄
     *          重新编译时才会成立。
     */
    TEST(SqliteConnection, ReconnectDoesNotReuseStatementsFromTheClosedHandle)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr);
        ASSERT_NE(executeRequired(connection, "INSERT INTO t VALUES (1, 'a')"), nullptr);

        constexpr const char *kUpdateSql = "UPDATE t SET name = ? WHERE id = ?";
        for (std::size_t roundIndex = 0; roundIndex < 3U; ++roundIndex)
        {
            // 必须显式写成 std::string：字面量的类型是 const char*，而 variant 的转换构造会按
            // 「标准转换优于用户定义转换」给 bool 那一支计分，不写出来就静默绑成 true
            const std::array<DatabaseValue, 2> parameters{std::string{"warm-up"}, std::int64_t{1}};
            ASSERT_NE(connection.execute(kUpdateSql, std::span<const DatabaseValue>(parameters)), nullptr) << connection.lastError();
        }

        connection.disconnect();
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        // 换句柄即换库：内存库里那张表已经不在了，这条语句必须是重新编译的那一份在跑
        ASSERT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM sqlite_master WHERE name = 't'"), std::optional<std::int64_t>(0))
                << "重连没有拿到新的空库，用例的前提不成立";
        ASSERT_NE(executeRequired(connection, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr);
        ASSERT_NE(executeRequired(connection, "INSERT INTO t VALUES (7, 'fresh')"), nullptr);

        const std::array<DatabaseValue, 2> parameters{std::string{"after-reconnect"}, std::int64_t{7}};
        const std::unique_ptr<DatabaseResult> result = connection.execute(kUpdateSql, std::span<const DatabaseValue>(parameters));
        ASSERT_NE(result, nullptr) << connection.lastError();
        EXPECT_EQ(result->affectedRowCount(), 1) << "重连后同一条 SQL 用到了旧句柄上的游标";
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t WHERE id = 7 AND name = 'after-reconnect'"),
                  std::optional<std::int64_t>(1));
    }

    /**
     * @brief 验证非有限的浮点参数在绑定前就被拒绝，而不是被静默改写成 NULL 落库
     *
     * @details REAL 的存储格式装不下 NaN 与无穷大，sqlite3_bind_double 会把它们**改绑成 NULL**：
     *          「写入一个数」于是变成「写入空值」，语句执行成功、无告警、读回来是 NULL。
     *          被量的那一列刻意保持可空——若给它加 NOT NULL，旧实现会撞 constraint 失败而"看起来也在拒绝"，
     *          本用例就失去了证伪能力。
     */
    TEST(SqliteConnection, NonFiniteDoubleParameterIsRejectedInsteadOfSilentlyBoundAsNull)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(executeRequired(connection, "CREATE TABLE t (id INTEGER PRIMARY KEY, value REAL)"), nullptr);

        const std::array<double, 3> nonFiniteValues{std::numeric_limits<double>::quiet_NaN(),
                                                    std::numeric_limits<double>::infinity(),
                                                    -std::numeric_limits<double>::infinity()};
        for (std::size_t valueIndex = 0; valueIndex < nonFiniteValues.size(); ++valueIndex)
        {
            const std::array<DatabaseValue, 2> parameters{static_cast<std::int64_t>(valueIndex), nonFiniteValues[valueIndex]};
            EXPECT_EQ(connection.execute("INSERT INTO t VALUES (?, ?)", std::span<const DatabaseValue>(parameters)), nullptr)
                    << "第 " << valueIndex << " 个非有限取值被当成可写入的参数放过了";
            // 文案要能定位到具体是第几个参数，并给出替代做法（否则调用方只能自己去猜是 NaN 还是 inf）
            EXPECT_TRUE(containsText(connection.lastError(), "第 2 个参数")) << connection.lastError();
            EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();
        }

        // 三条语句都没有留下任何行：留下「value IS NULL」的行正是这个缺陷的原始形态
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t"), std::optional<std::int64_t>(0));
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t WHERE value IS NULL"), std::optional<std::int64_t>(0));

        // 有限取值不受影响，且能原样读回（含边界上的极大有限值）
        const std::array<DatabaseValue, 2> finiteParameters{std::int64_t{7}, 3.5};
        ASSERT_NE(connection.execute("INSERT INTO t VALUES (?, ?)", std::span<const DatabaseValue>(finiteParameters)), nullptr)
                << connection.lastError();
        const std::array<DatabaseValue, 2> extremeFiniteParameters{std::int64_t{8}, std::numeric_limits<double>::max()};
        ASSERT_NE(connection.execute("INSERT INTO t VALUES (?, ?)", std::span<const DatabaseValue>(extremeFiniteParameters)), nullptr)
                << connection.lastError();

        const std::unique_ptr<DatabaseResult> readBack =
                executeRequired(connection, "SELECT value FROM t WHERE id = 7");
        ASSERT_TRUE(readBack->next());
        EXPECT_EQ(std::get<double>(readBack->getValue(0)), 3.5);
        EXPECT_EQ(readScalarInteger(connection, "SELECT COUNT(*) FROM t WHERE value IS NULL"), std::optional<std::int64_t>(0));
    }

} // namespace AsynGyanis::Database
