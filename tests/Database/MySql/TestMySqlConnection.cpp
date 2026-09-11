/**
 * @file TestMySqlConnection.cpp
 * @brief MySqlConnection 单元测试：MySQL 报错桩的失败语义与驱动无关的连接骨架
 * @details 本构建里 CMake 选项 DATABASE_WITH_MYSQL 默认 OFF，因此未定义 DATABASE_HAS_MYSQL，
 *          MySqlConnection 编出来的是「每个入口都把失败写清楚」的报错桩：
 *          connect() 恒为 false、execute() 恒为 nullptr、isConnected() 恒为 false，
 *          并把「当前构建未编译 MySQL 驱动」写进 lastError()。
 *          为了让本文件在 -DDATABASE_WITH_MYSQL=ON 时也不产生假失败：
 *          - 只依赖「未连接」这一前提的断言（配置回显、超时往返、未连接时的失败路径）无条件执行，
 *            真实驱动在构造阶段既不分配句柄也不做 IO，这些断言在两种构建下同义；
 *          - 只有「桩专属」的断言（connect 恒假、报错文案指向缺失驱动）在真实驱动构建下 GTEST_SKIP。
 *          异常文本一律只断言「非空 + 含关键子串 + 含本地化文案」，不硬编码整句中文。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/MySql/MySqlConnection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace AsynGyanis::Database
{
    namespace
    {
#ifdef DATABASE_HAS_MYSQL
        /// 当前构建是否编译了真实的 libmysqlclient 驱动
        constexpr bool kMySqlDriverCompiled = true;
#else
        /// 当前构建为报错桩：桩语义用例才会执行
        constexpr bool kMySqlDriverCompiled = false;
#endif

        /// 基类 DatabaseConnection 声明的连接超时默认毫秒数
        constexpr int kDefaultConnectTimeoutMilliseconds = 5000;

        /// 基类 DatabaseConnection 声明的单条命令执行超时默认毫秒数
        constexpr int kDefaultQueryTimeoutMilliseconds = 30000;

        /**
         * @brief 判断文本是否含非 ASCII 字节，用作「面向使用者的中文文案」的稳定判据
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
    } // namespace

    // ------------------------------------------------------------------------
    // 与是否编译真实驱动无关的骨架行为
    // ------------------------------------------------------------------------

    TEST(MySqlConnection, ReportsMySqlDatabaseType)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // databaseType() 不依赖连接状态，桩构建下同样返回本类型
        EXPECT_EQ(connection.databaseType(), DatabaseType::MySql);
        EXPECT_STREQ(databaseTypeName(connection.databaseType()), "MySql");
    }

    TEST(MySqlConnection, ConfigurationEchoesConstructorArgument)
    {
        ConnectionConfig configuration;
        configuration.host     = "mysql.internal";
        configuration.port     = 3307;
        configuration.userName = "reporter";
        configuration.password = "p@ss%word";
        configuration.database = "warehouse";

        const MySqlConnection connection(configuration);

        // 构造阶段只登记配置：五个字段全部原样回显，驱动不做任何裁剪或补默认值
        const ConnectionConfig &stored = connection.configuration();
        EXPECT_EQ(stored.host, "mysql.internal");
        EXPECT_EQ(stored.port, 3307);
        EXPECT_EQ(stored.userName, "reporter");
        EXPECT_EQ(stored.password, "p@ss%word");
        EXPECT_EQ(stored.database, "warehouse");
    }

    TEST(MySqlConnection, ConfigurationDefaultsAreEchoedWithoutRewrite)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 工厂方法给出的默认值不因驱动而变（port 0 才会被客户端库回落到 3306，这里显式给了端口）
        EXPECT_EQ(connection.configuration().host, "127.0.0.1");
        EXPECT_EQ(connection.configuration().port, 3306);
        EXPECT_EQ(connection.configuration().database, "test");
    }

    TEST(MySqlConnection, TimeoutsStartWithDocumentedDefaults)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
        EXPECT_EQ(connection.queryTimeout(), kDefaultQueryTimeoutMilliseconds);
    }

    TEST(MySqlConnection, ConnectTimeoutSetterRoundTripsBeforeConnect)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        connection.setConnectTimeout(1200);
        EXPECT_EQ(connection.connectTimeout(), 1200);

        // 边界：0 与负值由驱动折算成「不超时」，getter 仍如实回显写入值（校验发生在 connect 阶段）
        connection.setConnectTimeout(0);
        EXPECT_EQ(connection.connectTimeout(), 0);
        connection.setConnectTimeout(-1);
        EXPECT_EQ(connection.connectTimeout(), -1);
    }

    TEST(MySqlConnection, QueryTimeoutSetterRoundTripsIndependently)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        connection.setQueryTimeout(750);

        // 两个超时是彼此独立的存储，改一个不应影响另一个
        EXPECT_EQ(connection.queryTimeout(), 750);
        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
    }

    TEST(MySqlConnection, StartsDisconnectedWithoutAnyErrorRecorded)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_TRUE(connection.lastError().empty());
        // 构造本身不会失败，因此不提前占用 lastError()
        EXPECT_TRUE(connection.serverVersion().empty());
    }

    TEST(MySqlConnection, ConnectWithEmptyHostFailsWithoutServerContact)
    {
        ConnectionConfig configuration = ConnectionConfig::mySqlDefault();
        configuration.host = "";

        MySqlConnection connection(configuration);

        // 空主机在两种构建下都必须当场失败：真实驱动把它挡在 mysql_init 之前，不产生网络往返
        const auto startedAt = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.connect());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_LT(elapsed.count(), 1000);
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
    }

    TEST(MySqlConnection, ExecuteWithoutConnectionReturnsNullResult)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT 1");

        // 基类契约：失败一律交回空指针，原因写进 lastError()，调用方只判空指针即可发现问题
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("MySQL"), std::string::npos) << connection.lastError();
    }

    TEST(MySqlConnection, DisconnectWithoutConnectionIsSafe)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 未连接且无残留句柄时是安全的空操作，析构函数会无条件走这条路径
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_FALSE(connection.isConnected());
    }

    TEST(MySqlConnection, DisconnectKeepsLastFailureReason)
    {
        ConnectionConfig configuration = ConnectionConfig::mySqlDefault();
        configuration.host = "";

        MySqlConnection connection(configuration);
        ASSERT_FALSE(connection.connect());
        const std::string failureReason = connection.lastError();
        ASSERT_FALSE(failureReason.empty());

        connection.disconnect();

        // disconnect() 刻意不清 lastError：失败路径上先写好的根因不能被断开动作抹掉
        EXPECT_EQ(connection.lastError(), failureReason);
        EXPECT_FALSE(connection.isConnected());
    }

    TEST(MySqlConnection, ServerVersionIsEmptyWhileDisconnected)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 与 SQLite 驱动不同：MySQL 的服务端版本必须持有已连接句柄才有值
        EXPECT_TRUE(connection.serverVersion().empty());
    }

    TEST(MySqlConnection, TransactionHelpersFailWhileDisconnected)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 三个便捷封装都走 execute()，未连接时按同一约定返回 false
        EXPECT_FALSE(connection.beginTransaction());
        EXPECT_FALSE(connection.commit());
        EXPECT_FALSE(connection.rollback());
        EXPECT_FALSE(connection.lastError().empty());
    }

    // ------------------------------------------------------------------------
    // 报错桩专属语义（未编译 libmysqlclient 时）
    // ------------------------------------------------------------------------

    TEST(MySqlConnection, ConnectFailsWithMissingDriverReason)
    {
        if (kMySqlDriverCompiled)
        {
            GTEST_SKIP() << "当前构建已编译 MySQL 驱动，报错桩语义不适用";
        }

        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 桩必须明确拒绝，绝不能让「什么都没做」被误当成连接成功
        EXPECT_FALSE(connection.connect());
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    TEST(MySqlConnection, ConnectReportsChineseMissingDriverHint)
    {
        if (kMySqlDriverCompiled)
        {
            GTEST_SKIP() << "当前构建已编译 MySQL 驱动，报错桩语义不适用";
        }

        MySqlConnection connection(ConnectionConfig::mySqlDefault());
        ASSERT_FALSE(connection.connect());

        // 只断言关键子串：整句文案（含括号里的库名）可能随版本调整
        const std::string reason = connection.lastError();
        EXPECT_FALSE(reason.empty());
        EXPECT_TRUE(containsLocalizedText(reason)) << reason;
        EXPECT_NE(reason.find("MySQL"), std::string::npos) << reason;
        EXPECT_NE(reason.find("驱动"), std::string::npos) << reason;
    }

    TEST(MySqlConnection, EveryExecuteRewritesMissingDriverReason)
    {
        if (kMySqlDriverCompiled)
        {
            GTEST_SKIP() << "当前构建已编译 MySQL 驱动，报错桩语义不适用";
        }

        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 空命令与正常命令在桩里没有区别：每次调用都重新给出「驱动缺失」，不静默 no-op
        EXPECT_EQ(connection.execute(""), nullptr);
        EXPECT_NE(connection.lastError().find("驱动"), std::string::npos) << connection.lastError();
        EXPECT_EQ(connection.execute("SELECT 1"), nullptr);
        EXPECT_NE(connection.lastError().find("驱动"), std::string::npos) << connection.lastError();
    }

    TEST(MySqlConnection, ConstructingAndDestroyingManyStubConnectionsIsSafe)
    {
        if (kMySqlDriverCompiled)
        {
            GTEST_SKIP() << "当前构建已编译 MySQL 驱动，桩的无资源生命周期不适用";
        }

        // 桩不分配任何句柄，反复构造 + connect + 析构不应留下资源，也不应抛异常
        for (int round = 0; round < 32; ++round)
        {
            MySqlConnection connection(ConnectionConfig::mySqlDefault());
            static_cast<void>(connection.connect());
            EXPECT_FALSE(connection.isConnected());
        }
    }

} // namespace AsynGyanis::Database
