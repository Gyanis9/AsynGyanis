// 覆盖场景（只覆盖必须连上真实 Redis 才能验证的部分；不需要服务端即可成立的行为归 TestRedisConnection.cpp 管）：
// - ConnectsAndAnswersPing（含认证成功）
// - ConnectWithWrongPasswordFailsWithLocalizedReason
// - StatusAndIntegerRepliesMapToTheirTypes
// - StringValuesRoundTripVerbatim（中文/引号/空格/换行/内嵌 '\0'）
// - MultiArgumentCommandKeepsArgumentsSeparate（参数不被拼成一条命令）
// - MissingKeyYieldsEmptyResult / EmptyArrayYieldsEmptyResult
// - ArrayReplyExposesOneColumnPerElement
// - CommandErrorFailsWithLocalizedReason（连接级：nullptr + 中文原因）
// - PipelineBatchesCommandsAndFlushesInOrder / PipelineErrorReplySurfacesOnItsOwnResult
// - PipelineAcrossRisingAndFallingArgumentCountsKeepsCommandsIntact（参数条数升降交替仍逐条对齐）
// - SingleCommandArgumentTableHoldsAcrossInlineCapacity（单命令参数表在栈上容量两侧都等值送达）
// - ResetSessionStateDiscardsPendingPipelineCommands
// - ResetSessionStateDiscardsLeftoverTransaction（残留 MULTI 会让下一个借用者的写全被排队）
// - CompletedTransactionLeavesNothingForSessionReset（EXEC 之后复位不再发命令）
// - ResetSessionStateUnwatchesLeftoverWatch（残留 WATCH 会让下一个借用者的 EXEC 中止）
// - RejectedExecDoesNotSettleTheWatchItImplies（被退回的 EXEC 不算了结，复位仍补 UNWATCH）
// - PipelineSessionBookkeepingFollowsTheReply（管道那条路径同样按回复定记账）
// - SessionResetReturnsToTheConfiguredKeySpace（selectDatabase 换走的库在归还时还回配置值）
// - MonitorStyleSessionIsDroppedOnReturn（MONITOR 这类退不回去的模式：归还时断开而不是回池）
// - NonPositiveConnectTimeoutDoesNotMeanInstantFailure（connectTimeout 的 0 与负数＝不设超时，守卫性质见用例注释）
// - PooledReturnClearsSessionForNextBorrower（经连接池借还这一形状下，管道与 MULTI 都不串给下一个）
// - ConfiguredKeyspaceIsSelectedOnConnect
// - TextCommandPathSplitsArguments（execute() 的切词路径）
// - QueryTimeoutChangeAppliesToEstablishedConnection（建连后改超时当场生效，不必重连）
// 门控：`ASYN_REDIS_TEST_PASSWORD` **没有默认值**，未设置时整组 GTEST_SKIP，仓库零明文口令；
// 键空间默认 **15**（不用 0，免得混进使用者的工作库），键名由 makeKey() 保证唯一，清理只 DEL 自己的键、不 FLUSHDB。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Redis/RedisConnection.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
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

    using TestSupport::readEnvironmentPortOrDefault;
    using TestSupport::readEnvironmentTextOrDefault;

    using TestSupport::containsLocalizedText;
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
            const std::string password = TestSupport::readEnvironmentVariableText("ASYN_REDIS_TEST_PASSWORD");
            if (password.empty())
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

            const std::string password = TestSupport::readEnvironmentVariableText("ASYN_REDIS_TEST_PASSWORD");
            if (password.empty())
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
            configuration.host                  = readEnvironmentTextOrDefault("ASYN_REDIS_TEST_HOST", "127.0.0.1");
            configuration.port                  = readEnvironmentPortOrDefault("ASYN_REDIS_TEST_PORT", 6379);
            configuration.userName              = readEnvironmentTextOrDefault("ASYN_REDIS_TEST_USER", "");
            configuration.password              = password;
            configuration.database              = readEnvironmentTextOrDefault("ASYN_REDIS_TEST_DATABASE", kDefaultTestKeyspace);
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

    /**
     * @brief 钉住环境变量配置能完成建连与认证，且 PING 的状态回复按字符串交出
     */
    TEST_F(RedisIntegrationTest, ConnectsAndAnswersPing)
    {
        // 建连（含认证）已在 SetUp 里完成；这里确认链路真的可用：PING 的状态回复是 "PONG"
        const std::optional<DatabaseValue> reply = runScalar({"PING"});
        ASSERT_TRUE(reply.has_value());

        const auto *text = std::get_if<std::string>(&reply.value());
        ASSERT_NE(text, nullptr) << "状态回复应按 std::string 交出";
        EXPECT_EQ(*text, "PONG");
    }

    /**
     * @brief 钉住错误口令建连失败时的原因是中文且点明认证环节，而不是笼统的连接失败
     */
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

    /**
     * @brief 钉住状态回复按字符串、整数回复按 int64 交出，且映射对多条命令一致
     */
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

    /**
     * @brief 钉住按字节存取的取值原样往返：中文、引号、换行与内嵌 '\0' 都不被改写或截断
     */
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

    /**
     * @brief 钉住参数逐个独立送达：含空格与引号的取值不会被拼成一条命令行
     */
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

    /**
     * @brief 钉住 nil 回复映射成 0 行 0 列的空结果集，且不把空结果当错误
     */
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

    /**
     * @brief 钉住空数组回复与 nil 同样落到空结果集，调用方无需区分两种协议形态
     */
    TEST_F(RedisIntegrationTest, EmptyArrayYieldsEmptyResult)
    {
        const std::string key = makeKey("empty-list");

        // 空数组与 nil 都落到空结果集：调用方不必为「没有元素」区分两种协议形态
        const std::unique_ptr<DatabaseResult> result = m_connection->executeCommand({"LRANGE", key, "0", "-1"});
        ASSERT_NE(result, nullptr) << m_connection->lastError();
        EXPECT_TRUE(result->isEmpty());
        EXPECT_TRUE(result->lastError().empty()) << result->lastError();
    }

    /**
     * @brief 钉住数组回复按「单行、一列一个元素」交出且保持服务端顺序，列名是合成占位名
     */
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

    /**
     * @brief 钉住连接级命令错误返回 nullptr 并把中文原因写进连接的 lastError()
     */
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

    /**
     * @brief 钉住管道里单条命令失败不拖垮整批，错误原因留在该条自己的结果上
     */
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

    /**
     * @brief 钉住管道一次 flush 按登记顺序收齐回复，且非法命令在登记阶段就被拒
     */
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

    /**
     * @brief 钉住相邻命令参数条数一升一降时，每条命令仍只带着自己的参数送达
     * @details 发送阶段那对「指针 + 长度」暂存表跨条复用，于是「上一条的参数尾巴接到下一条」成为
     *          这条路径独有的风险；条数先 5→2 再 2→7 才能把复用方向的两个侧面都钉住。
     */
    TEST_F(RedisIntegrationTest, PipelineAcrossRisingAndFallingArgumentCountsKeepsCommandsIntact)
    {
        const std::string firstKey  = makeKey("wide-a");
        const std::string secondKey = makeKey("wide-b");
        const std::string thirdKey  = makeKey("wide-c");

        // 参数条数依次 5 / 2 / 2 / 7 / 5：暂存表少清一次，第 2 条就会把第 1 条的尾巴一起发出去
        ASSERT_TRUE(m_connection->pipelineCommand("MSET " + firstKey + " 1 " + secondKey + " 2"));
        ASSERT_TRUE(m_connection->pipelineCommand("GET " + firstKey));
        ASSERT_TRUE(m_connection->pipelineCommand("INCR " + thirdKey));
        ASSERT_TRUE(m_connection->pipelineCommand("MSET " + firstKey + " 10 " + secondKey + " 20 " + thirdKey + " 30"));
        ASSERT_TRUE(m_connection->pipelineCommand("MGET " + firstKey + " " + secondKey + " " + thirdKey));

        const std::vector<std::unique_ptr<DatabaseResult>> replies = m_connection->flushPipeline();
        ASSERT_EQ(replies.size(), 5U);
        for (const std::unique_ptr<DatabaseResult> &reply: replies)
        {
            ASSERT_NE(reply, nullptr);
            // 一条命令被接上多余的参数，服务端必回 wrong number of arguments 的 error，
            // 而管道路径把 error 留在**该条自己**的结果上，因此这里逐条验空
            EXPECT_TRUE(reply->lastError().empty()) << reply->lastError();
        }

        // 逐条核对取值：只判「没报错」不够，参数错位仍可能拼出一条合法但含义不同的命令
        EXPECT_EQ(std::get<std::string>(replies[0]->getValue(0)), "OK");
        EXPECT_EQ(std::get<std::string>(replies[1]->getValue(0)), "1");
        EXPECT_EQ(std::get<std::int64_t>(replies[2]->getValue(0)), 1);
        EXPECT_EQ(std::get<std::string>(replies[3]->getValue(0)), "OK");

        ASSERT_EQ(replies[4]->columnCount(), 3U);
        EXPECT_EQ(std::get<std::string>(replies[4]->getValue(0)), "10");
        EXPECT_EQ(std::get<std::string>(replies[4]->getValue(1)), "20");
        EXPECT_EQ(std::get<std::string>(replies[4]->getValue(2)), "30");
    }

    /**
     * @brief 钉住单命令路径的参数表在「栈上容量」两侧都不改变送达结果
     * @details 发送前的「指针 + 长度」数组现在放在栈上（至多 8 个参数），超出才退回堆。
     *          两侧各钉一条：参数被接错、丢失或多送都会体现在长度与顺序上，
     *          所以既验 LLEN 也按 LRANGE 逐项比对
     */
    TEST_F(RedisIntegrationTest, SingleCommandArgumentTableHoldsAcrossInlineCapacity)
    {
        const std::string inlineKey = makeKey("inline-args");
        const std::string heapKey   = makeKey("heap-args");

        // 8 个参数（命令名 + 键 + 6 个值）：正好落在栈上容量之内
        ASSERT_TRUE(m_connection->executeCommand({"LPUSH", inlineKey, "a1", "a2", "a3", "a4", "a5", "a6"}) != nullptr)
            << m_connection->lastError();
        // 12 个参数：超出容量，走堆上的兜底数组
        ASSERT_TRUE(m_connection->executeCommand({"LPUSH", heapKey, "b1", "b2", "b3", "b4", "b5", "b6",
                                                 "b7", "b8", "b9", "b10"}) != nullptr)
            << m_connection->lastError();

        const std::optional<DatabaseValue> inlineLength = runScalar({"LLEN", inlineKey});
        const std::optional<DatabaseValue> heapLength   = runScalar({"LLEN", heapKey});
        ASSERT_TRUE(inlineLength.has_value() && heapLength.has_value());
        EXPECT_EQ(std::get<std::int64_t>(inlineLength.value()), 6) << "栈上参数表把六个值送丢了";
        EXPECT_EQ(std::get<std::int64_t>(heapLength.value()), 10) << "堆上参数表把十个值送丢了";

        // 逐项比对顺序：LPUSH 逐个插到队头，因此读出应为写入的逆序
        const std::unique_ptr<DatabaseResult> reversed = m_connection->executeCommand({"LRANGE", heapKey, "0", "-1"});
        ASSERT_NE(reversed, nullptr) << m_connection->lastError();
        ASSERT_EQ(reversed->columnCount(), 10U);
        EXPECT_EQ(std::get<std::string>(reversed->getValue(0)), "b10");
        EXPECT_EQ(std::get<std::string>(reversed->getValue(9)), "b1");
    }

    /**
     * @brief 钉住归还连接时的会话复位丢弃残留管道命令：命令既不发出也不串给下一个借用者
     */
    TEST_F(RedisIntegrationTest, ResetSessionStateDiscardsPendingPipelineCommands)
    {
        const std::string key = makeKey("reset-session");

        // 登记一条管道命令但**不 flush**：这正是「借了连接没写完就还回去」的形态
        ASSERT_TRUE(m_connection->pipelineCommand("SET " + key + " must-not-be-sent"));

        // 归还路径上的会话复位（池在归还时统一调它）：残留命令必须被丢掉
        m_connection->resetSessionState();

        const std::vector<std::unique_ptr<DatabaseResult>> replies = m_connection->flushPipeline();
        EXPECT_TRUE(replies.empty()) << "复位之后仍有命令被发出：残留管道会串给下一个借用者";

        // 反向确认：那条命令确实没到服务端（EXISTS 回 0）
        const std::optional<DatabaseValue> existsValue = runScalar({"EXISTS", key});
        ASSERT_TRUE(existsValue.has_value());
        EXPECT_EQ(std::get<std::int64_t>(*existsValue), 0) << "被丢弃的管道命令却在服务端生效了";
    }

    /**
     * @brief 钉住归还连接时的会话复位会了结上一个借用者留下的 MULTI：写命令不再被静默排队
     * @details MULTI 留在服务端一侧，本地清管道缓冲清不掉它。不 DISCARD 时下一个借用者的每条写命令
     *          都只收到 +QUEUED 而根本不执行——返回值非空、看着像成功，数据却一条都没落库。
     */
    TEST_F(RedisIntegrationTest, ResetSessionStateDiscardsLeftoverTransaction)
    {
        const std::string key = makeKey("reset-transaction");

        // 命令名按小写发出：Redis 不区分大小写，本类的记账因此也必须不区分
        ASSERT_NE(m_connection->execute("multi"), nullptr) << m_connection->lastError();

        const std::unique_ptr<DatabaseResult> queuedReply = m_connection->executeCommand({"SET", key, "queued-only"});
        ASSERT_NE(queuedReply, nullptr) << m_connection->lastError();
        // 先钉住「排队」这一形态本身：后面的断言才有对照意义
        ASSERT_TRUE(std::holds_alternative<std::string>(queuedReply->getValue(0)));
        EXPECT_EQ(std::get<std::string>(queuedReply->getValue(0)), "QUEUED");

        // 归还路径上的会话复位（池在归还时统一调它）：未了结的事务必须被 DISCARD 掉
        m_connection->resetSessionState();

        ASSERT_NE(m_connection->executeCommand({"SET", key, "after-reset"}), nullptr) << m_connection->lastError();

        const std::optional<DatabaseValue> readBack = runScalar({"GET", key});
        ASSERT_TRUE(readBack.has_value());
        EXPECT_EQ(std::get<std::string>(*readBack), "after-reset") << "复位之后写命令仍被排进上一个借用者的事务";
    }

    /**
     * @brief 钉住已经 EXEC 了结的事务不再触发复位命令，也不影响后续取值
     * @details 记账若不在 EXEC 时清零，归还路径会白发一条 DISCARD 并换来一句
     *          「DISCARD without MULTI」——白付一次往返，还把错误原因留在 lastError() 里冒充本次失败。
     */
    TEST_F(RedisIntegrationTest, CompletedTransactionLeavesNothingForSessionReset)
    {
        const std::string key = makeKey("reset-executed");

        ASSERT_NE(m_connection->executeCommand({"MULTI"}), nullptr) << m_connection->lastError();
        ASSERT_NE(m_connection->executeCommand({"SET", key, "1"}), nullptr) << m_connection->lastError();
        ASSERT_NE(m_connection->executeCommand({"EXEC"}), nullptr) << m_connection->lastError();

        m_connection->resetSessionState();
        EXPECT_TRUE(m_connection->lastError().empty()) << "事务已经了结，复位却发出了被服务端拒绝的命令："
                                                      << m_connection->lastError();

        const std::optional<DatabaseValue> readBack = runScalar({"GET", key});
        ASSERT_TRUE(readBack.has_value());
        EXPECT_EQ(std::get<std::string>(*readBack), "1");
    }

    /**
     * @brief 钉住会话复位撤掉残留的 WATCH：别的连接改动被盯过的键，不再让下一个借用者的 EXEC 中止
     * @details 只 WATCH 过、没进 MULTI 时 DISCARD 会被服务端拒绝且不撤监视，因此复位必须发 UNWATCH。
     *          监视残留的表现很有迷惑性：下一个借用者自己 MULTI + EXEC 一句没问题，却因为
     *          上一个借用者盯过的键被改动而拿到 nil（事务被判冲突中止），写命令一条都没执行。
     */
    TEST_F(RedisIntegrationTest, ResetSessionStateUnwatchesLeftoverWatch)
    {
        const std::string key = makeKey("reset-watch");
        ASSERT_NE(m_connection->executeCommand({"SET", key, "initial"}), nullptr) << m_connection->lastError();

        ASSERT_NE(m_connection->executeCommand({"WATCH", key}), nullptr) << m_connection->lastError();

        // 归还路径上的会话复位：此刻本连接唯一残留的会话状态就是这条监视
        m_connection->resetSessionState();

        // 另开一条连接改动同一个键：监视还在的话，这一步就会让这个借用者的事务注定中止
        RedisConnection otherBorrower(m_configuration);
        ASSERT_TRUE(otherBorrower.connect()) << otherBorrower.lastError();
        ASSERT_NE(otherBorrower.executeCommand({"SET", key, "from-next-borrower"}), nullptr) << otherBorrower.lastError();

        // 回到这条连接上按正常事务走一遍：监视已被撤掉，EXEC 才会真的执行写命令
        ASSERT_NE(m_connection->executeCommand({"MULTI"}), nullptr) << m_connection->lastError();
        ASSERT_NE(m_connection->executeCommand({"SET", key, "committed-by-me"}), nullptr) << m_connection->lastError();
        const std::unique_ptr<DatabaseResult> execReply = m_connection->executeCommand({"EXEC"});
        ASSERT_NE(execReply, nullptr) << m_connection->lastError();
        EXPECT_FALSE(execReply->isEmpty()) << "事务被服务端判成冲突而中止：残留的 WATCH 没有在复位时撤掉";

        const std::optional<DatabaseValue> readBack = runScalar({"GET", key});
        ASSERT_TRUE(readBack.has_value());
        EXPECT_EQ(std::get<std::string>(*readBack), "committed-by-me");

        otherBorrower.disconnect();
    }

    /**
     * @brief 钉住「被服务端退回的 EXEC 不算把 WATCH 了结」：会话复位仍会补上 UNWATCH
     * @details Redis 的 EXEC 不在 MULTI 里时回一句 -EXEC without MULTI 就原路返回，那句监视留在服务端。
     *          把这条 error 回复当成「事务已了结」，本地账就跟着清零，归还时什么也不发——下一个借用者
     *          的 EXEC 中不中止，改由上一个借用者盯过的键决定。
     */
    TEST_F(RedisIntegrationTest, RejectedExecDoesNotSettleTheWatchItImplies)
    {
        const std::string key = makeKey("rejected-exec-watch");
        ASSERT_NE(m_connection->executeCommand({"SET", key, "initial"}), nullptr) << m_connection->lastError();

        ASSERT_NE(m_connection->executeCommand({"WATCH", key}), nullptr) << m_connection->lastError();

        // 没进 MULTI 就发 EXEC：服务端退回这条命令，本连接的监视照旧有效
        EXPECT_EQ(m_connection->executeCommand({"EXEC"}), nullptr) << "EXEC without MULTI 本该按失败处理";
        EXPECT_FALSE(m_connection->lastError().empty()) << "被拒的命令要把服务端原文留在 lastError() 里";

        m_connection->resetSessionState();

        // 另开一条连接改动被盯过的键：监视没被撤掉时，这一步就让本连接的下一个事务注定中止
        RedisConnection otherBorrower(m_configuration);
        ASSERT_TRUE(otherBorrower.connect()) << otherBorrower.lastError();
        ASSERT_NE(otherBorrower.executeCommand({"SET", key, "changed-by-other"}), nullptr) << otherBorrower.lastError();

        ASSERT_NE(m_connection->executeCommand({"MULTI"}), nullptr) << m_connection->lastError();
        ASSERT_NE(m_connection->executeCommand({"SET", key, "committed-by-me"}), nullptr) << m_connection->lastError();
        const std::unique_ptr<DatabaseResult> execReply = m_connection->executeCommand({"EXEC"});
        ASSERT_NE(execReply, nullptr) << m_connection->lastError();
        EXPECT_FALSE(execReply->isEmpty()) << "被拒的 EXEC 把 WATCH 记成了已了结，下一个借用者的事务被中止";

        const std::optional<DatabaseValue> readBack = runScalar({"GET", key});
        ASSERT_TRUE(readBack.has_value());
        EXPECT_EQ(std::get<std::string>(*readBack), "committed-by-me");

        otherBorrower.disconnect();
    }

    /**
     * @brief 钉住管道那条路径的会话记账按回复定，不按 append 定
     * @details append 那一刻还不知道服务端认不认这条命令：在 append 处记账，「WATCH + EXEC」这样一条
     *          管道就会把监视记没了，于是复位时漏发 UNWATCH。改到读回复处按条记账之后，同一套判据
     *          在两条发送路径上成立。
     */
    TEST_F(RedisIntegrationTest, PipelineSessionBookkeepingFollowsTheReply)
    {
        const std::string key = makeKey("pipeline-watch");
        ASSERT_NE(m_connection->executeCommand({"SET", key, "initial"}), nullptr) << m_connection->lastError();

        ASSERT_TRUE(m_connection->pipelineCommand("WATCH " + key));
        ASSERT_TRUE(m_connection->pipelineCommand("EXEC"));

        const std::vector<std::unique_ptr<DatabaseResult> > replies = m_connection->flushPipeline();
        ASSERT_EQ(replies.size(), 2U);
        ASSERT_NE(replies[1], nullptr);
        EXPECT_FALSE(replies[1]->lastError().empty()) << "管道里的 EXEC 应当以 error 回复收场，否则这条用例没有构造出前提";

        m_connection->resetSessionState();

        RedisConnection otherBorrower(m_configuration);
        ASSERT_TRUE(otherBorrower.connect()) << otherBorrower.lastError();
        ASSERT_NE(otherBorrower.executeCommand({"SET", key, "changed-by-other"}), nullptr) << otherBorrower.lastError();

        ASSERT_NE(m_connection->executeCommand({"MULTI"}), nullptr) << m_connection->lastError();
        ASSERT_NE(m_connection->executeCommand({"SET", key, "committed-by-me"}), nullptr) << m_connection->lastError();
        const std::unique_ptr<DatabaseResult> execReply = m_connection->executeCommand({"EXEC"});
        ASSERT_NE(execReply, nullptr) << m_connection->lastError();
        EXPECT_FALSE(execReply->isEmpty()) << "管道里被拒的 EXEC 把 WATCH 记成了已了结";

        otherBorrower.disconnect();
    }

    /**
     * @brief 钉住会话复位把键空间还回配置里那个编号：借出去时是什么库，还回来还是什么库
     * @details selectDatabase() 换的是服务端一侧的会话状态，本地清不掉。池上限为 1 时下一个借用者
     *          拿到的就是同一条连接，他还按配置以为自己停在 15 号库，写进去的键却落在别人库里，
     *          而且一句报错都没有。
     */
    TEST_F(RedisIntegrationTest, SessionResetReturnsToTheConfiguredKeySpace)
    {
        constexpr int kForeignKeySpaceIndex = 3;

        const std::string foreignKey = makeKey("keyspace-foreign");
        const std::string homeKey    = makeKey("keyspace-home");

        ASSERT_TRUE(m_connection->selectDatabase(kForeignKeySpaceIndex)) << m_connection->lastError();
        ASSERT_NE(m_connection->executeCommand({"SET", foreignKey, "in-foreign-db"}), nullptr) << m_connection->lastError();
        // 前提：这条连接此刻确实停在别的库上（同一个键在配置库里还不存在）
        ASSERT_TRUE(runScalar({"GET", foreignKey}).has_value()) << "SELECT 没有生效，用例没有构造出可判定的前提";

        m_connection->resetSessionState();
        ASSERT_TRUE(m_connection->isConnected()) << "复位把连接弄断了，下面的判据无从落地";

        // 已还回配置里的库：那个外库键在这里读不到，而本库的读写一切正常
        EXPECT_FALSE(runScalar({"GET", foreignKey}).has_value()) << "复位后仍停在别处，键空间串给了下一个借用者";
        ASSERT_NE(m_connection->executeCommand({"SET", homeKey, "in-configured-db"}), nullptr) << m_connection->lastError();
        const std::optional<DatabaseValue> homeRead = runScalar({"GET", homeKey});
        ASSERT_TRUE(homeRead.has_value()) << "回到配置库这一步没做成：连本库的写都读不到";
        EXPECT_EQ(std::get<std::string>(*homeRead), "in-configured-db");

        // 外库那个键只能由认识它的连接来删：TearDown 的清扫扫得到配置库，扫不到 3 号库
        RedisConnection foreignCleaner(m_configuration);
        ASSERT_TRUE(foreignCleaner.connect()) << foreignCleaner.lastError();
        ASSERT_TRUE(foreignCleaner.selectDatabase(kForeignKeySpaceIndex)) << foreignCleaner.lastError();
        static_cast<void>(foreignCleaner.executeCommand({"DEL", foreignKey}));
        foreignCleaner.disconnect();
    }

    /**
     * @brief 钉住「退不回去的会话模式」在归还时被断开，而不是带着错位的回复流回池
     * @details MONITOR 之后服务端持续推送，本类按「一条命令一次回复」读，下一位借用者读到的
     *          会是别人的跟踪行。这类模式没有可靠的撤销命令（RESET 要 6.2+），因此判据就是
     *          isConnected() 转假：池的健康检查看到假就会另起一条。
     */
    TEST_F(RedisIntegrationTest, MonitorStyleSessionIsDroppedOnReturn)
    {
        ASSERT_NE(m_connection->executeCommand({"MONITOR"}), nullptr) << m_connection->lastError();
        ASSERT_TRUE(m_connection->isConnected());

        m_connection->resetSessionState();

        EXPECT_FALSE(m_connection->isConnected()) << "MONITOR 之后的连接被当成健康连接交还给下一个借用者";

        // 对照：没进过这种模式的连接复位后照常可用（否则上面的断开看不出差别）
        RedisConnection cleanConnection(m_configuration);
        ASSERT_TRUE(cleanConnection.connect()) << cleanConnection.lastError();
        cleanConnection.resetSessionState();
        EXPECT_TRUE(cleanConnection.isConnected()) << "干净的连接也被复位路径断开了";
        cleanConnection.disconnect();
    }

    /**
     * @brief 守卫非正值的连接超时按「不设超时」处理，而不是被当成配置错误拒掉
     * @details setQueryTimeout() 明写「0 与负数一律按不设超时」，MySQL 驱动同口径；建连这一侧原先把
     *          -1 毫秒折算成 tv_sec=0、tv_usec=-1000 交给 select()，那是一次无效或零窗口的等待。
     * @warning 本用例证伪不成立：把换算改回旧写法它照样绿——回环地址上的握手在第一帧就已完成，
     *          零窗口的等待也读得到「可写」。真正的差别只在慢速链路上看得见，这里钉住的是较弱的一条：
     *          非正值仍然要能连上并发得出命令，而不是被折算成立刻失败或直接判成非法配置。
     */
    TEST_F(RedisIntegrationTest, NonPositiveConnectTimeoutDoesNotMeanInstantFailure)
    {
        for (const int timeoutMilliseconds: {0, -1})
        {
            RedisConnection connection(m_configuration);
            connection.setConnectTimeout(timeoutMilliseconds);
            EXPECT_TRUE(connection.connect()) << "connectTimeout=" << timeoutMilliseconds
                                              << " 被折算成立刻超时：" << connection.lastError();
            EXPECT_TRUE(connection.isConnected());

            // 连上了还得能发命令：只把 connect() 判成成功而句柄不可用，同样是不设超时没落地
            if (connection.isConnected())
            {
                EXPECT_NE(connection.executeCommand({"PING"}), nullptr) << connection.lastError();
            }
            connection.disconnect();
        }
    }

    /**
     * @brief 钉住「经连接池借还」这一使用形状下，残留会话不会串给下一个借用者
     *
     * @details 上面几条直接调 resetSessionState()，钉住的是驱动那一层；真正会出事的形状是借用者把
     *          「登记了却没 flush 的管道」与「没 DISCARD 的 MULTI」原样还进池。池上限设 1，
     *          保证第二个借用者拿到的就是同一条连接——换一条新连接的话这条用例什么也没钉住。
     */
    TEST_F(RedisIntegrationTest, PooledReturnClearsSessionForNextBorrower)
    {
        const std::string staleKey = makeKey("pooled-stale");
        const std::string freshKey = makeKey("pooled-fresh");

        PoolConfig poolConfiguration;
        poolConfiguration.maximumPoolSize            = 1;
        poolConfiguration.acquireTimeoutMilliseconds = 5000;
        const ConnectionConfig configuration         = m_configuration;
        ConnectionPool pool(
            [configuration]() -> std::unique_ptr<DatabaseConnection>
            {
                auto connection = std::make_unique<RedisConnection>(configuration);
                // 池的工厂契约要求交出「已经 connect() 完成」的连接
                static_cast<void>(connection->connect());
                return connection;
            },
            poolConfiguration);

        // 命令数组与管道接口在 RedisConnection 上而不是基类，取值前先按类型取回具体驱动
        const RedisConnection *firstBorrowedConnection = nullptr;
        {
            const PooledConnection borrowed = pool.acquire();
            ASSERT_TRUE(borrowed) << "池里没能建起 Redis 连接";
            auto *redisConnection = dynamic_cast<RedisConnection *>(borrowed.operator->());
            ASSERT_TRUE(redisConnection != nullptr) << "池交出的不是 RedisConnection";
            firstBorrowedConnection = redisConnection;

            ASSERT_TRUE(redisConnection->pipelineCommand("SET " + staleKey + " must-not-be-sent"));
            ASSERT_NE(redisConnection->executeCommand({"MULTI"}), nullptr) << redisConnection->lastError();
            // 带着未 flush 的管道与未了结的事务离开作用域：归还路径要把两样都了结
        }

        const PooledConnection nextBorrowed = pool.acquire();
        ASSERT_TRUE(nextBorrowed) << "上一个借用者的连接没有回到池里";
        auto *nextConnection = dynamic_cast<RedisConnection *>(nextBorrowed.operator->());
        ASSERT_TRUE(nextConnection != nullptr) << "池交出的不是 RedisConnection";
        ASSERT_EQ(nextConnection, firstBorrowedConnection) << "池没有复用同一条连接，判据无从落地";

        // 未 flush 的管道只停在本地暂存表：复位丢表之后这次 GET 只能读到 nil
        const std::unique_ptr<DatabaseResult> staleRead = nextConnection->executeCommand({"GET", staleKey});
        ASSERT_NE(staleRead, nullptr) << nextConnection->lastError();
        EXPECT_TRUE(staleRead->isEmpty()) << "上一个借用者的 SET 串到了这条连接上";

        // MULTI 停在服务端一侧，本地清缓冲清不掉：没 DISCARD 时这条写只会收到 +QUEUED 而不落库
        const std::unique_ptr<DatabaseResult> freshWrite = nextConnection->executeCommand({"SET", freshKey, "fresh"});
        ASSERT_NE(freshWrite, nullptr) << nextConnection->lastError();
        EXPECT_EQ(std::get<std::string>(freshWrite->getValue(0)), "OK");

        const std::optional<DatabaseValue> readBack = runScalar({"GET", freshKey});
        ASSERT_TRUE(readBack.has_value());
        EXPECT_EQ(std::get<std::string>(*readBack), "fresh") << "写命令被排进上一个借用者的事务，数据没落库";
    }

    /**
     * @brief 钉住连接期按配置执行键空间选择：同一个键在默认 0 号库中不可见
     */
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

    /**
     * @brief 钉住 execute() 的整行切词路径与显式参数列表结果一致，未闭合引号在本地被拒
     */
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

    /** @brief 钉住已建立的连接上改 queryTimeout 当场生效：阻塞命令被新值截断，不必断开重连 */
    TEST_F(RedisIntegrationTest, QueryTimeoutChangeAppliesToEstablishedConnection)
    {
        // BLPOP 的超时参数给 0 是「服务端无限期挂住这条命令」，能截断它的只有客户端的收发超时。
        // 若实现只在 connect() 读一次这个值，这一步要等满建连时的 30 秒默认值，用例就红在耗时上界
        m_connection->setQueryTimeout(200);
        const std::string blockedKey = makeKey("blpop-timeout");

        const auto startedAt = std::chrono::steady_clock::now();
        const std::unique_ptr<DatabaseResult> blocked = m_connection->executeCommand({"BLPOP", blockedKey, "0"});
        const auto elapsedMilliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();

        ASSERT_EQ(blocked, nullptr) << "BLPOP 拿到了回复，说明新的收发超时没落到上下文上";
        EXPECT_LT(elapsedMilliseconds, 3000) << "耗时 " << elapsedMilliseconds << " 毫秒，不像是 200 毫秒截断的";
        EXPECT_TRUE(containsLocalizedText(m_connection->lastError())) << m_connection->lastError();

        // 收发超时属传输层失败：回复流的位置已不可知，这条连接必须被判为不可再用（池据此换掉它）
        EXPECT_FALSE(m_connection->isConnected());
    }

} // namespace AsynGyanis::Database
