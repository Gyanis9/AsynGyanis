/**
 * @file TestSqliteConnection.cpp
 * @brief SqliteConnection 单元测试：真实 SQLite 驱动的建库、单语句执行契约、事务与连接级计数器
 * @details SQLite 是进程内引擎，本文件全部用例零外部服务：内存库走 ":memory:"，文件库走
 *          TestSupport::TemporaryDatabaseFile 生成的临时路径，用例结束即删除文件与同名 -wal/-shm/-journal 残留。
 *          钉住的实现契约（重构时刻意定下的语义，破坏即视为回归）：
 *          1、execute() 一次只执行一条语句：分号后还有可执行语句时整次调用失败，一条都不执行；
 *             尾部只剩空白、注释或多余分号不算额外语句；
 *          2、queryTimeout() 在 connect() 时映射成 sqlite3_busy_timeout，非正值映射为 0；
 *             该映射经 PRAGMA busy_timeout 直读验证，不做依赖时钟的等待断言；
 *          3、启动期两条 PRAGMA（WAL / 外键）失败不致命：内存库的 journal_mode 仍是 memory，
 *             文件库则确实切到 wal，两条 PRAGMA 都不该留下错误文本；
 *          4、lastInsertRowId() 是连接级计数器，只由 INSERT 刷新，与结果集快照互不影响；
 *          5、未连接时各入口一致失败并给出中文说明，serverVersion() 与 databaseType() 不依赖连接。
 *          确认无法安全覆盖、因此不做断言的行为：
 *          1、命令长度超过 INT_MAX 的拒绝分支——需要构造 2GB 字符串，代价与收益不成比例；
 *          2、sqlite3_close_v2 返回 SQLITE_MISUSE 的分支——只有在外部抢先关闭句柄时才会触发，
 *             属于本类文档明令禁止的用法，不为其制造非法状态。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace AsynGyanis::Database
{
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
         * @brief 判断文本是否含非 ASCII 字节，用作「驱动自己拼了中文说明」的稳定判据
         * @details 断言只查「有中文 + 有底层关键英文原文 + 有错误码」，不硬编码整句中文，
         *          避免 SQLite 版本升级改了英文措辞时用例集体失败。
         * @param text 待判定文本
         * @return true 至少有一个字节的最高位被置起
         */
        bool containsLocalizedText(const std::string &text)
        {
            for (const char character: text)
            {
                if (static_cast<unsigned char>(character) >= 0x80)
                {
                    return true;
                }
            }
            return false;
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
         * @brief 执行一条按契约应当成功的命令
         * @details 失败时把 SQL 文本与 lastError() 一起报出来，省去每个用例手抄两遍；
         *          只用 EXPECT 不用 ASSERT，因此返回的指针可能为空，调用方需自行 ASSERT_NE 决定是否中止。
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
         * @brief 安全取出整型列值
         * @param value 待判定的数据库值
         * @return std::optional<std::int64_t> 值的类型不是整数时返回空值而不是抛异常
         */
        std::optional<std::int64_t> asInteger(const DatabaseValue &value)
        {
            const auto *integer = std::get_if<std::int64_t>(&value);
            return integer == nullptr ? std::nullopt : std::optional<std::int64_t>(*integer);
        }

        /**
         * @brief 安全取出文本列值
         * @param value 待判定的数据库值
         * @return std::optional<std::string> 值的类型不是字符串时返回空值
         */
        std::optional<std::string> asText(const DatabaseValue &value)
        {
            const auto *text = std::get_if<std::string>(&value);
            return text == nullptr ? std::nullopt : std::optional<std::string>(*text);
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

    TEST(SqliteConnection, DatabaseTypeIsSqliteWithoutConnection)
    {
        const SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // databaseType() 不查句柄，未连接时同样可用于日志与分派断言
        EXPECT_EQ(connection.databaseType(), DatabaseType::Sqlite);
        EXPECT_STREQ(databaseTypeName(connection.databaseType()), "Sqlite");
    }

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

    TEST(SqliteConnection, TransactionHelpersAllFailWithoutConnection)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        // 三个事务入口共用 execute() 的前置检查，未连接时行为必须一致
        EXPECT_FALSE(connection.beginTransaction());
        EXPECT_TRUE(containsText(connection.lastError(), "未连接")) << connection.lastError();
        EXPECT_FALSE(connection.commit());
        EXPECT_FALSE(connection.rollback());
    }

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

    TEST(SqliteConnection, ConnectsToInMemoryDatabaseAndExposesNativeHandle)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());

        ASSERT_TRUE(connection.connect()) << connection.lastError();

        EXPECT_TRUE(connection.isConnected());
        EXPECT_NE(connection.nativeHandle(), nullptr);
        // 内存库改不了 WAL 属正常场景，启动期 PRAGMA 不该留下任何错误文本
        EXPECT_TRUE(connection.lastError().empty()) << connection.lastError();
    }

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

    TEST_F(SqliteConnectedMemoryDatabase, EmptyCommandIsRejected)
    {
        const std::unique_ptr<DatabaseResult> result = connection().execute("");

        // 空命令先被拦下，不占用一次 prepare
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "为空")) << connection().lastError();
    }

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

    TEST_F(SqliteConnectedMemoryDatabase, MultipleStatementsAreRejectedWithoutRunningTheFirst)
    {
        const std::string script = "CREATE TABLE pairs (leftValue INTEGER, rightValue INTEGER);"
                                   " INSERT INTO pairs VALUES (1, 2);";

        const std::unique_ptr<DatabaseResult> result = connection().execute(script);

        // 旧实现会执行前半段、静默丢掉后半段；现在必须整次调用失败
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsLocalizedText(connection().lastError())) << connection().lastError();
        // 只校验「中文说明 + 提到语句」：额外语句编译失败与整次拒绝两条分支措辞不同，不该钉死
        EXPECT_TRUE(containsText(connection().lastError(), "语句")) << connection().lastError();

        // 一条都不执行：首条建表语句没有生效
        EXPECT_EQ(connection().execute("SELECT * FROM pairs"), nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "no such table")) << connection().lastError();
    }

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

    TEST_F(SqliteConnectedMemoryDatabase, InvalidSqlIsRejectedWithUnderlyingReason)
    {
        const std::unique_ptr<DatabaseResult> result = connection().execute("SELECT * FROM missing_table");

        // 中文动作说明 + SQLite 原始英文原因 + 错误码，三者缺一不可
        EXPECT_EQ(result, nullptr);
        EXPECT_TRUE(containsText(connection().lastError(), "SQL 语句")) << connection().lastError();
        EXPECT_TRUE(containsText(connection().lastError(), "no such table")) << connection().lastError();
        EXPECT_TRUE(containsText(connection().lastError(), "错误码")) << connection().lastError();
    }

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

    TEST_F(SqliteConnectedMemoryDatabase, BeginAndCommitKeepInsertedRow)
    {
        ASSERT_TRUE(connection().beginTransaction()) << connection().lastError();
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Committed')"), nullptr);
        ASSERT_TRUE(connection().commit()) << connection().lastError();

        EXPECT_EQ(readScalarInteger(connection(), "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(4));
        EXPECT_EQ(connection().lastInsertRowId(), 4);
    }

    TEST_F(SqliteConnectedMemoryDatabase, RollbackDiscardsInsertedRow)
    {
        ASSERT_TRUE(connection().beginTransaction()) << connection().lastError();
        ASSERT_NE(executeRequired(connection(), "INSERT INTO users (name) VALUES ('Discarded')"), nullptr);
        ASSERT_TRUE(connection().rollback()) << connection().lastError();

        // 回滚后计数回到事务前的三条
        EXPECT_EQ(readScalarInteger(connection(), "SELECT COUNT(*) FROM users"), std::optional<std::int64_t>(3));
    }

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

} // namespace AsynGyanis::Database
