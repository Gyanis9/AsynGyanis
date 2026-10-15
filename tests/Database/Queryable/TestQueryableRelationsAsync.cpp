// 关系装载的异步面（台账 3.3 第一半的收尾）：`toListAsync()` 走的是同一条装载路径。
//
// 与 TestQueryableRelations.cpp 分开成文件，是因为这里要起一个常驻事件循环线程（EventLoopThread）；
// 那一份全是同步用例，不该为一个异步形状把整个夹具抬起来。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/Relations.h"
#include "Database/Queryable/TableSchema.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::Queryable::Relation;
    using AsynGyanis::Database::Queryable::TableSchema;
    using AsynGyanis::Database::TestSupport::EventLoopThread;

    /// 作者（多对一的「一」）
    struct AsyncRelationAuthor
    {
        std::int64_t id;   ///< 主键
        std::string  name; ///< 姓名
    };

    /// 帖子：一个多对一关系成员（不进 kColumns，只进 kRelations）
    struct AsyncRelationPost
    {
        std::int64_t                       id;       ///< 主键
        std::string                        title;    ///< 标题
        std::int64_t                       authorId; ///< 外键列
        std::optional<AsyncRelationAuthor> author;   ///< 装载结果
    };
} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AsyncRelationAuthor>
{
    static constexpr std::string_view kTableName = "async_rel_authors";
    static constexpr auto             kColumns   = std::tuple{
            Column(&AsyncRelationAuthor::id, "id"),
            Column(&AsyncRelationAuthor::name, "name"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AsyncRelationPost>
{
    static constexpr std::string_view kTableName = "async_rel_posts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&AsyncRelationPost::id, "id"),
            Column(&AsyncRelationPost::title, "title"),
            Column(&AsyncRelationPost::authorId, "author_id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
    static constexpr auto             kRelations  = std::tuple{Relation(&AsyncRelationPost::author, "author_id")};
};

/**
 * @brief 钉住异步路径同样会跑关系装载（判据与同步路径一致，只是恢复点在事件循环线程上）
 * @details 异步版的装载在工作线程上执行（`submit` 的 lambda 里），因此「外层连接释放之后才补取」
 *          这条纪律在那一侧同样要成立：池上限为 1 时，握着取数连接去开第二条查询就是死等。
 *          本用例把池压到 1 正是为了把这条钉出来
 * @note 证伪：把 toListAsync 里 `for (loaders)` 那一段删掉，`author` 会保持空，第一条断言红
 */
TEST(QueryableRelationsAsync, LoadsTheSameWayOnTheAsyncPath)
{
    PoolConfig poolConfiguration;
    poolConfiguration.maximumPoolSize    = 1;
    std::unique_ptr<ConnectionPool> pool = std::make_unique<ConnectionPool>(
            []()
            {
                auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                connection->connect();
                return connection;
            },
            poolConfiguration);

    {
        // 建表的租约先归还：池上限为 1，后面的 ORM 调用要能再借到它
        PooledConnection connection = pool->acquire();
        ASSERT_TRUE(connection);
        ASSERT_NE(connection->execute("CREATE TABLE async_rel_authors (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr) << connection->lastError();
        ASSERT_NE(connection->execute("CREATE TABLE async_rel_posts (id INTEGER PRIMARY KEY, title TEXT NOT NULL, author_id INTEGER NOT NULL)"), nullptr)
                << connection->lastError();
    }

    {
        Queryable<AsyncRelationAuthor> authors(*pool);
        ASSERT_EQ(authors.insert(AsyncRelationAuthor{.id = 1, .name = "异步甲方"}), 1);
        Queryable<AsyncRelationPost> posts(*pool);
        ASSERT_EQ(posts.insert(AsyncRelationPost{.id = 10, .title = "异步篇", .authorId = 1}), 1);
    }

    EventLoopThread loopRunner;
    ASSERT_TRUE(loopRunner.waitUntilRunning()) << "后台事件循环未在时限内进入运行状态";

    const auto completed = loopRunner.runToCompletion(Queryable<AsyncRelationPost>(*pool).then(&AsyncRelationPost::author).toListAsync(loopRunner.loop()));
    ASSERT_EQ(completed.error, nullptr);
    ASSERT_TRUE(completed.value.has_value());

    const std::vector<AsyncRelationPost> &posts = *completed.value;
    ASSERT_EQ(posts.size(), 1U);
    ASSERT_TRUE(posts[0].author.has_value()) << "异步路径没跑关系装载";
    EXPECT_EQ(posts[0].author->name, "异步甲方");
}
