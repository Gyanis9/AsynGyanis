// 覆盖场景（用 hiredis 公开的协议解析接口把完整 RESP 文本还原成真实回复，零网络、零服务端）：
// - 无回复即空集：形状、游标、列元数据、取值与错误状态（真实驱动与桩构建同义）
// - 标量回复：状态/整数/零值/空批量字符串/内嵌 '\0' 的批量字符串各按自己的类型交出
// - 数组回复：一列一元素、越界与空数组、嵌套数组降级为文本、nil 子元素占位、error 回复
// - 游标与合成列名：单行只访问一次、reset 后重放、valueN 按名与按索引取值一致
// 未编译 hiredis 时结果集退化成永远为空的对象，只有「无回复」一组用例仍可跑。

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisResult.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifdef DATABASE_HAS_REDIS
// 与 RedisConnection.cpp 保持一致的包含顺序：先经 Platform 层拿到 struct timeval 与网络头，
// 再包含 hiredis；本文件只用公开的协议解析接口，不构造 timeval
#include "Platform/Platform.h"
#include <hiredis/hiredis.h>
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

            void *rawReply = nullptr;
            const bool fedSuccessfully = redisReaderFeed(reader, respText.data(), respText.size()) == REDIS_OK;
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
        ASSERT_TRUE(payload.has_value());

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
        ASSERT_TRUE(nestedList.has_value());

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
        ASSERT_TRUE(bracketed.has_value());
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
        ASSERT_TRUE(withHole.has_value());

        EXPECT_EQ(result->columnCount(), 2u);
        EXPECT_EQ(*withHole, (std::vector<std::string>{"foo", ""}));
        EXPECT_EQ(textAt(*result, 1), std::optional<std::string>("x"));
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
