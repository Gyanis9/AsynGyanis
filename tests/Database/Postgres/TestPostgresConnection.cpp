/**
 * @file TestPostgresConnection.cpp
 * @brief PostgresConnection 单元测试：真实驱动的离线失败语义与驱动无关的连接骨架
 * @details DATABASE_WITH_POSTGRES 默认开启，因此 libpq 可用时编出的是真实驱动
 *          （CMake 定义 DATABASE_HAS_POSTGRES），探测不到客户端库时才退化为报错桩。
 *          本文件因此只断言「不需要 PostgreSQL 服务端就能成立」的行为，两种构建配置下同义：
 *          - 配置回显、超时往返、databaseType()、未连接时的各条失败路径、指向未监听端口的
 *            connect() 在超时内失败、句柄为空、错误文本为中文且点明 PostgreSQL；
 *          - 容器类型参数被拒（真实驱动专属：桩构建里参数化路径在「驱动缺失」处就返回了，
 *            根本走不到参数校验，该用例在桩构建下显式跳过）；
 *          - 含 NUL 字节的文本参数被拒（同上属真实驱动专属）：PostgreSQL 的文本类型存不了 NUL，
 *            驱动提前拦下并给出「改用 BYTEA + 十六进制文本」的可行出路，同时保证不含 NUL 的
 *            文本（含十六进制写法本身、空串）照常放行；
 *          - 真实驱动专属且必须有服务端才能验证的部分（真实查询结果、影响行数、
 *            serverVersion() 取值、参数个数与 $n 不匹配的服务端报错）本文件不做断言，
 *            由 TestPostgresIntegration.cpp 在有服务端时覆盖。
 *          异常文本一律只断言「非空 + 含关键子串 + 含本地化文案」，不硬编码整句中文。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Postgres/PostgresConnection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
#ifdef DATABASE_HAS_POSTGRES
        /// 当前构建是否编译了真实的 libpq 驱动
        constexpr bool kPostgresDriverCompiled = true;
#else
        /// 当前构建为报错桩：桩语义下的附加断言才会执行
        constexpr bool kPostgresDriverCompiled = false;
#endif

        /// 基类 DatabaseConnection 声明的连接超时默认毫秒数
        constexpr int kDefaultConnectTimeoutMilliseconds = 5000;

        /// 基类 DatabaseConnection 声明的单条命令执行超时默认毫秒数
        constexpr int kDefaultQueryTimeoutMilliseconds = 30000;

        /// 离线建连用例使用的连接超时毫秒数：够短不拖慢测试，又够长不被调度抖动误判
        constexpr int kShortConnectTimeoutMilliseconds = 200;

        /// 单条离线建连用例允许的最长耗时毫秒数，是「不挂死」的硬上界。
        /// libpq 的 connect_timeout 只接受整秒（200 毫秒向上取整成 1 秒），而某些环境下回环的
        /// 未监听端口不会立刻回 RST（SYN 被丢弃），一次尝试因此要等满该超时，这里给 1 秒留出调度余量
        constexpr long long kMaximumOfflineCallMilliseconds = 3000;

        /**
         * @brief 本机上一个确定没有监听的端口
         * @details 取注册端口区间内、远离 PostgreSQL(5432)/MySQL(3306)/Redis(6379) 等常见服务，
         *          且在 Linux(32768+) 与 Windows(49152+) 临时端口范围之下：
         *          既不会被本机服务占用，也不会被系统当作源端口分配出去。
         *          即使该端口上恰好有别的东西在监听，PostgreSQL 握手也必然失败，用例结论不变
         */
        constexpr std::uint16_t kUnmonitoredPort = 16392;

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
    // 与是否编译真实驱动无关的骨架行为
    // ------------------------------------------------------------------------

    TEST(PostgresConnection, ReportsPostgreSqlDatabaseType)
    {
        const PostgresConnection connection(ConnectionConfig::postgresDefault());

        // databaseType() 不依赖连接状态，桩构建下同样返回本类型
        EXPECT_EQ(connection.databaseType(), DatabaseType::PostgreSql);
        EXPECT_STREQ(databaseTypeName(connection.databaseType()), "PostgreSql");
    }

    TEST(PostgresConnection, ConfigurationEchoesConstructorArgument)
    {
        ConnectionConfig configuration;
        configuration.host     = "postgres.internal";
        configuration.port     = 6432;
        configuration.userName = "reporter";
        configuration.password = "p@ss%word";
        configuration.database = "warehouse";

        const PostgresConnection connection(configuration);

        // 构造阶段只登记配置：五个字段全部原样回显，驱动不做任何裁剪或补默认值
        const ConnectionConfig &stored = connection.configuration();
        EXPECT_EQ(stored.host, "postgres.internal");
        EXPECT_EQ(stored.port, 6432);
        EXPECT_EQ(stored.userName, "reporter");
        EXPECT_EQ(stored.password, "p@ss%word");
        EXPECT_EQ(stored.database, "warehouse");
    }

    TEST(PostgresConnection, ConfigurationDefaultsAreEchoedWithoutRewrite)
    {
        const PostgresConnection connection(ConnectionConfig::postgresDefault());

        // 工厂方法给出的默认值不因驱动而变（端口 5432 是 PostgreSQL 的默认端口，显式给出）
        EXPECT_EQ(connection.configuration().host, "127.0.0.1");
        EXPECT_EQ(connection.configuration().port, 5432);
        EXPECT_EQ(connection.configuration().userName, "postgres");
        EXPECT_EQ(connection.configuration().database, "postgres");
    }

    TEST(PostgresConnection, TimeoutsStartWithDocumentedDefaults)
    {
        const PostgresConnection connection(ConnectionConfig::postgresDefault());

        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
        EXPECT_EQ(connection.queryTimeout(), kDefaultQueryTimeoutMilliseconds);
    }

    TEST(PostgresConnection, ConnectTimeoutSetterRoundTripsBeforeConnect)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());

        connection.setConnectTimeout(1200);
        EXPECT_EQ(connection.connectTimeout(), 1200);

        // 边界：0 与负值由驱动折算成「不超时」（省略 libpq 的 connect_timeout 关键字），
        // getter 仍如实回显写入值（换算发生在 connect 阶段）
        connection.setConnectTimeout(0);
        EXPECT_EQ(connection.connectTimeout(), 0);
        connection.setConnectTimeout(-1);
        EXPECT_EQ(connection.connectTimeout(), -1);
    }

    TEST(PostgresConnection, QueryTimeoutSetterRoundTripsIndependently)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());

        connection.setQueryTimeout(750);

        // 两个超时是彼此独立的存储，改一个不应影响另一个
        EXPECT_EQ(connection.queryTimeout(), 750);
        EXPECT_EQ(connection.connectTimeout(), kDefaultConnectTimeoutMilliseconds);
    }

    TEST(PostgresConnection, StartsDisconnectedWithoutAnyErrorRecorded)
    {
        const PostgresConnection connection(ConnectionConfig::postgresDefault());

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_TRUE(connection.lastError().empty());
        // 构造本身不会失败，因此不提前占用 lastError()
        EXPECT_TRUE(connection.serverVersion().empty());
    }

    TEST(PostgresConnection, ConnectWithEmptyHostFailsWithoutServerContact)
    {
        ConnectionConfig configuration = ConnectionConfig::postgresDefault();
        configuration.host = "";

        PostgresConnection connection(configuration);

        // 空主机在两种构建下都必须当场失败：真实驱动把它挡在 PQconnectdbParams 之前，不产生网络往返
        const auto startedAt = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.connect());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_LT(elapsed.count(), kMaximumOfflineCallMilliseconds);
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("PostgreSQL"), std::string::npos) << connection.lastError();
    }

    TEST(PostgresConnection, ExecuteWithoutConnectionReturnsNullResult)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());

        const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT 1");

        // 基类契约：失败一律交回空指针，原因写进 lastError()，调用方只判空指针即可发现问题
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("PostgreSQL"), std::string::npos) << connection.lastError();
    }

    TEST(PostgresConnection, ParameterizedExecuteWithoutConnectionReturnsNullResult)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());
        const std::vector<DatabaseValue> parameters{std::int64_t{1}};

        const std::unique_ptr<DatabaseResult> result = connection.execute("SELECT $1", parameters);

        // 参数化路径同样必须在未连接时明确失败，而不是静默把参数丢掉或按 NULL 执行。
        // 注意这里验证的是「未连接」这一条判定：参数个数与语句里的 $n 是否匹配要在服务端
        // 解析语句之后才知道，属于服务端依赖路径
        EXPECT_EQ(result, nullptr);
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_TRUE(containsLocalizedText(connection.lastError()));
        EXPECT_NE(connection.lastError().find("PostgreSQL"), std::string::npos) << connection.lastError();
        EXPECT_NE(connection.lastError().find("未连接"), std::string::npos) << connection.lastError();
    }

    TEST(PostgresConnection, DisconnectWithoutConnectionIsSafe)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());

        // 未连接且无残留句柄时是安全的空操作，析构函数会无条件走这条路径
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_FALSE(connection.isConnected());
    }

    TEST(PostgresConnection, DisconnectKeepsLastFailureReason)
    {
        ConnectionConfig configuration = ConnectionConfig::postgresDefault();
        configuration.host = "";

        PostgresConnection connection(configuration);
        ASSERT_FALSE(connection.connect());
        const std::string failureReason = connection.lastError();
        ASSERT_FALSE(failureReason.empty());

        connection.disconnect();

        // disconnect() 刻意不清 lastError：失败路径上先写好的根因不能被断开动作抹掉
        EXPECT_EQ(connection.lastError(), failureReason);
        EXPECT_FALSE(connection.isConnected());
    }

    TEST(PostgresConnection, ServerVersionIsEmptyWhileDisconnected)
    {
        const PostgresConnection connection(ConnectionConfig::postgresDefault());

        // 与 SQLite 驱动不同：PostgreSQL 的服务端版本必须持有已连接句柄才有值
        // （server_version 是握手阶段服务端送来的会话参数）。真实取值需要可用的服务端
        EXPECT_TRUE(connection.serverVersion().empty());
    }

    TEST(PostgresConnection, TransactionHelpersFailWhileDisconnected)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());

        // 三个便捷封装都走 execute()，未连接时按同一约定返回 false
        EXPECT_FALSE(connection.beginTransaction());
        EXPECT_FALSE(connection.commit());
        EXPECT_FALSE(connection.rollback());
        EXPECT_FALSE(connection.lastError().empty());
    }

    // ------------------------------------------------------------------------
    // 参数校验发生在任何连接动作之前（真实驱动专属）
    // ------------------------------------------------------------------------

    TEST(PostgresConnection, ContainerParametersAreRejectedWithoutServerContact)
    {
        // 桩构建里参数化路径在「驱动缺失」处就返回，根本不进入参数校验，本用例只对真实驱动成立
        if (!kPostgresDriverCompiled)
        {
            GTEST_SKIP() << "当前构建未编译 PostgreSQL 驱动，参数校验路径不存在";
        }

        PostgresConnection connection(ConnectionConfig::postgresDefault());
        ASSERT_FALSE(connection.isConnected());

        // 列表：对应 Redis List 形态。容器无法作为单个标量参数绑定，
        // 正确用法是展开成多个标量参数（IN 列表由方言展开），这里必须明确失败
        const std::vector<DatabaseValue> listParameters{std::vector<std::string>{"甲", "乙"}};
        const std::unique_ptr<DatabaseResult> listResult = connection.execute("SELECT $1", listParameters);
        EXPECT_EQ(listResult, nullptr);
        const std::string listReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(listReason)) << listReason;
        // 文案与 MySQL 驱动刻意保持一致（只有中文说明 + 参数序号 + 类型名，不带驱动名前缀），
        // 因此这里只断言「容器」与类型名这两个稳定关键词
        EXPECT_NE(listReason.find("容器"), std::string::npos) << listReason;
        EXPECT_NE(listReason.find("List"), std::string::npos) << listReason;

        // 哈希：对应 Redis Hash 形态
        const std::vector<DatabaseValue> hashParameters{
            std::unordered_map<std::string, std::string>{{"键", "值"}}};
        EXPECT_EQ(connection.execute("SELECT $1", hashParameters), nullptr);
        const std::string hashReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(hashReason)) << hashReason;
        EXPECT_NE(hashReason.find("容器"), std::string::npos) << hashReason;
        EXPECT_NE(hashReason.find("Hash"), std::string::npos) << hashReason;

        // 上面两条失败都发生在连接状态判定之前，说明「容器参数被拒」这条判定不依赖服务端即可复现；
        // 而合法标量参数在未连接时仍必须给出「未连接」这条真正的原因，两条判定互不遮蔽
        const std::vector<DatabaseValue> scalarParameters{std::int64_t{1}};
        EXPECT_EQ(connection.execute("SELECT $1", scalarParameters), nullptr);
        EXPECT_NE(connection.lastError().find("未连接"), std::string::npos) << connection.lastError();

        // 全过程没有发生过任何连接，句柄始终为空
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    TEST(PostgresConnection, TextParametersWithNulByteAreRejectedWithoutServerContact)
    {
        // 与容器判定同属纯本地校验，桩构建里参数化路径在「驱动缺失」处就返回，因此只对真实驱动成立
        if (!kPostgresDriverCompiled)
        {
            GTEST_SKIP() << "当前构建未编译 PostgreSQL 驱动，参数校验路径不存在";
        }

        PostgresConnection connection(ConnectionConfig::postgresDefault());
        ASSERT_FALSE(connection.isConnected());

        // PostgreSQL 的文本类型在编码层面不允许 NUL，服务端只会以「invalid byte sequence」拒绝，
        // 调用方很难从那种报文里定位到「某个字符串混进了 '\0'」，因此驱动提前拦下并给出替代做法
        const std::vector<DatabaseValue> nulParameters{std::string("a\0b", 3)};
        const std::unique_ptr<DatabaseResult> nulResult = connection.execute("SELECT $1", nulParameters);
        EXPECT_EQ(nulResult, nullptr);

        const std::string nulReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(nulReason)) << nulReason;
        // 断言「NUL」与「BYTEA」两个稳定关键词：前者说明拦的是什么，后者给出可行出路
        EXPECT_NE(nulReason.find("NUL"), std::string::npos) << nulReason;
        EXPECT_NE(nulReason.find("BYTEA"), std::string::npos) << nulReason;

        // 关键边界：不含 NUL 的文本（哪怕内容本身就是「十六进制文本」）必须照常放行，
        // 否则会把「按 BYTEA 的十六进制写法送出」这条推荐做法也一起挡掉
        const std::vector<DatabaseValue> hexTextParameters{std::string("\\x48656c6c6f")};
        EXPECT_EQ(connection.execute("SELECT $1", hexTextParameters), nullptr);
        EXPECT_NE(connection.lastError().find("未连接"), std::string::npos) << connection.lastError();

        // 边界再往前一步：空串（长度为 0）与「以 NUL 结尾的 C 字符串字面量」都不是本判定要拦的东西，
        // 前者合法、后者在 std::string 构造时已被截断成不含 NUL 的文本
        const std::vector<DatabaseValue> emptyTextParameters{std::string()};
        EXPECT_EQ(connection.execute("SELECT $1", emptyTextParameters), nullptr);
        EXPECT_NE(connection.lastError().find("未连接"), std::string::npos) << connection.lastError();

        const std::vector<DatabaseValue> cStringParameters{std::string("abc")};
        EXPECT_EQ(connection.execute("SELECT $1", cStringParameters), nullptr);
        EXPECT_NE(connection.lastError().find("未连接"), std::string::npos) << connection.lastError();

        // 同容器用例：全过程没有发生过任何连接
        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    TEST(PostgresConnection, CommandTextWithNulByteIsRejectedWithoutServerContact)
    {
        if (!kPostgresDriverCompiled)
        {
            GTEST_SKIP() << "当前构建未编译 PostgreSQL 驱动，本判定只在真实驱动里生效";
        }

        PostgresConnection connection(ConnectionConfig::postgresDefault());
        ASSERT_FALSE(connection.isConnected());

        // 命令文本内嵌 NUL 时，libpq 会把它当成字符串结尾：命令被静默截断成前半句，
        // 而「前半句恰好合法」的情况不会报任何错，因此必须在本地拦下。
        // 这里显式拼出 NUL，避免依赖「字面量长度参数写对」这种容易出错又看不出意图的写法
        const std::string truncatedCommand = std::string("SELECT 1") + '\0' + "; DROP TABLE important";
        EXPECT_EQ(connection.execute(truncatedCommand), nullptr);
        const std::string plainReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(plainReason)) << plainReason;
        EXPECT_NE(plainReason.find("NUL"), std::string::npos) << plainReason;

        // 参数化重载走的是同一个 PQexecParams 命令文本参数，必须同样被拦
        const std::vector<DatabaseValue> parameters{std::int64_t{1}};
        EXPECT_EQ(connection.execute(truncatedCommand, parameters), nullptr);
        const std::string parameterizedReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(parameterizedReason)) << parameterizedReason;
        EXPECT_NE(parameterizedReason.find("NUL"), std::string::npos) << parameterizedReason;

        // 两个重载的 NUL 判定都在连接判定之前，因此未连接时给出的是 NUL 而不是「未连接」；
        // 反过来，不含 NUL 的命令仍应落到真正的「未连接」上，两条判定互不遮蔽
        EXPECT_EQ(connection.execute("SELECT 1", parameters), nullptr);
        EXPECT_NE(connection.lastError().find("未连接"), std::string::npos) << connection.lastError();

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    // ------------------------------------------------------------------------
    // 指向未监听端口的离线建连路径（真实驱动与报错桩都必须失败）
    // ------------------------------------------------------------------------

    TEST(PostgresConnection, ConnectToUnmonitoredLocalPortFailsWithinTimeout)
    {
        PostgresConnection connection(makeOfflineConfiguration());
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
        EXPECT_NE(connection.lastError().find("PostgreSQL"), std::string::npos) << connection.lastError();

        // 失败路径必须先摘错误文本再 PQfinish：走到这里句柄已置空，重复断开是安全的空操作
        EXPECT_NO_THROW(connection.disconnect());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    TEST(PostgresConnection, ConnectFailureKeepsLocalizedReasonAndStaysDisconnected)
    {
        PostgresConnection connection(makeOfflineConfiguration());
        connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

        ASSERT_FALSE(connection.connect());
        const std::string failureReason = connection.lastError();

        // 面向使用者的文本一律中文，并点明是哪个驱动出的问题
        EXPECT_TRUE(containsLocalizedText(failureReason)) << failureReason;
        EXPECT_NE(failureReason.find("PostgreSQL"), std::string::npos) << failureReason;

        // 未编译客户端库时，原因文本必须明确指出「驱动缺失」而不是含糊的连不上；
        // 编译了驱动时失败来自真实的建连调用。这里与 MySQL 侧断言的差异：libpq 的连接级
        // 错误（PQerrorMessage）不含任何数字错误码（SQLSTATE 只在查询类结果集上给出），
        // 因此只断言点明动作的中文前缀「连接失败」，不硬找「错误码」
        if (!kPostgresDriverCompiled)
        {
            EXPECT_NE(failureReason.find("驱动"), std::string::npos) << failureReason;
        }
        else
        {
            EXPECT_NE(failureReason.find("连接失败"), std::string::npos) << failureReason;
        }

        EXPECT_FALSE(connection.isConnected());
        EXPECT_EQ(connection.nativeHandle(), nullptr);
    }

    // ------------------------------------------------------------------------
    // 反复建连/析构不泄漏、不崩溃
    // ------------------------------------------------------------------------

    TEST(PostgresConnection, RepeatedOfflineConnectAttemptsAreSafe)
    {
        // 每次失败都必须把句柄释放干净：真实驱动会在失败路径上 PQfinish 并置空（建连失败的
        // 会话同样占内存，只有 PQfinish 能回收），桩本就不分配句柄。
        // 反复走一遍用于发现「失败路径漏释放」这类缺陷。轮数刻意压小：
        // 每次尝试都要等满 1 秒的建连超时（见 kMaximumOfflineCallMilliseconds 的说明）
        for (int round = 0; round < 4; ++round)
        {
            PostgresConnection connection(makeOfflineConfiguration());
            connection.setConnectTimeout(kShortConnectTimeoutMilliseconds);

            EXPECT_FALSE(connection.connect());
            EXPECT_FALSE(connection.isConnected());
            EXPECT_EQ(connection.nativeHandle(), nullptr);
        }
    }

    TEST(PostgresConnection, EveryExecuteOverloadKeepsFailingWhileDisconnected)
    {
        PostgresConnection connection(ConnectionConfig::postgresDefault());
        const std::vector<DatabaseValue> parameters{std::int64_t{7}};

        // 空命令、正常命令与参数化命令在未连接时都必须明确失败，每次调用都给出原因而非静默 no-op
        EXPECT_EQ(connection.execute(""), nullptr);
        const std::string emptyCommandReason = connection.lastError();
        EXPECT_TRUE(containsLocalizedText(emptyCommandReason)) << emptyCommandReason;

        EXPECT_EQ(connection.execute("SELECT 1"), nullptr);
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();

        EXPECT_EQ(connection.execute("SELECT $1", parameters), nullptr);
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();

        // 未编译驱动时三条路径都应点出「驱动缺失」这一根因
        if (!kPostgresDriverCompiled)
        {
            EXPECT_NE(emptyCommandReason.find("驱动"), std::string::npos) << emptyCommandReason;
        }
    }

} // namespace AsynGyanis::Database
