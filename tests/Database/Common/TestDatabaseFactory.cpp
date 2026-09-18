// DatabaseFactory 单元测试：按类型建驱动、按配置推断类型、端口猜测与无法判定时的报错。
// 覆盖场景：
// - CreateByTypeReturnsMySqlDriver / CreateByTypeReturnsRedisDriver / CreateByTypeReturnsSqliteDriver
// - CreateByTypeKeepsEveryDriverDisconnected / CreateByTypeHandsConfigurationThroughToDriver / CreateByTypeIgnoresPortInference
// - CreateByTypeThrowsForOutOfRangeEnumValue
// - CreateFromConfigUsesMySqlDefaultPort / CreateFromConfigUsesRedisDefaultPort / CreateFromConfigFallsBackToSqliteWhenPortUnspecified
// - CreateFromConfigRejectsNetworkConfigWithoutPort / CreateFromConfigFallsBackToSqliteWhenOnlyDatabaseNameGiven
// - CreateFromConfigPrefersPortOverDatabasePath / CreateFromConfigThrowsForUnknownPortEvenWithDatabase /
//   CreateFromConfigThrowsWithoutPortAndWithoutDatabase
// - GuessTypeRecognizesMySqlDefaultPort / GuessTypeRecognizesRedisDefaultPort / GuessTypeReturnsEmptyForUnrecognizedPorts
// 工厂只负责「挑驱动」，不负责连接，因此全部用例都在未连接状态下断言：只读 databaseType() / configuration()，
// 不触发任何 IO 与文件系统访问。异常文本只断言「非空 + 含关键子串 + 含本地化（多字节）文案」，不硬编码整句中文，
// 实现文案调整不会连带改坏用例。

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseType.h"
#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{

    using TestSupport::containsLocalizedText;
    namespace
    {
        /// 两个已知默认端口之外的端口，用于驱动「端口未知」分支
        constexpr std::uint16_t kUnknownPort = 46379;

        /// guessType 应当返回空值的端口样本：含未指定哨兵 0、他厂引擎端口与已知端口的邻近值
        const std::vector<std::uint16_t> kUnrecognizedPorts = {0, 80, 1433, 3307, 5432, 6380, 27017, 65535};

        /**
         * @brief 执行一段预期抛 std::invalid_argument 的调用并取回异常文本
         * @tparam Invocation 无参可调用对象
         * @param invocation 触发工厂调用的表达式
         * @return std::string 异常 what() 文本；未抛出时返回空串（调用方据此判失败）
         */
        template<typename Invocation>
        std::string captureInvalidArgumentMessage(Invocation invocation)
        {
            try
            {
                invocation();
            }
            catch (const std::invalid_argument &error)
            {
                return {error.what()};
            }

            return {};
        }
    } // namespace

    // ------------------------------------------------------------------------
    // create(type, config)
    // ------------------------------------------------------------------------

    /** @brief 钉住按显式类型拿到 MySQL 驱动，且连接的 databaseType() 如实回显 */
    TEST(DatabaseFactory, CreateByTypeReturnsMySqlDriver)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::MySql, ConnectionConfig::mySqlDefault());

        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::MySql);
    }

    /** @brief 钉住按显式类型拿到 Redis 驱动，且连接的 databaseType() 如实回显 */
    TEST(DatabaseFactory, CreateByTypeReturnsRedisDriver)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Redis, ConnectionConfig::redisDefault());

        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::Redis);
    }

    /** @brief 钉住按显式类型拿到 SQLite 驱动，且连接的 databaseType() 如实回显 */
    TEST(DatabaseFactory, CreateByTypeReturnsSqliteDriver)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Sqlite, ConnectionConfig::sqliteDefault());

        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
    }

    /** @brief 钉住工厂只挑驱动不建连：三条路径交回的连接都处于未连接状态 */
    TEST(DatabaseFactory, CreateByTypeKeepsEveryDriverDisconnected)
    {
        // 工厂只挑驱动不做连接：返回的连接一律处于未连接状态，connect() 才是发起 IO 的唯一入口
        const std::unique_ptr<DatabaseConnection> mySqlConnection = DatabaseFactory::create(DatabaseType::MySql, ConnectionConfig::mySqlDefault());
        const std::unique_ptr<DatabaseConnection> redisConnection = DatabaseFactory::create(DatabaseType::Redis, ConnectionConfig::redisDefault());
        const std::unique_ptr<DatabaseConnection> sqliteConnection = DatabaseFactory::create(DatabaseType::Sqlite, ConnectionConfig::sqliteDefault());

        EXPECT_FALSE(mySqlConnection->isConnected());
        EXPECT_FALSE(redisConnection->isConnected());
        EXPECT_FALSE(sqliteConnection->isConnected());
    }

    /** @brief 钉住配置对象原样交给驱动，工厂不增删改 host/port/账号/库名任何字段 */
    TEST(DatabaseFactory, CreateByTypeHandsConfigurationThroughToDriver)
    {
        ConnectionConfig configuration;
        configuration.host     = "db.internal";
        configuration.port     = 5555;
        configuration.userName = "svc";
        configuration.password = "secret";
        configuration.database = "analytics";

        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Redis, configuration);

        // 配置由基类登记并原样回显，工厂不增删改任何字段
        const ConnectionConfig &stored = connection->configuration();
        EXPECT_EQ(stored.host, "db.internal");
        EXPECT_EQ(stored.port, 5555);
        EXPECT_EQ(stored.userName, "svc");
        EXPECT_EQ(stored.password, "secret");
        EXPECT_EQ(stored.database, "analytics");
    }

    /** @brief 钉住显式类型优先：不因端口像 MySQL 就把 SQLite 类型带偏 */
    TEST(DatabaseFactory, CreateByTypeIgnoresPortInference)
    {
        // 显式指定类型时不再看端口：MySQL 默认端口 + Sqlite 类型的组合按类型走
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Sqlite, ConnectionConfig::mySqlDefault());

        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
    }

    /** @brief 钉住越界类型枚举按「无法判定」抛出，而不是静默给出默认驱动 */
    TEST(DatabaseFactory, CreateByTypeThrowsForOutOfRangeEnumValue)
    {
        // 三个 case 覆盖完之后没有 default 返回值，越界取值必须抛而不是给出静默默认驱动
        const DatabaseType unknownType = static_cast<DatabaseType>(99);
        const ConnectionConfig configuration = ConnectionConfig::sqliteDefault();

        EXPECT_THROW(static_cast<void>(DatabaseFactory::create(unknownType, configuration)), std::invalid_argument);

        const std::string message = captureInvalidArgumentMessage(
                [&unknownType, &configuration]()
                {
                    static_cast<void>(DatabaseFactory::create(unknownType, configuration));
                });

        EXPECT_FALSE(message.empty());
        EXPECT_TRUE(containsLocalizedText(message)) << message;
        // 异常文本会带上 databaseTypeName 的结果，越界取值即 "Unknown"
        EXPECT_NE(message.find("Unknown"), std::string::npos) << message;
    }

    // ------------------------------------------------------------------------
    // create(config)
    // ------------------------------------------------------------------------

    /** @brief 钉住 3306 按端口推断成 MySQL（guessType 的判据） */
    TEST(DatabaseFactory, CreateFromConfigUsesMySqlDefaultPort)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(ConnectionConfig::mySqlDefault());

        EXPECT_EQ(connection->databaseType(), DatabaseType::MySql);
    }

    /** @brief 钉住 6379 按端口推断成 Redis（guessType 的判据） */
    TEST(DatabaseFactory, CreateFromConfigUsesRedisDefaultPort)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(ConnectionConfig::redisDefault());

        EXPECT_EQ(connection->databaseType(), DatabaseType::Redis);
    }

    /** @brief 钉住 port 为 0 且有库名时按嵌入式 SQLite 处理，库路径原样交给驱动 */
    TEST(DatabaseFactory, CreateFromConfigFallsBackToSqliteWhenPortUnspecified)
    {
        // SQLite 不需要端口：port 为 0 且给了库路径就按嵌入式库处理（不能回退成 MySQL）
        const std::unique_ptr<DatabaseConnection> memoryConnection = DatabaseFactory::create(ConnectionConfig::sqliteDefault());
        const std::unique_ptr<DatabaseConnection> fileConnection = DatabaseFactory::create(ConnectionConfig::sqliteDefault("data/application.db"));

        EXPECT_EQ(memoryConnection->databaseType(), DatabaseType::Sqlite);
        EXPECT_EQ(fileConnection->databaseType(), DatabaseType::Sqlite);
        // 回退只挑驱动，路径同样原样交给驱动，工厂不去打开或校验文件
        EXPECT_EQ(memoryConnection->configuration().database, ":memory:");
        EXPECT_EQ(fileConnection->configuration().database, "data/application.db");
    }

    /** @brief 钉住带网络库特征却缺端口的配置被当场拒绝，不静默回落成同名本地文件 */
    TEST(DatabaseFactory, CreateFromConfigRejectsNetworkConfigWithoutPort)
    {
        // 带网络库特征（host/账号）却只缺端口的配置按「无法判定类型」当场拒绝：多半是写错了配置
        // 而不是想要一个本地库；静默换库（把 MySQL 配置悄悄写成同名本地文件）是最难排查的一类故障。
        // 依据：DatabaseFactory::create() 的 SQLite 回落以「主机/账号/口令全空」为前提
        ConnectionConfig configuration;
        configuration.host     = "127.0.0.1";
        configuration.userName = "root";
        configuration.database = ":memory:";

        EXPECT_THROW(static_cast<void>(DatabaseFactory::create(configuration)), Base::InvalidArgumentException);
    }

    /** @brief 钉住「只想用嵌入式库」的形态（只给库名、无网络特征）才走 SQLite 回落 */
    TEST(DatabaseFactory, CreateFromConfigFallsBackToSqliteWhenOnlyDatabaseNameGiven)
    {
        // 真正「只想用嵌入式库」的形态：只给库名（或 ":memory:"），没有任何网络库特征
        ConnectionConfig configuration;
        configuration.database = ":memory:";

        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(configuration);

        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
    }

    /** @brief 钉住端口可判定时优先于库名字段（Redis 的 database 是键空间编号而非路径） */
    TEST(DatabaseFactory, CreateFromConfigPrefersPortOverDatabasePath)
    {
        // 端口能判定类型时优先按端口走：database 对 Redis 的解释是键空间编号，不是文件路径
        const ConnectionConfig configuration = ConnectionConfig::redisDefault();

        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(configuration);

        EXPECT_EQ(connection->databaseType(), DatabaseType::Redis);
    }

    /** @brief 钉住未知端口一律拒绝并把 offending 端口号写进异常文本，交由调用方显式指定驱动 */
    TEST(DatabaseFactory, CreateFromConfigThrowsForUnknownPortEvenWithDatabase)
    {
        // 未知端口一律抛出、交由调用方显式指定驱动：回退成 MySQL 会把 SQLite 之类误判成永远连不上的 MySQL；
        // SQLite 回退只在 port == 0 且给了库路径时成立
        ConnectionConfig configuration;
        configuration.port     = kUnknownPort;
        configuration.database = "analytics";

        EXPECT_THROW(static_cast<void>(DatabaseFactory::create(configuration)), std::invalid_argument);

        const std::string message = captureInvalidArgumentMessage(
                [&configuration]()
                {
                    static_cast<void>(DatabaseFactory::create(configuration));
                });

        EXPECT_FALSE(message.empty());
        EXPECT_TRUE(containsLocalizedText(message)) << message;
        // 文本里带上 offending 端口号，便于把报错定位回具体配置
        EXPECT_NE(message.find(std::to_string(kUnknownPort)), std::string::npos) << message;
    }

    /** @brief 钉住既无端口也无库名时报「无法判定类型」，不以猜测填充默认驱动 */
    TEST(DatabaseFactory, CreateFromConfigThrowsWithoutPortAndWithoutDatabase)
    {
        // 只有 host 不足以判定类型：既没有可猜的端口，也没有可作为嵌入式库依据的库名
        ConnectionConfig configuration;
        configuration.host = "127.0.0.1";

        EXPECT_THROW(static_cast<void>(DatabaseFactory::create(configuration)), std::invalid_argument);

        const std::string message = captureInvalidArgumentMessage(
                [&configuration]()
                {
                    static_cast<void>(DatabaseFactory::create(configuration));
                });

        EXPECT_FALSE(message.empty());
        EXPECT_TRUE(containsLocalizedText(message)) << message;
        EXPECT_NE(message.find(std::to_string(configuration.port)), std::string::npos) << message;
    }

    // ------------------------------------------------------------------------
    // guessType
    // ------------------------------------------------------------------------

    /** @brief 钉住 guessType 精确识别 MySQL 默认端口 3306 */
    TEST(DatabaseFactory, GuessTypeRecognizesMySqlDefaultPort)
    {
        const std::optional<DatabaseType> guessed = DatabaseFactory::guessType(3306);

        ASSERT_TRUE(guessed.has_value());
        EXPECT_EQ(*guessed, DatabaseType::MySql);
    }

    /** @brief 钉住 guessType 精确识别 Redis 默认端口 6379 */
    TEST(DatabaseFactory, GuessTypeRecognizesRedisDefaultPort)
    {
        const std::optional<DatabaseType> guessed = DatabaseFactory::guessType(6379);

        ASSERT_TRUE(guessed.has_value());
        EXPECT_EQ(*guessed, DatabaseType::Redis);
    }

    /** @brief 钉住 guessType 不做模糊匹配：哨兵 0、他厂端口与已知端口的邻近值都返回空 */
    TEST(DatabaseFactory, GuessTypeReturnsEmptyForUnrecognizedPorts)
    {
        // 只认已实现驱动的默认端口：他厂引擎端口与已知端口的邻近值（3307 / 6380）都不做
        // 「差一点就算」的模糊匹配，未指定哨兵 0 同样返回空值
        for (const std::uint16_t port: kUnrecognizedPorts)
        {
            EXPECT_FALSE(DatabaseFactory::guessType(port).has_value()) << port;
        }
    }

} // namespace AsynGyanis::Database
