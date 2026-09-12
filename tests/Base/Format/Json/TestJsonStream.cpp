/**
 * @file TestJsonStream.cpp
 * @brief JsonReader 与 JsonStreamWriter 单元测试：跨分片事件一致性、上限与与 DOM 写出交叉验证
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonReader.h"
#include "Base/Format/Json/JsonStreamWriter.h"

#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/Json/JsonWriter.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/Value/FormatValue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 取事件类型名，便于把事件序列压成可比较的字符串
         * @param type 事件类型
         * @return const char* 类型名
         */
        const char *eventTypeName(const JsonEventType type) noexcept
        {
            switch (type)
            {
                case JsonEventType::StreamStart:
                    return "StreamStart";
                case JsonEventType::StreamEnd:
                    return "StreamEnd";
                case JsonEventType::ObjectStart:
                    return "ObjectStart";
                case JsonEventType::ObjectEnd:
                    return "ObjectEnd";
                case JsonEventType::ArrayStart:
                    return "ArrayStart";
                case JsonEventType::ArrayEnd:
                    return "ArrayEnd";
                case JsonEventType::Key:
                    return "Key";
                case JsonEventType::String:
                    return "String";
                case JsonEventType::Number:
                    return "Number";
                case JsonEventType::Bool:
                    return "Bool";
                case JsonEventType::Null:
                    return "Null";
            }
            return "Unknown";
        }

        /**
         * @brief 把一条事件压成 "类型:文本" 形式
         * @param event 事件
         * @return std::string 可比较的签名
         */
        std::string eventSignature(const JsonEvent &event)
        {
            return std::string(eventTypeName(event.type)) + ":" + event.text;
        }

        /**
         * @brief 取尽当前可产出的事件
         * @param reader 读取器
         * @return std::vector<std::string> 事件签名序列
         */
        std::vector<std::string> drainEvents(JsonReader &reader)
        {
            std::vector<std::string> signatures;
            while (const std::optional<JsonEvent> event = reader.nextEvent())
            {
                signatures.push_back(eventSignature(*event));
            }
            return signatures;
        }

        /**
         * @brief 一次性喂入并取尽事件
         * @param source JSON 文本
         * @param options 解析选项
         * @return std::vector<std::string> 事件签名序列
         */
        std::vector<std::string> signaturesOf(const std::string_view source, const JsonParseOptions &options = JsonParseOptions{})
        {
            const std::vector<JsonEvent> events = JsonReader::readAll(source, options);
            std::vector<std::string>     signatures;
            signatures.reserve(events.size());
            for (const JsonEvent &event: events)
            {
                signatures.push_back(eventSignature(event));
            }
            return signatures;
        }

        /**
         * @brief 按固定片段长度逐片喂入并取尽事件
         * @param source JSON 文本
         * @param chunkSize 每片字节数
         * @param options 解析选项
         * @return std::vector<std::string> 事件签名序列
         */
        std::vector<std::string> signaturesOfChunked(const std::string_view source, const std::size_t chunkSize, const JsonParseOptions &options = JsonParseOptions{})
        {
            JsonReader               reader(options);
            std::vector<std::string> signatures;

            std::size_t offset = 0;
            while (offset < source.size())
            {
                const std::size_t length = std::min(chunkSize, source.size() - offset);
                reader.feed(source.substr(offset, length));
                offset += length;

                // 每喂一片就把当前已能完整判定的事件取走，模拟真实的流式消费
                for (std::string &signature: drainEvents(reader))
                {
                    signatures.push_back(std::move(signature));
                }
            }

            reader.finish();
            for (std::string &signature: drainEvents(reader))
            {
                signatures.push_back(std::move(signature));
            }
            return signatures;
        }

        /**
         * @brief 把事件流回放给 JsonStreamWriter
         * @details StreamStart/StreamEnd 只表示流边界，不产生文本。
         * @param events 事件序列
         * @param options 序列化选项
         * @return std::string 拼装出的 JSON 文本
         */
        std::string replayEvents(const std::vector<JsonEvent> &events, const JsonWriteOptions &options)
        {
            JsonStreamWriter writer(options);

            for (const JsonEvent &event: events)
            {
                switch (event.type)
                {
                    case JsonEventType::ObjectStart:
                        writer.beginObject();
                        break;
                    case JsonEventType::ObjectEnd:
                        writer.endObject();
                        break;
                    case JsonEventType::ArrayStart:
                        writer.beginArray();
                        break;
                    case JsonEventType::ArrayEnd:
                        writer.endArray();
                        break;
                    case JsonEventType::Key:
                        writer.writeKey(event.text);
                        break;
                    case JsonEventType::String:
                        writer.writeString(event.text);
                        break;
                    case JsonEventType::Number:
                        writer.writeNumber(event.text);
                        break;
                    case JsonEventType::Bool:
                        writer.writeBool(event.text == "true");
                        break;
                    case JsonEventType::Null:
                        writer.writeNull();
                        break;
                    case JsonEventType::StreamStart:
                    case JsonEventType::StreamEnd:
                        break;
                }
            }

            return writer.takeOutput();
        }

        /**
         * @brief 交叉验证用的文档
         * @details 对象键刻意全部按升序书写（a<b<c<d<e<f<g<h<i<j<k），
         *          因为流式写出无法重排键，要与 JsonWriter 的 Sorted 输出逐字节一致就必须同序；
         *          同时覆盖空容器、非 ASCII、无符号大整数、负数与浮点。
         * @return std::string JSON 文本
         */
        std::string crossValidationSource()
        {
            return R"({"a":1,"b":[1,2,3],"c":{"d":true,"e":null},"f":"文本\n","g":1.5,"h":-0.25,"i":[],"j":{},"k":18446744073709551615})";
        }
    } // namespace

    // ============================================================================
    // JsonReader：事件序列
    // ============================================================================

    TEST(JsonStream, ReaderEmitsExpectedEventSequence)
    {
        const std::vector<std::string> signatures = signaturesOf(R"({"a":[1,true,null,"s"]})");

        const std::vector<std::string> expected = {
                "StreamStart:",
                "ObjectStart:",
                "Key:a",
                "ArrayStart:",
                "Number:1",
                "Bool:true",
                "Null:",
                "String:s",
                "ArrayEnd:",
                "ObjectEnd:",
                "StreamEnd:"};

        EXPECT_EQ(signatures, expected);
    }

    TEST(JsonStream, ReaderDistinguishesKeysFromStringValues)
    {
        const std::vector<std::string> signatures = signaturesOf(R"({"same":"same"})");

        // 键走 Key 事件、值走 String 事件，消费方据此无需回溯上下文
        EXPECT_EQ(signatures[2], "Key:same");
        EXPECT_EQ(signatures[3], "String:same");
    }

    TEST(JsonStream, ReaderDecodesEscapesAndUnicode)
    {
        const std::vector<JsonEvent> events = JsonReader::readAll(R"(["\u0041","a\tb","\u4e2d"])");

        // StreamStart、ArrayStart、三条 String、ArrayEnd、StreamEnd
        ASSERT_EQ(events.size(), 7U);
        EXPECT_EQ(events[2].text, "A");
        EXPECT_EQ(events[3].text, "a\tb");
        EXPECT_EQ(events[4].text, "\xE4\xB8\xAD");
    }

    TEST(JsonStream, ReaderKeepsNumberLiteralVerbatim)
    {
        const std::vector<JsonEvent> events = JsonReader::readAll("[-1.5e-3,0.25,1e2]");

        // StreamStart、ArrayStart、三条 Number、ArrayEnd、StreamEnd
        ASSERT_EQ(events.size(), 7U);
        EXPECT_EQ(events[2].text, "-1.5e-3");
        EXPECT_EQ(events[3].text, "0.25");
        EXPECT_EQ(events[4].text, "1e2");
    }

    // ============================================================================
    // JsonReader：跨 feed 边界（本项目流式解析的核心保证）
    // ============================================================================

    TEST(JsonStream, ReaderSurvivesSingleByteFeeds)
    {
        const std::string source = R"({"a":[1,-2.5e3,true,false,null,"x\ty"],"b":{"c":{"d":"中\u4e2d"}},"e":"\ud83d\ude80"})";

        EXPECT_EQ(signaturesOfChunked(source, 1U), signaturesOf(source));
    }

    TEST(JsonStream, ReaderSurvivesThreeByteFeeds)
    {
        // 3 字节切分会把多字节 UTF-8 字符、转义序列与代理对都拦腰截断
        const std::string source = R"({"中文":"汉字\ud83d\ude80","num":-12345.678e-9,"arr":[[1],[2]],"flag":true})";

        EXPECT_EQ(signaturesOfChunked(source, 3U), signaturesOf(source));
    }

    TEST(JsonStream, ReaderSurvivesRandomSplitFeeds)
    {
        const std::string source = R"({"a":[1,-2.5e3,true,false,null,"x\ty"],"b":{"c":{"d":"中\u4e2d"}},"e":"\ud83d\ude80"})";
        const std::vector<std::string> expected = signaturesOf(source);

        // 固定种子保证可复现；1..4 字节的随机片段覆盖「切断 token/字符串/数字/转义」的全部形态
        for (std::uint32_t seed = 1; seed <= 8; ++seed)
        {
            std::mt19937 generator(seed);
            JsonReader   reader;
            std::vector<std::string> signatures;

            std::size_t offset = 0;
            while (offset < source.size())
            {
                const std::size_t length = std::min<std::size_t>(1 + generator() % 4, source.size() - offset);
                reader.feed(std::string_view(source).substr(offset, length));
                offset += length;

                for (std::string &signature: drainEvents(reader))
                {
                    signatures.push_back(std::move(signature));
                }
            }

            reader.finish();
            for (std::string &signature: drainEvents(reader))
            {
                signatures.push_back(std::move(signature));
            }

            EXPECT_EQ(signatures, expected) << "seed = " << seed;
        }
    }

    TEST(JsonStream, ReaderRebuildsSurrogatePairSplitAcrossFeeds)
    {
        const std::vector<JsonEvent> events = JsonReader::readAll(R"(["\ud83d\ude80"])");

        // StreamStart、ArrayStart、String、ArrayEnd、StreamEnd
        ASSERT_EQ(events.size(), 5U);
        EXPECT_EQ(events[2].text, "\xF0\x9F\x9A\x80");

        // 逐字节喂入必须得到同一结果
        const std::vector<std::string> signatures = signaturesOfChunked(R"(["\ud83d\ude80"])", 1U);
        EXPECT_EQ(signatures[2], std::string("String:") + "\xF0\x9F\x9A\x80");
    }

    TEST(JsonStream, ReaderCanBeDrainedIncrementally)
    {
        JsonReader reader;
        reader.feed("[1,");

        // 分隔符之后没有更多数据：只能取到已完整判定的三个事件
        const std::vector<std::string> firstBatch = drainEvents(reader);
        ASSERT_EQ(firstBatch.size(), 3U);
        EXPECT_EQ(firstBatch.back(), "Number:1");
        EXPECT_FALSE(reader.isFinished());

        reader.feed("2]");
        const std::vector<std::string> secondBatch = drainEvents(reader);
        EXPECT_EQ(secondBatch, (std::vector<std::string>{"Number:2", "ArrayEnd:"}));

        reader.finish();
        EXPECT_EQ(drainEvents(reader), (std::vector<std::string>{"StreamEnd:"}));
    }

    TEST(JsonStream, ReaderAcceptsByteOrderMarkSplitAcrossFeeds)
    {
        JsonReader reader;
        reader.feed("\xEF");
        // BOM 只喂到一个字节时不能提前判定，先返回空事件
        EXPECT_EQ(drainEvents(reader), (std::vector<std::string>{"StreamStart:"}));

        reader.feed("\xBB\xBF" R"({"a":1})");
        reader.finish();

        EXPECT_EQ(drainEvents(reader), (std::vector<std::string>{"ObjectStart:", "Key:a", "Number:1", "ObjectEnd:", "StreamEnd:"}));
    }

    // ============================================================================
    // JsonReader：与 JsonParser 一致的选项与错误分类
    // ============================================================================

    TEST(JsonStream, ReaderReusesLenientSyntaxSwitches)
    {
        const JsonParseOptions lenient{.allowComments = true, .allowTrailingCommas = true, .allowSingleQuotedStrings = true};
        const std::string      source = "// leading\n{'a':[1,2,] /* c */ ,}";

        EXPECT_EQ(signaturesOf(source, lenient),
                  (std::vector<std::string>{"StreamStart:", "ObjectStart:", "Key:a", "ArrayStart:", "Number:1", "Number:2", "ArrayEnd:", "ObjectEnd:", "StreamEnd:"}));

        // 同样文本在严格模式下必须报错（注释引导符不是合法值起始）
        EXPECT_THROW(static_cast<void>(signaturesOf(source, JsonParseOptions{})), FormatError);
    }

    TEST(JsonStream, ReaderSupportsLookaheadWithoutLosingEvents)
    {
        JsonReader reader;
        reader.feed("[1,2]");
        reader.finish();

        // hasNext() 只探测不消费：重复探测不会让 nextEvent() 丢掉事件
        EXPECT_TRUE(reader.hasNext());
        EXPECT_TRUE(reader.hasNext());
        EXPECT_EQ(eventSignature(*reader.nextEvent()), "StreamStart:");
        EXPECT_EQ(eventSignature(*reader.nextEvent()), "ArrayStart:");
        EXPECT_TRUE(reader.hasNext());
        EXPECT_EQ(eventSignature(*reader.nextEvent()), "Number:1");
        EXPECT_EQ(eventSignature(*reader.nextEvent()), "Number:2");
        EXPECT_EQ(eventSignature(*reader.nextEvent()), "ArrayEnd:");

        EXPECT_TRUE(reader.hasNext());
        EXPECT_EQ(eventSignature(*reader.nextEvent()), "StreamEnd:");
        EXPECT_FALSE(reader.hasNext());

        // 选项快照可用，便于调用方核对当前生效的上限
        EXPECT_EQ(reader.options().maximumDepth, JsonParseOptions{}.maximumDepth);
        EXPECT_TRUE(reader.isFinished());
    }

    TEST(JsonStream, ReaderReportsUnterminatedContainerAfterFinish)
    {
        JsonReader reader;
        reader.feed(R"({"a":1)");
        reader.finish();

        try
        {
            static_cast<void>(drainEvents(reader));
            FAIL() << "未闭合对象在 finish() 之后应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::UnterminatedContainer);
        }
    }

    TEST(JsonStream, ReaderReportsUnterminatedStringAfterFinish)
    {
        JsonReader reader;
        reader.feed(R"(["abc)");
        reader.finish();

        try
        {
            static_cast<void>(drainEvents(reader));
            FAIL() << "未闭合字符串在 finish() 之后应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::UnterminatedString);
        }
    }

    TEST(JsonStream, ReaderReportsEmptyInputAndTrailingContent)
    {
        try
        {
            static_cast<void>(JsonReader::readAll(""));
            FAIL() << "空输入应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::EmptyInput);
        }

        try
        {
            static_cast<void>(JsonReader::readAll("1 2"));
            FAIL() << "根值之后的残留内容应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::TrailingContent);
        }
    }

    TEST(JsonStream, ReaderReportsInvalidNumberSplitAcrossFeeds)
    {
        JsonReader reader;
        reader.feed("[");
        reader.feed("0");
        reader.feed("1");
        reader.finish();

        try
        {
            static_cast<void>(drainEvents(reader));
            FAIL() << "前导零应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::InvalidNumber);
        }
    }

    TEST(JsonStream, ReaderEnforcesDepthAndElementLimits)
    {
        const JsonParseOptions shallow{.maximumDepth = 2};
        EXPECT_NO_THROW(static_cast<void>(signaturesOf("[[ ]]", shallow)));
        EXPECT_THROW(static_cast<void>(signaturesOf("[[[]]]", shallow)), FormatError);

        const JsonParseOptions narrow{.maximumContainerElements = 2};
        EXPECT_NO_THROW(static_cast<void>(signaturesOf("[1,2]", narrow)));
        EXPECT_THROW(static_cast<void>(signaturesOf("[1,2,3]", narrow)), FormatError);
    }

    // ============================================================================
    // JsonStreamWriter：与 DOM 版 JsonWriter 的交叉验证
    // ============================================================================

    TEST(JsonStream, StreamWriterReproducesDomWriterByteForByte)
    {
        const std::string            source   = crossValidationSource();
        const std::vector<JsonEvent> events   = JsonReader::readAll(source);
        const FormatValue            document = JsonParser::parse(source);

        // 紧凑输出
        const JsonWriteOptions compact{.indentWidth = 0};
        EXPECT_EQ(replayEvents(events, compact), JsonWriter::write(document, compact));

        // 两空格缩进（默认）
        const JsonWriteOptions indented{.indentWidth = 2};
        EXPECT_EQ(replayEvents(events, indented), JsonWriter::write(document, indented));

        // ensureAscii：非 ASCII 全部转 \uXXXX
        const JsonWriteOptions ascii{.indentWidth = 2, .ensureAscii = true};
        EXPECT_EQ(replayEvents(events, ascii), JsonWriter::write(document, ascii));

        // 交叉验证的也是「读一遍再写一遍」与「读成 DOM 再写」的等价性
        EXPECT_EQ(JsonParser::parse(replayEvents(events, compact)) == document, true);
    }

    TEST(JsonStream, StreamWriterCanonicalizesNumbersLikeTheDomWriter)
    {
        // 原文 1e2 与 DOM 侧的 Double(100.0) 都应写出 100.0
        JsonStreamWriter writer(JsonWriteOptions{.indentWidth = 0});
        writer.writeNumber("1e2");
        EXPECT_EQ(writer.takeOutput(), JsonWriter::write(FormatValue(100.0), JsonWriteOptions{.indentWidth = 0}));

        JsonStreamWriter thirdWriter(JsonWriteOptions{.indentWidth = 0});
        thirdWriter.writeNumber("-0");
        EXPECT_EQ(thirdWriter.takeOutput(), JsonWriter::write(FormatValue(std::int64_t(0)), JsonWriteOptions{.indentWidth = 0}));

        // 非有限浮点与 DOM 侧一致写成 null
        JsonStreamWriter nullWriter(JsonWriteOptions{.indentWidth = 0});
        nullWriter.writeNumber(std::numeric_limits<double>::quiet_NaN());
        EXPECT_EQ(nullWriter.takeOutput(), "null");
    }

    TEST(JsonStream, StreamWriterHandlesEmptyContainersWithoutInnerSpace)
    {
        JsonStreamWriter writer(JsonWriteOptions{.indentWidth = 2});
        writer.beginObject();
        writer.writeKey("a");
        writer.beginArray();
        writer.endArray();
        writer.writeKey("b");
        writer.beginObject();
        writer.endObject();
        writer.endObject();

        EXPECT_EQ(writer.takeOutput(), JsonWriter::write(JsonParser::parse(R"({"a":[],"b":{}})"), JsonWriteOptions{.indentWidth = 2}));
    }

    TEST(JsonStream, StreamWriterGuardsDepthAndStructure)
    {
        // 深度守卫与 JsonWriter 一致：根容器为第 0 层
        JsonStreamWriter shallowWriter(JsonWriteOptions{.indentWidth = 0, .maximumDepth = 2});
        shallowWriter.beginArray();
        shallowWriter.beginArray();
        EXPECT_THROW(shallowWriter.beginArray(), FormatError);

        // 对象成员的值必须紧跟 writeKey
        JsonStreamWriter missingKeyWriter;
        missingKeyWriter.beginObject();
        EXPECT_THROW(missingKeyWriter.writeString("x"), FormatError);

        // 键写出后没有值就收尾
        JsonStreamWriter missingValueWriter;
        missingValueWriter.beginObject();
        missingValueWriter.writeKey("a");
        EXPECT_THROW(missingValueWriter.endObject(), FormatError);

        // 结束调用与当前层级不匹配
        JsonStreamWriter mismatchedWriter;
        mismatchedWriter.beginObject();
        EXPECT_THROW(mismatchedWriter.endArray(), FormatError);

        // 根值只能写出一次
        JsonStreamWriter twoRootsWriter;
        twoRootsWriter.writeNull();
        EXPECT_THROW(twoRootsWriter.writeNull(), FormatError);
    }

    TEST(JsonStream, StreamWriterRejectsInvalidRawNumber)
    {
        JsonStreamWriter writer;
        EXPECT_THROW(writer.writeNumber("abc"), FormatError);
        EXPECT_THROW(writer.writeNumber(""), FormatError);
    }

    TEST(JsonStream, StreamWriterReportsPendingDepth)
    {
        JsonStreamWriter writer(JsonWriteOptions{.indentWidth = 0});
        EXPECT_EQ(writer.depth(), 0U);

        writer.beginArray();
        EXPECT_EQ(writer.depth(), 1U);
        writer.beginObject();
        EXPECT_EQ(writer.depth(), 2U);

        writer.endObject();
        writer.endArray();
        EXPECT_EQ(writer.depth(), 0U);

        // output() 在任何时刻都可读取（可能是不完整文档），takeOutput() 后复位
        EXPECT_EQ(writer.takeOutput(), "[{}]");
        EXPECT_TRUE(writer.output().empty());
        EXPECT_EQ(writer.depth(), 0U);

        // reset() 丢弃已拼装文本并回到根层级，便于复用同一对象
        JsonStreamWriter reused(JsonWriteOptions{.indentWidth = 0});
        reused.beginArray();
        reused.reset();
        EXPECT_TRUE(reused.output().empty());
        EXPECT_EQ(reused.depth(), 0U);
        reused.writeNull();
        EXPECT_EQ(reused.output(), "null");
    }
} // namespace AsynGyanis::Base
