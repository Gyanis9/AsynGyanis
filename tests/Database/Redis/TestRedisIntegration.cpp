/**
 * @file TestRedisIntegration.cpp
 * @brief Redis 真机集成测试 —— 认证、命令往返、回复类型映射与管道
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 只覆盖必须连上真实 Redis 才能验证的部分：认证、命令参数的二进制安全往返、回复到 DatabaseValue 的映射、
 *          键空间选择与管道批量收发（不需要服务端即可成立的行为归 TestRedisConnection.cpp 管）。
 *          门控：`ASYN_REDIS_TEST_PASSWORD` **没有默认值**，未设置时整组 GTEST_SKIP，仓库零明文口令；
 *          键空间默认 **15**（不用 0，免得混进使用者的工作库），键名由 makeKey() 保证唯一，清理只 DEL 自己的键、不 FLUSHDB。
 */
// 覆盖场景：
// - ConnectsAndAnswersPing（含认证成功）
// - ConnectWithWrongPasswordFailsWithLocalizedReason
// - StatusAndIntegerRepliesMapToTheirTypes
// - StringValuesRoundTripVerbatim（中文/引号/空格/换行/内嵌 '\0'）
// - MultiArgumentCommandKeepsArgumentsSeparate（参数不被拼成一条命令）
// - MissingKeyYieldsEmptyResult / EmptyArrayYieldsEmptyResult
// - ArrayReplyExposesOneColumnPerElement
// - CommandErrorFailsWithLocalizedReason（连接级：nullptr + 中文原因）
// - PipelineBatchesCommandsAndFlushesInOrder / PipelineErrorReplySurfacesOnItsOwnResult
// - ConfiguredKeyspaceIsSelectedOnConnect
// - TextCommandPathSplitsArguments（execute() 的切词路径）

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisConnection.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
#if defined(DATABASE_HAS_REDIS)
        constexpr bool kRedisDriverCompiled = true;
#else
        constexpr bool kRedisDriverCompiled = false;
#endif

        /// 本套件写入的全部键共用的前缀：便于人工排查，也避免与使用者的键名撞车
        constexpr std::string_view kKeyPrefix = "asyngyanis:test:";

        /**
         * @brief 本进程专用的键前缀：基础前缀 + 一次性随机后缀
         *
         * @details 每个用例都由 ctest 起独立进程；并行执行同一套件时，若所有进程只用 kKeyPrefix，
         *          收尾扫描会看到别的进程正在使用的键并误报残留。加进程唯一后缀后各进程的键空间
         *          互不可见，收尾扫描只覆盖自己；后缀在首次调用时生成，进程内恒定。
         */
        [[nodiscard]] std::string testKeyPrefix()
        {
            static const std::string prefix = []() -> std::string
            {
                std::random_device device;
                return std::string(kKeyPrefix) + std::to_string(device()) + ":";
            }();
            return prefix;
        }

        /// 键空间编号的默认值：见文件头「刻意不用 0」的说明
        constexpr std::string_view kDefaultTestKeyspace = "15";

        /**
         * @brief 读取环境变量，未设置时返回默认值
         * @param variableName 环境变量名
         * @param defaultValue 未设置时使用的默认值
         * @return std::string 变量值或默认值
         */
        [[nodiscard]] std::string readEnvironment(const char *variableName, const std::string_view defaultValue)
        {
            const char *rawValue = std::getenv(variableName);
            return rawValue == nullptr ? std::string(defaultValue) : std::string(rawValue);
        }

        /**
         * @brief 判断文本中是否含非 ASCII 字节，用作「面向使用者的中文文案」的稳定判据
         * @param text 待检查文本
         * @return true 含非 ASCII 字节
         */
        [[nodiscard]] bool containsLocalizedText(const std::string &text)
        {
            for (const unsigned char byte: text)
            {
                if (byte >= 0x80U)
                {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    /**
     * @brief Redis 真机集成测试夹具
     *
     * @details 无口令时在 SetUp 里 GTEST_SKIP：此时对象只构造了一半（连接未建），
     *          TearDown 里的所有动作都先判连接是否可用，因此不会踩空。
     */
    class RedisIntegrationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!kRedisDriverCompiled)
            {
                GTEST_SKIP() << "当前构建未编译 Redis 驱动（未定义 DATABASE_HAS_REDIS），跳过真机用例";
            }

            // 口令没有默认值：缺失即整组跳过，绝不在仓库里放一个「貌似能用」的口令
            const char *password = std::getenv("ASYN_REDIS_TEST_PASSWORD");
            if (password == nullptr)
            {
                GTEST_SKIP() << "未设置 ASYN_REDIS_TEST_PASSWORD，跳过 Redis 真机用例";
            }

            m_configuration = configFromEnvironment(password);

            m_connection = std::make_unique<RedisConnection>(m_configuration);
            ASSERT_TRUE(m_connection->connect()) << m_connection->lastError();
        }

        void TearDown() override
        {
            if (m_connection == nullptr || !m_connection->isConnected())
            {
                return;
            }

            // 逐个 DEL 自己写过的键，绝不 FLUSHDB（见文件头的清理策略）
            for (const std::string &key: m_createdKeys)
            {
                static_cast<void>(m_connection->executeCommand({"DEL", key}));
            }

            m_connection->disconnect();
        }

        /**
         * @brief 全套用例跑完后核对临时键空间里没有本套件留下的键
         *
         * @details 每个用例的 TearDown 已逐键删除，这里是**兜底检查**：将来新增用例若忘了用 makeKey() 登记键名，
         *          就会在这里被抓住，而不是悄悄在使用者的 15 号库里留垃圾；没有 Redis 环境时静默返回。
         *          用 KEYS 可接受，是因为这是专用临时库、模式又限定到本进程前缀（生产代码里当然不该用 KEYS）。
         */
        static void TearDownTestSuite()
        {
            if (!kRedisDriverCompiled)
            {
                return;
            }

            const char *password = std::getenv("ASYN_REDIS_TEST_PASSWORD");
            if (password == nullptr)
            {
                return;
            }

            RedisConnection connection(configFromEnvironment(password));
            if (!connection.connect())
            {
                return;
            }

            const std::unique_ptr<DatabaseResult> leftoverKeys =
                connection.executeCommand({"KEYS", testKeyPrefix() + "*"});
            if (leftoverKeys != nullptr && !leftoverKeys->isEmpty())
            {
                std::string leftoverNames;
                for (std::size_t index = 0; index < leftoverKeys->columnCount(); ++index)
                {
                    leftoverNames += " " + std::get<std::string>(leftoverKeys->getValue(index));
                }
                ADD_FAILURE() << "临时键空间里仍有本套件留下的键：" << leftoverNames;
            }

            connection.disconnect();
        }

        /**
         * @brief 按环境变量组装连接配置（SetUp 与整套兜底检查共用）
         * @param password 已确认非空的口令
         * @return ConnectionConfig 指向被测服务端的配置
         */
        [[nodiscard]] static ConnectionConfig configFromEnvironment(const std::string &password)
        {
            ConnectionConfig configuration      = ConnectionConfig::redisDefault();
            configuration.host                  = readEnvironment("ASYN_REDIS_TEST_HOST", "127.0.0.1");
            configuration.port                  = static_cast<std::uint16_t>(std::stoi(readEnvironment("ASYN_REDIS_TEST_PORT", "6379")));
            configuration.userName              = readEnvironment("ASYN_REDIS_TEST_USER", "");
            configuration.password              = password;
            configuration.database              = readEnvironment("ASYN_REDIS_TEST_DATABASE", kDefaultTestKeyspace);
            return configuration;
        }

        /**
         * @brief 生成本套件专用的键名并登记待清理
         * @param suffix 键名后缀，用于区分用例与用途
         * @return std::string 完整键名
         */
        [[nodiscard]] std::string makeKey(const std::string_view suffix)
        {
            std::string key = testKeyPrefix() + std::string(suffix);
            m_createdKeys.push_back(key);
            return key;
        }

        /**
         * @brief 执行一条命令并把「唯一那一行」的值取出（标量回复用）
         * @param arguments 命令名与参数，逐个独立送出
         * @return std::optional<DatabaseValue> 值；空结果集（nil）时为空
         */
        [[nodiscard]] std::optional<DatabaseValue> runScalar(const std::vector<std::string_view> &arguments)
        {
            const std::unique_ptr<DatabaseResult> result = m_connection->executeCommand(arguments);
            EXPECT_NE(result, nullptr) << m_connection->lastError();
            if (result == nullptr || result->isEmpty())
            {
                return std::nullopt;
            }
            return result->getValue(0);
        }

        ConnectionConfig                   m_configuration;      ///< 由环境变量组装的连接配置
        std::unique_ptr<RedisConnection>   m_connection;         ///< 用例独占的连接
        std::vector<std::string>           m_createdKeys;        ///< 本用例写入的键，TearDown 逐个删除
    };

    TEST_F(RedisIntegrationTest, ConnectsAndAnswersPing)
    {
        // 建连（含认证）已在 SetUp 里完成；这里确认链路真的可用：PING 的状态回复是 "PONG"
        const std::optional<DatabaseValue> reply = runScalar({"PING"});
        ASSERT_TRUE(reply.has_value());

        const auto *text = std::get_if<std::string>(&reply.value());
        ASSERT_NE(text, nullptr) << "状态回复应按 std::string 交出";
        EXPECT_EQ(*text, "PONG");
    }

    TEST_F(RedisIntegrationTest, ConnectWithWrongPasswordFailsWithLocalizedReason)
    {
        ConnectionConfig wrongConfiguration = m_configuration;
        wrongConfiguration.password        = "definitely-not-the-password";

        RedisConnection wrongConnection(wrongConfiguration);
        EXPECT_FALSE(wrongConnection.connect());
        EXPECT_FALSE(wrongConnection.isConnected());

        // 原因是中文，且点明是认证环节：只说「连接失败」会让人去查网络
        const std::string reason = wrongConnection.lastError();
        EXPECT_FALSE(reason.empty());
        EXPECT_TRUE(containsLocalizedText(reason)) << reason;
        EXPECT_NE(reason.find("认证"), std::string::npos) << reason;
    }

    TEST_F(RedisIntegrationTest, StatusAndIntegerRepliesMapToTheirTypes)
    {
        const std::string key = makeKey("typed");

        // SET 的状态回复是 "OK"（REDIS_REPLY_STATUS → std::string）
        const std::optional<DatabaseValue> setReply = runScalar({"SET", key, "1"});
        ASSERT_TRUE(setReply.has_value());
        ASSERT_NE(std::get_if<std::string>(&setReply.value()), nullptr);
        EXPECT_EQ(std::get<std::string>(setReply.value()), "OK");

        // INCR 的整数回复是 int64（REDIS_REPLY_INTEGER → std::int64_t）
        const std::optional<DatabaseValue> incremented = runScalar({"INCR", key});
        ASSERT_TRUE(incremented.has_value());
        const auto *integerValue = std::get_if<std::int64_t>(&incremented.value());
        ASSERT_NE(integerValue, nullptr) << "整数回复应按 std::int64_t 交出";
        EXPECT_EQ(*integerValue, 2);

        // STRLEN 同样走整数回复，用于确认映射不是「只有 INCR 恰好对」
        const std::optional<DatabaseValue> length = runScalar({"STRLEN", key});
        ASSERT_TRUE(length.has_value());
        ASSERT_NE(std::get_if<std::int64_t>(&length.value()), nullptr);
        EXPECT_EQ(std::get<std::int64_t>(length.value()), 1);

        // DEL 的整数回复表示删除个数
        const std::optional<DatabaseValue> deleted = runScalar({"DEL", key});
        ASSERT_TRUE(deleted.has_value());
        EXPECT_EQ(std::get<std::int64_t>(deleted.value()), 1);
    }

    TEST_F(RedisIntegrationTest, StringValuesRoundTripVerbatim)
    {
        const std::string key = makeKey("verbatim");

        // 内嵌 '\0' 与换行都由 redisCommandArgv 的「指针 + 长度」形式完整送出，
        // 这正是 Redis 与 SQL 文本类型的根本差别：它按字节存，不做编码解释
        const std::string payload = std::string("张三 said \"hi\" -- not a comment\n") + '\0' + "tail";

        const std::optional<DatabaseValue> setReply = runScalar({"SET", key, payload});
        ASSERT_TRUE(setReply.has_value());

        const std::optional<DatabaseValue> fetched = runScalar({"GET", key});
        ASSERT_TRUE(fetched.has_value());
        const auto *text = std::get_if<std::string>(&fetched.value());
        ASSERT_NE(text, nullptr);
        // 逐字节相等：含 '\0' 的取值若被零终止假设截断，这里立刻暴露
        EXPECT_EQ(text->size(), payload.size());
        EXPECT_EQ(*text, payload);
    }

    TEST_F(RedisIntegrationTest, MultiArgumentCommandKeepsArgumentsSeparate)
    {
        const std::string key = makeKey("separate");

        // 取值里含空格与引号：若驱动把参数拼成一条命令行，服务端收到的参数个数就不对
        // （要么报错、要么把后半段当成额外的键），这里用「读回的取值原样」来证明参数是分开送的
        const std::string payload = "value with spaces and \"quotes\"";
        ASSERT_TRUE(runScalar({"SET", key, payload}).has_value());

        const std::optional<DatabaseValue> fetched = runScalar({"GET", key});
        ASSERT_TRUE(fetched.has_value());
        ASSERT_NE(std::get_if<std::string>(&fetched.value()), nullptr);
        EXPECT_EQ(std::get<std::string>(fetched.value()), payload);
    }

    TEST_F(RedisIntegrationTest, MissingKeyYieldsEmptyResult)
    {
        const std::string key = makeKey("missing");

        const std::unique_ptr<DatabaseResult> result = m_connection->executeCommand({"GET", key});
        ASSERT_NE(result, nullptr) << m_connection->lastError();

        // nil 回复按契约映射成「0 行 0 列」的空结果集，而不是一行 monostate
        EXPECT_TRUE(result->isEmpty());
        EXPECT_EQ(result->rowCount(), 0U);
        EXPECT_EQ(result->columnCount(), 0U);
        // 空结果不是错误：基类契约里 error 回复才会把服务端原文写进 lastError()
        EXPECT_TRUE(result->lastError().empty()) << result->lastError();
        EXPECT_FALSE(result->next());
    }

    TEST_F(RedisIntegrationTest, EmptyArrayYieldsEmptyResult)
    {
        const std::string key = makeKey("empty-list");

        // 空数组与 nil 都落到空结果集：调用方不必为「没有元素」区分两种协议形态
        const std::unique_ptr<DatabaseResult> result = m_connection->executeCommand({"LRANGE", key, "0", "-1"});
        ASSERT_NE(result, nullptr) << m_connection->lastError();
        EXPECT_TRUE(result->isEmpty());
        EXPECT_TRUE(result->lastError().empty()) << result->lastError();
    }

    TEST_F(RedisIntegrationTest, ArrayReplyExposesOneColumnPerElement)
    {
        const std::string key = makeKey("array");

        // 用 RPUSH 造一个三元素列表，再整体读回：数组回复按「一列一个元素」交出
        for (const std::string_view element: {std::string_view("甲"), std::string_view("乙"), std::string_view("丙")})
        {
            ASSERT_TRUE(runScalar({"RPUSH", key, element}).has_value());
        }

        const std::unique_ptr<DatabaseResult> result = m_connection->executeCommand({"LRANGE", key, "0", "-1"});
        ASSERT_NE(result, nullptr) << m_connection->lastError();
        ASSERT_EQ(result->columnCount(), 3U);
        EXPECT_EQ(result->rowCount(), 1U);

        // 顺序即服务端返回顺序：列表的 LRANGE 保证按索引升序
        EXPECT_EQ(std::get<std::string>(result->getValue(0)), "甲");
        EXPECT_EQ(std::get<std::string>(result->getValue(1)), "乙");
        EXPECT_EQ(std::get<std::string>(result->getValue(2)), "丙");

        // 列名是按下标合成的占位名，不承载语义（Redis 没有列名概念）
        const std::optional<std::string> firstName = result->columnName(0);
        ASSERT_TRUE(firstName.has_value());
        EXPECT_EQ(firstName.value(), "value0");
    }

    TEST_F(RedisIntegrationTest, CommandErrorFailsWithLocalizedReason)
    {
        // 参数个数不对：服务端回 error 回复。真机实测确认本驱动的契约是
        // **返回 nullptr 并把原因写进连接的 lastError()**（与 MySQL/SQLite 驱动一致），
        // 而不是返回一个「错误类型的结果」——后者只在管道里才有（见下一个用例）
        EXPECT_EQ(m_connection->executeCommand({"GET"}), nullptr);

        const std::string reason = m_connection->lastError();
        EXPECT_FALSE(reason.empty());
        // 只断言驱动自己产生的那部分文本：前缀是稳定契约，服务端原文的措辞与大小写
        // 随版本而变（实测 Redis 把命令名回显成小写 'get'），钉它只会得到脆弱的断言
        EXPECT_TRUE(containsLocalizedText(reason)) << reason;
        EXPECT_NE(reason.find("Redis 服务器返回错误"), std::string::npos) << reason;
    }

    TEST_F(RedisIntegrationTest, PipelineErrorReplySurfacesOnItsOwnResult)
    {
        const std::string key = makeKey("pipe-error");

        // 管道里「一次 flush 收多条回复」，因此单条命令失败不能整体失败：
        // 好的那条要正常给出结果，坏的那条要把服务端原文留在**它自己**的结果上
        ASSERT_TRUE(m_connection->pipelineCommand("SET " + key + " ok"));
        ASSERT_TRUE(m_connection->pipelineCommand("GET")); // 少参数，服务端必回 error

        const std::vector<std::unique_ptr<DatabaseResult>> replies = m_connection->flushPipeline();
        ASSERT_EQ(replies.size(), 2U);

        ASSERT_NE(replies[0], nullptr);
        EXPECT_EQ(std::get<std::string>(replies[0]->getValue(0)), "OK");

        // 第二份回复是 error：按基类契约，RedisResult 会把服务端原文摘进自己的 lastError()
        ASSERT_NE(replies[1], nullptr);
        EXPECT_FALSE(replies[1]->lastError().empty()) << "管道里的 error 回复必须把原因留在该条结果上";
    }

    TEST_F(RedisIntegrationTest, PipelineBatchesCommandsAndFlushesInOrder)
    {
        const std::string firstKey  = makeKey("pipe-a");
        const std::string secondKey = makeKey("pipe-b");

        // 登记三条命令（只入队不发送），再一次 flush 收全部回复
        ASSERT_TRUE(m_connection->pipelineCommand("SET " + firstKey + " alpha"));
        ASSERT_TRUE(m_connection->pipelineCommand("SET " + secondKey + " beta"));
        ASSERT_TRUE(m_connection->pipelineCommand("GET " + firstKey));

        const std::vector<std::unique_ptr<DatabaseResult>> replies = m_connection->flushPipeline();
        ASSERT_EQ(replies.size(), 3U);

        // 回复顺序与登记顺序严格一致：这是管道唯一容易被写错的地方
        ASSERT_NE(replies[0], nullptr);
        EXPECT_EQ(std::get<std::string>(replies[0]->getValue(0)), "OK");
        ASSERT_NE(replies[1], nullptr);
        EXPECT_EQ(std::get<std::string>(replies[1]->getValue(0)), "OK");
        ASSERT_NE(replies[2], nullptr);
        EXPECT_EQ(std::get<std::string>(replies[2]->getValue(0)), "alpha");

        // 非法命令在登记阶段就被拒，不会留到 flush 时才爆
        EXPECT_FALSE(m_connection->pipelineCommand("SET \"unterminated"));
        EXPECT_FALSE(m_connection->lastError().empty());
    }

    TEST_F(RedisIntegrationTest, ConfiguredKeyspaceIsSelectedOnConnect)
    {
        const std::string key = makeKey("keyspace");
        ASSERT_TRUE(runScalar({"SET", key, "in-test-keyspace"}).has_value());

        // 另开一条连到 0 号库的连接：同一个键在那里必须不存在，才说明 SELECT 真的生效了
        ConnectionConfig defaultKeyspaceConfiguration = m_configuration;
        defaultKeyspaceConfiguration.database        = "0";

        RedisConnection defaultKeyspaceConnection(defaultKeyspaceConfiguration);
        ASSERT_TRUE(defaultKeyspaceConnection.connect()) << defaultKeyspaceConnection.lastError();

        const std::unique_ptr<DatabaseResult> result = defaultKeyspaceConnection.executeCommand({"GET", key});
        ASSERT_NE(result, nullptr) << defaultKeyspaceConnection.lastError();
        EXPECT_TRUE(result->isEmpty()) << "键出现在 0 号库，说明连接期没有 SELECT 到配置的键空间";

        defaultKeyspaceConnection.disconnect();
    }

    TEST_F(RedisIntegrationTest, TextCommandPathSplitsArguments)
    {
        const std::string key = makeKey("text-path");

        // execute() 走的是「整行切词」路径：与 executeCommand() 的显式参数列表必须得到同一结果
        const std::unique_ptr<DatabaseResult> setResult = m_connection->execute("SET " + key + " via-text-path");
        ASSERT_NE(setResult, nullptr) << m_connection->lastError();

        const std::unique_ptr<DatabaseResult> getResult = m_connection->execute("GET " + key);
        ASSERT_NE(getResult, nullptr) << m_connection->lastError();
        EXPECT_EQ(std::get<std::string>(getResult->getValue(0)), "via-text-path");

        // 引号未闭合属切词失败：本地即可判掉，且原因是中文
        EXPECT_EQ(m_connection->execute("SET \"unterminated"), nullptr);
        EXPECT_TRUE(containsLocalizedText(m_connection->lastError())) << m_connection->lastError();
    }

} // namespace AsynGyanis::Database
