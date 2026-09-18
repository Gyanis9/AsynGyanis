// 覆盖场景（两种构建配置下同义：libmysqlclient 可用时是真实驱动，否则是报错桩）：
// - 与驱动无关的连接骨架：databaseType、配置回显、超时默认值与 setter 往返
// - 未连接 / 空主机 / 未监听端口的失败路径：有界返回、原因中文且点明 MySQL、句柄不残留
// - 未连接时各 execute 重载与事务便捷封装一律明确失败，不静默丢弃参数或按 NULL 执行
// 真实驱动专属的行为（参数个数不匹配、真实查询结果、serverVersion()）需要可用服务端，不在此文件断言。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/MySql/MySqlConnection.h"
#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{

    using TestSupport::containsLocalizedText;
    namespace
    {
#ifdef DATABASE_HAS_MYSQL
        /// 当前构建是否编译了真实的 libmysqlclient 驱动
        constexpr bool kMySqlDriverCompiled = true;
#else
        /// 当前构建为报错桩：桩语义下的附加断言才会执行
        constexpr bool kMySqlDriverCompiled = false;
#endif

        /// 基类 DatabaseConnection 声明的连接超时默认毫秒数
        constexpr int kDefaultConnectTimeoutMilliseconds = 5000;

        /// 基类 DatabaseConnection 声明的单条命令执行超时默认毫秒数
        constexpr int kDefaultQueryTimeoutMilliseconds = 30000;

        /// 离线建连用例使用的连接超时毫秒数：够短不拖慢测试，又够长不被调度抖动误判
        constexpr int kShortConnectTimeoutMilliseconds = 200;

        /// 单条离线建连用例允许的最长耗时毫秒数，是「不挂死」的硬上界。
        /// MySQL 客户端只接受整秒的 MYSQL_OPT_CONNECT_TIMEOUT（200 毫秒向上取整成 1 秒），
        /// 而某些环境下回环的未监听端口不会立刻回 RST（SYN 被丢弃），一次尝试因此要等满该超时，
        /// 这里给 1 秒的超时留出调度余量
        constexpr long long kMaximumOfflineCallMilliseconds = 3000;

        /**
         * @brief 本机上一个确定没有监听的端口
         * @details 取注册端口区间内、远离 MySQL(3306) 与系统临时端口段（Linux 32768+ / Windows 49152+）的编号：
         *          既不会被本机服务占用，也不会被系统当作源端口分配出去；该端口上恰好有别的服务在监听时，
         *          MySQL 握手仍必然失败，用例结论不变
         */
        constexpr std::uint16_t kUnmonitoredPort = 16391;

        /**
         * @brief 构造一份指向本机未监听端口的离线配置
         * @return ConnectionConfig host 为本机回环、port 为未监听端口
         */
        ConnectionConfig makeOfflineConfiguration()
        {
            ConnectionConfig configuration;
            configuration.host = "127.0.0.1";
            configuration.port = kUnmonitoredPort;
            return configuration;
        }

    } // namespace

    // ------------------------------------------------------------------------
    // 与是否编译真实驱动无关的骨架行为
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住 databaseType() 不依赖连接状态，桩构建下同样返回 MySql
     */
    TEST(MySqlConnection, ReportsMySqlDatabaseType)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // databaseType() 不依赖连接状态，桩构建下同样返回本类型
        EXPECT_EQ(connection.databaseType(), DatabaseType::MySql);
        EXPECT_STREQ(databaseTypeName(connection.databaseType()), "MySql");
    }

    /**
     * @brief 钉住构造阶段只登记配置：五个字段原样回显，驱动不做裁剪或补默认值
     */
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

    /**
     * @brief 钉住工厂默认值不因驱动而改写（host/port/database 三项）
     */
    TEST(MySqlConnection, ConfigurationDefaultsAreEchoedWithoutRewrite)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 工厂方法给出的默认值不因驱动而变（port 0 才会被客户端库回落到 3306，这里显式给了端口）
        EXPECT_EQ(connection.configuration().host, "127.0.0.1");
        EXPECT_EQ(connection.configuration().port, 3306);
        EXPECT_EQ(connection.configuration().database, "test");
    }

    /**
     * @brief 钉住基类声明的连接/查询超时默认值原样生效
     */
    TEST(MySqlConnection, TimeoutsStartWithDocumentedDefaults)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
        EXPECT_EQ(connection.queryTimeout(), kDefaultQueryTimeoutMilliseconds);
    }

    /**
     * @brief 钉住连接超时 setter 往返，含 0 与负值由 getter 如实回显（校验推迟到 connect）
     */
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

    /**
     * @brief 钉住两个超时彼此独立存储，改一个不影响另一个
     */
    TEST(MySqlConnection, QueryTimeoutSetterRoundTripsIndependently)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        connection.setQueryTimeout(750);

        // 两个超时是彼此独立的存储，改一个不应影响另一个
        EXPECT_EQ(connection.queryTimeout(), 750);
        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
    }

    /**
     * @brief 钉住未连接初值：无句柄、无错误、无服务端版本
     */
    TEST(MySqlConnection, StartsDisconnectedWithoutAnyErrorRecorded)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_TRUE(connection.lastError().empty());
        // 构造本身不会失败，因此不提前占用 lastError()
        EXPECT_TRUE(connection.serverVersion().empty());
    }

    /**
     * @brief 钉住空主机在本地被拒：有界返回、不产生网络往返、原因点明 MySQL
     */
    TEST(MySqlConnection, ConnectWithEmptyHostFailsWithoutServerContact)
    {
        ConnectionConfig configuration = ConnectionConfig::mySqlDefault();
        configuration.host = "";

        MySqlConnection connection(configuration);

        // 空主机在两种构建下都必须当场失败：真实驱动把它挡在 mysql_init 之前，不产生网络往返
        const auto startedAt = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.connect());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_LT(elapsed.count(), kMaximumOfflineCallMilliseconds);
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("MySQL"), std::string::npos) << connection.lastError();
    }

    /**
     * @brief 钉住未连接时 execute() 按基类契约返回 nullptr 并写中文原因
     */
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

    /**
     * @brief 钉住参数化路径在未连接时明确失败，不静默丢参数或按 NULL 执行
     */
    TEST(MySqlConnection, ParameterizedExecuteWithoutConnectionReturnsNullResult)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());
        const std::vector<DatabaseValue> parameters{std::int64_t{1}};

        const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT ?", parameters);

        // 参数化路径同样必须在未连接时明确失败，而不是静默把参数丢掉或按 NULL 执行。
        // 注意这里验证的是「未连接」这一条判定：参数个数与占位符个数是否匹配要在
        // prepare 之后（需要服务端）才知道，属于服务端依赖路径
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("MySQL"), std::string::npos) << connection.lastError();
    }

    /**
     * @brief 钉住未连接时 disconnect() 是幂等空操作，析构依赖这条路径
     */
    TEST(MySqlConnection, DisconnectWithoutConnectionIsSafe)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 未连接且无残留句柄时是安全的空操作，析构函数会无条件走这条路径
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_FALSE(connection.isConnected());
    }

    /**
     * @brief 钉住 disconnect() 不清上一条失败根因，失败原因不会被断开动作抹掉
     */
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

    /**
     * @brief 钉住未连接时 serverVersion() 为空：MySQL 的服务端版本必须持有已连接句柄才有值
     */
    TEST(MySqlConnection, ServerVersionIsEmptyWhileDisconnected)
    {
        const MySqlConnection connection(ConnectionConfig::mySqlDefault());

        // 与 SQLite 驱动不同：MySQL 的服务端版本必须持有已连接句柄才有值。
        // 真实取值需要可用的服务端，本用例只断言未连接时的约定
        EXPECT_TRUE(connection.serverVersion().empty());
    }

    /**
     * @brief 钉住 beginTransaction/commit/rollback 未连接时按同一约定失败并写原因
     */
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
    // 指向未监听端口的离线建连路径（真实驱动与报错桩都必须失败）
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住指向未监听端口的 connect() 有界失败、句柄不残留且可安全再断开
     */
    TEST(MySqlConnection, ConnectToUnmonitoredLocalPortFailsWithinTimeout)
    {
        MySqlConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        // 回环上无人监听时立刻收到连接拒绝；超时设置只是环境异常时的上界保险
        const auto startedAt = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.connect());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_LT(elapsed.count(), kMaximumOfflineCallMilliseconds);
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("MySQL"), std::string::npos) << connection.lastError();

        // 失败路径必须先摘错误文本再释放句柄：走到这里句柄已置空，重复断开是安全的空操作
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    /**
     * @brief 钉住失败原因中文且点明 MySQL，并按构建形态分别指出驱动缺失或客户端错误码
     */
    TEST(MySqlConnection, ConnectFailureKeepsLocalizedReasonAndStaysDisconnected)
    {
        MySqlConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        ASSERT_FALSE(connection.connect());
        const std::string failureReason = connection.lastError();

        // 面向使用者的文本一律中文，并点明是哪个驱动出的问题
        EXPECT_TRUE(containsLocalizedText(failureReason)) << failureReason;
        EXPECT_NE(failureReason.find("MySQL"), std::string::npos) << failureReason;

        // 未编译客户端库时，原因文本必须明确指出「驱动缺失」而不是含糊的连不上；
        // 编译了驱动时失败来自真实的建连调用，文本里带客户端库原文与错误码
        if (!kMySqlDriverCompiled)
        {
            EXPECT_NE(failureReason.find("驱动"), std::string::npos) << failureReason;
        }
        else
        {
            EXPECT_NE(failureReason.find("错误码"), std::string::npos) << failureReason;
        }

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    // ------------------------------------------------------------------------
    // 反复建连/析构不泄漏、不崩溃
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住反复失败重连不泄漏句柄、不留下已连接状态
     */
    TEST(MySqlConnection, RepeatedOfflineConnectAttemptsAreSafe)
    {
        // 每次失败都必须把句柄释放干净：真实驱动会在失败路径上 mysql_close 并置空，
        // 桩本就不分配句柄。反复走一遍用于发现「失败路径漏释放」这类缺陷。
        // 轮数刻意压小：每次尝试都要等满 1 秒的建连超时（见 kMaximumOfflineCallMilliseconds 的说明）
        for (int round = 0; round < 4; ++round)
        {
            MySqlConnection connection(makeOfflineConfiguration());
            connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

            EXPECT_FALSE(connection.connect());
            EXPECT_FALSE(connection.isConnected());
            EXPECT_EQ(connection.nativeHandle(), nullptr);
        }
    }

    /**
     * @brief 钉住空命令/普通命令/参数化命令三条执行入口在未连接时都明确失败并给出原因
     */
    TEST(MySqlConnection, EveryExecuteOverloadKeepsFailingWhileDisconnected)
    {
        MySqlConnection connection(ConnectionConfig::mySqlDefault());
        const std::vector<DatabaseValue> parameters{std::int64_t{7}};

        // 空命令、正常命令与参数化命令在未连接时都必须明确失败，每次调用都给出原因而非静默 no-op
        EXPECT_EQ(connection.execute(""), nullptr);
        const std::string emptyCommandReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(emptyCommandReason)) << emptyCommandReason;

        EXPECT_EQ(connection.execute("SELECT 1"), nullptr);
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();

        EXPECT_EQ(connection.execute("SELECT ?", parameters), nullptr);
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();

        // 未编译驱动时三条路径都应点出「驱动缺失」这一根因
        if (!kMySqlDriverCompiled)
        {
            EXPECT_NE(emptyCommandReason.find("驱动"), std::string::npos) << emptyCommandReason;
        }
    }

} // namespace AsynGyanis::Database
