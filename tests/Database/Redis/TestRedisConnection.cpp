/**
 * @file TestRedisConnection.cpp
 * @brief RedisConnection 单元测试：真实 hiredis 驱动在「无可用 Redis 服务」下的离线行为
 * @details 本构建已找到 hiredis（DATABASE_HAS_REDIS），因此被测对象是真实驱动而不是报错桩；
 *          但测试环境没有可用的 Redis 服务，所以本文件只覆盖离线可达的路径：
 *          未连接状态的初值、配置回显、超时设置往返、connect() 对未监听端口的有界失败、
 *          未连接时各执行入口返回 nullptr 并写中文原因、管道登记与丢弃、disconnect() 幂等。
 *          任何需要真实服务端的语义（AUTH、SELECT、切词后的命令回包、管道回复对齐）都不在此断言。
 *          超时统一设成百毫秒量级并用 steady_clock 判定上限，保证单条用例远小于 1 秒：
 *          回环地址上没有监听端口会立刻收到 RST，超时只是防止环境异常时挂死的保险。
 *          错误文案只断言「非空 + 含关键子串 + 含本地化文案」，不硬编码整句中文。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Redis/RedisConnection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
#ifdef DATABASE_HAS_REDIS
        /// 当前构建是否编译了真实的 hiredis 驱动
        constexpr bool kRedisDriverCompiled = true;
#else
        /// 当前构建为报错桩：依赖真实驱动语义的用例据此跳过
        constexpr bool kRedisDriverCompiled = false;
#endif

        /// 基类 DatabaseConnection 声明的连接超时默认毫秒数
        constexpr int kDefaultConnectTimeoutMilliseconds = 5000;

        /// 基类 DatabaseConnection 声明的单条命令执行超时默认毫秒数
        constexpr int kDefaultQueryTimeoutMilliseconds = 30000;

        /// 离线建连用例使用的连接超时毫秒数：够短不拖慢测试，又够长不被调度抖动误判
        constexpr int kShortConnectTimeoutMilliseconds = 200;

        /// 单条离线网络用例允许的最长耗时毫秒数，是「不挂死」的硬上界
        constexpr long long kMaximumOfflineCallMilliseconds = 1000;

        /**
         * @brief 本机上一个确定没有监听的端口
         * @details 取注册端口区间内、远离 Redis(6379)/Sentinel(26379)/MySQL(3306) 等常见服务，
         *          且在 Linux(32768+) 与 Windows(49152+) 临时端口范围之下，
         *          因此既不会被本机服务占用，也不会被系统当作源端口分配出去。
         *          若某台机器确实在此端口起了服务，本文件依赖拒绝的用例会误报，换端口即可。
         */
        constexpr std::uint16_t kUnmonitoredPort = 16390;

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
    // 未连接状态的初值与配置
    // ------------------------------------------------------------------------

    TEST(RedisConnection, ReportsRedisDatabaseType)
    {
        const RedisConnection connection(ConnectionConfig::redisDefault());

        // databaseType() 不依赖连接状态，桩构建下同样是这个语义
        EXPECT_EQ(connection.databaseType(), DatabaseType::Redis);
        EXPECT_STREQ(databaseTypeName(connection.databaseType()), "Redis");
    }

    TEST(RedisConnection, ConfigurationEchoesConstructorArgument)
    {
        ConnectionConfig configuration;
        configuration.host     = "redis.internal";
        configuration.port     = 6380;
        configuration.userName = "acl-user";
        configuration.password = "p@ss%word";
        configuration.database = "3"; ///< Redis 把该字段解释为键空间编号

        const RedisConnection connection(configuration);

        // 构造阶段零 IO：五个字段原样回显，键空间编号不被解析成整数
        const ConnectionConfig &stored = connection.configuration();
        EXPECT_EQ(stored.host, "redis.internal");
        EXPECT_EQ(stored.port, 6380);
        EXPECT_EQ(stored.userName, "acl-user");
        EXPECT_EQ(stored.password, "p@ss%word");
        EXPECT_EQ(stored.database, "3");
    }

    TEST(RedisConnection, StartsDisconnectedWithoutAnyErrorRecorded)
    {
        const RedisConnection connection(ConnectionConfig::redisDefault());

        // isConnected() 的双判据（标志 + 上下文）在构造后都应为假，且没有残留错误
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_TRUE(connection.lastError().empty());
    }

    TEST(RedisConnection, TimeoutsStartWithDocumentedDefaults)
    {
        const RedisConnection connection(ConnectionConfig::redisDefault());

        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
        EXPECT_EQ(connection.queryTimeout(), kDefaultQueryTimeoutMilliseconds);
    }

    TEST(RedisConnection, ConnectTimeoutSetterRoundTripsBeforeConnect)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);
        EXPECT_EQ(connection.connectTimeout(), kShortConnectTimeoutMilliseconds);

        // 0 与负值由驱动折算成「不设超时」，getter 仍如实回显写入值（校验发生在 connect 阶段）
        connection.setConnectTimeout(0);
        EXPECT_EQ(connection.connectTimeout(), 0);
        connection.setConnectTimeout(-1);
        EXPECT_EQ(connection.connectTimeout(), -1);
    }

    TEST(RedisConnection, QueryTimeoutSetterRoundTripsIndependently)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        connection.setQueryTimeout(750);

        // 两个超时是彼此独立的存储，改一个不应影响另一个
        EXPECT_EQ(connection.queryTimeout(), 750);
        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
    }

    // ------------------------------------------------------------------------
    // connect() 的离线失败路径
    // ------------------------------------------------------------------------

    TEST(RedisConnection, ConnectWithEmptyHostFailsWithoutServerContact)
    {
        ConnectionConfig configuration = makeOfflineConfiguration();
        configuration.host = "";

        RedisConnection connection(configuration);
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        // 空主机在真实驱动里被挡在 redisConnectWithTimeout 之前，桩构建里更是直接失败
        const auto startedAt = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.connect());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_LT(elapsed.count(), kMaximumOfflineCallMilliseconds);
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
    }

    TEST(RedisConnection, ConnectToUnmonitoredLocalPortFailsWithinTimeout)
    {
        RedisConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        // 回环上没有监听端口即刻收到 RST；200 毫秒只是环境异常时的上界保险
        const auto startedAt = std::chrono::steady_clock::now();
        const bool connected = connection.connect();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_FALSE(connected);
        EXPECT_LT(elapsed.count(), kMaximumOfflineCallMilliseconds);
    }

    TEST(RedisConnection, FailedConnectRecordsLocalizedReason)
    {
        RedisConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        ASSERT_FALSE(connection.connect());

        // 失败必须给出看得见的原因：hiredis 的 errstr 会被包进中文说明并带上底层错误码。
        // 不断言具体端口或错误码文本：实现只保证「摘取 errstr + 错误码」，端口号不必然出现在文案里
        const std::string reason = connection.lastError();
        EXPECT_FALSE(reason.empty());
        EXPECT_TRUE(containsLocalizedText(reason)) << reason;
        EXPECT_NE(reason.find("Redis"), std::string::npos) << reason;
    }

    TEST(RedisConnection, FailedConnectLeavesNoContextBehind)
    {
        RedisConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        ASSERT_FALSE(connection.connect());

        // 失败路径必须「先摘 errstr、再 redisFree」，不留半开上下文，也不留悬垂错误文本
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    TEST(RedisConnection, RepeatedFailedConnectStaysStable)
    {
        RedisConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        // 同一对象反复尝试不应积累状态：每次都是干净失败，每次都有原因
        for (int round = 0; round < 3; ++round)
        {
            EXPECT_FALSE(connection.connect());
            EXPECT_FALSE(connection.isConnected());
            EXPECT_EQ(connection.nativeHandle(), nullptr);
            EXPECT_FALSE(connection.lastError().empty());
        }
    }

    // ------------------------------------------------------------------------
    // 未连接时的执行入口
    // ------------------------------------------------------------------------

    TEST(RedisConnection, ExecuteWithoutConnectionReturnsNullResult)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        const std::unique_ptr<DatabaseResult> result = connection.execute("GET mykey");

        // 基类契约：失败一律交回空指针，原因写进 lastError()
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("Redis"), std::string::npos) << connection.lastError();
    }

    TEST(RedisConnection, ExecuteCommandWithoutConnectionReturnsNullResult)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        const std::vector<std::string_view> arguments{std::string_view("GET"), std::string_view("mykey")};
        const std::unique_ptr<DatabaseResult> result = connection.executeCommand(arguments);

        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_NE(connection.lastError().find("Redis"), std::string::npos) << connection.lastError();
    }

    TEST(RedisConnection, ExecuteCommandWithEmptyArgumentsReturnsNullResult)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        const std::vector<std::string_view> emptyArguments;
        const std::unique_ptr<DatabaseResult> result = connection.executeCommand(emptyArguments);

        // 参数为空的检查排在连接检查之前，两条路径都不发送任何字节
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
    }

    TEST(RedisConnection, SelectDatabaseRejectsNegativeIndex)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        // 负编号在前置校验处就被拒绝，不会变成一条服务端看不懂的错误命令
        EXPECT_FALSE(connection.selectDatabase(-1));
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_NE(connection.lastError().find("Redis"), std::string::npos) << connection.lastError();
    }

    TEST(RedisConnection, SelectDatabaseWithoutConnectionFails)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        // 合法编号也救不了未连接：走 executeCommand 的失败路径，原因如实回传
        EXPECT_FALSE(connection.selectDatabase(0));
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_FALSE(connection.isConnected());
    }

    // ------------------------------------------------------------------------
    // 管道缓冲区：登记与丢弃
    // ------------------------------------------------------------------------

    TEST(RedisConnection, PipelineCommandRejectsUnclosedQuote)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        // 切词在登记阶段完成：引号未闭合当场反馈，不会登记半个参数等到 flush 才发现
        EXPECT_FALSE(connection.pipelineCommand("SET mykey \"unclosed"));
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_NE(connection.lastError().find("Redis"), std::string::npos) << connection.lastError();
    }

    TEST(RedisConnection, PipelineCommandRejectsBlankCommand)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        // 整行只有空白等价于零参数命令，Redis 侧只会得到一条 ERR，这里直接拒绝
        EXPECT_FALSE(connection.pipelineCommand("   \t "));
        EXPECT_FALSE(connection.lastError().empty());
    }

    TEST(RedisConnection, FlushPipelineWithoutConnectionReturnsNoResults)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        static_cast<void>(connection.pipelineCommand("GET mykey"));
        const std::vector<std::unique_ptr<DatabaseResult>> results = connection.flushPipeline();

        // 未连接时整批命令一个字节都不发，原因写清「均未发送」，列表为空而不是塞 nullptr
        EXPECT_TRUE(results.empty());
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("Redis"), std::string::npos) << connection.lastError();

        // 登记过的命令一律丢弃：第二次 flush 面对的是空缓冲区，不会把旧命令重放到新连接上
        EXPECT_TRUE(connection.flushPipeline().empty());
    }

    TEST(RedisConnection, PipelineRegistrationNeedsNoConnection)
    {
        if (!kRedisDriverCompiled)
        {
            GTEST_SKIP() << "报错桩里连登记都没有意义，本用例校验真实驱动的「登记阶段零 IO」";
        }

        RedisConnection connection(ConnectionConfig::redisDefault());

        // 登记只切词入队，真正的网络往返发生在 flushPipeline()，因此未连接也允许登记
        EXPECT_TRUE(connection.pipelineCommand("SET counter 1"));
        EXPECT_TRUE(connection.lastError().empty());
        EXPECT_FALSE(connection.isConnected());
    }

    TEST(RedisConnection, FlushEmptyPipelineIsNotAnError)
    {
        if (!kRedisDriverCompiled)
        {
            GTEST_SKIP() << "报错桩里所有入口都无条件写「驱动缺失」，本用例校验真实驱动的空管道语义";
        }

        RedisConnection connection(ConnectionConfig::redisDefault());

        const std::vector<std::unique_ptr<DatabaseResult>> results = connection.flushPipeline();

        // 空管道不是错误：交出空列表且不写任何原因，也不触碰连接状态
        EXPECT_TRUE(results.empty());
        EXPECT_TRUE(connection.lastError().empty());
        EXPECT_FALSE(connection.isConnected());
    }

    // ------------------------------------------------------------------------
    // disconnect() 与析构
    // ------------------------------------------------------------------------

    TEST(RedisConnection, DisconnectWithoutConnectionIsSafe)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        // 未连接且无残留上下文时是安全的空操作，析构函数会无条件走这条路径
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_FALSE(connection.isConnected());
    }

    TEST(RedisConnection, DisconnectAfterFailedConnectIsSafe)
    {
        RedisConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);
        ASSERT_FALSE(connection.connect());

        // connect() 的失败路径内部已断开一次，调用方再断开必须是幂等的空操作
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    TEST(RedisConnection, DestroyingNeverConnectedConnectionIsSafe)
    {
        // 析构无条件调用 disconnect()：从未连接过的对象安静离场即可，不应抛任何异常。
        // 缓冲区里还留着一条已登记的命令，一并验证「丢弃未发送命令」不触发 IO
        EXPECT_NO_THROW(
                {
                    RedisConnection connection(ConnectionConfig::redisDefault());
                    static_cast<void>(connection.pipelineCommand("GET mykey"));
                });
    }

} // namespace AsynGyanis::Database
