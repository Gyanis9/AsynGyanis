// 显式关系装载（台账 3.3）的判据：`then()` 分批补取与 `include()` 一次 JOIN 两个方向。
//
// 全部用 SQLite 内存库（池上限 1）。真机那一侧另有 MySQL 版本（TestMySqlIntegration.cpp）。
// 不做懒加载代理（提示词判死）：这里测的就是「点名装载」这一件事。

#include "Base/Exception/LogicException.h"

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/Relations.h"
#include "Database/Queryable/TableSchema.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
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

    /// 作者（多对一的「一」）
    struct RelationAuthor
    {
        std::int64_t id;   ///< 主键
        std::string  name; ///< 姓名
    };

    /// 评论（一对多的「多」）
    struct RelationComment
    {
        std::int64_t               id;     ///< 主键
        std::int64_t               postId; ///< 所属帖子的主键（外键在子表这一侧）
        std::string                text;   ///< 正文
        std::optional<std::string> note;   ///< 可空的备注列：include 的投影要能与普通映射一样把 NULL 读成空值
    };

    /// 帖子：两个关系成员都不进 kColumns（它们不是列），只进 kRelations
    struct RelationPost
    {
        std::int64_t                  id;       ///< 主键
        std::string                   title;    ///< 标题
        std::int64_t                  authorId; ///< 外键列（多对一）
        std::optional<RelationAuthor> author;   ///< 装载结果：作者
        std::vector<RelationComment>  comments; ///< 装载结果：评论
    };
} // namespace

// ========================================================================
// TableSchema 特化
// ========================================================================

template<>
struct AsynGyanis::Database::Queryable::TableSchema<RelationAuthor>
{
    static constexpr std::string_view kTableName = "rel_authors";
    static constexpr auto             kColumns   = std::tuple{
            Column(&RelationAuthor::id, "id"),
            Column(&RelationAuthor::name, "name"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<RelationComment>
{
    static constexpr std::string_view kTableName = "rel_comments";
    static constexpr auto             kColumns   = std::tuple{
            Column(&RelationComment::id, "id"),
            Column(&RelationComment::postId, "post_id"),
            Column(&RelationComment::text, "text"),
            Column(&RelationComment::note, "note"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<RelationPost>
{
    static constexpr std::string_view kTableName = "rel_posts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&RelationPost::id, "id"),
            Column(&RelationPost::title, "title"),
            Column(&RelationPost::authorId, "author_id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";

    /// 两条关系的方向由成员类型定：`optional<RelationAuthor>` 是多对一（外键列在本表），
    /// `vector<RelationComment>` 是一对多（外键列在对方表）
    static constexpr auto kRelations = std::tuple{
            Relation(&RelationPost::author, "author_id"),
            Relation(&RelationPost::comments, "post_id"),
    };
};

namespace
{
    using AsynGyanis::Base::LogicException;

    /// 子行的返回顺序由服务端定（本仓没有子行排序的声明口）：判据一律按「文本 → 备注」的集合比对，
    /// 不按位置取元素——顺序在 SQLite 与 MySQL 上实测就不一样
    [[nodiscard]] std::map<std::string, std::optional<std::string>> notesByText(const std::vector<RelationComment> &comments)
    {
        std::map<std::string, std::optional<std::string>> byText;
        for (const RelationComment &comment: comments)
        {
            byText.emplace(comment.text, comment.note);
        }
        return byText;
    }

    /**
     * @brief 关系用例的夹具：内存库 + 两张主表 + 三行帖子（其中一行的作者不存在）
     */
    class QueryableRelationsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            PoolConfig poolConfiguration;
            poolConfiguration.maximumPoolSize = 1;
            m_pool                            = std::make_unique<ConnectionPool>(
                    []()
                    {
                        auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                        connection->connect();
                        return connection;
                    },
                    poolConfiguration);

            {
                // 建表的连接在本块结束时归还：池上限为 1，下面的 ORM 插入要能再借到它
                // （握着这条租约去建 Queryable，resolveDialect() 会等满借用超时——这一格踩过）
                PooledConnection connection = m_pool->acquire();
                ASSERT_TRUE(connection);
                ASSERT_NE(connection->execute("CREATE TABLE rel_authors (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr) << connection->lastError();
                ASSERT_NE(connection->execute("CREATE TABLE rel_comments (id INTEGER PRIMARY KEY, post_id INTEGER NOT NULL, text TEXT NOT NULL, note TEXT)"), nullptr)
                        << connection->lastError();
                ASSERT_NE(connection->execute("CREATE TABLE rel_posts (id INTEGER PRIMARY KEY, title TEXT NOT NULL, author_id INTEGER NOT NULL)"), nullptr)
                        << connection->lastError();
            }

            // 种子数据走 ORM 插入：顺带证明两个关系成员不影响 INSERT（它们不在 kColumns 里）
            Queryable<RelationAuthor> authors(*m_pool);
            ASSERT_EQ(authors.insert(RelationAuthor{.id = 1, .name = "甲方"}), 1);
            ASSERT_EQ(authors.insert(RelationAuthor{.id = 2, .name = "乙方"}), 1);

            Queryable<RelationPost> posts(*m_pool);
            ASSERT_EQ(posts.insert(RelationPost{.id = 10, .title = "第一篇", .authorId = 1}), 1);
            ASSERT_EQ(posts.insert(RelationPost{.id = 11, .title = "第二篇", .authorId = 2}), 1);
            // 第三篇指向一个不存在的作者：装载不到时该留下什么，由用例钉住
            ASSERT_EQ(posts.insert(RelationPost{.id = 12, .title = "孤儿篇", .authorId = 999}), 1);

            Queryable<RelationComment> comments(*m_pool);
            ASSERT_EQ(comments.insert(RelationComment{.id = 100, .postId = 10, .text = "第一评", .note = "置顶"}), 1);
            ASSERT_EQ(comments.insert(RelationComment{.id = 101, .postId = 10, .text = "第二评", .note = std::nullopt}), 1);
            ASSERT_EQ(comments.insert(RelationComment{.id = 102, .postId = 11, .text = "另一篇的评", .note = std::nullopt}), 1);
            // 12 号帖一条评论都没有：装载后应当是空列表
        }

        std::unique_ptr<ConnectionPool> m_pool; ///< 用例独占的连接池
    };
} // namespace

// ========================================================================
// 多对一：一次补取把「作者」挂上去
// ========================================================================

/**
 * @brief 钉住多对一装载：外键命中的挂上，命不中的保持空
 * @details 三条帖子两两不同作者、其中一条指向不存在的作者。判据两半：命中的两条各自挂对了人
 *          （不是「都挂成同一行」——那是最容易写错的形状），命不中的那条保持空
 * @note 证伪：把装载里的键匹配去掉（按顺序对号入座），第一条帖子会挂上乙方，本用例当场红
 */
TEST_F(QueryableRelationsTest, LoadsManyToOneAndLeavesMissingTargetsEmpty)
{
    const std::vector<RelationPost> posts = Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).then(&RelationPost::author).toList();

    ASSERT_EQ(posts.size(), 3U);
    ASSERT_TRUE(posts[0].author.has_value());
    EXPECT_EQ(posts[0].author->name, "甲方") << "第一条帖子挂错了作者";
    ASSERT_TRUE(posts[1].author.has_value());
    EXPECT_EQ(posts[1].author->name, "乙方") << "两条帖子挂成了同一个作者";
    EXPECT_FALSE(posts[2].author.has_value()) << "对端不在了却挂上了一行（或挂了个默认构造的作者）";
}

// ========================================================================
// 一对多：一次补取把「评论」按外键分组挂上
// ========================================================================

/**
 * @brief 钉住一对多装载：子行按外键分组，没有子行的那一行留空列表
 * @details 空列表与「保持原值」是两件事：装载过的语义是「这就是全部子行」，留一个可能是别人塞进去的
 *          旧值会谎报数据。判据同时钉住「每一条只拿到自己的子行」（分组键不能错位）
 */
TEST_F(QueryableRelationsTest, LoadsOneToManyAndLeavesEmptyListForChildlessRows)
{
    const std::vector<RelationPost> posts = Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).then(&RelationPost::comments).toList();

    ASSERT_EQ(posts.size(), 3U);
    const std::map<std::string, std::optional<std::string>> notes = notesByText(posts[0].comments);
    ASSERT_EQ(notes.size(), 2U) << "第一条帖子的评论数不对";
    EXPECT_TRUE(notes.contains("第一评"));
    EXPECT_TRUE(notes.contains("第二评"));
    ASSERT_EQ(posts[1].comments.size(), 1U) << "第二条帖子的评论数不对（分组键错位会把别家的评论挂上来）";
    EXPECT_EQ(posts[1].comments[0].text, "另一篇的评");
    EXPECT_TRUE(posts[2].comments.empty()) << "没有子行的那一条挂上了东西（或没被清成空列表）";
}

// ========================================================================
// 串联、first() 与声明错误
// ========================================================================

/**
 * @brief 钉住两个方向可以同时点名，且互不干扰
 */
TEST_F(QueryableRelationsTest, ChainsBothDirectionsOnOneQuery)
{
    const std::vector<RelationPost> posts =
            Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).then(&RelationPost::author).then(&RelationPost::comments).toList();

    ASSERT_EQ(posts.size(), 3U);
    ASSERT_TRUE(posts[0].author.has_value());
    EXPECT_EQ(posts[0].author->name, "甲方");
    EXPECT_EQ(posts[0].comments.size(), 2U);
    ASSERT_TRUE(posts[1].author.has_value());
    EXPECT_EQ(posts[1].comments.size(), 1U) << "第二条的关系在串联装载里丢了";
}

/**
 * @brief 钉住 first() 也走同一条装载路径
 */
TEST_F(QueryableRelationsTest, LoadsThroughFirstAsWell)
{
    const std::optional<RelationPost> post = Queryable<RelationPost>(*m_pool).where(Column(&RelationPost::id, "id") == std::int64_t{10}).then(&RelationPost::author).first();

    ASSERT_HAS_VALUE(post);
    ASSERT_TRUE(post->author.has_value()) << "first() 没跑关系装载";
    EXPECT_EQ(post->author->name, "甲方");
}

/**
 * @brief 钉住「没在 kRelations 里声明过的成员」在 then() 当场被拒
 * @details 装配错误要在第一次查询时就出声：等到装载时才发现少了一个成员，症状会是「少挂了一半数据」
 *          这类看不见的错。`title` 是合法的列、但不是关系——这正是这一格要区分开的形状
 */
TEST_F(QueryableRelationsTest, RefusesMembersThatAreNotDeclaredAsRelations)
{
    try
    {
        static_cast<void>(Queryable<RelationPost>(*m_pool).then(&RelationPost::title));
        FAIL() << "装载一个没声明过关系的成员却没有被拒";
    } catch (const LogicException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("kRelations"), std::string::npos) << message;
        EXPECT_NE(message.find("rel_posts"), std::string::npos) << "拒绝文案没点出是哪张表：" << message;
    }
}

// ========================================================================
// include：一次 LEFT JOIN 取回（3.3 的第二半）
// ========================================================================

/**
 * @brief 钉住多对一的 include：一条 LEFT JOIN 把对端带回来，配不上的留空
 * @details 判据两半：①查询树里真的有一条 LEFT JOIN 与别名投影——这是「一次取回」的可观测形状
 *          （`toSql()` 的调试文本按已解析的树渲染，走补取那条路的语句里不会有 JOIN）；
 *          ②按值匹配而不是按顺序对号入座
 * @note 证伪：把键匹配改成按顺序对号入座 → 第一条帖子挂上乙方，本用例红；
 *       把 LEFT JOIN 从查询树上摘掉（只留投影）→ 服务端报「不存在列 r0__id」，同样红
 */
TEST_F(QueryableRelationsTest, IncludeLoadsManyToOneWithASingleJoin)
{
    const std::string sql = Queryable<RelationPost>(*m_pool).include(&RelationPost::author).toSql();
    EXPECT_NE(sql.find("LEFT JOIN rel_authors AS r0"), std::string::npos) << sql;
    EXPECT_NE(sql.find("r0.id AS r0__id"), std::string::npos) << sql;
    EXPECT_NE(sql.find("rel_posts.id"), std::string::npos) << "基表列没有限定：JOIN 之后裸列名在同名列上是歧义" << sql;

    const std::vector<RelationPost> posts = Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).include(&RelationPost::author).toList();

    ASSERT_EQ(posts.size(), 3U) << "多对一按对端主键连接，不会把父行乘开，行数应当与基表一致";
    ASSERT_TRUE(posts[0].author.has_value());
    EXPECT_EQ(posts[0].author->name, "甲方") << "第一条帖子挂错了作者";
    ASSERT_TRUE(posts[1].author.has_value());
    EXPECT_EQ(posts[1].author->name, "乙方") << "两条帖子挂成了同一个作者";
    EXPECT_FALSE(posts[2].author.has_value()) << "LEFT JOIN 配不上时应当留空";
}

/**
 * @brief 钉住一对多的 include：JOIN 把父行乘开后要折叠回一行，可空列照旧读成空值
 * @details 三行父行、四条 JOIN 结果行（10 号两条子行、11 号一条、12 号零条）：判据是折叠后的
 *          行数回到 3、每条只拿到自己的子行、没有子行的留空列表。备注列一格有值一格 NULL——
 *          投影列的取值走的是与普通映射同一套转换，NULL 必须落到 optional 的空值上
 * @note 证伪：把折叠整段去掉 → 父行被乘开成 4 行，第一条断言红；把分组键换成空键 → 子行错位，红
 */
TEST_F(QueryableRelationsTest, IncludeFoldsOneToManyRowsBackIntoTheParent)
{
    const std::vector<RelationPost> posts = Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).include(&RelationPost::comments).toList();

    ASSERT_EQ(posts.size(), 3U) << "一对多没有折叠：父行被子行乘开了";
    const std::map<std::string, std::optional<std::string>> notes = notesByText(posts[0].comments);
    ASSERT_EQ(notes.size(), 2U) << "第一条帖子的评论数不对";
    ASSERT_TRUE(notes.contains("第一评"));
    EXPECT_EQ(notes.at("第一评"), "置顶") << "投影列的非空值没有读回来";
    ASSERT_TRUE(notes.contains("第二评"));
    EXPECT_FALSE(notes.at("第二评").has_value()) << "投影列的 NULL 没有落成空值";
    ASSERT_EQ(posts[1].comments.size(), 1U) << "第二条帖子的评论数不对（分组键错位会把别家的挂上来）";
    EXPECT_EQ(posts[1].comments[0].text, "另一篇的评");
    EXPECT_TRUE(posts[2].comments.empty()) << "没有子行的那一条应当留空列表";
}

/**
 * @brief 钉住两个方向可以同时 include，各用各的别名前缀互不干扰
 */
TEST_F(QueryableRelationsTest, IncludesBothDirectionsOnOneQuery)
{
    const std::string sql = Queryable<RelationPost>(*m_pool).include(&RelationPost::author).include(&RelationPost::comments).toSql();
    EXPECT_NE(sql.find("r1.text AS r1__text"), std::string::npos) << "第二条 include 的投影没有用上自己的前缀：" << sql;

    const std::vector<RelationPost> posts =
            Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).include(&RelationPost::author).include(&RelationPost::comments).toList();

    ASSERT_EQ(posts.size(), 3U);
    ASSERT_TRUE(posts[0].author.has_value());
    EXPECT_EQ(posts[0].author->name, "甲方");
    ASSERT_EQ(posts[0].comments.size(), 2U);
    ASSERT_TRUE(posts[1].author.has_value());
    EXPECT_EQ(posts[1].author->name, "乙方");
    ASSERT_EQ(posts[1].comments.size(), 1U);
    EXPECT_FALSE(posts[2].author.has_value());
    EXPECT_TRUE(posts[2].comments.empty());
}

/**
 * @brief 钉住 include 与 then 可以在同一条查询上混用
 * @details 两者落点不同：include 在取数时拆投影（工作在线程内的结果集上），then 在连接释放之后
 *          再发补取查询。混用时折叠先发生、补取发生在折叠后的行上——这条路径两处机制都在场
 */
TEST_F(QueryableRelationsTest, MixesIncludeWithThenOnOneQuery)
{
    const std::vector<RelationPost> posts =
            Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).include(&RelationPost::author).then(&RelationPost::comments).toList();

    ASSERT_EQ(posts.size(), 3U);
    ASSERT_TRUE(posts[0].author.has_value());
    EXPECT_EQ(posts[0].author->name, "甲方");
    ASSERT_EQ(posts[0].comments.size(), 2U);
    ASSERT_TRUE(posts[1].author.has_value());
    ASSERT_EQ(posts[1].comments.size(), 1U) << "补取发生在折叠后的行上";
    EXPECT_FALSE(posts[2].author.has_value());
    EXPECT_TRUE(posts[2].comments.empty());
}

/**
 * @brief 钉住 first() 与一对多的 include 同用时不拿 LIMIT 截断子行
 * @details LIMIT 数的是结果行（父行被乘开之后的），压到 1 会把第一条父行的第二条评论切掉。
 *          本方法因此对带折叠的 include 不压 LIMIT：取回全部结果再折叠，取首元素
 */
TEST_F(QueryableRelationsTest, IncludeThroughFirstKeepsEveryChildRow)
{
    const std::optional<RelationPost> post =
            Queryable<RelationPost>(*m_pool).orderBy(AsynGyanis::Database::Queryable::asc("id")).include(&RelationPost::author).include(&RelationPost::comments).first();

    ASSERT_HAS_VALUE(post);
    ASSERT_TRUE(post->author.has_value()) << "first() 没跑 include 的多对一装载";
    EXPECT_EQ(post->author->name, "甲方");
    // first() 对带折叠的 include 不压 LIMIT：压到 1 会把父行的第二条评论截掉
    const std::map<std::string, std::optional<std::string>> notes = notesByText(post->comments);
    ASSERT_EQ(notes.size(), 2U) << "第一条父行的子行被 LIMIT 截掉了";
    EXPECT_TRUE(notes.contains("第一评"));
    EXPECT_TRUE(notes.contains("第二评"));
}

/**
 * @brief 钉住「一对多的 include 与 LIMIT 不能同时用」在取数阶段被拒
 * @details 静默给出一份缺子行的结果比拒绝糟得多；这条判据同时钉住报错文案点出了两件事
 */
TEST_F(QueryableRelationsTest, RefusesCollectionIncludeWithPagination)
{
    try
    {
        static_cast<void>(Queryable<RelationPost>(*m_pool).include(&RelationPost::comments).limit(2).toList());
        FAIL() << "一对多的 include 与 LIMIT 同时用却没有被拒";
    } catch (const LogicException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("LIMIT"), std::string::npos) << message;
        EXPECT_NE(message.find("include"), std::string::npos) << message;
    }
}

/**
 * @brief 钉住 count() 不接受 include：聚合方向整段 SELECT 被 COUNT(*) 顶掉，JOIN 还会让计数虚高
 */
TEST_F(QueryableRelationsTest, RefusesAggregatesWithIncludes)
{
    try
    {
        static_cast<void>(Queryable<RelationPost>(*m_pool).include(&RelationPost::author).count());
        FAIL() << "count() 与 include 同时用却没有被拒";
    } catch (const LogicException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("count()"), std::string::npos) << "拒绝文案没点出是哪个方法：" << message;
        EXPECT_NE(message.find("include"), std::string::npos) << message;
    }
}

/**
 * @brief 钉住 include() 也当场拒绝没声明过关系的成员（与 then() 同一条纪律）
 */
TEST_F(QueryableRelationsTest, RefusesUndeclaredMembersOnInclude)
{
    try
    {
        static_cast<void>(Queryable<RelationPost>(*m_pool).include(&RelationPost::title));
        FAIL() << "include() 装载一个没声明过关系的成员却没有被拒";
    } catch (const LogicException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("include()"), std::string::npos) << "拒绝文案没点出是哪个方法：" << message;
        EXPECT_NE(message.find("rel_posts"), std::string::npos) << "拒绝文案没点出是哪张表：" << message;
    }
}
