// JsonFormatter 单元测试：字段完整性与省略规则、单行紧凑输出、转义与 UTF-8 直通
//
// 每条输出都用 nlohmann::json 解析回来断言（而不是匹配字符串片段）：格式化器的契约是
// 「产出合法 JSON」，把结果喂回解析器是唯一能同时验证合法性与取值的方式。

#include "Base/Log/Formatters/JsonFormatter.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/StackTrace.h"

#include <nlohmann/json.hpp>

#include "BaseTestSupport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /// 固定文本：断言 timestamp 字段时按它比对
        constexpr auto kFixedTimestamp = "2026-09-13 20:00:00.123";
        /// 事件携带的固定时刻：由上面那段本地挂钟折出，渲染回去即得同一文本，故断言与时区无关
        const TimestampMoment kFixedTimestampMoment = TestSupport::makeLocalMoment(2026, 9, 13, 20, 0, 0, 123);
        constexpr auto        kThreadId             = "tid-123456";
        constexpr auto        kLoggerName           = "json_formatter_logger";
        constexpr auto        kSourceFile           = "json_formatter_fixture.cpp";
        constexpr auto        kSourceFunction       = "fixtureFunction";
        constexpr int         kSourceLine           = 4271;

        /// 源码位置固定写死：位置取自 SourceLocation::current() 会让断言随用例行号漂移
        constexpr SourceLocation kFixtureLocation{kSourceFile, kSourceLine, kSourceFunction};

        /**
         * @brief 造一条字段齐备的日志事件
         * @param loggerName 日志器名字，空串表示根记录器
         */
        LogEvent makeEvent(std::string message, const std::string_view loggerName = kLoggerName)
        {
            return LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, kFixtureLocation, std::string(loggerName), std::move(message));
        }

        /**
         * @brief 取 JSON 对象里某个键的字符串值
         * @return 键不存在时为空串（断言由调用方负责）
         */
        std::string textField(const nlohmann::json &object, const std::string_view key)
        {
            const auto iterator = object.find(key);
            return iterator == object.end() ? std::string() : iterator->get<std::string>();
        }
    } // namespace

    /**
     * @brief 核心字段齐全，且输出能被 JSON 解析器解析回等价取值
     */
    TEST(JsonFormatterTest, RendersCoreFieldsAsParsableJsonObject)
    {
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent("请求处理完成"));

        const nlohmann::json parsed = nlohmann::json::parse(line);
        ASSERT_TRUE(parsed.is_object());
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
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent("匿名日志器", {}));

        const nlohmann::json parsed = nlohmann::json::parse(line);
        ASSERT_TRUE(parsed.is_object());
        EXPECT_FALSE(parsed.contains("logger"));
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
        EXPECT_EQ(textField(nlohmann::json::parse(line), "message"), message);
    }

    /**
     * @brief 引号、反斜杠与控制字符都要转义成合法 JSON 转义序列
     */
    TEST(JsonFormatterTest, EscapesQuotesBackslashesAndControlCharacters)
    {
        const std::string message = "say \"hi\" at C:\\logs\tnow";
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent(message));

        EXPECT_EQ(textField(nlohmann::json::parse(line), "message"), message);
    }

    /**
     * @brief 消息里的 NUL 字节必须转义为 \u0000 且能原样解析回来：日志内容不被截断
     */
    TEST(JsonFormatterTest, KeepsEmbeddedNulByteInMessage)
    {
        const std::string message("前\0后", 7);
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent(message));

        EXPECT_NE(line.find("\\u0000"), std::string::npos);
        EXPECT_EQ(textField(nlohmann::json::parse(line), "message"), message);
    }

    /**
     * @brief 中文按 UTF-8 原样写出，不膨胀成 \uXXXX
     */
    TEST(JsonFormatterTest, WritesNonAsciiTextAsRawUtf8Bytes)
    {
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent("中文日志"));

        EXPECT_NE(line.find("中文日志"), std::string::npos);
        EXPECT_EQ(line.find("\\u"), std::string::npos);
    }

    /**
     * @brief 非法 UTF-8 明确失败：不写出携带乱码字节的 JSON，也不替换字节凑一份「能解析」的输出
     */
    TEST(JsonFormatterTest, RejectsInvalidUtf8MessageBytes)
    {
        // 0xFF 不是任何 UTF-8 序列的合法字节
        std::string message = "坏";
        message.push_back(static_cast<char>(0xFF));
        message += "字节";
        JsonFormatter formatter;

        EXPECT_THROW(static_cast<void>(formatter.format(makeEvent(message))), Exception);
    }

    /**
     * @brief 追加语义：已有内容保留，JSON 落在其后
     * @details Sink 复用自己的行缓冲，版式必须往缓冲尾部追加而不是覆盖——这条与下一条一起钉住
     *          「直接摊进调用方缓冲」那条通道的两端。
     */
    TEST(JsonFormatterTest, AppendsLineIntoCallerBuffer)
    {
        JsonFormatter     formatter;
        const std::string prefix     = "缓冲里原有的内容:";
        std::string       buffer     = prefix;
        const std::size_t prefixSize = buffer.size();
        static_cast<void>(formatter.formatInto(buffer, makeEvent("追加")));

        EXPECT_EQ(buffer.compare(0, prefixSize, prefix), 0);
        const nlohmann::json appended = nlohmann::json::parse(buffer.substr(prefixSize));
        EXPECT_EQ(textField(appended, "message"), "追加");
    }

    /**
     * @brief 非法 UTF-8 抛出让缓冲一字未增：半途而废的半条 JSON 不能留在调用方手里
     */
    TEST(JsonFormatterTest, InvalidUtf8LeavesCallerBufferUntouched)
    {
        std::string message = "坏";
        message.push_back(static_cast<char>(0xFF));

        JsonFormatter formatter;
        std::string   buffer = "已经写好的内容";
        EXPECT_THROW(formatter.formatInto(buffer, makeEvent(message)), Exception);
        EXPECT_EQ(buffer, "已经写好的内容");
    }

    /**
     * @brief 产物是「自身解析后重新序列化」的不动点：转义表、键序与紧凑格式都取自库自己那一份
     * @details 版式改为直接摊进缓冲之后，序列化器不再是公开的 `dump()`，这条就是「输出逐字节没变」
     *          的可执行判据：任何一处转义或键序偏离库的规范写法，重新 dump 回来的文本就对不上。
     */
    TEST(JsonFormatterTest, OutputIsFixedPointOfParseAndDump)
    {
        JsonFormatter     formatter;
        const std::string line = formatter.format(makeEvent("转义 \b\f\n\r\t\x01\x7f 与 \" \\ 以及 ÿ ✓"));

        EXPECT_EQ(line, nlohmann::json::parse(line).dump());
    }

    /**
     * @brief 紧凑单行：没有缩进换行，也没有缩进用的空格
     */
    TEST(JsonFormatterTest, WritesCompactSingleLineJson)
    {
        JsonFormatter     formatter;
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
        JsonFormatter        formatter;
        const nlohmann::json parsed = nlohmann::json::parse(formatter.format(makeEvent("定位")));

#ifdef ASYN_DEBUG
        EXPECT_EQ(textField(parsed, "file"), kSourceFile);
        EXPECT_EQ(parsed.at("line").get<std::int64_t>(), kSourceLine);
        EXPECT_EQ(textField(parsed, "function"), kSourceFunction);
#else
        EXPECT_FALSE(parsed.contains("file"));
        EXPECT_FALSE(parsed.contains("line"));
        EXPECT_FALSE(parsed.contains("function"));
#endif
    }

    /**
     * @brief 行号写成 JSON 数字而不是字符串：采集端要能直接做数值比较
     */
    TEST(JsonFormatterTest, WritesLineNumberAsJsonNumber)
    {
        JsonFormatter        formatter;
        const nlohmann::json parsed = nlohmann::json::parse(formatter.format(makeEvent("行号")));

#ifdef ASYN_DEBUG
        ASSERT_TRUE(parsed.contains("line"));
        EXPECT_TRUE(parsed.at("line").is_number_integer() || parsed.at("line").is_number_unsigned());
#else
        EXPECT_FALSE(parsed.contains("line"));
#endif
    }

    /**
     * @brief 带栈的事件多出 stackTrace 字段（多帧文本），不带栈的事件不带该键
     */
    TEST(JsonFormatterTest, AddsStackTraceFieldOnlyWhenEventCarriesOne)
    {
        JsonFormatter formatter;

        const nlohmann::json plain = nlohmann::json::parse(formatter.format(makeEvent("无栈")));
        EXPECT_FALSE(plain.contains("stackTrace")) << "不带栈的事件不应出现 stackTrace 字段";

        LogEvent event   = makeEvent("带栈");
        event.stackTrace = captureStackTrace();
        if (!TestSupport::hasResolvedStackTraceFrames(formatStackTrace(event.stackTrace)))
        {
            GTEST_SKIP() << "本构建没有调试信息（既无 PDB 也无 -g），栈帧只剩模块加偏移，符号解析断言不适用";
        }

        const nlohmann::json withStack = nlohmann::json::parse(formatter.format(event));
        ASSERT_TRUE(withStack.contains("stackTrace"));
        EXPECT_FALSE(textField(withStack, "stackTrace").empty()) << "栈字段应带上解析后的帧文本";
    }

    /**
     * @brief 可选键在相邻两条之间不串味：复用的字段对象必须把上一条留着的可选键摘掉
     * @details 格式化器为省掉「每条重建全部键值」而复用一份线程局域对象，代价是可选键（logger、
     *          stackTrace）必须由实现显式摘除——忘了摘的那一版会把上一条的名字带进根日志器的行里。
     *          用例按「有名有栈 → 有名无栈 → 无名有栈 → 无名无栈」连着两轮，逐条断言键集合
     *          恰好等于该条应有的那几个，并把键数逐项对上（多出来的一定是没摘掉的旧键）。
     */
    TEST(JsonFormatterTest, OptionalKeysDoNotLeakBetweenAlternatingLines)
    {
        LogEvent withNameAndStack   = makeEvent("有名有栈", "leak_check_logger");
        withNameAndStack.stackTrace = captureStackTrace();
        LogEvent withStackNoName    = makeEvent("无名有栈", "");
        withStackNoName.stackTrace  = withNameAndStack.stackTrace;
        if (withNameAndStack.stackTrace.empty())
        {
            GTEST_SKIP() << "本构建的栈回溯拿不到帧，stackTrace 这一半的串味判据量不到";
        }

        const LogEvent withNameOnly = makeEvent("只有名字", "leak_check_logger");
        const LogEvent plainRoot    = makeEvent("根 logger 无栈", "");

        JsonFormatter                       formatter;
        const std::vector<const LogEvent *> round = {&withNameAndStack, &withNameOnly, &withStackNoName, &plainRoot};
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const LogEvent *event: round)
            {
                const nlohmann::json parsed = nlohmann::json::parse(formatter.format(*event));

                const bool hasLoggerName = !event->loggerNameView().empty();
                EXPECT_EQ(parsed.contains("logger"), hasLoggerName) << "logger 键与事件的名单不一致（第 " << pass << " 轮）";
                EXPECT_EQ(parsed.contains("stackTrace"), !event->stackTrace.empty()) << "stackTrace 键与事件是否带栈不一致";

                std::size_t expectedKeyCount = 4U + (hasLoggerName ? 1U : 0U) + (event->stackTrace.empty() ? 0U : 1U);
#ifdef ASYN_DEBUG
                expectedKeyCount += 3U;
#endif
                EXPECT_EQ(parsed.size(), expectedKeyCount) << "键数对不上：多出来的必是没摘掉的上一条字段";
                EXPECT_EQ(parsed.count("message"), 1U);
            }
        }
    }
} // namespace AsynGyanis::Base
