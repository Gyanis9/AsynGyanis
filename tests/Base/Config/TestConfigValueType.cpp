/**
 * @file TestConfigValueType.cpp
 * @brief ConfigValueType 单元测试：类型枚举名称映射、配置文件后缀判定与键路径拆分
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 全部合法的配置值类型枚举，顺序与 ConfigValue::VariantType 的变体下标一致
        const std::vector<ConfigValueType> kAllTypes = {
                ConfigValueType::Null,
                ConfigValueType::Bool,
                ConfigValueType::Int,
                ConfigValueType::Double,
                ConfigValueType::String,
                ConfigValueType::Array,
                ConfigValueType::Object,
        };

        /// 合法枚举值与类型名的完整映射表
        const std::vector<std::pair<ConfigValueType, const char *> > kTypeNameTable = {
                {ConfigValueType::Null, "null"},
                {ConfigValueType::Bool, "bool"},
                {ConfigValueType::Int, "int"},
                {ConfigValueType::Double, "double"},
                {ConfigValueType::String, "string"},
                {ConfigValueType::Array, "array"},
                {ConfigValueType::Object, "object"},
        };

        /// 超出枚举定义范围的值，用于驱动 typeName 的 default 分支
        /// （7 已被 FormatValueType::UInt 占用，此处只能从 8 起取值）
        const std::vector<std::uint8_t> kOutOfRangeValues = {8, 9, 100, 128, 254, 255};

        /// 带 YAML 后缀（含大小写混合）的路径样本
        const std::vector<std::string> kYamlPaths = {
                "config.yaml",
                "config.yml",
                "/path/to/config.yaml",
                "./relative/path/app.yml",
                ".yaml",
                ".yml",
                ".hidden.yaml",
                "a.yml",
                "my.config.v2.yaml",
                "Config.YaMl",
                "APP.YML",
                "settings.YmL",
        };

        /// 不应被判定为 YAML 的路径样本
        const std::vector<std::string> kNonYamlPaths = {
                "",
                "a",
                "config",
                ".y",
                ".ym",
                "yaml",
                "yml",
                "config.json",
                "config.xml",
                "config.toml",
                "config.ini",
                "yaml.txt",
                "yml.txt",
                "my.yaml.bak",
                "archive.tar.gz",
                "config.yamlx",
        };

        /// 带 JSON 后缀的路径样本
        const std::vector<std::string> kJsonPaths = {
                "config.json",
                "/path/to/config.json",
                ".json",
                "a.b.c.json",
                "manifest.JSON",
                "settings.Json",
        };

        /// 不应被判定为 JSON 的路径样本
        const std::vector<std::string> kNonJsonPaths = {
                "",
                "json",
                ".js",
                ".jsonx",
                "config.yaml",
                "config.yml",
                "data.jsonl",
                "notes.txt",
        };

        /// 既非 YAML 也非 JSON 的路径样本：isConfigFile（两者取并集）应全部拒绝
        const std::vector<std::string> kNonConfigPaths = {
                "",
                "a",
                "config",
                ".y",
                ".ym",
                ".js",
                ".jsonx",
                "yaml",
                "yml",
                "json",
                "config.xml",
                "config.toml",
                "config.ini",
                "yaml.txt",
                "yml.txt",
                "data.jsonl",
                "notes.txt",
                "my.yaml.bak",
                "archive.tar.gz",
                "config.yamlx",
                "Makefile",
                "README.MD",
        };
    } // namespace

    // ============================================================================
    // typeName：枚举到可读名称的映射
    // ============================================================================

    TEST(ConfigValueTypeTest, TypeNameMapsEveryEnumerator)
    {
        for (const auto &[type, expectedName]: kTypeNameTable)
        {
            EXPECT_STREQ(typeName(type), expectedName) << "type ordinal=" << static_cast<int>(type);
        }
    }

    TEST(ConfigValueTypeTest, TypeNameReturnsNonNullForEveryValidEnumerator)
    {
        for (const ConfigValueType type: kAllTypes)
        {
            const char *name = typeName(type);
            ASSERT_NE(name, nullptr);
            EXPECT_FALSE(std::string(name).empty());
            EXPECT_STRNE(name, "unknown");
        }
    }

    TEST(ConfigValueTypeTest, TypeNameReturnsUnknownForOutOfRangeEnumerators)
    {
        for (const std::uint8_t rawValue: kOutOfRangeValues)
        {
            EXPECT_STREQ(typeName(static_cast<ConfigValueType>(rawValue)), "unknown") << "rawValue=" << static_cast<int>(rawValue);
        }
    }

    TEST(ConfigValueTypeTest, TypeNameOfMapsEveryStoredVariantType)
    {
        EXPECT_STREQ(typeNameOf<bool>(), "bool");
        EXPECT_STREQ(typeNameOf<int64_t>(), "int");
        EXPECT_STREQ(typeNameOf<double>(), "double");
        EXPECT_STREQ(typeNameOf<std::string>(), "string");
        EXPECT_STREQ(typeNameOf<std::nullptr_t>(), "null");
    }

    TEST(ConfigValueTypeTest, TypeNameOfUsesContainerSpecializationsForArrayAndObject)
    {
        EXPECT_STREQ(typeNameOf<ConfigArray>(), "array");
        EXPECT_STREQ(typeNameOf<ConfigObject>(), "object");
        EXPECT_STREQ(typeNameOf<ConfigArray>(), typeName(ConfigValueType::Array));
        EXPECT_STREQ(typeNameOf<ConfigObject>(), typeName(ConfigValueType::Object));
    }

    // ============================================================================
    // isYamlFile / isJsonFile / isConfigFile：后缀判定
    // ============================================================================

    TEST(ConfigValueTypeTest, IsYamlFileAcceptsYamlAndYmlSuffixInAnyCase)
    {
        for (const std::string &path: kYamlPaths)
        {
            EXPECT_TRUE(isYamlFile(path)) << "path=" << path;
        }
    }

    TEST(ConfigValueTypeTest, IsYamlFileRejectsOtherAndMissingSuffix)
    {
        for (const std::string &path: kNonYamlPaths)
        {
            EXPECT_FALSE(isYamlFile(path)) << "path=" << path;
        }
    }

    TEST(ConfigValueTypeTest, IsJsonFileAcceptsJsonSuffixInAnyCase)
    {
        for (const std::string &path: kJsonPaths)
        {
            EXPECT_TRUE(isJsonFile(path)) << "path=" << path;
        }
    }

    TEST(ConfigValueTypeTest, IsJsonFileRejectsYamlAndNearMatchSuffixes)
    {
        for (const std::string &path: kNonJsonPaths)
        {
            EXPECT_FALSE(isJsonFile(path)) << "path=" << path;
        }
    }

    TEST(ConfigValueTypeTest, IsConfigFileAcceptsEverySupportedSuffix)
    {
        for (const std::string &path: kYamlPaths)
        {
            EXPECT_TRUE(isConfigFile(path)) << "path=" << path;
        }
        for (const std::string &path: kJsonPaths)
        {
            EXPECT_TRUE(isConfigFile(path)) << "path=" << path;
        }
    }

    TEST(ConfigValueTypeTest, IsConfigFileRejectsUnsupportedAndEmptyPaths)
    {
        for (const std::string &path: kNonConfigPaths)
        {
            EXPECT_FALSE(isConfigFile(path)) << "path=" << path;
        }
    }

    TEST(ConfigValueTypeTest, SuffixHelpersSurviveRepeatedCalls)
    {
        // 迁移自旧 Catch2 压力用例：迭代次数由 10000 降至 1000，结果保持确定性
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            EXPECT_TRUE(isYamlFile("config.yaml"));
            EXPECT_TRUE(isJsonFile("CONFIG.JSON"));
            EXPECT_FALSE(isYamlFile("config.json"));
            EXPECT_FALSE(isJsonFile("config.yml"));
        }
    }

    // ============================================================================
    // splitKey：键路径拆分
    // ============================================================================

    TEST(ConfigValueTypeTest, SplitKeySplitsMultiLevelDottedKey)
    {
        EXPECT_EQ(splitKey("server.host"), (std::vector<std::string>{"server", "host"}));
        EXPECT_EQ(splitKey("a.b.c.d.e"), (std::vector<std::string>{"a", "b", "c", "d", "e"}));
        EXPECT_EQ(splitKey("database.connection.pool.maxSize"), (std::vector<std::string>{"database", "connection", "pool", "maxSize"}));
    }

    TEST(ConfigValueTypeTest, SplitKeyReturnsSingleSegmentForLeafKey)
    {
        EXPECT_EQ(splitKey("server"), (std::vector<std::string>{"server"}));
        EXPECT_EQ(splitKey("debug.enabled"), (std::vector<std::string>{"debug", "enabled"}));
    }

    TEST(ConfigValueTypeTest, SplitKeyIgnoresEmptySegmentsFromConsecutiveAndSurroundingDelimiters)
    {
        EXPECT_EQ(splitKey("a..b"), (std::vector<std::string>{"a", "b"}));
        EXPECT_EQ(splitKey("a....b"), (std::vector<std::string>{"a", "b"}));
        EXPECT_EQ(splitKey(".leading"), (std::vector<std::string>{"leading"}));
        EXPECT_EQ(splitKey("trailing."), (std::vector<std::string>{"trailing"}));
        EXPECT_EQ(splitKey("."), (std::vector<std::string>{}));
        EXPECT_EQ(splitKey(".."), (std::vector<std::string>{}));
    }

    TEST(ConfigValueTypeTest, SplitKeyReturnsEmptyResultForEmptyInput)
    {
        EXPECT_TRUE(splitKey("").empty());
        EXPECT_TRUE(splitKey("", '.').empty());
        EXPECT_TRUE(splitKey("", '/').empty());
    }

    TEST(ConfigValueTypeTest, SplitKeyHonoursCustomDelimiter)
    {
        EXPECT_EQ(splitKey("a/b/c", '/'), (std::vector<std::string>{"a", "b", "c"}));
        EXPECT_EQ(splitKey("key:value", ':'), (std::vector<std::string>{"key", "value"}));
        EXPECT_EQ(splitKey("a--b-", '-'), (std::vector<std::string>{"a", "b"}));
        EXPECT_EQ(splitKey("-", '-'), (std::vector<std::string>{}));
    }

    TEST(ConfigValueTypeTest, SplitKeyKeepsForeignDelimiterInsideSegments)
    {
        EXPECT_EQ(splitKey("a.b.c", '/'), (std::vector<std::string>{"a.b.c"}));
        EXPECT_EQ(splitKey("a/b", '.'), (std::vector<std::string>{"a/b"}));
    }

    TEST(ConfigValueTypeTest, SplitKeyPreservesSpecialCharactersInSegments)
    {
        EXPECT_EQ(splitKey("hello_world.foo-bar.baz123"), (std::vector<std::string>{"hello_world", "foo-bar", "baz123"}));
        EXPECT_EQ(splitKey("part number.dose"), (std::vector<std::string>{"part number", "dose"}));
        EXPECT_EQ(splitKey("中文键.mysql.连接池"), (std::vector<std::string>{"中文键", "mysql", "连接池"}));
    }

    TEST(ConfigValueTypeTest, SplitKeyHandlesManySegmentsAndLongSingleSegment)
    {
        constexpr int kSegmentCount = 200;

        std::string key;
        for (int segmentIndex = 0; segmentIndex < kSegmentCount; ++segmentIndex)
        {
            if (segmentIndex > 0)
            {
                key += '.';
            }
            key += "segment" + std::to_string(segmentIndex);
        }

        const std::vector<std::string> parts = splitKey(key);
        ASSERT_EQ(parts.size(), static_cast<size_t>(kSegmentCount));
        EXPECT_EQ(parts.front(), "segment0");
        EXPECT_EQ(parts.back(), "segment" + std::to_string(kSegmentCount - 1));

        const std::string longSegment(4096, 'x');
        const std::vector<std::string> singlePart = splitKey(longSegment);
        ASSERT_EQ(singlePart.size(), 1U);
        EXPECT_EQ(singlePart.front().size(), longSegment.size());
    }

    TEST(ConfigValueTypeTest, SplitKeyIsStableAcrossRepeatedCalls)
    {
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            const std::vector<std::string> parts = splitKey("server.listen.address");
            ASSERT_EQ(parts.size(), 3U);
            EXPECT_EQ(parts[1], "listen");
        }
    }
} // namespace AsynGyanis::Base
