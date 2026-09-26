// Redis 订阅与键空间通知消费的用例：解析形状、无服务端的入口判据，以及真机上把推送读干净
//
// 分三层，缺哪一层都会留下查不出的空档：
// 1. parseKeyspaceNotification() 是纯字符串工作，两种构建（有/无 hiredis）都要跑得动，
//    而「__keyspace__ 与 __keyevent__ 的两半正好互换」这件事只靠读文档极易记反；
// 2. 空目标列表、未连接这类判据不需要服务端，也不能只在桩构建里成立；
// 3. 真机层验的是「确认回复被收干」「超时的空等不弄坏连接」「退订后能回到一条命令一条回复」
//    ——这三条都得跟真的服务端过一遍才作数，hiredis 怎么报读超时尤其只能实测。
//
// 真机门控与 TestRedisIntegration.cpp 同一条：未设置 ASYN_REDIS_TEST_PASSWORD 时整组 GTEST_SKIP，
// 仓库里不留明文口令。notify-keyspace-events 是**服务端全局**配置：改之前先读回原值，收尾原样还回去。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisConnection.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

        /// 本套件写入的键共用的前缀：便于人工排查，也避免与使用者的键撞车
        constexpr std::string_view kKeyPrefix = "asyngyanis:subscribe-test:";
    } // namespace

    // ------------------------------------------------------------------
    // 1. 通知形状的解析（不需要服务端）
    // ------------------------------------------------------------------

    /**
     * @brief __keyspace__ 形状：频道带键名，正文是事件名
     */
    TEST(RedisKeyspaceNotificationParsing, KeyspaceChannelCarriesTheKeyAndThePayloadCarriesTheEvent)
    {
        const std::optional<RedisKeyspaceNotification> notification = RedisConnection::parseKeyspaceNotification("__keyspace@3__:user:42", "set");
        ASSERT_TRUE(notification.has_value());
        EXPECT_FALSE(notification->isKeyEvent);
        EXPECT_EQ(notification->database, 3);
        EXPECT_EQ(notification->key, "user:42") << "键名里带 ':' 时只能从第一个 ':' 切，切错就少一截";
        EXPECT_EQ(notification->event, "set");
    }

    /**
     * @brief __keyevent__ 形状：两半正好互换，频道带事件名、正文是键
     */
    TEST(RedisKeyspaceNotificationParsing, KeyEventChannelSwapsKeyAndEvent)
    {
        const std::optional<RedisKeyspaceNotification> notification = RedisConnection::parseKeyspaceNotification("__keyevent@0__:expired", "session:7");
        ASSERT_TRUE(notification.has_value());
        EXPECT_TRUE(notification->isKeyEvent);
        EXPECT_EQ(notification->database, 0);
        EXPECT_EQ(notification->event, "expired");
        EXPECT_EQ(notification->key, "session:7");
    }

    /**
     * @brief 看着像但不是的频道一律不解释，而不是凑出一个 notification
     * @details 认错的后果是静默给出错误的键名（例如把 @0 当成库号的一部分），比直接拒绝难查得多。
     *          这批里特意放了「只差一对下划线」与「只差一个下划线」两种：真实频道形状是
     *          `__keyspace@<库>__:<名>`，那对下划线是库号与名字之间的唯一分隔，判据必须真在读它，
     *          而不是任何不匹配都恰好落到同一个分支（本用例的第一版就是因为解析器认错形状而全红的）。
     */
    TEST(RedisKeyspaceNotificationParsing, LookalikeChannelsAreRejected)
    {
        for (const std::string_view channel:
             {std::string_view{""}, std::string_view{"__keyspace@__:k"}, std::string_view{"__keyspace@abc__:k"}, std::string_view{"__keyspace@-1__:k"},
              std::string_view{"__keyspace@3:user:42"}, std::string_view{"__keyspace@3_:user:42"}, std::string_view{"__keyspace@3__user:42"}, std::string_view{"__keyspace@0__:"},
              std::string_view{"keyspace@0__:k"}, std::string_view{"__keyevent@0__"}})
        {
            EXPECT_FALSE(RedisConnection::parseKeyspaceNotification(channel, "set").has_value()) << "这个频道名不该被解释成键空间通知：" << channel;
        }
    }

    // ------------------------------------------------------------------
    // 2. 不需要服务端的入口判据（两种构建下都要成立）
    // ------------------------------------------------------------------

    /**
     * @brief 空目标列表当场拒绝，不发出任何字节
     * @details 发出去会得到一条「ERR wrong number of arguments」，而 SUBSCRIBE 一旦被服务端认下，
     *          连接就被标成推送形态——白丢一条可复用的连接
     */
    TEST(RedisSubscription, EmptyTargetListIsRejectedBeforeTouchingTheSocket)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        EXPECT_FALSE(connection.subscribe(std::span<const std::string_view>{}));
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_FALSE(connection.psubscribe(std::span<const std::string_view>{}));
        EXPECT_FALSE(connection.lastError().empty());
        EXPECT_FALSE(connection.isSubscribing()) << "没发出去的命令不该把连接标成订阅形态";
    }

    /**
     * @brief 未连接时读推送给出中文原因，而不是挂住或返回一条空推送
     */
    TEST(RedisSubscription, ReadingWithoutAConnectionReportsWhy)
    {
        RedisConnection connection(ConnectionConfig::redisDefault());

        const std::optional<RedisPushReply> reply = connection.readPushReply(std::chrono::milliseconds{10});
        EXPECT_FALSE(reply.has_value());
        EXPECT_FALSE(connection.lastError().empty()) << "空返回值既可能是「没有消息」也可能是「读不到」，必须留下原因";
    }

    // ------------------------------------------------------------------
    // 3. 真机：把推送读干净、等不到不弄坏连接、退订后回到正常形态
    // ------------------------------------------------------------------

    namespace
    {
        /**
         * @brief 真机订阅用例的夹具：组配置、开键空间事件，收尾把服务端配置还回去
         */
        class RedisSubscriptionIntegration : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                if constexpr (!kRedisDriverCompiled)
                {
                    GTEST_SKIP() << "当前构建未编译 Redis 驱动（未定义 DATABASE_HAS_REDIS），跳过真机用例";
                }
                const std::string password = TestSupport::readEnvironmentVariableText("ASYN_REDIS_TEST_PASSWORD");
                if (password.empty())
                {
                    GTEST_SKIP() << "未设置 ASYN_REDIS_TEST_PASSWORD，跳过 Redis 真机用例";
                }

                m_configuration          = ConnectionConfig::redisDefault();
                m_configuration.host     = TestSupport::readEnvironmentTextOrDefault("ASYN_REDIS_TEST_HOST", "127.0.0.1");
                m_configuration.port     = TestSupport::readEnvironmentPortOrDefault("ASYN_REDIS_TEST_PORT", 6379);
                m_configuration.userName = TestSupport::readEnvironmentTextOrDefault("ASYN_REDIS_TEST_USER", "");
                m_configuration.password = password;
                m_configuration.database = TestSupport::readEnvironmentTextOrDefault("ASYN_REDIS_TEST_DATABASE", "15");

                // 写入侧用另一条连接：订阅中的连接不能再发普通命令——推送会插在回复前面，
                // 「一条命令一条回复」的配对当场错位（这正是要分开的原因，也是调用方要守的规矩）
                m_writer = std::make_unique<RedisConnection>(m_configuration);
                if (!m_writer->connect())
                {
                    GTEST_SKIP() << "连不上 Redis 服务端，跳过真机用例：" << m_writer->lastError();
                }

                // notify-keyspace-events 是服务端全局配置：先读回原值，TearDown 原样还回去，
                // 免得把并行使用同一个服务端的别处（或下一次运行）留在打开状态
                // CONFIG GET 回的是「参数名、参数值」两元素数组：值在下标 1，取 0 会读回参数名本身
                const std::unique_ptr<DatabaseResult> current = TestSupport::executeRequired(*m_writer, "CONFIG GET notify-keyspace-events");
                ASSERT_NE(current, nullptr);
                m_previousNotifyEvents = TestSupport::asText(current->getValue(1)).value_or("");

                ASSERT_NE(TestSupport::executeRequired(*m_writer, "CONFIG SET notify-keyspace-events KEA"), nullptr)
                        << "本机 Redis 不让改 notify-keyspace-events，真机订阅用例跑不了";
            }

            void TearDown() override
            {
                if (m_writer == nullptr)
                {
                    return;
                }
                // 还原失败不报：用例已经把该看的看过了，这里再断言只会把环境问题放大成红
                static_cast<void>(m_writer->execute("CONFIG SET notify-keyspace-events " + (m_previousNotifyEvents.empty() ? "\"\"" : m_previousNotifyEvents)));
            }

            /**
             * @brief 本进程专用的唯一键名：并行跑同一套件时互不干扰
             * @return std::string 完整键名
             */
            [[nodiscard]] std::string makeKey()
            {
                return std::string(kKeyPrefix) + TestSupport::makeUniqueDatabaseName("k");
            }

            /// 键空间编号：频道名里用的是数字形式，配置里存的是文本
            [[nodiscard]] int keySpaceIndex() const
            {
                return std::atoi(m_configuration.database.c_str());
            }

            /**
             * @brief 等一条推送，最多等 waitTimeout，超时按「没等到」计
             */
            [[nodiscard]] std::optional<RedisPushReply> readWithin(RedisConnection &connection, const int waitTimeoutMilliseconds)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{waitTimeoutMilliseconds};
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (const std::optional<RedisPushReply> reply = connection.readPushReply(std::chrono::milliseconds{50}))
                    {
                        return reply;
                    }
                    if (!connection.lastError().empty())
                    {
                        // 不是「这次没等到」而是读坏了（原因已写下、连接已被断开）：再等也没有意义
                        return std::nullopt;
                    }
                }
                return std::nullopt;
            }

            ConnectionConfig                 m_configuration;
            std::unique_ptr<RedisConnection> m_writer;
            std::string                      m_previousNotifyEvents;
        };
    } // namespace

    /**
     * @brief 订阅之后，键的改动作为推送回到本连接
     * @details 同时钉住两件事：SUBSCRIBE 的确认已被 subscribe() 收干（第一手读到的就是消息而不是
     *          「subscribe 确认」），以及 __keyspace__ 通知解出来的键与事件确实对得上。
     */
    TEST_F(RedisSubscriptionIntegration, SubscriptionDeliversKeySpaceNotifications)
    {
        RedisConnection subscriber(m_configuration);
        ASSERT_TRUE(subscriber.connect()) << subscriber.lastError();

        const std::string key     = makeKey();
        const std::string channel = "__keyspace@" + std::to_string(keySpaceIndex()) + "__:" + key;

        const std::vector<std::string>      channelList{channel};
        const std::vector<std::string_view> channelViews(channelList.begin(), channelList.end());
        ASSERT_TRUE(subscriber.subscribe(channelViews)) << subscriber.lastError();
        EXPECT_TRUE(subscriber.isSubscribing());

        ASSERT_NE(m_writer->execute("SET " + key + " 1"), nullptr) << m_writer->lastError();

        const std::optional<RedisPushReply> setNotification = readWithin(subscriber, 2000);
        ASSERT_TRUE(setNotification.has_value()) << "订阅后 SET 没有收到通知：" << subscriber.lastError();
        EXPECT_EQ(setNotification->kind, "message") << "第一手就该是消息：读到 subscribe 确认说明没收干净";
        EXPECT_EQ(setNotification->channel, channel);
        EXPECT_EQ(setNotification->payload, "set");

        ASSERT_NE(m_writer->execute("DEL " + key), nullptr) << m_writer->lastError();
        const std::optional<RedisPushReply> delNotification = readWithin(subscriber, 2000);
        ASSERT_TRUE(delNotification.has_value()) << subscriber.lastError();

        const std::optional<RedisKeyspaceNotification> notification = RedisConnection::parseKeyspaceNotification(delNotification->channel, delNotification->payload);
        ASSERT_TRUE(notification.has_value());
        EXPECT_EQ(notification->key, key);
        EXPECT_EQ(notification->event, "del");
    }

    /**
     * @brief 等不到消息的空等不弄坏连接，下一条推送仍能读到
     * @details hiredis 把读超时报告成上下文错误。若本类照普通命令的失败路径处理，每空等一次就得
     *          重连一次——那等于「订阅消费」根本不可用。这条用例是唯一能证伪这一点的判据。
     */
    TEST_F(RedisSubscriptionIntegration, IdleWaitKeepsTheConnectionReadable)
    {
        RedisConnection subscriber(m_configuration);
        ASSERT_TRUE(subscriber.connect()) << subscriber.lastError();

        const std::string                   key     = makeKey();
        const std::string                   channel = "__keyspace@" + std::to_string(keySpaceIndex()) + "__:" + key;
        const std::vector<std::string>      channelList{channel};
        const std::vector<std::string_view> channelViews(channelList.begin(), channelList.end());
        ASSERT_TRUE(subscriber.subscribe(channelViews)) << subscriber.lastError();

        // 先空等两轮：这一段时间上没有任何推送
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            EXPECT_FALSE(subscriber.readPushReply(std::chrono::milliseconds{80}).has_value()) << "本轮本就不该有消息";
            EXPECT_TRUE(subscriber.lastError().empty()) << "空等被判成了失败：" << subscriber.lastError();
        }

        ASSERT_NE(m_writer->execute("SET " + key + " 2"), nullptr) << m_writer->lastError();
        const std::optional<RedisPushReply> reply = readWithin(subscriber, 2000);
        ASSERT_TRUE(reply.has_value()) << "空等过之后连接再也读不到推送：" << subscriber.lastError();
        EXPECT_EQ(reply->kind, "message");
    }

    /**
     * @brief 模式订阅的 pmessage 把模式、命中频道与正文三段分别交出
     */
    TEST_F(RedisSubscriptionIntegration, PatternSubscriptionReportsPatternChannelAndPayload)
    {
        RedisConnection subscriber(m_configuration);
        ASSERT_TRUE(subscriber.connect()) << subscriber.lastError();

        const std::string                   key     = makeKey();
        const std::string                   pattern = "__keyevent@" + std::to_string(keySpaceIndex()) + "__:*";
        const std::vector<std::string>      patternList{pattern};
        const std::vector<std::string_view> patternViews(patternList.begin(), patternList.end());
        ASSERT_TRUE(subscriber.psubscribe(patternViews)) << subscriber.lastError();

        ASSERT_NE(m_writer->execute("SET " + key + " 3"), nullptr) << m_writer->lastError();

        const std::optional<RedisPushReply> reply = readWithin(subscriber, 2000);
        ASSERT_TRUE(reply.has_value()) << subscriber.lastError();
        EXPECT_EQ(reply->kind, "pmessage");
        EXPECT_EQ(reply->pattern, pattern);
        EXPECT_EQ(reply->channel, "__keyevent@" + std::to_string(keySpaceIndex()) + "__:set");
        EXPECT_EQ(reply->payload, key);
    }

    /**
     * @brief 退订之后这条连接回到「一条命令一条回复」的形态
     * @details 验的是 unsubscribeAll() 把确认读干净了：留着没读的确认，下面那条 SET 的回复就会
     *          被上一条确认顶掉——那正是 TestRedisIntegration 里「订阅形态的连接必须断开」的成因。
     */
    TEST_F(RedisSubscriptionIntegration, UnsubscribeAllRestoresTheCommandReplyShape)
    {
        RedisConnection subscriber(m_configuration);
        ASSERT_TRUE(subscriber.connect()) << subscriber.lastError();

        const std::string                   key = makeKey();
        const std::vector<std::string>      channelList{"__keyspace@" + std::to_string(keySpaceIndex()) + "__:" + key};
        const std::vector<std::string_view> channelViews(channelList.begin(), channelList.end());
        ASSERT_TRUE(subscriber.subscribe(channelViews)) << subscriber.lastError();

        ASSERT_TRUE(subscriber.unsubscribeAll()) << subscriber.lastError();
        EXPECT_FALSE(subscriber.isSubscribing());

        const std::unique_ptr<DatabaseResult> afterUnsubscribe = subscriber.execute("SET " + key + " 4");
        ASSERT_NE(afterUnsubscribe, nullptr) << "退订后仍读不回正常回复：" << subscriber.lastError();
        ASSERT_NE(m_writer->execute("DEL " + key), nullptr);
    }
} // namespace AsynGyanis::Database
