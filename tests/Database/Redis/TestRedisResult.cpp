// 覆盖场景（用 hiredis 公开的协议解析接口把完整 RESP 文本还原成真实回复，零网络、零服务端）：
// 无回复即空集：形状、游标、列元数据、取值与错误状态（真实驱动与桩构建同义）
// 标量回复：状态/整数/零值/空批量字符串/内嵌 '\0' 的批量字符串各按自己的类型交出
// 数组回复：一列一元素、越界与空数组、嵌套数组降级为文本、nil 子元素占位、error 回复
// RESP3 容器（SET 等）：成员按 elements 逐列交出、空容器与空数组同为空集
// RESP3 类型化回复：map 交出一份哈希并占一列（空 map 是「有一份空哈希」）、bool 与 double 按类型交出、
//   数组里的 bool 元素同样按类型、map 里带类型的值按文本折进哈希、verbatim 交出剥掉子类型后的正文
// 游标与合成列名：单行只访问一次、reset 后重放、valueN 按名与按索引取值一致
// 未编译 hiredis 时结果集退化成永远为空的对象，只有「无回复」一组用例仍可跑。

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisResult.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef DATABASE_HAS_REDIS
// 与 RedisConnection.cpp 保持一致的包含顺序：先经 Platform 层拿到 struct timeval 与网络头，
// 再包含 hiredis；本文件只用公开的协议解析接口，不构造 timeval
#include <hiredis/hiredis.h>
#include "Platform/Platform.h"
#endif

namespace AsynGyanis::Database
{
    namespace
    {
        /**
         * @brief 构造一份「没有回复」的结果集，用于覆盖空集契约
         * @return std::unique_ptr<RedisResult> 持有 nullptr 回复的结果集
         * @note 构造函数显式接受 nullptr，语义是「什么都没有」而不是错误
         */
        std::unique_ptr<RedisResult> makeEmptyResult()
        {
            return std::make_unique<RedisResult>(nullptr);
        }
    } // namespace

    // ------------------------------------------------------------------------
    // 无回复即空集：不依赖 hiredis，真实驱动与桩构建同义
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住「没有回复」在构造后就是 0 行 0 列的空集，而不是等首次访问才失败
     */
    TEST(RedisResult, WithoutReplyItIsAlreadyEmpty)
    {
        const std::unique_ptr<RedisResult> result = makeEmptyResult();

        EXPECT_TRUE(result->isEmpty());
        EXPECT_EQ(result->rowCount(), 0u);
        EXPECT_EQ(result->columnCount(), 0u);
    }

    /**
     * @brief 钉住空集上 next() 恒为假，reset() 也不改变这一结论
     */
    TEST(RedisResult, WithoutReplyCursorNeverAdvances)
    {
        const std::unique_ptr<RedisResult> result = makeEmptyResult();

        // 空集没有那一行可交：反复调用 next() 与 reset() 之后依然是 false
        EXPECT_FALSE(result->next());
        EXPECT_FALSE(result->next());
        result->reset();
        EXPECT_FALSE(result->next());
    }

    /**
     * @brief 钉住空集没有任何列元数据，列名与列索引查询一律未命中
     */
    TEST(RedisResult, WithoutReplyHasNoColumnMetadata)
    {
        const std::unique_ptr<RedisResult> result = makeEmptyResult();

        EXPECT_FALSE(result->columnName(0).has_value());
        EXPECT_TRUE(result->columnNames().empty());
        EXPECT_FALSE(result->columnIndex("value0").has_value());
    }

    /**
     * @brief 钉住空集上按索引或按名取值一律得到 monostate，与越界同义
     */
    TEST(RedisResult, WithoutReplyValuesReadAsNull)
    {
        const std::unique_ptr<RedisResult> result = makeEmptyResult();

        // 取值失败一律 monostate：无回复、索引越界、列名不存在三类输入共用这一语义
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(std::size_t{0})));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(std::size_t{99})));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue("value0")));
    }

    /**
     * @brief 钉住空集不携带错误、无原生句柄，replyType() 为 0
     */
    TEST(RedisResult, WithoutReplyReportsNoErrorAndNoHandle)
    {
        const std::unique_ptr<RedisResult> result = makeEmptyResult();

        EXPECT_FALSE(result->isError());
        EXPECT_EQ(result->replyType(), 0);
        EXPECT_EQ(result->nativeHandle(), nullptr);
        EXPECT_TRUE(result->lastError().empty());
    }

    /**
     * @brief 钉住 const 读取路径（next/getValue/列名查询）不改写错误状态
     */
    TEST(RedisResult, ValueReadsOnEmptyResultDoNotCreateErrorState)
    {
        std::unique_ptr<RedisResult> result = makeEmptyResult();

        // 基类契约：next/getValue 等 const 读取路径不得改写错误状态
        static_cast<void>(result->next());
        static_cast<void>(result->getValue(std::size_t{3}));
        EXPECT_TRUE(result->lastError().empty());
        EXPECT_FALSE(result->isError());
    }

#ifdef DATABASE_HAS_REDIS
    namespace
    {
        /**
         * @brief 用 hiredis 公开的协议解析接口把一段完整 RESP 文本还原成 redisReply
         * @details 这是 RedisResult 唯一可离线构造的路径：不 new 结构体、不填内部字段，
         *          交给 freeReplyObject 的始终是由 hiredis 自己分配的对象。
         * @param respText 完整的 RESP 回复文本，必须以 CRLF 结尾
         * @return redisReply* 解析出的回复，所有权移交调用方；解析失败或输入不含完整回复时返回 nullptr
         */
        redisReply *parseRespReply(const std::string &respText)
        {
            redisReader *reader = redisReaderCreate();
            if (reader == nullptr)
            {
                return nullptr;
            }

            void      *rawReply           = nullptr;
            const bool fedSuccessfully    = redisReaderFeed(reader, respText.data(), respText.size()) == REDIS_OK;
            const bool parsedSuccessfully = fedSuccessfully && redisReaderGetReply(reader, &rawReply) == REDIS_OK;
            redisReaderFree(reader);

            auto *reply = static_cast<redisReply *>(rawReply);
            if (!parsedSuccessfully)
            {
                // 协议出错时 hiredis 可能已交出半截回复，所有权仍在调用方手里，必须就地释放
                if (reply != nullptr)
                {
                    freeReplyObject(reply);
                }
                return nullptr;
            }

            return reply;
        }

        /**
         * @brief 由一段完整 RESP 文本构造结果集
         * @param respText RESP 回复文本
         * @return std::unique_ptr<RedisResult> 结果集；解析失败返回 nullptr（用例据此判前置条件）
         */
        std::unique_ptr<RedisResult> makeResultFromResp(const std::string &respText)
        {
            redisReply *reply = parseRespReply(respText);
            if (reply == nullptr)
            {
                return nullptr;
            }

            // 所有权在此移交：此后由 RedisResult 析构调用 freeReplyObject
            return std::make_unique<RedisResult>(reply);
        }

        /**
         * @brief 按列索引取文本值
         * @param result 结果集
         * @param index 列索引
         * @return std::optional<std::string> 该列的文本；该列不是字符串备选时返回空值
         */
        std::optional<std::string> textAt(const RedisResult &result, const std::size_t index)
        {
            const DatabaseValue value = result.getValue(index);
            if (const std::string *text = std::get_if<std::string>(&value); text != nullptr)
            {
                return *text;
            }
            return std::nullopt;
        }

        /**
         * @brief 按列索引取列表值
         * @param result 结果集
         * @param index 列索引
         * @return std::optional<std::vector<std::string>> 该列的列表；该列不是列表备选时返回空值
         */
        std::optional<std::vector<std::string>> listAt(const RedisResult &result, const std::size_t index)
        {
            const DatabaseValue value = result.getValue(index);
            if (const std::vector<std::string> *list = std::get_if<std::vector<std::string>>(&value); list != nullptr)
            {
                return *list;
            }
            return std::nullopt;
        }

        /**
         * @brief 按列索引取哈希值（RESP3 的 map 走这一格）
         * @param result 结果集
         * @param index 列索引
         * @return std::optional<std::unordered_map<std::string, std::string>> 该列的哈希；该列不是哈希备选时返回空值
         */
        [[nodiscard]] std::optional<std::unordered_map<std::string, std::string>> hashAt(const RedisResult &result, const std::size_t index)
        {
            const DatabaseValue value = result.getValue(index);
            if (const auto *hash = std::get_if<std::unordered_map<std::string, std::string>>(&value); hash != nullptr)
            {
                return *hash;
            }
            return std::nullopt;
        }

        /**
         * @brief 按列索引取布尔值（RESP3 的 #t / #f 走这一格）
         * @param result 结果集
         * @param index 列索引
         * @return std::optional<bool> 该列的布尔；该列不是布尔备选时返回空值
         */
        [[nodiscard]] std::optional<bool> boolAt(const RedisResult &result, const std::size_t index)
        {
            const DatabaseValue value = result.getValue(index);
            if (const bool *flag = std::get_if<bool>(&value); flag != nullptr)
            {
                return *flag;
            }
            return std::nullopt;
        }

        /**
         * @brief 按列索引取浮点值（RESP3 的 ,<double> 走这一格）
         * @param result 结果集
         * @param index 列索引
         * @return std::optional<double> 该列的浮点；该列不是浮点备选时返回空值
         */
        [[nodiscard]] std::optional<double> doubleAt(const RedisResult &result, const std::size_t index)
        {
            const DatabaseValue value = result.getValue(index);
            if (const double *number = std::get_if<double>(&value); number != nullptr)
            {
                return *number;
            }
            return std::nullopt;
        }
    } // namespace

    // ------------------------------------------------------------------------
    // 标量回复：一行一列
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住 +status 回复映射成一行一列的字符串，replyType 保持 STATUS
     */
    TEST(RedisResult, StatusReplyReadsAsSingleStringColumn)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("+PONG\r\n");
        ASSERT_NE(result, nullptr) << "RESP 文本解析失败，用例前置条件不成立";

        EXPECT_EQ(result->replyType(), REDIS_REPLY_STATUS);
        EXPECT_EQ(result->rowCount(), 1u);
        EXPECT_EQ(result->columnCount(), 1u);
        EXPECT_FALSE(result->isEmpty());
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("PONG"));
        EXPECT_STREQ(databaseValueTypeName(result->getValue(std::size_t{0})), "String");
    }

    /**
     * @brief 钉住整数回复（含负数）按 int64 交出，与 DatabaseValue 的整型备选对齐
     */
    TEST(RedisResult, IntegerReplyReadsAsInt64Column)
    {
        const std::unique_ptr<RedisResult> positive = makeResultFromResp(":42\r\n");
        const std::unique_ptr<RedisResult> negative = makeResultFromResp(":-1000\r\n");
        ASSERT_NE(positive, nullptr);
        ASSERT_NE(negative, nullptr);

        // hiredis 把 Redis 的 64 位整数放在 integer 里，与 DatabaseValue 的整型备选完全对齐
        EXPECT_EQ(positive->replyType(), REDIS_REPLY_INTEGER);
        EXPECT_TRUE(std::holds_alternative<std::int64_t>(positive->getValue(std::size_t{0})));
        EXPECT_EQ(std::get<std::int64_t>(positive->getValue(std::size_t{0})), 42);
        EXPECT_EQ(std::get<std::int64_t>(negative->getValue(std::size_t{0})), -1000);
    }

    /**
     * @brief 钉住「服务端确实给了 0」与「没有值」的区分，合法零值不被吞成空集
     */
    TEST(RedisResult, ScalarZeroIsNotAnEmptyResult)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp(":0\r\n");
        ASSERT_NE(result, nullptr);

        // 「服务端确实给了 0」与「没有值」必须区分开，否则 INCR/EXISTS 之类命令的合法结果会被吞掉
        EXPECT_FALSE(result->isEmpty());
        EXPECT_EQ(result->rowCount(), 1u);
        EXPECT_EQ(std::get<std::int64_t>(result->getValue(std::size_t{0})), 0);
    }

    /**
     * @brief 钉住零长度批量字符串是「有值且为空」，不是空集也不是 monostate
     */
    TEST(RedisResult, BulkStringReplyKeepsEmptyPayloadAsString)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("$0\r\n\r\n");
        ASSERT_NE(result, nullptr);

        // 零长度批量字符串是「值为空串」，不是空结果集，也不是 monostate
        EXPECT_EQ(result->replyType(), REDIS_REPLY_STRING);
        EXPECT_FALSE(result->isEmpty());
        EXPECT_EQ(result->columnCount(), 1u);
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>(""));
    }

    /**
     * @brief 钉住按「指针 + 长度」取值：内嵌 '\0' 之后的字节不被截断
     */
    TEST(RedisResult, BulkStringReplyPreservesEmbeddedNullByte)
    {
        // 载荷 5 字节，其中含一个内嵌 '\0'：hiredis 按 str + len 表达文本，取值也必须按长度拷贝
        std::string binaryReply;
        binaryReply += "$5\r\n";
        binaryReply += std::string("a\0bcd", 5);
        binaryReply += "\r\n";

        const std::unique_ptr<RedisResult> result = makeResultFromResp(binaryReply);
        ASSERT_NE(result, nullptr) << "含内嵌 '\\0' 的 RESP 文本解析失败，用例前置条件不成立";

        const std::optional<std::string> payload = textAt(*result, 0);
        ASSERT_HAS_VALUE(payload);

        // 长度保住即证明按 str + len 取值：内嵌 '\0' 之后的字节还在，没有在第一个 '\0' 处被截断
        EXPECT_EQ(payload->size(), 5u);
        EXPECT_EQ((*payload)[1], '\0');
        EXPECT_EQ(payload->find("bcd"), static_cast<std::string::size_type>(2));
    }

    // ------------------------------------------------------------------------
    // 数组回复：一行 N 列
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住数组回复按单行 N 列交出，第 i 列即第 i 个元素
     */
    TEST(RedisResult, ArrayReplyMapsEachElementToAColumn)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
        ASSERT_NE(result, nullptr);

        // 退化单行契约：数组回复是 1 行 N 列，第 i 列即第 i 个元素
        EXPECT_EQ(result->replyType(), REDIS_REPLY_ARRAY);
        EXPECT_EQ(result->rowCount(), 1u);
        EXPECT_EQ(result->columnCount(), 2u);
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("foo"));
        EXPECT_EQ(textAt(*result, 1), std::optional<std::string>("bar"));
    }

    /**
     * @brief 钉住越界列读为 monostate 且不因此进入错误状态
     */
    TEST(RedisResult, OutOfRangeColumnReadsAsNull)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*1\r\n$1\r\nx\r\n");
        ASSERT_NE(result, nullptr);

        // 越界取值只回 monostate，绝不改写错误状态（const 读取路径）
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(std::size_t{1})));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue(std::size_t{9999})));
        EXPECT_TRUE(result->lastError().empty());
    }

    /**
     * @brief 钉住空数组回复是 0 行 0 列的空集，isEmpty 与 rowCount()==0 等价
     */
    TEST(RedisResult, EmptyArrayReplyIsAnEmptyResult)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*0\r\n");
        ASSERT_NE(result, nullptr);

        // KEYS/HRANDFIELD 之类的「没有命中」就是空数组：0 行 0 列，isEmpty 与 rowCount()==0 等价
        EXPECT_EQ(result->replyType(), REDIS_REPLY_ARRAY);
        EXPECT_EQ(result->columnCount(), 0u);
        EXPECT_EQ(result->rowCount(), 0u);
        EXPECT_TRUE(result->isEmpty());
        EXPECT_FALSE(result->next());
    }

    /**
     * @brief 钉住嵌套数组的元素按十进制文本摊平，结构保不住时不静默丢值
     */
    TEST(RedisResult, ArrayReplyFlattensNestedIntegersToText)
    {
        // 顶层数组 [ [7, 8], "x" ]：列表备选类型只有字符串，整数子元素按十进制文本保留
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*2\r\n*2\r\n:7\r\n:8\r\n$1\r\nx\r\n");
        ASSERT_NE(result, nullptr);

        const std::optional<std::vector<std::string>> nestedList = listAt(*result, 0);
        ASSERT_HAS_VALUE(nestedList);

        EXPECT_EQ(result->columnCount(), 2u);
        EXPECT_EQ(*nestedList, (std::vector<std::string>{"7", "8"}));
        EXPECT_STREQ(databaseValueTypeName(result->getValue(std::size_t{0})), "List");
        EXPECT_EQ(textAt(*result, 1), std::optional<std::string>("x"));
    }

    /**
     * @brief 钉住更深一层的数组降级为 "[…]" 文本，而不是静默丢数据
     */
    TEST(RedisResult, DoublyNestedArrayBecomesBracketedText)
    {
        // 顶层数组 [ [ [7, 8] ] ]：更深一层的数组牺牲结构，串成 "[7, 8]" 形式，换取不静默丢数据
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*1\r\n*1\r\n*2\r\n:7\r\n:8\r\n");
        ASSERT_NE(result, nullptr);

        const std::optional<std::vector<std::string>> bracketed = listAt(*result, 0);
        ASSERT_HAS_VALUE(bracketed);
        EXPECT_EQ(*bracketed, (std::vector<std::string>{"[7, 8]"}));
    }

    /**
     * @brief 钉住数组内的 nil 子元素用空串占位，不破坏后续元素的下标对齐
     */
    TEST(RedisResult, NilElementInsideArrayKeepsIndexAlignment)
    {
        // 顶层数组 [ ["foo", nil], "x" ]：nil 子元素用空串占位，保证 MGET 未命中那一项下标不串位
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*2\r\n*2\r\n$3\r\nfoo\r\n$-1\r\n$1\r\nx\r\n");
        ASSERT_NE(result, nullptr);

        const std::optional<std::vector<std::string>> withHole = listAt(*result, 0);
        ASSERT_HAS_VALUE(withHole);

        EXPECT_EQ(result->columnCount(), 2u);
        EXPECT_EQ(*withHole, (std::vector<std::string>{"foo", ""}));
        EXPECT_EQ(textAt(*result, 1), std::optional<std::string>("x"));
    }

    /**
     * @brief 钉住 RESP3 容器回复（这里是 SET）把成员逐列交出，而不是塌成一列空值
     * @details 驱动从不主动发 HELLO 3，但调用方可以自己切协议（连接归还时才被断开），
     *          服务端随后就会用 `~` / `%` 回容器。列数原先只认 REDIS_REPLY_ARRAY 这一个类型，
     *          于是一份两个成员的集合读起来是「有一列、没有值」——数据在回复里，却无声消失。
     *          判据改用元素个数：所有容器型都靠 elements 承载，也就不必依赖各版本命名不一的枚举常量。
     */
    TEST(RedisResult, ContainerReplyExposesItsMembersAsColumns)
    {
        const std::unique_ptr<RedisResult> setResult = makeResultFromResp("~2\r\n$1\r\na\r\n$1\r\nb\r\n");
        ASSERT_NE(setResult, nullptr) << "RESP 文本解析失败，用例前置条件不成立";
        ASSERT_NE(setResult->replyType(), REDIS_REPLY_ARRAY) << "这份回复根本不是数组，用例没构造出要钉的形状";

        EXPECT_EQ(setResult->columnCount(), 2u) << "容器成员没有各占一列";
        EXPECT_EQ(textAt(*setResult, 0), std::optional<std::string>("a"));
        EXPECT_EQ(textAt(*setResult, 1), std::optional<std::string>("b"));
        EXPECT_FALSE(setResult->isEmpty());
    }

    /**
     * @brief 钉住空容器是「有这份列表但一行都没有」，与「没有值」的 nil 同样按空集处理
     */
    TEST(RedisResult, EmptyContainerReplyIsAnEmptyResult)
    {
        const std::unique_ptr<RedisResult> emptySet = makeResultFromResp("~0\r\n");
        ASSERT_NE(emptySet, nullptr) << "RESP 文本解析失败，用例前置条件不成立";

        EXPECT_EQ(emptySet->columnCount(), 0u);
        EXPECT_TRUE(emptySet->isEmpty());
        EXPECT_TRUE(std::holds_alternative<std::monostate>(emptySet->getValue(std::size_t{0})));

        // 对照组：RESP2 的空数组一直是同样的语义，两者不能一个为空集、另一个为一列空值
        const std::unique_ptr<RedisResult> emptyArray = makeResultFromResp("*0\r\n");
        ASSERT_NE(emptyArray, nullptr);
        EXPECT_TRUE(emptyArray->isEmpty());
        EXPECT_EQ(emptyArray->columnCount(), 0u);
    }

    // ------------------------------------------------------------------------
    // RESP3 的类型化回复：map / bool / double / verbatim
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住 RESP3 的 map 交出一份哈希、占一列，而不是 2N 列让调用方自己配对
     * @details 这正是「容器被有损折成列表」那一格：服务端在 RESP3 下把 `HGETALL`、
     *          `CONFIG GET` 回成 map，配对关系本来就在协议里，按交替下标交回去等于把它拆散了再
     *          让每个人自己重拼一遍（拼错一位就静默拿到别人的值）。
     *          判据两半：列数是 1；哈希里逐对可见。
     * @note 证伪：把 map 的类型分支删掉（退回按 elements 逐列），`columnCount()==1` 与哈希那两条当场红。
     */
    TEST(RedisResult, MapReplyBecomesASingleHashColumn)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("%2\r\n$5\r\nalpha\r\n$1\r\n1\r\n$4\r\nbeta\r\n$3\r\n2.5\r\n");
        ASSERT_NE(result, nullptr) << "RESP 文本解析失败，用例前置条件不成立";

        EXPECT_EQ(result->columnCount(), 1u) << "map 被拆成了交替的两列，配对关系又得调用方自己猜";
        const std::optional<std::unordered_map<std::string, std::string>> entries = hashAt(*result, 0);
        ASSERT_HAS_VALUE(entries) << "map 没有按哈希备选交出";
        EXPECT_EQ(entries->size(), 2u);
        EXPECT_EQ(entries->at("alpha"), "1");
        EXPECT_EQ(entries->at("beta"), "2.5");
        EXPECT_FALSE(result->isEmpty());
    }

    /**
     * @brief 钉住空 map 是「有一份空哈希」，与空数组的「一行都没有」分得开
     * @details `%0` 与 `*0` 在服务端说的是两件事实：前者是「这个集合里确实没有配对项」，
     *          后者是「没有成员」。两者都折成空集就会把「查了、答案是空」读成「什么都没查」，
     *          调用方分不出键不存在与键存在但为空。
     */
    TEST(RedisResult, EmptyMapReadsAsAnEmptyHashRatherThanNoReply)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("%0\r\n");
        ASSERT_NE(result, nullptr) << "RESP 文本解析失败，用例前置条件不成立";

        EXPECT_EQ(result->columnCount(), 1u);
        EXPECT_FALSE(result->isEmpty()) << "空哈希被读成了「没有回复」";
        const std::optional<std::unordered_map<std::string, std::string>> entries = hashAt(*result, 0);
        ASSERT_HAS_VALUE(entries);
        EXPECT_TRUE(entries->empty());
    }

    /**
     * @brief 钉住 map 里带类型的值按文本折进哈希，而配对关系仍在
     * @details 哈希备选只接受字符串：值一侧的整数（`:7`）与双精度（`,1`）按十进制文本保留。
     *          这一格是「有损」的边界，如实钉住现状而不是让它看起来无损——层级与类型不保留，
     *          但键与值的归属一定是对的（`CONFIG GET` 那一族实际就用在这个形状上）。
     */
    TEST(RedisResult, MapValuesThatCarryTheirOwnTypeAreKeptAsText)
    {
        // 一份 map、两对：值一侧分别是整数与一个集合（`:7` 与 `~2 …`）
        const std::unique_ptr<RedisResult> result = makeResultFromResp("%2\r\n$5\r\ncount\r\n:7\r\n$7\r\nmembers\r\n~2\r\n$1\r\na\r\n$1\r\nb\r\n");
        ASSERT_NE(result, nullptr) << "RESP 文本解析失败，用例前置条件不成立";

        const std::optional<std::unordered_map<std::string, std::string>> entries = hashAt(*result, 0);
        ASSERT_HAS_VALUE(entries);
        EXPECT_EQ(entries->size(), 2u) << "配对关系错位：多出一项或少了一项";
        EXPECT_EQ(entries->at("count"), "7") << "整数值没按十进制文本留住";
        EXPECT_EQ(entries->at("members"), "[a, b]") << "嵌套容器作为值时层级压平，但成员不能丢";
        EXPECT_EQ(entries->find("a"), entries->end()) << "集合的成员被当成了哈希自己的键";
    }

    /**
     * @brief 钉住 RESP3 布尔按 bool 交出：它不带任何文本，原先会连一列都不算
     * @details hiredis 把 `#t` / `#f` 放在 integer 里而 str 是空指针，因此旧的列数判据
     *          （「带 str 才算一列」）会把一条真答复折成空结果——`SMISMEMBER` 在 RESP3 下就是
     *          一串布尔，整条命令读起来像没查到。判据取两侧：true 与 false 都要是 bool，
     *          而不是「一个为真、一个为没值」。
     * @note 证伪：删掉列数判据里的布尔那一格，两条用例都红在「0 列」；删掉取值分支里的布尔那一格，
     *       红在备选类型上。
     */
    TEST(RedisResult, BoolReplyReadsAsBooleanNotAsEmptyResult)
    {
        const std::unique_ptr<RedisResult> yes = makeResultFromResp("#t\r\n");
        const std::unique_ptr<RedisResult> no  = makeResultFromResp("#f\r\n");
        ASSERT_NE(yes, nullptr);
        ASSERT_NE(no, nullptr);

        EXPECT_EQ(yes->columnCount(), 1u) << "布尔回复不带文本，被折成了没有一列";
        EXPECT_EQ(boolAt(*yes, 0), std::optional<bool>(true));
        EXPECT_FALSE(yes->isEmpty());
        EXPECT_EQ(boolAt(*no, 0), std::optional<bool>(false)) << "假被读成了「没有值」";
        EXPECT_FALSE(no->isEmpty());
    }

    /**
     * @brief 钉住数组里的布尔元素同样按 bool 交出（`SMISMEMBER` 的真实形状）
     */
    TEST(RedisResult, BooleanElementsInsideArrayKeepTheirType)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*3\r\n#t\r\n#f\r\n#t\r\n");
        ASSERT_NE(result, nullptr);

        EXPECT_EQ(result->columnCount(), 3u);
        EXPECT_EQ(boolAt(*result, 0), std::optional<bool>(true));
        EXPECT_EQ(boolAt(*result, 1), std::optional<bool>(false));
        EXPECT_EQ(boolAt(*result, 2), std::optional<bool>(true));
    }

    /**
     * @brief 钉住 RESP3 双精度按 double 交出，而不是它的十进制文本
     * @details hiredis 同时把值放在 dval 与原始文本里；交回文本会让调用方以为拿到的是字符串
     *          （`std::get_if<double>` 恒空），数值口径也就此丢失
     */
    TEST(RedisResult, DoubleReplyReadsAsDoubleNotAsText)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp(",3.5\r\n");
        ASSERT_NE(result, nullptr);

        EXPECT_EQ(result->columnCount(), 1u);
        EXPECT_EQ(doubleAt(*result, 0), std::optional<double>(3.5));
        EXPECT_EQ(textAt(*result, 0), std::nullopt) << "双精度仍按文本交出，调用方按数值读会一律拿空";
    }

    /**
     * @brief 钉住 verbatim 回复交出去掉三字节子类型之后的正文
     * @details `INFO` 一类在 RESP3 下回 `=<len>\r\ntxt:<正文>`。hiredis 自己把子类型放在 vtype、
     *          str 只留正文（实测 len 不含那四个字节），因此这里要拿到的就是 `# Server…` 那段，
     *          不该带 "txt:" 前缀——带上会让调用方按行解析时第一行就错。
     */
    TEST(RedisResult, VerbatimReplyKeepsItsPayload)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("=15\r\ntxt:Some string\r\n");
        ASSERT_NE(result, nullptr);

        EXPECT_EQ(result->columnCount(), 1u);
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("Some string"));
    }

    // ------------------------------------------------------------------------
    // error 回复
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住 error 回复被标记 isError，服务端原文逐字进入 lastError()
     */
    TEST(RedisResult, ErrorReplyIsFlaggedAndKeepsServerText)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("-WRONGTYPE Operation against a key\r\n");
        ASSERT_NE(result, nullptr);

        // 构造这一非 const 写路径把服务端原文摘进 lastError()，管道路径靠 isError() 逐条定位失败命令
        EXPECT_TRUE(result->isError());
        EXPECT_EQ(result->replyType(), REDIS_REPLY_ERROR);
        EXPECT_EQ(result->lastError(), "WRONGTYPE Operation against a key");
        EXPECT_EQ(result->rowCount(), 1u);
        EXPECT_EQ(result->columnCount(), 1u);
        EXPECT_FALSE(result->isEmpty());
    }

    /**
     * @brief 钉住 error 回复的取值仍可按文本读出，与状态/字符串回复同一载体
     */
    TEST(RedisResult, ErrorReplyValueIsReadableAsText)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("-ERR bad command\r\n");
        ASSERT_NE(result, nullptr);

        // error 与 string/status 一样由 str/len 承载，因此按文本交出
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("ERR bad command"));
    }

    /**
     * @brief 钉住多次 const 读取（越界、按名、next）不会改写已记录的错误原文
     */
    TEST(RedisResult, ReadingValuesDoesNotRewriteErrorState)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("-ERR bad command\r\n");
        ASSERT_NE(result, nullptr);

        const std::string originalError = result->lastError();

        // const 读取路径反复越界取值也不得改写 m_lastError，否则「N 条里哪一条失败」就无从判定
        static_cast<void>(result->getValue(std::size_t{99}));
        static_cast<void>(result->getValue("value7"));
        static_cast<void>(result->next());
        static_cast<void>(result->columnIndex("nope"));

        EXPECT_EQ(result->lastError(), originalError);
        EXPECT_TRUE(result->isError());
    }

    // ------------------------------------------------------------------------
    // 游标与合成列名
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住 while(next()) 惯用法对单行结果恰好走一轮，reset 后可重放
     */
    TEST(RedisResult, SingleRowIsVisitedOnceAndReturnsAfterReset)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("+OK\r\n");
        ASSERT_NE(result, nullptr);

        // while (result->next()) 这个基类惯用法在此恰好只走一轮
        EXPECT_TRUE(result->next());
        EXPECT_FALSE(result->next());
        EXPECT_FALSE(result->next());

        result->reset();
        EXPECT_TRUE(result->next());
        EXPECT_FALSE(result->next());
    }

    /**
     * @brief 钉住数据在内存中即可读：next() 不 gate 取值，这是与游标式结果集的关键差异
     */
    TEST(RedisResult, ValuesReadableWithoutAdvancingCursor)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*2\r\n$1\r\na\r\n$1\r\nb\r\n");
        ASSERT_NE(result, nullptr);

        // 与 SQLite 驱动的关键差异：数据已在内存里，next() 不 gate 任何可读性
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("a"));
        ASSERT_TRUE(result->next());
        EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("a"));
        EXPECT_FALSE(result->next());
        EXPECT_EQ(textAt(*result, 1), std::optional<std::string>("b"));
    }

    /**
     * @brief 钉住列名按 valueN 合成且与 columnCount 一致，越界列名为空
     */
    TEST(RedisResult, ColumnNamesAreSynthesizedFromIndex)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
        ASSERT_NE(result, nullptr);

        // Redis 没有列名概念，只能给出按下标合成的无语义名字，用于撑起依赖列名的通用遍历代码
        EXPECT_EQ(result->columnName(0), std::optional<std::string>("value0"));
        EXPECT_EQ(result->columnName(1), std::optional<std::string>("value1"));
        EXPECT_EQ(result->columnName(2), std::optional<std::string>("value2"));
        EXPECT_FALSE(result->columnName(3).has_value());

        const std::vector<std::string> names = result->columnNames();
        EXPECT_EQ(names.size(), result->columnCount());
        EXPECT_EQ(names, (std::vector<std::string>{"value0", "value1", "value2"}));
    }

    /**
     * @brief 钉住列名反查按精确匹配（区分大小写），非合成名与越界名一律未命中
     */
    TEST(RedisResult, ColumnIndexInvertsSynthesizedNames)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*2\r\n$1\r\na\r\n$1\r\nb\r\n");
        ASSERT_NE(result, nullptr);

        EXPECT_EQ(result->columnIndex("value0"), std::optional<std::size_t>(0));
        EXPECT_EQ(result->columnIndex("value1"), std::optional<std::size_t>(1));

        // 合成名字按精确匹配比较：区分大小写，也不接受来路不明的名字
        EXPECT_FALSE(result->columnIndex("Value0").has_value());
        EXPECT_FALSE(result->columnIndex("value").has_value());
        EXPECT_FALSE(result->columnIndex("a").has_value());
        EXPECT_FALSE(result->columnIndex("value2").has_value());
    }

    /**
     * @brief 钉住按名与按索引两条取值路径的判定完全一致
     */
    TEST(RedisResult, ValueByNameMatchesValueByIndex)
    {
        const std::unique_ptr<RedisResult> result = makeResultFromResp("*2\r\n:1\r\n$3\r\ntwo\r\n");
        ASSERT_NE(result, nullptr);

        // 按名与按索引两条路径的越界与 nil 判定必须完全一致
        EXPECT_EQ(result->getValue("value0"), result->getValue(std::size_t{0}));
        EXPECT_EQ(result->getValue("value1"), result->getValue(std::size_t{1}));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(result->getValue("value9")));
        EXPECT_EQ(textAt(*result, 1), std::optional<std::string>("two"));
        EXPECT_EQ(std::get<std::int64_t>(result->getValue(std::size_t{0})), 1);
    }

    /**
     * @brief 钉住 nativeHandle() 只借出指针：所有权仍属结果集，由结果集析构释放
     */
    TEST(RedisResult, NativeHandleIsBorrowedNotOwned)
    {
        {
            const std::unique_ptr<RedisResult> result = makeResultFromResp("$3\r\nfoo\r\n");
            ASSERT_NE(result, nullptr);

            // nativeHandle() 只借出指针：所有权仍属结果集，调用方不得 freeReplyObject。
            // 拿到它才能按 REDIS_REPLY_* 自行解读嵌套结构，这正是头文件里给出的高级用法
            ASSERT_NE(result->nativeHandle(), nullptr);
            EXPECT_EQ(result->nativeHandle()->type, REDIS_REPLY_STRING);
            EXPECT_EQ(result->nativeHandle()->len, static_cast<size_t>(3));
            EXPECT_EQ(textAt(*result, 0), std::optional<std::string>("foo"));
        }

        // 作用域结束即由 RedisResult 析构调用 freeReplyObject：本用例不留下任何可继续读的裸指针
        SUCCEED();
    }

#endif // DATABASE_HAS_REDIS

} // namespace AsynGyanis::Database
