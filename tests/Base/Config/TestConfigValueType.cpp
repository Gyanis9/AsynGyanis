// ConfigValueType 单元测试：类型枚举名称映射、配置文件后缀判定与键路径拆分。

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
        /// 全部合法的配置值类型枚举（nlohmann value_t 的全部取值）
        const std::vector<ConfigValueType> kAllTypes = {
                ConfigValueType::null,           ConfigValueType::object,          ConfigValueType::array,        ConfigValueType::string, ConfigValueType::boolean,
                ConfigValueType::number_integer, ConfigValueType::number_unsigned, ConfigValueType::number_float, ConfigValueType::binary, ConfigValueType::discarded,
        };

        /// 合法枚举值与类型名的完整映射表
        const std::vector<std::pair<ConfigValueType, const char *>> kTypeNameTable = {
                {ConfigValueType::null, "null"},           {ConfigValueType::boolean, "bool"},
                {ConfigValueType::number_integer, "int"},  {ConfigValueType::number_unsigned, "uint"},
                {ConfigValueType::number_float, "double"}, {ConfigValueType::string, "string"},
                {ConfigValueType::array, "array"},         {ConfigValueType::object, "object"},
                {ConfigValueType::binary, "binary"},       {ConfigValueType::discarded, "discarded"},
        };

        /// 超出枚举定义范围的值，用于驱动 typeName 的 default 分支
        /// （0..9 已被 value_t 全部占用，此处只能从 10 起取值）
        const std::vector<std::uint8_t> kOutOfRangeValues = {10, 11, 100, 128, 254, 255};

        /// 带 YAML 后缀（含大小写混合）的路径样本
        const std::vector<std::string> kYamlPaths = {
                "config.yaml", "config.yml", "/path/to/config.yaml", "./relative/path/app.yml", ".yaml", ".yml", ".hidden.yaml", "a.yml", "my.config.v2.yaml",
                "Config.YaMl", "APP.YML",    "settings.YmL",
        };

        /// 不应被判定为 YAML 的路径样本
        const std::vector<std::string> kNonYamlPaths = {
                "",        "a",           "config",         ".y",           ".ym", "yaml", "yml", "config.json", "config.xml", "config.toml", "config.ini", "yaml.txt",
                "yml.txt", "my.yaml.bak", "archive.tar.gz", "config.yamlx",
        };

        /// 带 JSON 后缀的路径样本
        const std::vector<std::string> kJsonPaths = {
                "config.json", "/path/to/config.json", ".json", "a.b.c.json", "manifest.JSON", "settings.Json",
        };

        /// 不应被判定为 JSON 的路径样本
        const std::vector<std::string> kNonJsonPaths = {
                "", "json", ".js", ".jsonx", "config.yaml", "config.yml", "data.jsonl", "notes.txt",
        };

        /// 既非 YAML 也非 JSON 的路径样本：isConfigFile（两者取并集）应全部拒绝
        const std::vector<std::string> kNonConfigPaths = {
                "",          "a",           "config",         ".y",           ".ym",        ".js",       ".jsonx",  "yaml",
                "yml",       "json",        "config.xml",     "config.toml",  "config.ini", "yaml.txt",  "yml.txt", "data.jsonl",
                "notes.txt", "my.yaml.bak", "archive.tar.gz", "config.yamlx", "Makefile",   "README.MD",
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

    TEST(ConfigValueTypeTest, TypeNameIsStableAcrossRepeatedCalls)
    {
        // 错误文案依赖这些名称保持稳定：重复调用必须给出同一份字面量
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            EXPECT_STREQ(typeName(ConfigValueType::number_integer), "int");
            EXPECT_STREQ(typeName(ConfigValueType::number_unsigned), "uint");
            EXPECT_STREQ(typeName(ConfigValueType::boolean), "bool");
        }
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
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            EXPECT_TRUE(isYamlFile("config.yaml"));
            EXPECT_TRUE(isJsonFile("CONFIG.JSON"));
            EXPECT_FALSE(isYamlFile("config.json"));
            EXPECT_FALSE(isJsonFile("config.yml"));
        }
    }

} // namespace AsynGyanis::Base
