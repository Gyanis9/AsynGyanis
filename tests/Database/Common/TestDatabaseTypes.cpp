/**
 * @file TestDatabaseTypes.cpp
 * @brief 数据库类型契约单元测试：类型名映射、统一值类型名映射与连接配置默认值
 * @details 覆盖 Common 层三个纯数据契约：
 *          - databaseTypeName：枚举 → 可读名称，越界取值退化成 "Unknown"；
 *          - databaseValueTypeName：DatabaseValue 八个备选 → 各自的类型名，映射由 std::visit 按实际类型给出；
 *          - ConnectionConfig：默认构造全空，三个 *Default 工厂方法只填本驱动真正读取的字段。
 *          全部用例都不触碰任何驱动与网络，属于零依赖的纯函数断言。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        /// 全部受支持的数据库类型，顺序与 DatabaseType 的声明顺序一致
        const std::vector<DatabaseType> kAllDatabaseTypes = {
                DatabaseType::MySql,
                DatabaseType::Redis,
                DatabaseType::Sqlite,
        };

        /// 合法枚举值与类型名的完整映射表，是 databaseTypeName 的唯一断言依据
        const std::vector<std::pair<DatabaseType, const char *>> kDatabaseTypeNameTable = {
                {DatabaseType::MySql, "MySql"},
                {DatabaseType::Redis, "Redis"},
                {DatabaseType::Sqlite, "Sqlite"},
        };

        /// 超出枚举定义范围的取值：枚举值可能来自反序列化或内存被写坏，用于驱动 default 分支
        /// @note 起点必须等于枚举成员个数：新增成员后要把它上移，否则会把新成员当成越界值
        const std::vector<int> kOutOfRangeDatabaseTypeValues = {3, 4, 99, 127, 255, -1};

        /**
         * @brief 构造覆盖全部七个备选的数据库值样本
         * @return std::vector<std::pair<DatabaseValue, const char *>> 值与其期望类型名的配对
         */
        std::vector<std::pair<DatabaseValue, const char *>> makeValueTypeNameTable()
        {
            std::unordered_map<std::string, std::string> hashValue;
            hashValue.emplace("field", "value");

            return {
                    {DatabaseValue{std::monostate{}}, "Null"},
                    {DatabaseValue{true}, "Bool"},
                    {DatabaseValue{std::int64_t{42}}, "Int64"},
                    {DatabaseValue{3.5}, "Double"},
                    {DatabaseValue{std::string("text")}, "String"},
                    {DatabaseValue{std::vector<std::string>{"first", "second"}}, "List"},
                    {DatabaseValue{hashValue}, "Hash"},
                    {DatabaseValue{BinaryBytes{0x5C, 0x00, 0x41}}, "Bytes"},
            };
        }
    } // namespace

    // ------------------------------------------------------------------------
    // databaseTypeName
    // ------------------------------------------------------------------------

    TEST(DatabaseType, MapsEverySupportedTypeToItsOwnName)
    {
        for (const auto &[type, expectedName]: kDatabaseTypeNameTable)
        {
            EXPECT_STREQ(databaseTypeName(type), expectedName);
        }
    }

    TEST(DatabaseType, ReturnsUnknownForOutOfRangeEnumValues)
    {
        // default 分支存在的意义就是「不崩溃、给出可读名称」，因此越界取值也必须返回字面量
        for (const int rawValue: kOutOfRangeDatabaseTypeValues)
        {
            EXPECT_STREQ(databaseTypeName(static_cast<DatabaseType>(rawValue)), "Unknown");
        }
    }

    TEST(DatabaseType, KeepsNamesDistinctAcrossSupportedTypes)
    {
        // 名称会被写进日志与异常文本，两个类型撞名会让排查方向直接错掉
        for (const DatabaseType leftType: kAllDatabaseTypes)
        {
            for (const DatabaseType rightType: kAllDatabaseTypes)
            {
                if (leftType == rightType)
                {
                    continue;
                }

                EXPECT_STRNE(databaseTypeName(leftType), databaseTypeName(rightType));
            }
        }
    }

    // ------------------------------------------------------------------------
    // databaseValueTypeName
    // ------------------------------------------------------------------------

    TEST(DatabaseValue, NamesEmptyValueAsNull)
    {
        const DatabaseValue nullValue{std::monostate{}};

        EXPECT_STREQ(databaseValueTypeName(nullValue), "Null");
    }

    TEST(DatabaseValue, NamesBooleanValueAsBool)
    {
        // 真与假共用同一个备选，类型名与具体取值无关
        const DatabaseValue trueValue{true};
        const DatabaseValue falseValue{false};

        EXPECT_STREQ(databaseValueTypeName(trueValue), "Bool");
        EXPECT_STREQ(databaseValueTypeName(falseValue), "Bool");
    }

    TEST(DatabaseValue, NamesIntegerValueAsInt64)
    {
        const DatabaseValue integerValue{std::int64_t{-9223372036854775807LL}};

        EXPECT_STREQ(databaseValueTypeName(integerValue), "Int64");
    }

    TEST(DatabaseValue, NamesDoubleValueAsDouble)
    {
        const DatabaseValue doubleValue{0.0};

        EXPECT_STREQ(databaseValueTypeName(doubleValue), "Double");
    }

    TEST(DatabaseValue, NamesStringValueAsString)
    {
        // 空串代表「驱动给出的零长度文本」，与「没有值」的 monostate 必须区分开
        const DatabaseValue textValue{std::string("payload")};
        const DatabaseValue emptyTextValue{std::string{}};

        EXPECT_STREQ(databaseValueTypeName(textValue), "String");
        EXPECT_STREQ(databaseValueTypeName(emptyTextValue), "String");
    }

    TEST(DatabaseValue, NamesStringListValueAsList)
    {
        const DatabaseValue listValue{std::vector<std::string>{}};

        EXPECT_STREQ(databaseValueTypeName(listValue), "List");
    }

    TEST(DatabaseValue, NamesStringHashMapValueAsHash)
    {
        const DatabaseValue hashValue{std::unordered_map<std::string, std::string>{}};

        EXPECT_STREQ(databaseValueTypeName(hashValue), "Hash");
    }

    TEST(DatabaseValue, NamesBinaryValueAsBytes)
    {
        // 零长二进制也要报 Bytes 而不是 Null：空 BLOB 与 SQL NULL 是两件事
        const DatabaseValue byteValue{BinaryBytes{0x5C, 0x00, 0x41}};
        const DatabaseValue emptyByteValue{BinaryBytes{}};

        EXPECT_STREQ(databaseValueTypeName(byteValue), "Bytes");
        EXPECT_STREQ(databaseValueTypeName(emptyByteValue), "Bytes");
    }

    TEST(DatabaseValue, KeepsNamesDistinctAcrossAllAlternatives)
    {
        // 映射走的是 std::visit + is_same_v：一旦某条分支漏写或写错类型，这里会立刻撞名
        const std::vector<std::pair<DatabaseValue, const char *>> typeNameTable = makeValueTypeNameTable();
        for (std::size_t leftIndex = 0; leftIndex < typeNameTable.size(); ++leftIndex)
        {
            for (std::size_t rightIndex = leftIndex + 1; rightIndex < typeNameTable.size(); ++rightIndex)
            {
                EXPECT_STRNE(databaseValueTypeName(typeNameTable[leftIndex].first),
                             databaseValueTypeName(typeNameTable[rightIndex].first));
            }
        }
    }

    TEST(DatabaseValue, GivesEveryAlternativeTheDocumentedTypeName)
    {
        for (const auto &[value, expectedName]: makeValueTypeNameTable())
        {
            EXPECT_STREQ(databaseValueTypeName(value), expectedName);
        }
    }

    // ------------------------------------------------------------------------
    // ConnectionConfig
    // ------------------------------------------------------------------------

    TEST(ConnectionConfig, DefaultConstructionLeavesEveryFieldUnset)
    {
        const ConnectionConfig configuration;

        // port 为 0 表示「未指定」，是工厂判定 SQLite 回退的依据，不能改成别的哨兵值
        EXPECT_TRUE(configuration.host.empty());
        EXPECT_EQ(configuration.port, 0);
        EXPECT_TRUE(configuration.userName.empty());
        EXPECT_TRUE(configuration.password.empty());
        EXPECT_TRUE(configuration.database.empty());
    }

    TEST(ConnectionConfig, MySqlDefaultTargetsLocalServerOnStandardPort)
    {
        const ConnectionConfig configuration = ConnectionConfig::mySqlDefault();

        // 3306 同时是 DatabaseFactory::guessType 的判据，改动这里要同步核对工厂用例
        EXPECT_EQ(configuration.host, "127.0.0.1");
        EXPECT_EQ(configuration.port, 3306);
    }

    TEST(ConnectionConfig, MySqlDefaultUsesRootAccountOnTestSchema)
    {
        const ConnectionConfig configuration = ConnectionConfig::mySqlDefault();

        EXPECT_EQ(configuration.userName, "root");
        EXPECT_EQ(configuration.database, "test");
        // 默认配置不携带密码：本机开发库通常免密，把空密码写死成明文反而危险
        EXPECT_TRUE(configuration.password.empty());
    }

    TEST(ConnectionConfig, RedisDefaultTargetsLocalServerOnStandardPort)
    {
        const ConnectionConfig configuration = ConnectionConfig::redisDefault();

        EXPECT_EQ(configuration.host, "127.0.0.1");
        EXPECT_EQ(configuration.port, 6379);
    }

    TEST(ConnectionConfig, RedisDefaultLeavesCredentialAndKeySpaceEmpty)
    {
        const ConnectionConfig configuration = ConnectionConfig::redisDefault();

        // password 为空即「不发 AUTH」，database 为空即「不 SELECT」，两者都是驱动显式读取的开关
        EXPECT_TRUE(configuration.userName.empty());
        EXPECT_TRUE(configuration.password.empty());
        EXPECT_TRUE(configuration.database.empty());
    }

    TEST(ConnectionConfig, SqliteDefaultOpensInMemoryDatabaseWithoutHostOrPort)
    {
        const ConnectionConfig configuration = ConnectionConfig::sqliteDefault();

        // SQLite 是嵌入式引擎：host/port 保持未设置，驱动会忽略它们
        EXPECT_EQ(configuration.database, ":memory:");
        EXPECT_TRUE(configuration.host.empty());
        EXPECT_EQ(configuration.port, 0);
        EXPECT_TRUE(configuration.userName.empty());
        EXPECT_TRUE(configuration.password.empty());
    }

    TEST(ConnectionConfig, SqliteDefaultAcceptsCustomDatabasePath)
    {
        const ConnectionConfig configuration = ConnectionConfig::sqliteDefault("data/application.db");

        EXPECT_EQ(configuration.database, "data/application.db");
        EXPECT_EQ(configuration.port, 0);
    }

    TEST(ConnectionConfig, SqliteDefaultWithEmptyPathLeavesDatabaseEmpty)
    {
        // 构造函数不做任何校验：空路径会退化成「没有库名」的配置，
        // 正是 DatabaseFactory::create(config) 判定「无法推断类型」并抛出的那种输入
        const ConnectionConfig configuration = ConnectionConfig::sqliteDefault(std::string{});

        EXPECT_TRUE(configuration.database.empty());
        EXPECT_EQ(configuration.port, 0);
    }

} // namespace AsynGyanis::Database
