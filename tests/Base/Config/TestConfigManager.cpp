/**
 * @file TestConfigManager.cpp
 * @brief ConfigManager 单元测试：目录与文件加载、类型安全访问、同目录按文件名升序覆盖、
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
#include "Base/Exception/ConfigValidationException.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 配置目录里的第二份配置文件；名字排在 config.yaml 之后，因此后装载、覆盖前者
        constexpr const char *kSettingsFileName = "settings.json";

        /// 部署默认配置文件；同目录装载时排在 settings.json 之前，被后者覆盖——分层靠命名表达，库里没有特殊文件名
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
     *          进入与退出都关闭热加载并清空快照；clear() 不会重置已注册 schema，
     *          因此退出时显式注销 schema，防止跨用例状态泄漏。
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

        /**
         * @brief 记录临时目录内的文件清单与各自文本内容（按文件名升序）
         * @details 用于断言「库不写盘」：调用前后取两次快照并整体比较，即可同时覆盖
         *          「新增了文件」与「改写了已有文件」两种情况。
         * @return std::vector<std::pair<std::string, std::string> > 相对路径与文件文本
         */
        [[nodiscard]] std::vector<std::pair<std::string, std::string> > directoryFileSnapshot() const
        {
            std::vector<std::pair<std::string, std::string> > snapshot;

            std::error_code errorCode;
            for (const auto &entry: std::filesystem::recursive_directory_iterator(directory(), errorCode))
            {
                if (errorCode)
                {
                    break;
                }
                if (!entry.is_regular_file(errorCode))
                {
                    continue;
                }
                // 目录本身是绝对路径，直接用词法相对化，不触发文件系统访问
                const std::filesystem::path relative = entry.path().lexically_relative(directory());
                snapshot.emplace_back(relative.generic_string(), readFileText(entry.path()));
            }

            std::ranges::sort(snapshot);
            return snapshot;
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
        EXPECT_EQ(extra->type(), ConfigValueType::object);
        EXPECT_TRUE(extra->empty());
        EXPECT_EQ(extra->size(), 0U);

        const std::optional<ConfigValue> list = configuration().getOptional("list");
        ASSERT_TRUE(list.has_value());
        EXPECT_EQ(list->type(), ConfigValueType::array);
        EXPECT_TRUE(list->empty());

        EXPECT_EQ(configuration().getOptional("nested.blank")->type(), ConfigValueType::object);
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
        EXPECT_EQ(configuration().getOptional("missing")->type(), ConfigValueType::null);

        const std::optional<ConfigValue> list = configuration().getOptional("list");
        ASSERT_TRUE(list.has_value());
        EXPECT_EQ(list->size(), 2U);
        EXPECT_EQ((*list)[0].get<std::string>(), "a");
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryAcceptsJsonWithUtf8Bom)
    {
        // Windows 编辑器常写 BOM，而原生解析器不认它：加载前必须先剥掉
        writeFile("bom.json", std::string("\xEF\xBB\xBF") + "{\"port\": 8080}");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
        EXPECT_EQ(configuration().getInt("port", 0), 8080);
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryKeepsLargeUnsignedIntegerExact)
    {
        // 超出 int64 的取值必须原样保留为无符号整数：截断或退化成浮点都会给出一个错误的数
        writeFile("large.yaml", "big: 18446744073709551615\n");

        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        const std::optional<ConfigValue> big = configuration().getOptional("big");
        ASSERT_TRUE(big.has_value());
        EXPECT_EQ(big->type(), ConfigValueType::number_unsigned);
        EXPECT_EQ(big->get<std::uint64_t>(), std::numeric_limits<std::uint64_t>::max());
        // 取成有符号整数会溢出，必须回落默认值而不是回绕成负数
        EXPECT_EQ(configuration().getInt("big", -1), -1);
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
                  "textYes: yes\n"
                  "textNo: no\n"
                  "textOn: on\n"
                  "textOff: off\n"
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

        // 以显式表驱动逐条断言类型；非负整数与 JSON 侧口径一致地落无符号数
        const std::vector<std::pair<std::string, ConfigValueType> > expectedTypes = {
                {"boolTrue", ConfigValueType::boolean},
                {"boolFalse", ConfigValueType::boolean},
                // YAML 1.2 核心 schema 只认 true/false，yes/no/on/off 一律是字符串
                {"textYes", ConfigValueType::string},
                {"textNo", ConfigValueType::string},
                {"textOn", ConfigValueType::string},
                {"textOff", ConfigValueType::string},
                {"intPositive", ConfigValueType::number_unsigned},
                {"intNegative", ConfigValueType::number_integer},
                {"intZero", ConfigValueType::number_unsigned},
                {"doublePlain", ConfigValueType::number_float},
                {"textPlain", ConfigValueType::string},
                {"textQuotedNumber", ConfigValueType::string},
                {"textDottedVersion", ConfigValueType::string},
                {"textEmptyQuoted", ConfigValueType::string},
                {"nullExplicit", ConfigValueType::null},
                {"nullTilde", ConfigValueType::null},
                {"nullOmitted", ConfigValueType::null},
        };

        for (const auto &[key, expectedType]: expectedTypes)
        {
            const std::optional<ConfigValue> value = configuration().getOptional(key);
            ASSERT_TRUE(value.has_value()) << "key=" << key;
            EXPECT_EQ(value->type(), expectedType) << "key=" << key;
        }

        EXPECT_EQ(configuration().getString("textYes", ""), "yes");
        EXPECT_EQ(configuration().getString("textOn", ""), "on");
        EXPECT_EQ(configuration().getString("textOff", ""), "off");
        EXPECT_EQ(configuration().getString("textNo", ""), "no");
        // 1.1 风格的布尔词在 1.2 下不按布尔解析，取 bool 会落回默认值
        EXPECT_FALSE(configuration().getBool("textYes", false));
        EXPECT_FALSE(configuration().getBool("boolFalse", true));
        EXPECT_EQ(configuration().getInt("intNegative", 0), -9876);
        EXPECT_DOUBLE_EQ(configuration().getDouble("doublePlain", 0.0), 3.5);
        EXPECT_EQ(configuration().getString("textQuotedNumber", ""), "12345");
        EXPECT_EQ(configuration().getString("textDottedVersion", ""), "1.2.3");
        // 空引号标量按空字符串落地。注意原生 empty() 只对 null 与空容器为真（标量一律非空），
        // 空串要按字符串内容判断，不能拿 empty() 当「空串判定」用
        EXPECT_EQ(configuration().getOptional("textEmptyQuoted")->get<std::string>(), "");
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
        // 保留字符 @ 不能作为标量开头：错误必须带上原生库给出的可定位位置（出错的那一行）
        writeFile("bad.yaml", "a: 1\nb: @invalid\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "解析错误：")) << result.errors.front();
        EXPECT_TRUE(anyEntryContains(result.errors, "YAML 语法错误"));
        EXPECT_TRUE(anyEntryContains(result.errors, "第 2 行"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsDuplicateKeys)
    {
        // 库的 DOM 会把重复键的两份都留下：加载必须报错，而不是让后者静默覆盖前者的取值
        writeFile("dup.yaml", "server:\n  port: 1\n  port: 2\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "重复键"));
        EXPECT_FALSE(configuration().has("server.port"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsUnsupportedYamlTags)
    {
        // !!binary 之类的标签无法忠实表达为 JSON 值：明确报错，不静默丢成字符串
        writeFile("tagged.yaml", "payload: !!binary aGVsbG8=\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "不支持的 YAML 标签"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsNonScalarMapKeys)
    {
        writeFile("complex-key.yaml", "{[1, 2]: value}\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "键必须是标量"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsIntegerBeyond64Bit)
    {
        // 越界整数必须报错：静默回绕或退化成浮点都会给出一个看似正常的错误取值
        writeFile("huge.yaml", "big: 123456789012345678901234567890\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "超出 64 位表示范围"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryCutsOffCyclicAliases)
    {
        // 自引用别名：转换必须在上限处停下并报错，而不是无限递归
        writeFile("cycle.yaml", "loop: &loop\n  self: *loop\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "别名"));
    }

    TEST_F(ConfigManagerTest, LoadFromDirectoryCutsOffAliasBombs)
    {
        // 别名炸弹：每层把上一层引用三次，11 层展开后节点数（3^11 量级）已远超预算上限，
        // 转换必须在上限处失败，而不是继续指数级复制
        std::string yamlText = "level0: &level0 [\"x\"]\n";
        for (int level = 1; level <= 11; ++level)
        {
            const std::string previous = "level" + std::to_string(level - 1);
            yamlText += "level" + std::to_string(level) + ": &level" + std::to_string(level) + " [*" + previous + ", *" + previous + ", *" + previous + "]\n";
        }
        writeFile("bomb.yaml", yamlText);

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "节点总数超过上限"));
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

    TEST_F(ConfigManagerTest, LoadsOtherFilesWhenOneOverrideFileIsCorrupted)
    {
        writeFile(kDeployedConfigFileName, "app:\n  name: dashboard\n");
        writeFile(kSettingsFileName, "{ this is not valid json ");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        // 损坏的那份文件被点名进失败列表，但其余配置照常提交：单份文件坏掉不影响整目录装载
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "解析错误："));
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_EQ(configuration().getString("app.name", ""), "dashboard");
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

        // 只加载显式文件、未配置目录时同样可以启用：热加载开关不要求存在配置目录
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
        EXPECT_EQ(nameValue.type(), ConfigValueType::string);
        EXPECT_EQ(nameValue.get<std::string>(), "test");
        EXPECT_EQ(configuration().get("count").get<std::int64_t>(), 7);
    }

    TEST_F(ConfigManagerTest, GetOptionalReturnsValueOrNullopt)
    {
        writeFile("cfg.yaml", "name: test\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const std::optional<ConfigValue> present = configuration().getOptional("name");
        ASSERT_TRUE(present.has_value());
        EXPECT_EQ(present->get<std::string>(), "test");

        EXPECT_FALSE(configuration().getOptional("nonexistent").has_value());
    }

    TEST_F(ConfigManagerTest, TypedGetThrowsOnTypeMismatch)
    {
        writeFile("cfg.yaml", "name: test\nport: 8080\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        EXPECT_THROW(static_cast<void>(configuration().get<std::string>("port")), ConfigValidationException);
        EXPECT_THROW(static_cast<void>(configuration().get<int64_t>("name")), ConfigValidationException);
        EXPECT_THROW(static_cast<void>(configuration().get<int64_t>("nonexistent")), ConfigKeyNotFoundException);
        EXPECT_EQ(configuration().get<std::string>("name"), "test");

        try
        {
            static_cast<void>(configuration().get<std::string>("port"));
            FAIL() << "类型不匹配应当抛出 ConfigValidationException";
        } catch (const ConfigValidationException &exception)
        {
            EXPECT_EQ(exception.key(), "port");
            EXPECT_TRUE(textContains(exception.what(), "期望 string"));
            EXPECT_TRUE(textContains(exception.what(), "实际 uint"));
        }
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
        EXPECT_THROW(static_cast<void>(configuration().getRequired<int64_t>("name")), ConfigValidationException);
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
        EXPECT_EQ(snapshot.at("key").get<std::string>(), "value");
        EXPECT_EQ(snapshot.at("other").get<std::int64_t>(), 2);
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
    // setValue 的内存语义与「同目录按文件名升序覆盖」规则
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

    TEST_F(ConfigManagerTest, SetValueDoesNotWriteAnyFile)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        // 先固化目录内文件清单与内容，setValue 之后再整体比对：库负责读配置，写配置是应用层的事
        const std::vector<std::pair<std::string, std::string> > before = directoryFileSnapshot();
        ASSERT_EQ(before.size(), 1U);

        EXPECT_TRUE(configuration().setValue("app.theme", ConfigValue(std::string("dark"))));
        EXPECT_TRUE(configuration().setValue("app.added", ConfigValue(std::string("memory-only"))));

        // 内存里立即生效，但磁盘上既没有新增文件，也没有内容变化的文件
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("app.added", ""), "memory-only");
        EXPECT_EQ(directoryFileSnapshot(), before);
    }

    TEST_F(ConfigManagerTest, LaterNamedFileOverridesEarlierOneInSameDirectory)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n  language: en\n");
        writeFile(kSettingsFileName, "{\"app\": {\"theme\": \"dark\"}}");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());

        // 优先级的唯一依据：装载顺序就是文件名升序，config.yaml 在前、settings.json 在后
        const std::vector<std::string> loadedFiles = configuration().loadedFiles();
        ASSERT_EQ(loadedFiles.size(), 2U);
        EXPECT_TRUE(std::ranges::is_sorted(loadedFiles));
        EXPECT_EQ(std::filesystem::path(loadedFiles[0]).filename().string(), kDeployedConfigFileName);
        EXPECT_EQ(std::filesystem::path(loadedFiles[1]).filename().string(), kSettingsFileName);

        // 后装载者拿下同名键；它没提到的键保留先装载文件里的取值
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("app.language", ""), "en");
    }

    TEST_F(ConfigManagerTest, FileValuesWinOverInMemorySetValueAfterReload)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n");
        writeFile(kSettingsFileName, "{\"app\": {\"theme\": \"dark\"}}");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        // 内存里先置为第三个值；reload() 重新读文件，文件是唯一真源，取值必须回到 dark
        ASSERT_TRUE(configuration().setValue("app.theme", ConfigValue(std::string("navy"))));
        ASSERT_EQ(configuration().getString("app.theme", ""), "navy");

        EXPECT_TRUE(configuration().reload().success);
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
    }

    TEST_F(ConfigManagerTest, LaterNamedFileWinsAgainAfterClearAndReload)
    {
        writeFile(kDeployedConfigFileName, "app:\n  theme: light\n  language: en\n");
        writeFile(kSettingsFileName, "{\"app\": {\"theme\": \"dark\"}}");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        configuration().clear();
        const ConfigLoadResult reloaded = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(reloaded.success) << (reloaded.errors.empty() ? "" : reloaded.errors.front());
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("app.language", ""), "en");
    }

    TEST_F(ConfigManagerTest, LoadFilesAppliesLaterFileFromTheGivenList)
    {
        const std::filesystem::path deployedFile = writeFile(kDeployedConfigFileName, "app:\n  theme: light\n  language: en\n");
        const std::filesystem::path lateFile     = writeFile(kSettingsFileName, "{\"app\": {\"theme\": \"dark\"}}");

        // 显式文件列表按传入顺序合并，列表中靠后的文件拿下同名键
        const ConfigLoadResult result = configuration().loadFiles({deployedFile, lateFile});

        EXPECT_TRUE(result.success);
        EXPECT_EQ(configuration().getString("app.theme", ""), "dark");
        EXPECT_EQ(configuration().getString("app.language", ""), "en");
    }

    // ============================================================================
    // schema 校验
    // ============================================================================

    TEST_F(ConfigManagerTest, ValidateSchemaChecksCurrentSnapshot)
    {
        writeFile("cfg.yaml", "port: 8080\nname: server\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {
                ConfigSchemaEntry{"port", ConfigValueType::number_integer, true, 1.0, 65535.0},
                ConfigSchemaEntry{"name", ConfigValueType::string, true, std::nullopt, std::nullopt},
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
                ConfigSchemaEntry{"port", ConfigValueType::number_integer, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"host", ConfigValueType::string, true, std::nullopt, std::nullopt},
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

        const ConfigSchema schema = {ConfigSchemaEntry{"port", ConfigValueType::number_integer, true, 1.0, 65535.0}};

        EXPECT_TRUE(configuration().validateSchema(schema).valid);
        EXPECT_TRUE(configuration().validateSchema(schema).valid);
    }

    TEST_F(ConfigManagerTest, SetSchemaReportsViolationsOfLoadedSnapshot)
    {
        writeFile("cfg.yaml", "count: not-a-number\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigValidationResult result = configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"count", ConfigValueType::number_integer, true, std::nullopt, std::nullopt},
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
                ConfigSchemaEntry{"must.exist", ConfigValueType::string, true, std::nullopt, std::nullopt},
        }));

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        // 已注册 schema 只在提交时记录日志，不阻断加载流程
        EXPECT_TRUE(result.success);
        EXPECT_EQ(configuration().getInt("port", 0), 8080);
    }

    TEST_F(ConfigManagerTest, SetValueIsAppliedEvenWhenRegisteredSchemaRejectsItsType)
    {
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {ConfigSchemaEntry{"app.size", ConfigValueType::number_integer, true, std::nullopt, std::nullopt}};

        // 注册时即对当前（空）快照校验一次：缺失必需键被报告，但不阻断后续写入
        const ConfigValidationResult registration = configuration().setSchema(schema);
        EXPECT_FALSE(registration.valid);
        EXPECT_TRUE(anyEntryContains(registration.errors, "缺少必需配置键"));

        // 现状记录：schema 为建议性约束，setValue 不校验类型，仅由 validateSchema 暴露违规
        EXPECT_TRUE(configuration().setValue("app.size", ConfigValue(std::string("not-a-number"))));
        EXPECT_EQ(configuration().getOptional("app.size")->type(), ConfigValueType::string);

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

    /**
     * @brief 钉住：重载进行期间到达的变更不会丢——当前那轮收尾时接力再来一轮
     * @details 重载要读完整份目录，期间到达的变更（尤其是紧接着那次写入）若被直接丢弃，
     *          配置就会停在旧值直到用户下一次改动。用例用「回调里阻塞」把第一轮卡住，
     *          在阻塞期间再写一次文件，释放回调后必须等到第二轮、且那一轮读到的是新值。
     *          没有接力逻辑时第二轮永不到来，等待超时即判失败。
     */
    TEST_F(ConfigManagerTest, ChangeArrivingDuringReloadTriggersATrailingRound)
    {
        writeFile("cfg.yaml", "value: first\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        std::mutex              callbackMutex;
        std::condition_variable callbackCondition;
        int                     callbackCount          = 0;
        bool                    isFirstCallbackEntered = false;
        bool                    shouldReleaseCallback  = false;
        std::vector<std::string> observedValuesInCallbacks;

        const bool enabled = configuration().enableHotReload(
                [&](const ConfigLoadResult &)
                {
                    std::unique_lock lock(callbackMutex);
                    ++callbackCount;
                    observedValuesInCallbacks.push_back(configuration().getString("value"));
                    callbackCondition.notify_all();
                    if (callbackCount == 1)
                    {
                        isFirstCallbackEntered = true;
                        callbackCondition.notify_all();
                        // 卡住第一轮：把「重载进行中」这个窗口撑开，测试在里面写第二次
                        callbackCondition.wait(lock, [&shouldReleaseCallback] { return shouldReleaseCallback; });
                    }
                },
                std::chrono::milliseconds(50));

        if (!enabled)
        {
            GTEST_SKIP() << "本平台的文件监听器不可用，热重载用例跳过";
        }

        // 静置一小段再写：监听线程要先把 ReadDirectoryChangesW/inotify 投出去，
        // 紧跟着 enableHotReload 就写文件会落在武装之前（与其它监听用例同一处置）
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        writeFile("cfg.yaml", "value: trigger-first-round\n");

        {
            std::unique_lock lock(callbackMutex);
            ASSERT_TRUE(callbackCondition.wait_for(lock, std::chrono::seconds(8),
                                                   [&isFirstCallbackEntered] { return isFirstCallbackEntered; }))
                    << "第一次变更没有触发重载回调";
        }

        // 重载正卡在回调里：此刻再写一次，事件只能被记成「之后还要再来一轮」。
        // 先等过防抖窗口——同一路径 50ms 内的重复事件会被监听器按设计抑制（那是防抖，不是丢事件）
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        writeFile("cfg.yaml", "value: second\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 让文件监听把事件送达

        {
            std::unique_lock lock(callbackMutex);
            shouldReleaseCallback = true;
            callbackCondition.notify_all();
            EXPECT_TRUE(callbackCondition.wait_for(lock, std::chrono::seconds(5), [&callbackCount] { return callbackCount >= 2; }))
                    << "重载期间到达的变更被丢掉了：没有接力第二轮";
            const std::string joinedValues = [&observedValuesInCallbacks]
            {
                std::string joined;
                for (const std::string &value: observedValuesInCallbacks)
                {
                    joined += (joined.empty() ? "" : " | ") + value;
                }
                return joined;
            }();
            EXPECT_EQ(observedValuesInCallbacks.back(), "second")
                    << "接力那一轮读到的仍是旧值；每轮回调读到的值依次为：" << joinedValues;
            EXPECT_EQ(configuration().getString("value"), "second") << "最终生效的配置不是最新那一份";
        }

        configuration().disableHotReload();
    }

    /**
     * @brief 钉住：热重载回调抛异常不会把进程带走，后续变更仍能继续触发重载
     * @details 回调跑在重载工作线程上，异常逃出线程函数就是 std::terminate——整进程没了。
     *          实现把任务体整体包在 try/catch 里（连「通知失败结果」那一次也单独兜住，
     *          因为抛的就是那个回调）。没有这层兜底时本用例会让整个测试进程消失
     */
    TEST_F(ConfigManagerTest, ThrowingHotReloadCallbackDoesNotKillTheProcess)
    {
        writeFile("cfg.yaml", "value: first\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        std::atomic<int> callbackCount{0};
        const bool       enabled = configuration().enableHotReload(
                [&callbackCount](const ConfigLoadResult &)
                {
                    callbackCount.fetch_add(1, std::memory_order_relaxed);
                    throw std::runtime_error("回调故意抛异常");
                },
                std::chrono::milliseconds(50));

        if (!enabled)
        {
            GTEST_SKIP() << "本平台的文件监听器不可用，热重载用例跳过";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        writeFile("cfg.yaml", "value: second\n");

        const bool receivedFirstCallback = [&callbackCount]
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (callbackCount.load(std::memory_order_relaxed) > 0)
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }();
        EXPECT_TRUE(receivedFirstCallback) << "抛异常的回调一次都没被调用：重载任务压根没跑";

        // 再改一次：任务机制必须还能继续工作（异常没有把 pending/dirty 卡死）
        const int countBeforeSecondChange = callbackCount.load(std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        writeFile("cfg.yaml", "value: third\n");

        bool receivedAnotherCallback = false;
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (callbackCount.load(std::memory_order_relaxed) > countBeforeSecondChange)
                {
                    receivedAnotherCallback = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        EXPECT_TRUE(receivedAnotherCallback) << "第一次异常之后重载再也没被触发：任务状态被卡死了";

        configuration().disableHotReload();
    }

    // ============================================================================
    // 并发读取
    // ============================================================================

    TEST_F(ConfigManagerTest, ConcurrentWritersKeepEveryKeyTheySet)
    {
        writeFile("cfg.yaml", "counter: 0\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        constexpr int            kwriterCount    = 4;
        constexpr int            kkeysPerWriter  = 25;
        std::vector<std::thread> writers;
        writers.reserve(kwriterCount);

        // 每个写者写自己的一组键：setValue 是「复制快照—改键—发布」事务，
        // 若写者之间不串行化，后发布者会把前一个写者刚写入的键整体覆盖掉
        for (int writerIndex = 0; writerIndex < kwriterCount; ++writerIndex)
        {
            writers.emplace_back([writerIndex]
            {
                for (int inner = 0; inner < kkeysPerWriter; ++inner)
                {
                    const std::string key = "writer" + std::to_string(writerIndex) + ".key" + std::to_string(inner);
                    ConfigManager::instance().setValue(key, ConfigValue(static_cast<std::int64_t>(inner)));
                }
            });
        }
        for (std::thread &writer: writers)
        {
            writer.join();
        }

        for (int writerIndex = 0; writerIndex < kwriterCount; ++writerIndex)
        {
            for (int inner = 0; inner < kkeysPerWriter; ++inner)
            {
                const std::string key = "writer" + std::to_string(writerIndex) + ".key" + std::to_string(inner);
                EXPECT_TRUE(configuration().has(key)) << "并发写入丢失了键 " << key;
            }
        }
        // 初始键也不能被写过程丢掉
        EXPECT_EQ(configuration().getInt("counter", -1), 0);
    }

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

    // ============================================================================
    // 段落还原（扁平点分键 → 嵌套对象）
    // ============================================================================

    TEST_F(ConfigManagerTest, GetSectionRebuildsNestedObjectFromFlatKeys)
    {
        writeFile("server.yaml",
                  "server:\n"
                  "  maximum_connections: 8\n"
                  "  expose_metrics: true\n"
                  "  limits:\n"
                  "    idle_timeout_ms: 1500\n"
                  "    read_timeout_ms: 2000\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigValue section = configuration().getSection("server");
        ASSERT_TRUE(section.is_object());

        // 直接子键与嵌套子对象都要在：还原的正是 flattenValue() 拆掉的那一层结构
        ASSERT_TRUE(section.contains("maximum_connections"));
        EXPECT_EQ(section.at("maximum_connections").get<std::int64_t>(), 8);
        EXPECT_TRUE(section.at("expose_metrics").get<bool>());

        ASSERT_TRUE(section.contains("limits"));
        const ConfigValue &limits = section.at("limits");
        ASSERT_TRUE(limits.is_object());
        EXPECT_EQ(limits.at("idle_timeout_ms").get<std::int64_t>(), 1500);
        EXPECT_EQ(limits.at("read_timeout_ms").get<std::int64_t>(), 2000);

        // 段名本身不是键，取值里不该出现这两层的外壳
        EXPECT_FALSE(section.contains("server"));
        EXPECT_FALSE(section.contains("maximum_connections.limits"));
    }

    TEST_F(ConfigManagerTest, GetSectionReturnsEmptyObjectWhenSectionIsAbsent)
    {
        writeFile("app.yaml", "application:\n  name: demo\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        // 缺段不是错误：消费方（如 HTTP 服务器配置读取器）据此走默认值
        const ConfigValue section = configuration().getSection("server");
        ASSERT_TRUE(section.is_object());
        EXPECT_TRUE(section.empty());
    }

    TEST_F(ConfigManagerTest, GetSectionIgnoresKeysThatMerelyShareTheNamePrefix)
    {
        writeFile("app.yaml",
                  "server_side:\n"
                  "  port: 1\n"
                  "serverSide:\n"
                  "  port: 2\n"
                  "server:\n"
                  "  port: 3\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        // 只有 server.port 属于这一段：前缀比较必须带上分隔符，否则同名前缀的段会被误捞
        const ConfigValue section = configuration().getSection("server");
        ASSERT_TRUE(section.is_object());
        ASSERT_EQ(section.size(), 1U);
        EXPECT_EQ(section.at("port").get<std::int64_t>(), 3);
    }

    TEST_F(ConfigManagerTest, GetSectionReturnsDetachedCopySurvivingLaterReload)
    {
        writeFile("first.yaml", "server:\n  maximum_connections: 8\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);
        const ConfigValue beforeReload = configuration().getSection("server");

        writeFile("second.yaml", "server:\n  maximum_connections: 99\n");
        const std::filesystem::path secondFile = filePath("second.yaml");
        ASSERT_TRUE(configuration().loadFiles({secondFile}).success);

        // 取走的是副本：热重载换代快照后，先前那一份仍是当时的取值
        EXPECT_EQ(beforeReload.at("maximum_connections").get<std::int64_t>(), 8);
        EXPECT_EQ(configuration().getSection("server").at("maximum_connections").get<std::int64_t>(), 99);
    }
} // namespace AsynGyanis::Base
