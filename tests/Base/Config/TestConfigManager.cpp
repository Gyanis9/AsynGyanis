/**
 * @file TestConfigManager.cpp
 * @brief ConfigManager 单元测试：目录与文件加载、类型安全访问、settings.json 用户覆盖层、
 *        schema 校验、热加载开关状态机与并发读取
 * @details 热加载的真实文件监听行为由 tests/Platform/TestFileWatcher.cpp 覆盖，
 *          本文件只断言开关与状态机，不依赖真实文件事件时序。
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Config/ConfigManager.h"

#include "TestHelpers.h"
#include "BaseTestSupport.h"
#include "Base/Config/ConfigLoadResult.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Parser/Value/ValueAccessError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 用户覆盖层文件名，由 saveOverrides() 落在配置目录下
        constexpr const char *kSettingsFileName = "settings.json";

        /// 只读部署默认配置文件名，优先级低于 settings.json
        constexpr const char *kDeployedConfigFileName = "config.yaml";

        /**
         * @brief 读取文本文件全部内容
         * @param filePath 文件路径
         * @return std::string 文件文本，打不开时返回空串
         */
        std::string readFileText(const std::filesystem::path &filePath)
        {
            std::ifstream      file(filePath, std::ios::in | std::ios::binary);
            std::ostringstream content;
            content << file.rdbuf();
            return content.str();
        }

        /**
         * @brief 判断文本是否包含子串
         * @param haystack 待检查文本
         * @param needle 子串
         * @return true 包含
         */
        bool textContains(const std::string &haystack, const std::string &needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /**
         * @brief 判断字符串列表中是否存在包含子串的条目
         * @param entries 字符串列表
         * @param needle 子串
         * @return true 至少一条命中
         */
        bool anyEntryContains(const std::vector<std::string> &entries, const std::string &needle)
        {
            return std::any_of(entries.begin(), entries.end(),
                               [&needle](const std::string &entry)
                               {
                                   return textContains(entry, needle);
                               });
        }
    } // namespace

    /**
     * @brief ConfigManager 测试夹具
     *
     * @details ConfigManager 是进程级单例：每个用例全程持有 configTestMutex()，
     *          进入与退出都关闭热加载并清空快照；退出时额外把待持久化覆盖排空到
     *          本夹具的临时目录（clear() 不会重置覆盖集与已注册 schema），
     *          防止跨用例状态泄漏。
     */
    class ConfigManagerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_lock = std::unique_lock<std::mutex>(configTestMutex());
            ConfigManager::instance().disableHotReload();
            ConfigManager::instance().clear();
            m_temporaryDirectory.emplace("ConfigManager");
        }

        void TearDown() override
        {
            ConfigManager &configuration = ConfigManager::instance();
            configuration.disableHotReload();
            static_cast<void>(configuration.setSchema(ConfigSchema{}));

            const std::filesystem::path drainDirectory = directory() / "pending-override-drain";
            std::error_code             ignored;
            std::filesystem::create_directories(drainDirectory, ignored);
            static_cast<void>(configuration.loadFromDirectory(drainDirectory));
            static_cast<void>(configuration.saveOverrides());
            configuration.clear();

            m_temporaryDirectory.reset();
            m_lock.unlock();
        }

        /// ConfigManager 单例引用
        [[nodiscard]] ConfigManager &configuration() const noexcept
        {
            return ConfigManager::instance();
        }

        /// 本用例独占的临时目录绝对路径
        [[nodiscard]] const std::filesystem::path &directory() const
        {
            return m_temporaryDirectory->path();
        }

        /**
         * @brief 拼接临时目录下的相对路径
         * @param relativePath 相对路径，可含子目录
         * @return std::filesystem::path 绝对路径
         */
        [[nodiscard]] std::filesystem::path filePath(const std::string &relativePath) const
        {
            return directory() / relativePath;
        }

        /**
         * @brief 在临时目录内写文件，缺失的父目录自动创建
         * @param relativePath 相对路径
         * @param content 文件内容
         * @return std::filesystem::path 写入后的绝对路径
         */
        std::filesystem::path writeFile(const std::string &relativePath, const std::string &content) const
        {
            EXPECT_TRUE(m_temporaryDirectory->writeNestedFile(relativePath, content));
            return filePath(relativePath);
        }

        /// settings.json 是否已落盘
        [[nodiscard]] bool settingsFileExists() const
        {
            return std::filesystem::exists(filePath(kSettingsFileName));
        }

        /// settings.json 的文本内容
        [[nodiscard]] std::string settingsFileText() const
        {
            return readFileText(filePath(kSettingsFileName));
        }

        /// 已加载文件列表中的文件名集合（升序，忽略目录前缀差异）
        [[nodiscard]] std::vector<std::string> loadedFileNames() const
        {
            std::vector<std::string> names;
            for (const std::string &entry: configuration().loadedFiles())
            {
                names.push_back(std::filesystem::path(entry).filename().string());
            }
            std::ranges::sort(names);
            return names;
        }

    private:
        std::optional<TestSupport::TemporaryDirectory> m_temporaryDirectory; ///< 用例独占临时目录
        std::unique_lock<std::mutex>                   m_lock;               ///< 串行化单例访问的互斥锁持有者
    };

    // ============================================================================
    // 单例与结果结构
    // ============================================================================

    TEST_F(ConfigManagerTest, InstanceAlwaysReturnsTheSameSingleton)
    {
        static_assert(!std::is_copy_constructible_v<ConfigManager>);
        static_assert(!std::is_copy_assignable_v<ConfigManager>);
        static_assert(!std::is_move_constructible_v<ConfigManager>);

        ConfigManager &first  = ConfigManager::instance();
        ConfigManager &second = ConfigManager::instance();

        EXPECT_EQ(&first, &second);
        EXPECT_EQ(&first, &configuration());
    }

    TEST_F(ConfigManagerTest, LoadResultDefaultsToFailureAndConvertsToBool)
    {
        ConfigLoadResult result;

        EXPECT_FALSE(result.success);
        EXPECT_FALSE(static_cast<bool>(result));
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_TRUE(result.failedFiles.empty());
        EXPECT_TRUE(result.errors.empty());

        result.success = true;
        EXPECT_TRUE(static_cast<bool>(result));
    }

    // ============================================================================
    // loadFromDirectory
    // ============================================================================

    TEST_F(ConfigManagerTest, LoadFromDirectoryWithMissingDirectoryFailsWithReason)
    {
        const std::filesystem::path missingDirectory = filePath("does-not-exist");

        const ConfigLoadResult result = configuration().loadFromDirectory(missingDirectory);

        EXPECT_FALSE(result.success);
        EXPECT_FALSE(static_cast<bool>(result));
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_FALSE(result.errors.empty());
        EXPECT_TRUE(anyEntryContains(result.errors, "配置目录不存在"));
        EXPECT_TRUE(configuration().keys().empty());
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryWithRegularFileFails)
    {
        const std::filesystem::path filePathInPlaceOfDirectory = writeFile("plain.yaml", "key: value\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(filePathInPlaceOfDirectory);

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_TRUE(anyEntryContains(result.errors, "路径不是目录"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryWithEmptyDirectorySucceedsWithoutAnyFile)
    {
        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_TRUE(result.failedFiles.empty());
        EXPECT_TRUE(configuration().keys().empty());
        EXPECT_EQ(configuration().configDirectory(), directory());
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryRecursivelyIncludesSubdirectoryFiles)
    {
        writeFile("root.yaml", "root: true\n");
        writeFile("group/nested.yaml", "group:\n  depth: 2\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory(), true);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_TRUE(configuration().getBool("root", false));
        EXPECT_EQ(configuration().getInt("group.depth", 0), 2);
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryNonRecursiveSkipsSubdirectoryFiles)
    {
        writeFile("root.yaml", "root: true\n");
        writeFile("group/nested.yaml", "group:\n  depth: 2\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory(), false);

        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.loadedFiles.size(), 1U);
        EXPECT_EQ(loadedFileNames(), (std::vector<std::string>{"root.yaml"}));
        EXPECT_TRUE(configuration().has("root"));
        EXPECT_FALSE(configuration().has("group.depth"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryIgnoresFilesWithUnsupportedSuffix)
    {
        writeFile("app.yaml", "app:\n  name: demo\n");
        writeFile("notes.txt", "not a config file\n");
        writeFile("app.toml", "key = 'value'\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 1U);
        EXPECT_TRUE(result.failedFiles.empty());
        EXPECT_TRUE(result.errors.empty());
        EXPECT_EQ(configuration().getString("app.name", ""), "demo");
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryMergesKeysFromEveryConfigFile)
    {
        writeFile("server.yaml", "server:\n  host: localhost\n  port: 8080\n");
        writeFile("storage.yaml", "database:\n  url: db.local\n  poolSize: 10\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_EQ(configuration().getString("server.host", ""), "localhost");
        EXPECT_EQ(configuration().getInt("server.port", 0), 8080);
        EXPECT_EQ(configuration().getString("database.url", ""), "db.local");
        EXPECT_EQ(configuration().getInt("database.poolSize", 0), 10);
        EXPECT_EQ(configuration().keys().size(), 4U);
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryFlattensNestedMapsIntoDottedKeys)
    {
        writeFile("deep.yaml",
                  "database:\n"
                  "  mysql:\n"
                  "    host: db.local\n"
                  "    port: 3306\n"
                  "  redis:\n"
                  "    port: 6379\n");

        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().keys(),
                  (std::vector<std::string>{"database.mysql.host", "database.mysql.port", "database.redis.port"}));
        EXPECT_EQ(configuration().get<std::string>("database.mysql.host", "missing"), "db.local");
        EXPECT_EQ(configuration().getInt("database.redis.port", 0), 6379);
        // 中间层节点被展开后不再以单独键存在
        EXPECT_FALSE(configuration().has("database"));
        EXPECT_FALSE(configuration().has("database.mysql"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryKeepsEmptyMapAndEmptyListAsLeafValues)
    {
        writeFile("leafs.yaml", "extra: {}\nlist: []\nnested:\n  blank: {}\n");

        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const std::optional<ConfigValue> extra = configuration().getOptional("extra");
        ASSERT_TRUE(extra.has_value());
        EXPECT_EQ(extra->type(), ConfigValueType::Object);
        EXPECT_TRUE(extra->empty());
        EXPECT_EQ(extra->size(), 0U);

        const std::optional<ConfigValue> list = configuration().getOptional("list");
        ASSERT_TRUE(list.has_value());
        EXPECT_EQ(list->type(), ConfigValueType::Array);
        EXPECT_TRUE(list->empty());

        EXPECT_EQ(configuration().getOptional("nested.blank")->type(), ConfigValueType::Object);
    }

    TEST_F(ConfigManagerTest, LoadFromDirectorySkipsUnparsableFileAndRecordsItAsFailed)
    {
        writeFile("good.yaml", "valid: true\n");
        writeFile("bad.yaml", "[[1, 2\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_EQ(loadedFileNames(), (std::vector<std::string>{"good.yaml"}));
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_TRUE(textContains(result.failedFiles.front(), "bad.yaml"));
        EXPECT_FALSE(result.errors.empty());
        EXPECT_TRUE(configuration().getBool("valid", false));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryKeepsPreviousSnapshotWhenEveryFileFails)
    {
        writeFile("first/ok.yaml", "first: 1\n");
        writeFile("second/broken.yaml", "[[1, 2\n");

        ASSERT_TRUE(configuration().loadFromDirectory(filePath("first")).success);
        ASSERT_EQ(configuration().getInt("first", 0), 1);

        const ConfigLoadResult result = configuration().loadFromDirectory(filePath("second"));

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_EQ(result.failedFiles.size(), 1U);
        // 全失败不提交：仍可读上一次的有效快照与目录
        EXPECT_EQ(configuration().getInt("first", 0), 1);
        EXPECT_EQ(configuration().configDirectory(), filePath("first"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryAcceptsEmptyFileWithoutProducingKeys)
    {
        writeFile("empty.yaml", "");
        writeFile("valid.yaml", "key: value\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_TRUE(result.failedFiles.empty());
        EXPECT_EQ(configuration().keys().size(), 1U);
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsFilesWhoseRootIsNotAMap)
    {
        writeFile("list.yaml", "- item1\n- item2\n");
        writeFile("scalar.yaml", "just-a-plain-scalar\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_EQ(result.failedFiles.size(), 2U);
        EXPECT_TRUE(anyEntryContains(result.errors, "根节点必须是映射"));
        EXPECT_TRUE(anyEntryContains(result.errors, "sequence"));
        EXPECT_TRUE(anyEntryContains(result.errors, "scalar"));
        EXPECT_TRUE(configuration().keys().empty());
    }

    // ============================================================================
    // JSON 支持与类型推断
    // ============================================================================

    TEST_F(ConfigManagerTest, LoadFromDirectoryReadsNestedKeysFromJsonFile)
    {
        writeFile("app.json",
                  "{\n"
                  "  \"server\": {\"host\": \"localhost\", \"port\": 8080},\n"
                  "  \"debug\": true,\n"
                  "  \"ratio\": 1.25,\n"
                  "  \"missing\": null,\n"
                  "  \"list\": [\"a\", \"b\"]\n"
                  "}\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
        EXPECT_EQ(configuration().getString("server.host", ""), "localhost");
        EXPECT_EQ(configuration().getInt("server.port", 0), 8080);
        EXPECT_TRUE(configuration().getBool("debug", false));
        EXPECT_DOUBLE_EQ(configuration().getDouble("ratio", 0.0), 1.25);
        EXPECT_EQ(configuration().getOptional("missing")->type(), ConfigValueType::Null);

        const std::optional<ConfigValue> list = configuration().getOptional("list");
        ASSERT_TRUE(list.has_value());
        EXPECT_EQ(list->size(), 2U);
        EXPECT_EQ((*list)[0].asString(), "a");
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryMergesYamlAndJsonFromSameDirectory)
    {
        writeFile("a-first.yaml", "shared: fromYaml\nyamlOnly: 1\n");
        writeFile("b-second.json", "{\"shared\": \"fromJson\", \"jsonOnly\": 2}");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_EQ(configuration().getInt("yamlOnly", 0), 1);
        EXPECT_EQ(configuration().getInt("jsonOnly", 0), 2);
        // 按路径排序加载，b-second.json 覆盖 a-first.yaml 的同名键
        EXPECT_EQ(configuration().getString("shared", ""), "fromJson");
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryInfersYamlScalarTypes)
    {
        writeFile("types.yaml",
                  "boolTrue: true\n"
                  "boolFalse: false\n"
                  "boolYes: yes\n"
                  "boolNo: no\n"
                  "boolOn: on\n"
                  "boolOff: off\n"
                  "intPositive: 12345\n"
                  "intNegative: -9876\n"
                  "intZero: 0\n"
                  "doublePlain: 3.5\n"
                  "textPlain: hello_world\n"
                  "textQuotedNumber: \"12345\"\n"
                  "textDottedVersion: 1.2.3\n"
                  "textEmptyQuoted: \"\"\n"
                  "nullExplicit: null\n"
                  "nullTilde: ~\n"
                  "nullOmitted:\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());
        ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());

        // 替换 GENERATE：以显式表驱动逐条断言类型
        const std::vector<std::pair<std::string, ConfigValueType> > expectedTypes = {
                {"boolTrue", ConfigValueType::Bool},
                {"boolFalse", ConfigValueType::Bool},
                {"boolYes", ConfigValueType::Bool},
                {"boolNo", ConfigValueType::Bool},
                {"boolOn", ConfigValueType::Bool},
                {"boolOff", ConfigValueType::Bool},
                {"intPositive", ConfigValueType::Int},
                {"intNegative", ConfigValueType::Int},
                {"intZero", ConfigValueType::Int},
                {"doublePlain", ConfigValueType::Double},
                {"textPlain", ConfigValueType::String},
                {"textQuotedNumber", ConfigValueType::String},
                {"textDottedVersion", ConfigValueType::String},
                {"textEmptyQuoted", ConfigValueType::String},
                {"nullExplicit", ConfigValueType::Null},
                {"nullTilde", ConfigValueType::Null},
                {"nullOmitted", ConfigValueType::Null},
        };

        for (const auto &[key, expectedType]: expectedTypes)
        {
            const std::optional<ConfigValue> value = configuration().getOptional(key);
            ASSERT_TRUE(value.has_value()) << "key=" << key;
            EXPECT_EQ(value->type(), expectedType) << "key=" << key;
        }

        EXPECT_TRUE(configuration().getBool("boolYes", false));
        EXPECT_TRUE(configuration().getBool("boolOn", false));
        EXPECT_FALSE(configuration().getBool("boolOff", true));
        EXPECT_FALSE(configuration().getBool("boolNo", true));
        EXPECT_FALSE(configuration().getBool("boolFalse", true));
        EXPECT_EQ(configuration().getInt("intNegative", 0), -9876);
        EXPECT_DOUBLE_EQ(configuration().getDouble("doublePlain", 0.0), 3.5);
        EXPECT_EQ(configuration().getString("textQuotedNumber", ""), "12345");
        EXPECT_EQ(configuration().getString("textDottedVersion", ""), "1.2.3");
        EXPECT_TRUE(configuration().getOptional("textEmptyQuoted")->empty());
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryScalesToHundredsOfKeys)
    {
        // 迁移自旧 Catch2 压力用例：键数保持在上限 1000 以内
        constexpr int kKeyCount = 500;

        std::string yamlContent;
        yamlContent.reserve(static_cast<size_t>(kKeyCount) * 16);
        for (int index = 0; index < kKeyCount; ++index)
        {
            yamlContent += "key" + std::to_string(index) + ": " + std::to_string(index) + "\n";
        }
        writeFile("many.yaml", yamlContent);

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_EQ(configuration().keys().size(), static_cast<size_t>(kKeyCount));
        EXPECT_EQ(configuration().getInt("key0", -1), 0);
        EXPECT_EQ(configuration().getInt("key499", -1), kKeyCount - 1);
    }

    // ============================================================================
    // loadFiles
    // ============================================================================

    TEST_F(ConfigManagerTest, LoadFilesWithEmptyListSucceedsWithoutLoadingAnything)
    {
        const ConfigLoadResult result = configuration().loadFiles({});

        EXPECT_TRUE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_TRUE(result.failedFiles.empty());
        EXPECT_TRUE(configuration().keys().empty());
    }

    TEST_F(ConfigManagerTest, LoadFilesMergesTheGivenFileList)
    {
        const std::vector<std::filesystem::path> files = {
                writeFile("a.yaml", "alpha: 1\n"),
                writeFile("b.json", "{\"beta\": 2}"),
        };

        const ConfigLoadResult result = configuration().loadFiles(files);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_EQ(configuration().getInt("alpha", 0), 1);
        EXPECT_EQ(configuration().getInt("beta", 0), 2);
    }

    TEST_F(ConfigManagerTest, LoadFilesRecordsMissingFileAsFailed)
    {
        const ConfigLoadResult result = configuration().loadFiles({filePath("nowhere.yaml")});

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_TRUE(anyEntryContains(result.errors, "文件不存在"));
        EXPECT_TRUE(configuration().keys().empty());
    }

    TEST_F(ConfigManagerTest, LoadFilesRejectsUnsupportedFileSuffix)
    {
        const std::filesystem::path textFile = writeFile("notes.txt", "anything\n");

        const ConfigLoadResult result = configuration().loadFiles({textFile});

        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_TRUE(anyEntryContains(result.errors, "不支持的配置文件格式"));
    }

    TEST_F(ConfigManagerTest, LoadReportsParseErrorWithLineAndColumn)
    {
        // 制表符缩进被手搓 YAML 解析器拒绝，错误必须带上可定位的行列
        writeFile("bad.yaml", "root:\n\tchild: 1\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "解析错误：")) << result.errors.front();
        EXPECT_TRUE(anyEntryContains(result.errors, "第 2 行"));
        EXPECT_TRUE(anyEntryContains(result.errors, "第 1 列"));
    }

    TEST_F(ConfigManagerTest, LoadAcceptsEmptyFileWithoutProducingKeys)
    {
        writeFile("empty.yaml", "");
        writeFile("blank.yml", "   \n\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_TRUE(result.errors.empty());
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_TRUE(configuration().keys().empty());
    }

    TEST_F(ConfigManagerTest, LoadRejectsTopLevelSequenceWithLegacyWording)
    {
        writeFile("list.yaml", "- alpha\n- beta\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "根节点必须是映射，实际为 sequence"));
    }

    TEST_F(ConfigManagerTest, QuotedNumbersStayStringsAfterSwitchToOwnParser)
    {
        writeFile("quoted.yaml", "password: \"8080\"\nratio: \"1.5\"\nplain: 8080\n");

        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().getText("password", ""), "8080");
        EXPECT_EQ(configuration().getText("ratio", ""), "1.5");
        // 值模型不做隐式转换：字符串键取整数只会落回默认值
        EXPECT_EQ(configuration().getInt("password", -1), -1);
        EXPECT_EQ(configuration().getInt("plain", -1), 8080);
    }

    TEST_F(ConfigManagerTest, SaveOverridesRecoversFromCorruptedSettingsFile)
    {
        writeFile(kDeployedConfigFileName, "app:\n  name: dashboard\n");
        writeFile(kSettingsFileName, "{ this is not valid json ");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        // 损坏的覆盖层文件会被点名，但其余配置照常提交
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "解析错误："));
        EXPECT_EQ(configuration().getString("app.name", ""), "dashboard");

        ASSERT_TRUE(configuration().setAndPersist("app.theme", ConfigValue(std::string("dark"))));

        const std::string saved = readFileText(filePath(kSettingsFileName));
        EXPECT_TRUE(textContains(saved, "\"app.theme\": \"dark\"")) << saved;
        EXPECT_FALSE(textContains(saved, "this is not valid json")) << saved;
    }

    TEST_F(ConfigManagerTest, SaveOverridesAbsorbsAndRemovesLegacyUiFile)
    {
        writeFile(kDeployedConfigFileName, "app:\n  name: dashboard\n");
        writeFile("ui.yaml", "app.theme: light\napp.width: 1200\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        ASSERT_TRUE(configuration().setAndPersist("app.width", ConfigValue(std::int64_t(1600))));

        EXPECT_FALSE(std::filesystem::exists(filePath("ui.yaml")));

        const std::string saved = readFileText(filePath(kSettingsFileName));
        EXPECT_TRUE(textContains(saved, "\"app.theme\": \"light\"")) << saved;
        // 同名键以本次修改为准，旧覆盖层值不会回灌
        EXPECT_TRUE(textContains(saved, "\"app.width\": 1600")) << saved;
        EXPECT_FALSE(textContains(saved, "1200")) << saved;
    }

    TEST_F(ConfigManagerTest, LoadFilesCommitsPartialSuccessAndStillReportsFailure)
    {
        const std::vector<std::filesystem::path> files = {
                writeFile("ok.yaml", "committed: true\n"),
                filePath("nowhere.yaml"),
        };

        const ConfigLoadResult result = configuration().loadFiles(files);

        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 1U);
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_TRUE(configuration().getBool("committed", false));
    }

    TEST_F(ConfigManagerTest, LoadFilesRecordsCommonDirectoryAndEnablesReload)
    {
        const std::filesystem::path configFile = writeFile("cfg.yaml", "value: 7\n");

        ASSERT_TRUE(configuration().loadFiles({configFile}).success);

        // 全部文件同处一个目录时，该目录被记为配置目录，reload 不再报“未设置目录”
        EXPECT_EQ(configuration().configDirectory(), directory());
        const ConfigLoadResult reloaded = configuration().reload();
        EXPECT_TRUE(reloaded.success) << reloaded.errors.front();
        EXPECT_EQ(configuration().getInt("value", 0), 7);
    }

    TEST_F(ConfigManagerTest, LoadFilesAcrossDirectoriesKeepsPreviousConfigDirectory)
    {
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_EQ(configuration().configDirectory(), directory());

        const std::vector<std::filesystem::path> files = {
                writeFile("root-level.yaml", "rootKey: 1\n"),
                writeFile("nested/inner.yaml", "nestedKey: 2\n"),
        };

        const ConfigLoadResult result = configuration().loadFiles(files);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        // 跨目录时不猜测公共父目录，保留既有配置目录
        EXPECT_EQ(configuration().configDirectory(), directory());
    }

    TEST_F(ConfigManagerTest, HotReloadCanBeEnabledAfterLoadingExplicitFiles)
    {
        const std::filesystem::path configFile = writeFile("cfg.yaml", "watched: true\n");
        ASSERT_TRUE(configuration().loadFiles({configFile}).success);

        // 此前 loadFiles 不设配置目录，导致热加载只能返回 false；修复后应可启用
        EXPECT_TRUE(configuration().enableHotReload());
        EXPECT_TRUE(configuration().isHotReloadEnabled());

        configuration().disableHotReload();
        EXPECT_FALSE(configuration().isHotReloadEnabled());
        // 重复关闭必须安全
        EXPECT_NO_THROW(configuration().disableHotReload());
    }

    // ============================================================================
    // 配置访问
    // ============================================================================

    TEST_F(ConfigManagerTest, GetThrowsWithKeyWhenKeyIsMissing)
    {
        writeFile("cfg.yaml", "name: test\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_THROW(static_cast<void>(configuration().get("missing.key")), ConfigKeyNotFoundException);

        try
        {
            static_cast<void>(configuration().get("missing.key"));
            FAIL() << "缺失键应当抛出 ConfigKeyNotFoundException";
        } catch (const ConfigKeyNotFoundException &exception)
        {
            EXPECT_EQ(exception.key(), "missing.key");
        }
    }

    TEST_F(ConfigManagerTest, GetReturnsValueCopyForExistingKey)
    {
        writeFile("cfg.yaml", "name: test\ncount: 7\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigValue nameValue = configuration().get("name");
        EXPECT_EQ(nameValue.type(), ConfigValueType::String);
        EXPECT_EQ(nameValue.asString(), "test");
        EXPECT_EQ(configuration().get("count").asInt(), 7);
    }

    TEST_F(ConfigManagerTest, GetOptionalReturnsValueOrNullopt)
    {
        writeFile("cfg.yaml", "name: test\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const std::optional<ConfigValue> present = configuration().getOptional("name");
        ASSERT_TRUE(present.has_value());
        EXPECT_EQ(present->asString(), "test");

        EXPECT_FALSE(configuration().getOptional("nonexistent").has_value());
    }

    TEST_F(ConfigManagerTest, TypedGetThrowsOnTypeMismatch)
    {
        writeFile("cfg.yaml", "name: test\nport: 8080\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_THROW(static_cast<void>(configuration().get<std::string>("port")), ValueAccessError);
        EXPECT_THROW(static_cast<void>(configuration().get<int64_t>("name")), ValueAccessError);
        EXPECT_THROW(static_cast<void>(configuration().get<int64_t>("nonexistent")), ConfigKeyNotFoundException);
        EXPECT_EQ(configuration().get<std::string>("name"), "test");
    }

    TEST_F(ConfigManagerTest, GetWithDefaultReturnsValueOrFallback)
    {
        writeFile("cfg.yaml", "name: test\nport: 8080\nenabled: true\nratio: 1.5\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().get<std::string>("name", "fallback"), "test");
        EXPECT_EQ(configuration().get<int64_t>("port", 0), 8080);
        EXPECT_EQ(configuration().get<bool>("enabled", false), true);
        EXPECT_DOUBLE_EQ(configuration().get<double>("ratio", 0.0), 1.5);

        EXPECT_EQ(configuration().get<std::string>("nonexistent", "fallback"), "fallback");
        EXPECT_EQ(configuration().get<int64_t>("nonexistent", 99), 99);
        EXPECT_EQ(configuration().get<bool>("nonexistent", true), true);
    }

    TEST_F(ConfigManagerTest, GetWithDefaultFallsBackOnTypeMismatch)
    {
        // Int 与 Double、Bool 与 Int 之间不做隐式转换，类型不符即回落默认值
        writeFile("cfg.yaml", "port: 8080\nratio: 1.5\nenabled: true\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_DOUBLE_EQ(configuration().get<double>("port", -1.0), -1.0);
        EXPECT_EQ(configuration().get<int64_t>("ratio", -1), -1);
        EXPECT_EQ(configuration().get<int64_t>("enabled", -1), -1);
        EXPECT_EQ(configuration().get<bool>("port", true), true);
        EXPECT_EQ(configuration().get<std::string>("port", "fallback"), "fallback");
    }

    TEST_F(ConfigManagerTest, GetRequiredReturnsValueOrThrows)
    {
        writeFile("cfg.yaml", "name: test\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().getRequired<std::string>("name"), "test");
        EXPECT_THROW(static_cast<void>(configuration().getRequired<std::string>("nonexistent")), ConfigKeyNotFoundException);
        EXPECT_THROW(static_cast<void>(configuration().getRequired<int64_t>("name")), ValueAccessError);
    }

    TEST_F(ConfigManagerTest, ConvenienceGettersReadLoadedValues)
    {
        writeFile("cfg.yaml", "debug: true\ncount: 42\npi: 3.5\ntitle: Hello World\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().getBool("debug"));
        EXPECT_EQ(configuration().getInt("count"), 42);
        EXPECT_DOUBLE_EQ(configuration().getDouble("pi"), 3.5);
        EXPECT_EQ(configuration().getString("title"), "Hello World");
    }

    TEST_F(ConfigManagerTest, ConvenienceGettersUseDefaultsForMissingKeys)
    {
        writeFile("cfg.yaml", "present: 1\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_FALSE(configuration().getBool("nonexistent", false));
        EXPECT_TRUE(configuration().getBool("nonexistent", true));
        EXPECT_EQ(configuration().getInt("nonexistent", 100), 100);
        EXPECT_DOUBLE_EQ(configuration().getDouble("nonexistent", 1.5), 1.5);
        EXPECT_EQ(configuration().getString("nonexistent", "default"), "default");
        EXPECT_EQ(configuration().getText("nonexistent", "default"), "default");
        // 默认形参：布尔 false、整数 0、浮点 0.0、空串
        EXPECT_FALSE(configuration().getBool("nonexistent"));
        EXPECT_EQ(configuration().getInt("nonexistent"), 0);
        EXPECT_DOUBLE_EQ(configuration().getDouble("nonexistent"), 0.0);
        EXPECT_TRUE(configuration().getString("nonexistent").empty());
    }

    TEST_F(ConfigManagerTest, ConvenienceGettersUseDefaultsOnTypeMismatch)
    {
        writeFile("cfg.yaml", "text: hello\nport: 8080\nflag: true\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().getInt("text", -1), -1);
        EXPECT_EQ(configuration().getBool("port", true), true);
        EXPECT_DOUBLE_EQ(configuration().getDouble("port", -0.5), -0.5);
        EXPECT_EQ(configuration().getString("port", "fallback"), "fallback");
        EXPECT_EQ(configuration().getString("flag", "fallback"), "fallback");
    }

    TEST_F(ConfigManagerTest, GetTextStringifiesScalarValues)
    {
        writeFile("cfg.yaml", "text: hello\nport: 8080\nflag: true\noff: false\nratio: 1.5\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().getText("text", "default"), "hello");
        EXPECT_EQ(configuration().getText("port", "default"), "8080");
        EXPECT_EQ(configuration().getText("flag", "default"), "true");
        EXPECT_EQ(configuration().getText("off", "default"), "false");
        EXPECT_EQ(configuration().getText("ratio", "default"), "1.5");
    }

    TEST_F(ConfigManagerTest, GetTextFallsBackForMissingKeysAndUnstringableTypes)
    {
        writeFile("cfg.yaml", "blank: null\nlist: [1, 2]\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().getText("blank", "default"), "default");
        EXPECT_EQ(configuration().getText("list", "default"), "default");
        EXPECT_EQ(configuration().getText("nonexistent", "default"), "default");
        EXPECT_TRUE(configuration().getText("blank").empty());
    }

    TEST_F(ConfigManagerTest, GetTextIgnoresNestedObjectValues)
    {
        // 展开后数组/对象只保留叶子，此处构造一个真正的对象叶子
        writeFile("cfg.yaml", "extra: {}\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(configuration().getText("extra", "fallback"), "fallback");
    }

    // ============================================================================
    // 查询接口
    // ============================================================================

    TEST_F(ConfigManagerTest, HasReflectsKeyExistence)
    {
        writeFile("cfg.yaml", "server:\n  host: localhost\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().has("server.host"));
        EXPECT_FALSE(configuration().has("server"));
        EXPECT_FALSE(configuration().has("server.nonexistent"));
        EXPECT_FALSE(configuration().has(""));
    }

    TEST_F(ConfigManagerTest, KeysReturnsSortedFlatKeyList)
    {
        writeFile("cfg.yaml", "zulu: 3\nalpha: 1\nserver:\n  port: 80\nmike: 2\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const std::vector<std::string> keys = configuration().keys();

        EXPECT_EQ(keys, (std::vector<std::string>{"alpha", "mike", "server.port", "zulu"}));
        EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    }

    TEST_F(ConfigManagerTest, DumpReturnsDetachedSnapshot)
    {
        writeFile("cfg.yaml", "key: value\nother: 2\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigKeyValueMap snapshot = configuration().dump();

        ASSERT_EQ(snapshot.size(), 2U);
        EXPECT_EQ(snapshot.at("key").asString(), "value");
        EXPECT_EQ(snapshot.at("other").asInt(), 2);
        EXPECT_EQ(snapshot.size(), configuration().keys().size());
    }

    TEST_F(ConfigManagerTest, MutatingDumpedSnapshotDoesNotAffectManager)
    {
        writeFile("cfg.yaml", "key: value\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        ConfigKeyValueMap snapshot = configuration().dump();
        snapshot["injected"]       = ConfigValue(std::string("injected"));
        snapshot["key"]            = ConfigValue(std::string("overwritten"));

        EXPECT_FALSE(configuration().has("injected"));
        EXPECT_EQ(configuration().getString("key", ""), "value");
        EXPECT_EQ(configuration().dump().size(), 1U);
    }

    TEST_F(ConfigManagerTest, LoadedFilesAndConfigDirectoryReflectLastLoad)
    {
        writeFile("a.yaml", "alpha: 1\n");
        writeFile("b.yaml", "beta: 2\n");

        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_EQ(loadedFileNames(), (std::vector<std::string>{"a.yaml", "b.yaml"}));
        EXPECT_EQ(configuration().loadedFiles().size(), 2U);
        EXPECT_EQ(configuration().configDirectory(), directory());
    }

    TEST_F(ConfigManagerTest, ClearRemovesValuesFilesAndDirectory)
    {
        writeFile("cfg.yaml", "key: value\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_TRUE(configuration().has("key"));

        configuration().clear();

        EXPECT_FALSE(configuration().has("key"));
        EXPECT_FALSE(configuration().getOptional("key").has_value());
        EXPECT_TRUE(configuration().keys().empty());
        EXPECT_TRUE(configuration().dump().empty());
        EXPECT_TRUE(configuration().loadedFiles().empty());
        EXPECT_TRUE(configuration().configDirectory().empty());
    }

    TEST_F(ConfigManagerTest, ValidateRequiredListsOnlyMissingKeys)
    {
        writeFile("cfg.yaml", "name: test\nport: 8080\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const std::vector<std::string> missing = configuration().validateRequired({"name", "port", "host", "database.url"});

        EXPECT_EQ(missing, (std::vector<std::string>{"host", "database.url"}));
        EXPECT_TRUE(configuration().validateRequired({"name", "port"}).empty());
        EXPECT_TRUE(configuration().validateRequired({}).empty());
    }

    // ============================================================================
    // reload
    // ============================================================================

    TEST_F(ConfigManagerTest, ReloadWithoutConfigDirectoryFails)
    {
        const ConfigLoadResult result = configuration().reload();

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.loadedFiles.empty());
        EXPECT_TRUE(anyEntryContains(result.errors, "尚未设置配置目录"));
    }

    TEST_F(ConfigManagerTest, ReloadFailsAfterClearResetsDirectory)
    {
        writeFile("cfg.yaml", "key: value\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        configuration().clear();
        const ConfigLoadResult result = configuration().reload();

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "尚未设置配置目录"));
    }

    TEST_F(ConfigManagerTest, ReloadPicksUpNewlyAddedFile)
    {
        writeFile("app.yaml", "key: original\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_EQ(configuration().getString("key", ""), "original");

        writeFile("extra.yaml", "extra: added\n");

        const ConfigLoadResult result = configuration().reload();

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.loadedFiles.size(), 2U);
        EXPECT_EQ(configuration().getString("extra", ""), "added");
    }

    TEST_F(ConfigManagerTest, ReloadPicksUpChangedValue)
    {
        writeFile("app.yaml", "port: 1\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_EQ(configuration().getInt("port", 0), 1);

        writeFile("app.yaml", "port: 2\n");

        EXPECT_TRUE(configuration().reload().success);
        EXPECT_EQ(configuration().getInt("port", 0), 2);
    }

    // ============================================================================
    // setValue 与 settings.json 用户覆盖层
    // ============================================================================

    TEST_F(ConfigManagerTest, SetValueAppliesImmediatelyToSnapshot)
    {
        writeFile("cfg.yaml", "existing: 1\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().setValue("app.name", ConfigValue(std::string("dashboard"))));
        EXPECT_TRUE(configuration().setValue("app.size", ConfigValue(static_cast<std::int64_t>(3))));

        EXPECT_TRUE(configuration().has("app.name"));
        EXPECT_EQ(configuration().getString("app.name", ""), "dashboard");
        EXPECT_EQ(configuration().getInt("app.size", 0), 3);
        // keys() 每次调用返回新的临时容器，必须先固化再取迭代器，
        // 否则 begin()/end() 来自不同 vector，调试版迭代器校验会断言失败
        const std::vector<std::string> loadedKeys = configuration().keys();
        EXPECT_TRUE(std::find(loadedKeys.begin(), loadedKeys.end(), "app.name") != loadedKeys.end());
    }

    TEST_F(ConfigManagerTest, SetValueOverwritesExistingKeyInMemory)
    {
        writeFile("cfg.yaml", "app:\n  theme: light\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_EQ(configuration().getString("app.theme", ""), "light");

        EXPECT_TRUE(configuration().setValue("app.theme", ConfigValue(std::string("dark"))));

        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().keys().size(), 1U);
    }

    TEST_F(ConfigManagerTest, SetValueRejectsEmptyKey)
    {
        EXPECT_FALSE(configuration().setValue("", ConfigValue(std::string("value"))));
        EXPECT_TRUE(configuration().keys().empty());
        EXPECT_FALSE(configuration().has(""));
    }

    TEST_F(ConfigManagerTest, SetAndPersistWritesSettingsJsonIntoConfigDirectory)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().setAndPersist("app.theme", ConfigValue(std::string("dark"))));

        ASSERT_TRUE(settingsFileExists());
        EXPECT_TRUE(textContains(settingsFileText(), "\"app.theme\""));
        EXPECT_TRUE(textContains(settingsFileText(), "dark"));
        EXPECT_TRUE(textContains(settingsFileText(), "\"dark\""));
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
    }

    TEST_F(ConfigManagerTest, SettingsJsonOverrideSurvivesFullReload)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n  language: en\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_TRUE(configuration().setAndPersist("app.theme", ConfigValue(std::string("dark"))));

        configuration().clear();
        const ConfigLoadResult reloaded = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(reloaded.success);
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("app.language", ""), "en");
    }

    TEST_F(ConfigManagerTest, SettingsJsonOverrideSurvivesReload)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_TRUE(configuration().setAndPersist("app.theme", ConfigValue(std::string("dark"))));

        // 内存里先被 setValue 改回另一个值，reload 必须让 settings.json 重新获胜
        ASSERT_TRUE(configuration().setValue("app.theme", ConfigValue(std::string("navy"))));

        EXPECT_TRUE(configuration().reload().success);
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
    }

    TEST_F(ConfigManagerTest, SettingsJsonOverrideAppliesOnExplicitFileListLoad)
    {
        const std::filesystem::path deployedFile = writeFile(kDeployedConfigFileName, "app:\n  theme: light\n  language: en\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_TRUE(configuration().setAndPersist("app.theme", ConfigValue(std::string("dark"))));

        configuration().clear();
        const ConfigLoadResult result = configuration().loadFiles({deployedFile, filePath(kSettingsFileName)});

        EXPECT_TRUE(result.success);
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("app.language", ""), "en");
    }

    TEST_F(ConfigManagerTest, PersistedScalarTypesSurviveRoundTrip)
    {
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().setValue("app.port", ConfigValue(static_cast<std::int64_t>(8080))));
        EXPECT_TRUE(configuration().setValue("app.ratio", ConfigValue(1.5)));
        EXPECT_TRUE(configuration().setValue("app.enabled", ConfigValue(true)));
        EXPECT_TRUE(configuration().setValue("app.name", ConfigValue(std::string("dashboard"))));
        EXPECT_TRUE(configuration().setValue("app.blank", ConfigValue(nullptr)));
        EXPECT_TRUE(configuration().saveOverrides());

        configuration().clear();
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_EQ(configuration().keys().size(), 5U);

        EXPECT_EQ(configuration().getOptional("app.port")->type(), ConfigValueType::Int);
        EXPECT_EQ(configuration().getInt("app.port", 0), 8080);
        EXPECT_EQ(configuration().getOptional("app.ratio")->type(), ConfigValueType::Double);
        EXPECT_DOUBLE_EQ(configuration().getDouble("app.ratio", 0.0), 1.5);
        EXPECT_EQ(configuration().getOptional("app.enabled")->type(), ConfigValueType::Bool);
        EXPECT_TRUE(configuration().getBool("app.enabled", false));
        EXPECT_EQ(configuration().getOptional("app.name")->type(), ConfigValueType::String);
        EXPECT_EQ(configuration().getString("app.name", ""), "dashboard");
        EXPECT_EQ(configuration().getOptional("app.blank")->type(), ConfigValueType::Null);
    }

    TEST_F(ConfigManagerTest, SaveOverridesTwiceKeepsPreviouslyPersistedKeys)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().setAndPersist("app.theme", ConfigValue(std::string("dark"))));
        EXPECT_TRUE(configuration().setAndPersist("ui.language", ConfigValue(std::string("zh"))));

        const std::string persistedText = settingsFileText();
        EXPECT_TRUE(textContains(persistedText, "\"app.theme\""));
        EXPECT_TRUE(textContains(persistedText, "\"ui.language\""));

        configuration().clear();
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("ui.language", ""), "zh");
    }

    TEST_F(ConfigManagerTest, SaveOverridesWithoutPendingChangesCreatesNoFile)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().saveOverrides());
        EXPECT_FALSE(settingsFileExists());
    }

    TEST_F(ConfigManagerTest, SaveOverridesWithoutConfigDirectoryFailsAndKeepsValueInMemory)
    {
        EXPECT_TRUE(configuration().setValue("app.theme", ConfigValue(std::string("dark"))));

        EXPECT_FALSE(configuration().saveOverrides());
        EXPECT_FALSE(configuration().setAndPersist("ui.language", ConfigValue(std::string("zh"))));
        EXPECT_FALSE(settingsFileExists());
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
    }

    TEST_F(ConfigManagerTest, PendingOverrideSurvivesClearAndIsWrittenAfterLoad)
    {
        EXPECT_TRUE(configuration().setValue("late.key", ConfigValue(std::string("late-value"))));

        configuration().clear();
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_TRUE(configuration().saveOverrides());
        EXPECT_TRUE(settingsFileExists());
        EXPECT_TRUE(textContains(settingsFileText(), "late.key"));

        configuration().clear();
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        EXPECT_EQ(configuration().getString("late.key", ""), "late-value");
    }

    TEST_F(ConfigManagerTest, SaveOverridesPersistsNumericLikeQuotedStringAsJsonString)
    {
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        EXPECT_TRUE(configuration().setAndPersist("access.pin", ConfigValue(std::string("12345"))));

        const std::string persistedText = settingsFileText();
        EXPECT_TRUE(textContains(persistedText, "\"12345\"")) << persistedText;

        configuration().clear();
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        EXPECT_EQ(configuration().getOptional("access.pin")->type(), ConfigValueType::String);
        EXPECT_EQ(configuration().getString("access.pin", ""), "12345");
    }

    // ============================================================================
    // schema 校验
    // ============================================================================

    TEST_F(ConfigManagerTest, ValidateSchemaChecksCurrentSnapshot)
    {
        writeFile("cfg.yaml", "port: 8080\nname: server\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {
                ConfigSchemaEntry{"port", ConfigValueType::Int, true, 1.0, 65535.0},
                ConfigSchemaEntry{"name", ConfigValueType::String, true, std::nullopt, std::nullopt},
        };

        const ConfigValidationResult result = configuration().validateSchema(schema);

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }

    TEST_F(ConfigManagerTest, ValidateSchemaReportsMissingAndMismatchedKeys)
    {
        writeFile("cfg.yaml", "port: not-a-number\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {
                ConfigSchemaEntry{"port", ConfigValueType::Int, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"host", ConfigValueType::String, true, std::nullopt, std::nullopt},
        };

        const ConfigValidationResult result = configuration().validateSchema(schema);

        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.errors.size(), 2U);
        EXPECT_TRUE(anyEntryContains(result.errors, "类型不符"));
        EXPECT_TRUE(anyEntryContains(result.errors, "缺少必需配置键"));
    }

    TEST_F(ConfigManagerTest, ValidateSchemaIsStableAcrossRepeatedCalls)
    {
        writeFile("cfg.yaml", "port: 8080\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {ConfigSchemaEntry{"port", ConfigValueType::Int, true, 1.0, 65535.0}};

        EXPECT_TRUE(configuration().validateSchema(schema).valid);
        EXPECT_TRUE(configuration().validateSchema(schema).valid);
    }

    TEST_F(ConfigManagerTest, SetSchemaReportsViolationsOfLoadedSnapshot)
    {
        writeFile("cfg.yaml", "count: not-a-number\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigValidationResult result = configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"count", ConfigValueType::Int, true, std::nullopt, std::nullopt},
        });

        EXPECT_FALSE(result.valid);
        ASSERT_EQ(result.errors.size(), 1U);
        EXPECT_TRUE(textContains(result.errors.front(), "count"));
        EXPECT_TRUE(textContains(result.errors.front(), "类型不符"));
    }

    TEST_F(ConfigManagerTest, SetSchemaWithEmptyListReportsValidSnapshot)
    {
        writeFile("cfg.yaml", "count: 1\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigValidationResult result = configuration().setSchema(ConfigSchema{});

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }

    TEST_F(ConfigManagerTest, RegisteredSchemaDoesNotBlockLoading)
    {
        writeFile("cfg.yaml", "port: 8080\n");
        static_cast<void>(configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"must.exist", ConfigValueType::String, true, std::nullopt, std::nullopt},
        }));

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        // 已注册 schema 只在提交时记录日志，不阻断加载流程
        EXPECT_TRUE(result.success);
        EXPECT_EQ(configuration().getInt("port", 0), 8080);
    }

    TEST_F(ConfigManagerTest, SetValueIsAppliedEvenWhenRegisteredSchemaRejectsItsType)
    {
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {ConfigSchemaEntry{"app.size", ConfigValueType::Int, true, std::nullopt, std::nullopt}};

        // 注册时即对当前（空）快照校验一次：缺失必需键被报告，但不阻断后续写入
        const ConfigValidationResult registration = configuration().setSchema(schema);
        EXPECT_FALSE(registration.valid);
        EXPECT_TRUE(anyEntryContains(registration.errors, "缺少必需配置键"));

        // 现状记录：schema 为建议性约束，setValue 不校验类型，仅由 validateSchema 暴露违规
        EXPECT_TRUE(configuration().setValue("app.size", ConfigValue(std::string("not-a-number"))));
        EXPECT_EQ(configuration().getOptional("app.size")->type(), ConfigValueType::String);

        const ConfigValidationResult result = configuration().validateSchema(schema);
        EXPECT_FALSE(result.valid);
        EXPECT_TRUE(anyEntryContains(result.errors, "类型不符"));
    }

    // ============================================================================
    // 热加载开关与状态机
    // ============================================================================

    TEST_F(ConfigManagerTest, EnableHotReloadWithoutConfigDirectoryFailsAndStaysDisabled)
    {
        EXPECT_FALSE(configuration().isHotReloadEnabled());

        EXPECT_FALSE(configuration().enableHotReload());

        EXPECT_FALSE(configuration().isHotReloadEnabled());
        EXPECT_NO_THROW(configuration().disableHotReload());
    }

    TEST_F(ConfigManagerTest, HotReloadStateFollowsEnableResultAndDisableIsIdempotent)
    {
        writeFile("cfg.yaml", "key: value\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        ASSERT_FALSE(configuration().isHotReloadEnabled());

        const bool enabled = configuration().enableHotReload(
                [](const ConfigLoadResult &)
                {
                },
                std::chrono::milliseconds(500));

        // 平台监听器不可用时允许启用失败，但状态必须与返回值一致
        EXPECT_EQ(configuration().isHotReloadEnabled(), enabled);
        if (enabled)
        {
            EXPECT_TRUE(configuration().enableHotReload());
            EXPECT_TRUE(configuration().isHotReloadEnabled());
        }

        configuration().disableHotReload();
        EXPECT_FALSE(configuration().isHotReloadEnabled());

        EXPECT_NO_THROW(configuration().disableHotReload());
        EXPECT_FALSE(configuration().isHotReloadEnabled());
    }

    TEST_F(ConfigManagerTest, DisableHotReloadAfterFailedEnableIsSafeToRepeat)
    {
        configuration().clear();
        EXPECT_FALSE(configuration().enableHotReload());

        EXPECT_NO_THROW(configuration().disableHotReload());
        EXPECT_NO_THROW(configuration().disableHotReload());
        EXPECT_FALSE(configuration().isHotReloadEnabled());
    }

    // ============================================================================
    // 并发读取
    // ============================================================================

    TEST_F(ConfigManagerTest, ConcurrentReadersObserveConsistentSnapshot)
    {
        writeFile("cfg.yaml", "counter: 42\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        constexpr int kReaderCount         = 4;
        constexpr int kIterationsPerReader = 250;

        std::atomic<int>         mismatches{0};
        std::vector<std::thread> readers;
        readers.reserve(kReaderCount);
        for (int readerIndex = 0; readerIndex < kReaderCount; ++readerIndex)
        {
            readers.emplace_back(
                    [&mismatches]()
                    {
                        for (int iteration = 0; iteration < kIterationsPerReader; ++iteration)
                        {
                            ConfigManager &manager = ConfigManager::instance();
                            if (manager.getInt("counter", -1) != 42)
                            {
                                mismatches.fetch_add(1);
                            }
                            if (!manager.has("counter"))
                            {
                                mismatches.fetch_add(1);
                            }
                            static_cast<void>(manager.keys());
                            static_cast<void>(manager.dump());
                        }
                    });
        }

        for (std::thread &reader: readers)
        {
            reader.join();
        }

        EXPECT_EQ(mismatches.load(), 0);
        EXPECT_EQ(configuration().getInt("counter", -1), 42);
    }
} // namespace AsynGyanis::Base
