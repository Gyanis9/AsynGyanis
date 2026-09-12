/**
 * @file TestYamlReader.cpp
 * @brief YamlReader 流式事件接口测试：事件序列、push/pull、指令、标签与错误定位
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Yaml/YamlReader.h"

#include "Base/Format/Yaml/YamlEvent.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/TextPosition.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 执行一段应当失败的扫描并取回解析错误
         * @param action 触发扫描的动作
         * @return FormatError 捕获到的错误对象副本
         */
        FormatError catchFormatError(const std::function<void()> &action)
        {
            try
            {
                action();
            } catch (const FormatError &error)
            {
                return error;
            }
            catch (...)
            {
                ADD_FAILURE() << "预期抛出 FormatError，实际抛出了其他异常";
                return FormatError("wrong exception type", TextPosition{});
            }

            ADD_FAILURE() << "预期抛出 FormatError，但扫描成功";
            return FormatError("no exception thrown", TextPosition{});
        }

        /**
         * @brief 统计事件中指定类型的数量
         * @param events 事件序列
         * @param type 目标类型
         * @return std::size_t 数量
         */
        std::size_t countEvents(const std::vector<YamlEvent> &events, const YamlEventType type)
        {
            std::size_t total = 0;
            for (const YamlEvent &event : events)
            {
                if (event.type == type)
                {
                    ++total;
                }
            }
            return total;
        }

        /**
         * @brief 查找第一个满足条件的标量事件
         * @param events 事件序列
         * @param predicate 判定函数
         * @return const YamlEvent* 命中事件指针，未命中返回 nullptr
         */
        const YamlEvent *findScalar(const std::vector<YamlEvent> &events, const std::function<bool(const YamlEvent &)> &predicate)
        {
            for (const YamlEvent &event : events)
            {
                if (event.type == YamlEventType::Scalar && predicate(event))
                {
                    return &event;
                }
            }
            return nullptr;
        }
    } // namespace

    TEST(YamlReader, EmitsStreamAndDocumentBoundariesForMultipleDocuments)
    {
        const std::vector<YamlEvent> events = YamlReader::readAll("---\na: 1\n---\nb: 2\n");

        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events.front().type, YamlEventType::StreamStart);
        EXPECT_EQ(events.back().type, YamlEventType::StreamEnd);
        EXPECT_EQ(countEvents(events, YamlEventType::DocumentStart), 2U);
        EXPECT_EQ(countEvents(events, YamlEventType::DocumentEnd), 2U);
        EXPECT_EQ(countEvents(events, YamlEventType::MappingStart), 2U);
        EXPECT_EQ(countEvents(events, YamlEventType::MappingEnd), 2U);
    }

    TEST(YamlReader, EmitsScalarWithStyleAndTag)
    {
        const std::vector<YamlEvent> events = YamlReader::readAll(
                "tagged: !!str hello\n"
                "quoted: 'x'\n"
                "block: |\n"
                "  y\n"
                "folded: >\n"
                "  z\n");

        const YamlEvent *tagged = findScalar(events, [](const YamlEvent &event)
        {
            return event.tag == "tag:yaml.org,2002:str";
        });
        ASSERT_NE(tagged, nullptr);
        EXPECT_EQ(tagged->text, "hello");
        EXPECT_EQ(tagged->style, YamlScalarStyle::Plain);

        ASSERT_NE(findScalar(events, [](const YamlEvent &event)
                    {
                        return event.style == YamlScalarStyle::SingleQuoted && event.text == "x";
                    }),
                  nullptr);
        ASSERT_NE(findScalar(events, [](const YamlEvent &event)
                    {
                        return event.style == YamlScalarStyle::Literal && event.text == "y\n";
                    }),
                  nullptr);
        ASSERT_NE(findScalar(events, [](const YamlEvent &event)
                    {
                        return event.style == YamlScalarStyle::Folded && event.text == "z\n";
                    }),
                  nullptr);
    }

    TEST(YamlReader, EmitsAnchorAndAliasEvents)
    {
        const std::vector<YamlEvent> events = YamlReader::readAll("a: &x 1\nb: *x\n");

        ASSERT_NE(findScalar(events, [](const YamlEvent &event)
                    {
                        return event.anchor == "x";
                    }),
                  nullptr);

        bool aliasFound = false;
        for (const YamlEvent &event : events)
        {
            if (event.type == YamlEventType::Alias)
            {
                aliasFound = event.alias == "x";
            }
        }
        EXPECT_TRUE(aliasFound);
        EXPECT_EQ(countEvents(events, YamlEventType::Anchor), 1U);
    }

    TEST(YamlReader, EmitsFlowContainerEvents)
    {
        const std::vector<YamlEvent> events = YamlReader::readAll("ports: [80, 443]\nlabels: {a: 1}\n");

        EXPECT_EQ(countEvents(events, YamlEventType::SequenceStart), 1U);
        EXPECT_EQ(countEvents(events, YamlEventType::SequenceEnd), 1U);
        EXPECT_EQ(countEvents(events, YamlEventType::MappingStart), 2U); // 根映射 + 流式映射
    }

    TEST(YamlReader, SupportsPushModeWithChunkedFeeding)
    {
        YamlReader reader;
        reader.feed("a: ");
        reader.feed("1\n");
        reader.finish();

        std::vector<YamlEvent> events;
        while (reader.hasNext())
        {
            if (std::optional<YamlEvent> event = reader.nextEvent())
            {
                events.push_back(std::move(*event));
            }
        }

        ASSERT_NE(findScalar(events, [](const YamlEvent &event)
                    {
                        return event.text == "1";
                    }),
                  nullptr);
        EXPECT_EQ(events.front().type, YamlEventType::StreamStart);
        EXPECT_EQ(events.back().type, YamlEventType::StreamEnd);
    }

    TEST(YamlReader, RejectsFeedingAfterInputIsFinished)
    {
        YamlReader reader;
        reader.feed("a: 1\n");
        reader.finish();

        const FormatError error = catchFormatError([&reader]
        {
            reader.feed("b: 2\n");
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
    }

    TEST(YamlReader, PullReturnsEmptyAtEventExhaustion)
    {
        YamlReader reader("a: 1\n");
        while (reader.hasNext())
        {
            ASSERT_TRUE(reader.nextEvent().has_value());
        }
        EXPECT_FALSE(reader.nextEvent().has_value());
    }

    TEST(YamlReader, IgnoresUnknownDirectiveAndRecordsWarning)
    {
        YamlReader reader("%FOO bar\n---\na: 1\n");

        // 未知指令按 §6.3 忽略；扫描仍产出完整事件流并记录警告
        EXPECT_TRUE(reader.hasNext());
        EXPECT_FALSE(reader.warnings().empty());
    }

    TEST(YamlReader, RejectsUnknownDirectiveWhenConfigured)
    {
        YamlParseOptions options;
        options.rejectUnknownDirectives = true;

        const FormatError error = catchFormatError([&options]
        {
            YamlReader reader("%FOO bar\n---\na: 1\n", options);
            static_cast<void>(reader.hasNext());
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
    }

    TEST(YamlReader, ValidatesYamlVersionDirective)
    {
        YamlReader accepted("%YAML 1.2\n---\na: 1\n");
        EXPECT_TRUE(accepted.hasNext());

        const FormatError rejected = catchFormatError([]
        {
            YamlReader reader("%YAML 2.0\n---\na: 1\n");
            static_cast<void>(reader.hasNext());
        });
        EXPECT_EQ(rejected.kind(), FormatErrorKind::InvalidKeyword);
        EXPECT_NE(std::string(rejected.what()).find("不支持的 YAML 版本"), std::string::npos);
    }

    TEST(YamlReader, KeepsColumnZeroDirectiveAsDirectiveAfterBlockScalar)
    {
        // 列 0 的 `%YAML` 仍按指令处理（§6.8）：1.1 触发降级警告，2.0 直接报 InvalidKeyword
        YamlReader legacy("%YAML 1.1\n---\na: 1\n");
        EXPECT_TRUE(legacy.hasNext());
        EXPECT_FALSE(legacy.warnings().empty());

        const FormatError error = catchFormatError([]
        {
            YamlReader reader("a: |\n  text\n%YAML 2.0\n---\nb: 1\n");
            static_cast<void>(reader.hasNext());
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
    }

    TEST(YamlReader, ExpandsTagDirectiveHandle)
    {
        const std::vector<YamlEvent> events = YamlReader::readAll("%TAG !e! tag:example.com,2000:\n---\na: !e!thing v\n");

        ASSERT_NE(findScalar(events, [](const YamlEvent &event)
                    {
                        return event.tag == "tag:example.com,2000:thing";
                    }),
                  nullptr);
    }

    TEST(YamlReader, ReportsTabIndentationWithLineAndColumn)
    {
        const FormatError error = catchFormatError([]
        {
            YamlReader reader("root:\n\tchild: 1\n");
            static_cast<void>(reader.hasNext());
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        EXPECT_NE(std::string(error.what()).find("制表符"), std::string::npos);
        EXPECT_EQ(error.position().lineNumber, 2U);
        EXPECT_EQ(error.position().columnNumber, 1U);
    }

    TEST(YamlReader, ReportsUnterminatedFlowContainer)
    {
        const FormatError error = catchFormatError([]
        {
            YamlReader reader("a: [1, 2\n");
            static_cast<void>(reader.hasNext());
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnterminatedContainer);
        EXPECT_NE(std::string(error.what()).find("流式序列未闭合"), std::string::npos);
    }

    TEST(YamlReader, ReportsUnterminatedQuotedScalar)
    {
        const FormatError error = catchFormatError([]
        {
            YamlReader reader("a: \"unclosed\n");
            static_cast<void>(reader.hasNext());
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnterminatedString);
    }
} // namespace AsynGyanis::Base
