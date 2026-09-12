/**
 * @file TestDatabaseFactory.cpp
 * @brief DatabaseFactory 单元测试：按类型建驱动、按配置推断类型、端口猜测与无法判定时的报错
 * @details 工厂只负责「挑驱动」，不负责连接，因此本文件全部用例都在未连接状态下断言：
 *          只读 databaseType() / configuration()，不触发任何 IO 与文件系统访问。
 *          异常文本只断言「非空 + 含关键子串 + 含本地化（多字节）文案」，不硬编码整句中文，
 *          实现文案调整不会连带改坏用例。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseType.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        /// 两个已知默认端口之外的端口，用于驱动「端口未知」分支
        constexpr std::uint16_t kUnknownPort = 46379;

        /// guessType 应当返回空值的端口样本：含未指定哨兵 0、他厂引擎端口与已知端口的邻近值
        /// guessType 应当返回空值的端口样本：含未指定哨兵 0、他厂引擎端口与已知端口的邻近值
        const std::vector<std::uint16_t> kUnrecognizedPorts = {0, 80, 1433, 3307, 5432, 6380, 27017, 65535};

        /**
         * @brief 判断文本是否含非 ASCII 字节，用作「面向使用者的中文文案」的稳定判据
         * @details 不硬编码整句中文：实现文案会随版本演进。只要文本里出现多字节字符，
         *          就说明给出的是本框架的本地化说明，而不是空串或底层库的英文原文。
         * @param text 待判定的文本
         * @return true 至少有一个字节的最高位被置起（UTF-8 多字节序列的特征）
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

    TEST(DatabaseFactory, CreateByTypeReturnsMySqlDriver)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::MySql, ConnectionConfig::mySqlDefault());

        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::MySql);
    }

    TEST(DatabaseFactory, CreateByTypeReturnsRedisDriver)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Redis, ConnectionConfig::redisDefault());

        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::Redis);
    }

    TEST(DatabaseFactory, CreateByTypeReturnsSqliteDriver)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Sqlite, ConnectionConfig::sqliteDefault());

        ASSERT_NE(connection, nullptr);
        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
    }

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

    TEST(DatabaseFactory, CreateByTypeIgnoresPortInference)
    {
        // 显式指定类型时不再看端口：MySQL 默认端口 + Sqlite 类型的组合按类型走
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(DatabaseType::Sqlite, ConnectionConfig::mySqlDefault());

        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
    }

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

    TEST(DatabaseFactory, CreateFromConfigUsesMySqlDefaultPort)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(ConnectionConfig::mySqlDefault());

        EXPECT_EQ(connection->databaseType(), DatabaseType::MySql);
    }

    TEST(DatabaseFactory, CreateFromConfigUsesRedisDefaultPort)
    {
        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(ConnectionConfig::redisDefault());

        EXPECT_EQ(connection->databaseType(), DatabaseType::Redis);
    }

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

    TEST(DatabaseFactory, CreateFromConfigFallsBackToSqliteWithHostButNoPort)
    {
        // 判定条件只看 port 与 database：填了 host 也不会被推成远程库，端口未指定即走嵌入式
        ConnectionConfig configuration;
        configuration.host     = "127.0.0.1";
        configuration.userName = "root";
        configuration.database = ":memory:";

        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(configuration);

        EXPECT_EQ(connection->databaseType(), DatabaseType::Sqlite);
    }

    TEST(DatabaseFactory, CreateFromConfigPrefersPortOverDatabasePath)
    {
        // 端口能判定类型时优先按端口走：database 对 Redis 的解释是键空间编号，不是文件路径
        const ConnectionConfig configuration = ConnectionConfig::redisDefault();

        const std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::create(configuration);

        EXPECT_EQ(connection->databaseType(), DatabaseType::Redis);
    }

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

    TEST(DatabaseFactory, GuessTypeRecognizesMySqlDefaultPort)
    {
        const std::optional<DatabaseType> guessed = DatabaseFactory::guessType(3306);

        ASSERT_TRUE(guessed.has_value());
        EXPECT_EQ(*guessed, DatabaseType::MySql);
    }

    TEST(DatabaseFactory, GuessTypeRecognizesRedisDefaultPort)
    {
        const std::optional<DatabaseType> guessed = DatabaseFactory::guessType(6379);

        ASSERT_TRUE(guessed.has_value());
        EXPECT_EQ(*guessed, DatabaseType::Redis);
    }

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
