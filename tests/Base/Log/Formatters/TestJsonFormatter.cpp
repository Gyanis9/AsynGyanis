/**
 * @file TestJsonFormatter.cpp
 * @brief JsonFormatter 单元测试：字段完整性与省略规则、单行紧凑输出、转义与 UTF-8 直通
 * @details 每条输出都用 JsonParser 解析回来断言（而不是匹配字符串片段）：格式化器的契约是
 *          「产出合法 JSON」，把结果喂回解析器是唯一能同时验证合法性与取值的方式。
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Formatters/JsonFormatter.h"

#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/Value/FormatValue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        constexpr auto kFixedTimestamp  = "2026-09-13 20:00:00.123";
        constexpr auto kThreadId        = "tid-123456";
        constexpr auto kLoggerName      = "json_formatter_logger";
        constexpr auto kSourceFile      = "json_formatter_fixture.cpp";
        constexpr auto kSourceFunction  = "fixtureFunction";
        constexpr int  kSourceLine      = 4271;

        /// 源码位置固定写死：位置取自 SourceLocation::current() 会让断言随用例行号漂移
        constexpr SourceLocation kFixtureLocation{kSourceFile, kSourceLine, kSourceFunction};

        /**
         * @brief 造一条字段齐备的日志事件
         * @param message 日志消息
         * @param loggerName 日志器名字，空串表示根记录器
         * @return LogEvent 事件
         */
        LogEvent makeEvent(std::string message, const std::string_view loggerName = kLoggerName)
        {
            return LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId, kFixtureLocation, std::string(loggerName), std::move(message));
        }

        /**
         * @brief 取 JSON 对象里某个键的字符串值
         * @param object 已解析的 JSON 对象
         * @param key 键名
         * @return std::string 取值；键不存在时为空串（断言由调用方负责）
         */
        std::string textField(const FormatValue &object, const std::string_view key)
        {
            const FormatValue *field = object.find(key);
            return field == nullptr ? std::string() : field->asString();
        }
    } // namespace

    /**
     * @brief 核心字段齐全，且输出能被 JSON 解析器解析回等价取值
     */
    TEST(JsonFormatterTest, RendersCoreFieldsAsParsableJsonObject)
    {
        JsonFormatter formatter;
        const std::string line = formatter.format(makeEvent("请求处理完成"));

        const FormatValue parsed = JsonParser::parse(line);
        ASSERT_TRUE(parsed.isObject());
        EXPECT_EQ(textField(parsed, "timestamp"), kFixedTimestamp);
        // 等级名不带文本版式的对齐空格：取值要能直接比对，不能是「INFO 加空格」
        const std::string levelText = textField(parsed, "level");
        EXPECT_EQ(levelText, "INFO");
        EXPECT_EQ(levelText.find(' '), std::string::npos);
        EXPECT_EQ(levelText, std::string_view(logLevelToString(LogLevel::Info)).substr(0, levelText.size()));
        EXPECT_EQ(textField(parsed, "logger"), kLoggerName);
        EXPECT_EQ(textField(parsed, "thread"), kThreadId);
        EXPECT_EQ(textField(parsed, "message"), "请求处理完成");
    }

    /**
     * @brief 根记录器没有名字：省略 logger 键，而不是写一个空串
     */
    TEST(JsonFormatterTest, OmitsLoggerKeyWhenLoggerNameIsEmpty)
    {
        JsonFormatter formatter;
        const std::string line = formatter.format(makeEvent("匿名日志器", {}));

        const FormatValue parsed = JsonParser::parse(line);
        ASSERT_TRUE(parsed.isObject());
        EXPECT_EQ(parsed.find("logger"), nullptr);
        // 其余字段照旧：省键不能顺手把日志本身也省掉
        EXPECT_EQ(textField(parsed, "message"), "匿名日志器");
    }

    /**
     * @brief 消息里的换行必须转义：一条日志一行是 JSON Lines 能被按行切分的前提
     */
    TEST(JsonFormatterTest, EscapesNewlinesSoEachEventStaysOnOneLine)
    {
        const std::string message = "第一行\n第二行\r\n第三行";
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent(message));

        EXPECT_EQ(line.find('\n'), std::string::npos);
        EXPECT_EQ(line.find('\r'), std::string::npos);
        EXPECT_EQ(textField(JsonParser::parse(line), "message"), message);
    }

    /**
     * @brief 引号、反斜杠与控制字符都要转义成合法 JSON 转义序列
     */
    TEST(JsonFormatterTest, EscapesQuotesBackslashesAndControlCharacters)
    {
        const std::string message = "say \"hi\" at C:\\logs\tnow";
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent(message));

        EXPECT_EQ(textField(JsonParser::parse(line), "message"), message);
    }

    /**
     * @brief 中文按 UTF-8 原样写出（ensureAscii 关闭），不膨胀成 \uXXXX
     */
    TEST(JsonFormatterTest, WritesNonAsciiTextAsRawUtf8Bytes)
    {
        JsonFormatter formatter;
        const std::string line = formatter.format(makeEvent("中文日志"));

        EXPECT_NE(line.find("中文日志"), std::string::npos);
        EXPECT_EQ(line.find("\\u"), std::string::npos);
    }

    /**
     * @brief 紧凑单行：没有缩进换行，也没有缩进用的空格
     */
    TEST(JsonFormatterTest, WritesCompactSingleLineJson)
    {
        JsonFormatter formatter;
        const std::string line = formatter.format(makeEvent("紧凑"));

        EXPECT_EQ(line.find('\n'), std::string::npos);
        EXPECT_EQ(line.find(", "), std::string::npos);
        EXPECT_EQ(line.front(), '{');
        EXPECT_EQ(line.back(), '}');
    }

    /**
     * @brief 源码位置字段随构建类型出现或消失：Debug 有 file/line/function，Release 一律没有
     */
    TEST(JsonFormatterTest, IncludesSourceLocationOnlyInDebugBuilds)
    {
        JsonFormatter formatter;
        const FormatValue parsed = JsonParser::parse(formatter.format(makeEvent("定位")));

#ifdef ASYN_DEBUG
        EXPECT_EQ(textField(parsed, "file"), kSourceFile);
        EXPECT_EQ(parsed.find("line")->asInt(), kSourceLine);
        EXPECT_EQ(textField(parsed, "function"), kSourceFunction);
#else
        EXPECT_EQ(parsed.find("file"), nullptr);
        EXPECT_EQ(parsed.find("line"), nullptr);
        EXPECT_EQ(parsed.find("function"), nullptr);
#endif
    }

    /**
     * @brief 行号写成 JSON 数字而不是字符串：采集端要能直接做数值比较
     */
    TEST(JsonFormatterTest, WritesLineNumberAsJsonNumber)
    {
        JsonFormatter formatter;
        const FormatValue parsed = JsonParser::parse(formatter.format(makeEvent("行号")));

#ifdef ASYN_DEBUG
        const FormatValue *lineField = parsed.find("line");
        ASSERT_NE(lineField, nullptr);
        EXPECT_TRUE(lineField->isIntegralNumber());
#else
        EXPECT_EQ(parsed.find("line"), nullptr);
#endif
    }
} // namespace AsynGyanis::Base
