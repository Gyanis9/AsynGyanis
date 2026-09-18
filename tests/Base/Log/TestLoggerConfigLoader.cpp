// LoggerConfigLoader 单元测试：从 YAML 配置构建日志器与各类 Sink 及容错跳过

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"

#include "Base/Config/ConfigLoadResult.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerConfigLoader.h"
#include "Base/Log/LoggerRegistry.h"
#include "BaseTestSupport.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 临时接管 std::cout 与 std::cerr 缓冲区的 RAII 助手
         *
         * @details ConsoleSink 把 Warn 及以上等级写到 std::cerr、其余写到 std::cout，
         *          因此两个流都要捕获，才能断言「控制台 Sink 是否真的挂上」；
         *          析构时按相反顺序还原缓冲区，避免吞掉 GoogleTest 自己的输出。
         */
        class ConsoleCapture
        {
        public:
            /**
             * @brief 把标准输出与标准错误重定向到内部字符串流
             */
            ConsoleCapture() :
                m_standardOutput(),
                m_standardError(),
                m_originalOutputBuffer(std::cout.rdbuf(m_standardOutput.rdbuf())),
                m_originalErrorBuffer(std::cerr.rdbuf(m_standardError.rdbuf()))
            {
            }

            /**
             * @brief 还原两个标准流的原始缓冲区
             */
            ~ConsoleCapture()
            {
                std::cerr.rdbuf(m_originalErrorBuffer);
                std::cout.rdbuf(m_originalOutputBuffer);
            }

            ConsoleCapture(const ConsoleCapture &) = delete;

            ConsoleCapture &operator=(const ConsoleCapture &) = delete;

            /** @brief 已捕获的文本，为两个流内容的拼接 */
            [[nodiscard]] std::string text() const
            {
                return m_standardOutput.str() + m_standardError.str();
            }

        private:
            std::ostringstream m_standardOutput;       ///< 承接 std::cout 的字符串流
            std::ostringstream m_standardError;        ///< 承接 std::cerr 的字符串流
            std::streambuf *   m_originalOutputBuffer; ///< std::cout 的原始 streambuf
            std::streambuf *   m_originalErrorBuffer;  ///< std::cerr 的原始 streambuf
        };

        /**
         * @brief 读取文本文件全部内容
         * @param filePath 文件路径
         * @return 文件内容，读不到时返回空串
         */
        std::string readTextFile(const std::filesystem::path &filePath)
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                return {};
            }
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        /** @brief 判断文本是否包含子串 */
        bool contains(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /** @brief 统计文件名列表中匹配正则的名称个数 */
        size_t countMatchingNames(const std::vector<std::string> &names, const std::regex &pattern)
        {
            return static_cast<size_t>(std::ranges::count_if(names,
                                                             [&pattern](const std::string &name)
                                                             {
                                                                 return std::regex_match(name, pattern);
                                                             }));
        }

        /** @brief 判断文件名列表中是否有任意名称匹配正则 */
        bool anyNameMatches(const std::vector<std::string> &names, const std::regex &pattern)
        {
            return std::ranges::any_of(names,
                                       [&pattern](const std::string &name)
                                       {
                                           return std::regex_match(name, pattern);
                                       });
        }
    } // namespace

    /**
     * @brief LoggerConfigLoader 测试夹具
     *
     * @details ConfigManager 与 LoggerRegistry 都是进程级单例，因此整个用例期间持有
     *          configTestMutex() 串行执行，并在 SetUp/TearDown 清空两者；
     *          配置文件与日志文件全部落在临时目录内，用例结束随目录一并删除。
     */
    class LoggerConfigLoaderTest : public ::testing::Test
    {
    protected:
        /// 配置文件中使用的日志段前缀
        static constexpr auto kdefaultPrefix = "logging";

        /// 写入临时目录的配置文件名
        static constexpr auto kconfigurationFileName = "logging.yaml";

        /**
         * @brief 构造夹具：先取得配置单例互斥锁，再创建临时目录
         */
        LoggerConfigLoaderTest() :
            m_configLock(configTestMutex()),
            m_temporaryDirectory("LoggerConfigLoader")
        {
        }

        void SetUp() override
        {
            ConfigManager::instance().disableHotReload();
            ConfigManager::instance().clear();
            LoggerRegistry::instance().clear();
        }

        void TearDown() override
        {
            // 先释放持有文件句柄的 Logger，再删除临时目录（Windows 上被打开的文件会让删除失败）。
            // clear() 只把日志器移入退休表以保护在途裸引用，句柄不会随它释放，必须再来一次 purge：
            // 本夹具串行执行、用例体内的引用都已出栈，此刻销毁是安全的
            LoggerRegistry::instance().clear();
            ConfigManager::instance().disableHotReload();
            ConfigManager::instance().clear();
            LoggerRegistry::instance().purgeRetiredLoggers();
        }

        /** @brief 把 YAML 写入临时目录并经 ConfigManager 真实加载（加载失败直接判定失败） */
        void loadConfiguration(const std::string &yamlContent) const
        {
            ASSERT_TRUE(m_temporaryDirectory.writeFile(kconfigurationFileName, yamlContent));

            const ConfigLoadResult result = ConfigManager::instance().loadFiles({configurationPath()});
            ASSERT_TRUE(result.success);
        }

        /** @brief 生成一份仅滚动策略不同的 rolling_file 配置并加载 */
        void loadRollingFileConfiguration(const std::string &policyName, const std::string &baseFilename) const
        {
            loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          base_filename: )" + baseFilename + R"(
          directory: rolling
          policy: )" + policyName + R"(
          max_size_mb: 2
          max_backup: 4
)");
        }

        /** @brief 以临时目录为基准目录执行一次日志配置加载 */
        void applyLogging(const std::string &configurationPrefix = kdefaultPrefix) const
        {
            LoggerConfigLoader::loadFromConfig(configurationPrefix, m_temporaryDirectory.path());
        }

        /** @brief 临时目录内某个相对路径（正斜杠分隔）对应的绝对路径 */
        [[nodiscard]] std::filesystem::path temporaryPath(const std::string &relativePath) const
        {
            return m_temporaryDirectory.path() / relativePath;
        }

        /** @brief 读取临时目录内的文本文件 */
        [[nodiscard]] std::string readTemporaryFile(const std::string &relativePath) const
        {
            return readTextFile(temporaryPath(relativePath));
        }

        /** @brief 临时目录中配置文件的绝对路径 */
        [[nodiscard]] std::filesystem::path configurationPath() const
        {
            return m_temporaryDirectory.path() / kconfigurationFileName;
        }

        /** @brief 收集目录内的普通文件路径（不递归），目录不存在时返回空列表 */
        [[nodiscard]] std::vector<std::filesystem::path> collectRegularFiles(const std::filesystem::path &directory) const
        {
            std::vector<std::filesystem::path> files;
            for (std::error_code iteratorError; const auto &entry: std::filesystem::directory_iterator(directory, iteratorError))
            {
                if (std::error_code statusError; entry.is_regular_file(statusError))
                {
                    files.push_back(entry.path());
                }
            }
            return files;
        }

        /** @brief 递归收集目录内的普通文件路径 */
        [[nodiscard]] std::vector<std::filesystem::path> collectRegularFilesRecursively(const std::filesystem::path &directory) const
        {
            std::vector<std::filesystem::path> files;
            for (std::error_code iteratorError; const auto &entry: std::filesystem::recursive_directory_iterator(directory, iteratorError))
            {
                if (std::error_code statusError; entry.is_regular_file(statusError))
                {
                    files.push_back(entry.path());
                }
            }
            return files;
        }

        /** @brief 递归列出临时目录内的所有普通文件（相对路径，正斜杠分隔，升序） */
        [[nodiscard]] std::vector<std::string> listTemporaryFiles() const
        {
            std::vector<std::string> names;
            for (const auto &file: collectRegularFilesRecursively(m_temporaryDirectory.path()))
            {
                names.push_back(file.lexically_relative(m_temporaryDirectory.path()).generic_string());
            }
            std::ranges::sort(names);
            return names;
        }

        /** @brief 列出目录内的普通文件名（升序） */
        [[nodiscard]] std::vector<std::string> listFilesIn(const std::filesystem::path &directory) const
        {
            std::vector<std::string> names;
            for (const auto &file: collectRegularFiles(directory))
            {
                names.push_back(file.filename().generic_string());
            }
            std::ranges::sort(names);
            return names;
        }

        /** @brief 判断目录内是否有任意文件包含指定文本 */
        [[nodiscard]] bool anyFileContainsIn(const std::filesystem::path &directory, const std::string &needle) const
        {
            for (const auto &file: collectRegularFiles(directory))
            {
                if (contains(readTextFile(file), needle))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief 写入一条日志并刷新，使文件 Sink 的内容立即可读 */
        static void logAndFlush(const std::string &loggerName, const LogLevel level, const std::string &message)
        {
            const Logger &logger = LoggerRegistry::instance().getLogger(loggerName);
            logger.log(level, message);
            logger.flush();
        }

        /// ConfigManager 是全局单例，必须串行化；声明在最前以保证最后析构
        std::lock_guard<std::mutex> m_configLock;

        TestSupport::TemporaryDirectory m_temporaryDirectory;
    };

    // ============================================================================
    // 根日志器与 global_level
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, MissingLoggersSectionCreatesRootLoggerWithConsoleSink)
    {
        loadConfiguration(R"(logging:
  global_level: DEBUG
)");

        applyLogging();

        const Logger &root = LoggerRegistry::instance().getRootLogger();
        EXPECT_EQ(root.getLevel(), LogLevel::Debug);

        std::string captured;
        {
            const ConsoleCapture capture;
            root.log(LogLevel::Info, "default console root");
            captured = capture.text();
        }
        EXPECT_TRUE(contains(captured, "default console root")) << captured;
    }

    /**
     * @brief 只配了具名 logger 时 root 也要就位：框架自身走 root，不能停在「无 sink」的默认状态
     */
    TEST_F(LoggerConfigLoaderTest, RootLoggerKeepsDefaultConsoleSinkWhenOnlyNamedLoggersConfigured)
    {
        loadConfiguration(R"(logging:
  global_level: DEBUG
  loggers:
    app:
      sinks:
        - type: console
          color: false
)");

        LoggerRegistry::instance().clear();
        applyLogging();

        const Logger &root = LoggerRegistry::instance().getRootLogger();
        EXPECT_EQ(root.getLevel(), LogLevel::Debug) << "root 没有跟着 global_level 走";

        std::string captured;
        {
            const ConsoleCapture capture;
            root.log(LogLevel::Info, "root still emits");
            captured = capture.text();
        }
        EXPECT_TRUE(contains(captured, "root still emits"))
                << "root 停在默认状态上：框架自身那些走 root 的日志被静默丢掉了";
    }

    /**
     * @brief max_backup 为负数时钳到 0 并给出诊断：负数转 size_t 会变成「一个都不删」
     */
    TEST_F(LoggerConfigLoaderTest, NegativeMaximumBackupIsClampedWithDiagnostic)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      sinks:
        - type: rolling_file
          base_filename: clamped_backup.log
          directory: rolling
          max_size_mb: 1
          max_backup: -1
)");

        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "max_backup=-1")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "钳制")) << diagnostic;

        // 钳到 0 之后仍应正常写出：非法取值不该让整个 sink 失效
        logAndFlush("root", LogLevel::Info, "clamped_backup_line");
        EXPECT_TRUE(contains(readTemporaryFile("rolling/clamped_backup.log"), "clamped_backup_line"))
                << readTemporaryFile("rolling/clamped_backup.log");
    }

    TEST_F(LoggerConfigLoaderTest, EmptyConfigurationCreatesRootLoggerAtInfoLevel)
    {
        applyLogging();

        const Logger &root = LoggerRegistry::instance().getRootLogger();
        EXPECT_EQ(root.getLevel(), LogLevel::Info);

        std::string captured;
        {
            const ConsoleCapture capture;
            root.log(LogLevel::Warn, "fallback console root");
            captured = capture.text();
        }
        EXPECT_TRUE(contains(captured, "fallback console root")) << captured;
    }

    TEST_F(LoggerConfigLoaderTest, GlobalLevelBecomesDefaultLevelOfConfiguredLogger)
    {
        loadConfiguration(R"(logging:
  global_level: WARN
  loggers:
    root:
      sinks:
        - type: console
          color: false
)");

        applyLogging();

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Warn);
    }

    TEST_F(LoggerConfigLoaderTest, GlobalLevelDefaultsToInfoWhenAbsent)
    {
        loadConfiguration(R"(logging:
  loggers:
    root:
      sinks:
        - type: console
          color: false
)");

        applyLogging();

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Info);
    }

    TEST_F(LoggerConfigLoaderTest, LoggerLevelOverridesGlobalLevel)
    {
        loadConfiguration(R"(logging:
  global_level: DEBUG
  loggers:
    root:
      level: ERROR
      sinks:
        - type: console
          color: false
)");

        applyLogging();

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Error);
    }

    TEST_F(LoggerConfigLoaderTest, InvalidLevelStringFallsBackToInfo)
    {
        loadConfiguration(R"(logging:
  global_level: NOT_A_LEVEL
  loggers:
    root:
      level: ALSO_NOT_A_LEVEL
      sinks:
        - type: console
          color: false
)");

        applyLogging();

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Info);
    }

    TEST_F(LoggerConfigLoaderTest, CustomPrefixSelectsOtherConfigurationSection)
    {
        loadConfiguration(R"(myapp_logging:
  global_level: ERROR
  loggers:
    root:
      level: WARN
      sinks:
        - type: console
          color: false
)");

        applyLogging("myapp_logging");

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Warn);
    }

    TEST_F(LoggerConfigLoaderTest, DefaultPrefixIgnoresOtherConfigurationSection)
    {
        loadConfiguration(R"(myapp_logging:
  global_level: ERROR
  loggers:
    root:
      level: WARN
      sinks:
        - type: console
          color: false
)");

        applyLogging();

        // 没有 logging 段，退回到默认 root：等级取 global_level 的缺省值 INFO
        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Info);
    }

    TEST_F(LoggerConfigLoaderTest, UnknownConfigurationKeysDoNotAffectLoading)
    {
        loadConfiguration(R"(logging:
  global_level: TRACE
  retry_interval_seconds: 30
  observers:
    - name: dashboard
      enabled: true
  loggers:
    root:
      level: DEBUG
      owner: diagnostics-team
      sinks:
        - type: file
          path: unaffected.log
          level: INFO
          flush_interval: never
)");

        applyLogging();

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Debug);

        logAndFlush("root", LogLevel::Info, "unaffected by unknown keys");

        EXPECT_TRUE(contains(readTemporaryFile("unaffected.log"), "unaffected by unknown keys"));
    }

    // ============================================================================
    // 多日志器
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, EachConfiguredLoggerKeepsOwnLevelAndSink)
    {
        loadConfiguration(R"(logging:
  global_level: TRACE
  loggers:
    database:
      level: ERROR
      sinks:
        - type: file
          path: database.log
    network:
      level: DEBUG
      sinks:
        - type: file
          path: network.log
    root:
      level: INFO
      sinks:
        - type: file
          path: root.log
)");

        applyLogging();

        ASSERT_EQ(LoggerRegistry::instance().loggerLevel("database"), LogLevel::Error);
        ASSERT_EQ(LoggerRegistry::instance().loggerLevel("network"), LogLevel::Debug);

        logAndFlush("database", LogLevel::Warn, "database warn dropped");
        logAndFlush("database", LogLevel::Error, "database error written");
        logAndFlush("network", LogLevel::Debug, "network debug written");
        logAndFlush("root", LogLevel::Info, "root info written");

        const std::string databaseContent = readTemporaryFile("database.log");
        EXPECT_FALSE(contains(databaseContent, "database warn dropped")) << databaseContent;
        EXPECT_TRUE(contains(databaseContent, "database error written")) << databaseContent;

        EXPECT_TRUE(contains(readTemporaryFile("network.log"), "network debug written"));

        const std::string rootContent = readTemporaryFile("root.log");
        EXPECT_TRUE(contains(rootContent, "root info written")) << rootContent;
        EXPECT_FALSE(contains(rootContent, "network debug written")) << rootContent;
    }

    TEST_F(LoggerConfigLoaderTest, LoggerWithoutSinksArrayOnlyAppliesLevel)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    headless:
      level: ERROR
)");

        applyLogging();

        EXPECT_EQ(LoggerRegistry::instance().getLogger("headless").getLevel(), LogLevel::Error);

        std::string captured;
        {
            const ConsoleCapture capture;
            logAndFlush("headless", LogLevel::Fatal, "no sink attached");
            captured = capture.text();
        }

        EXPECT_FALSE(contains(captured, "no sink attached")) << captured;

        const std::vector<std::string> expected{kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    // ============================================================================
    // console sink
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, ConsoleSinkWithoutColorWritesPlainText)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: console
          color: false
)");

        applyLogging();

        std::string captured;
        {
            const ConsoleCapture capture;
            LoggerRegistry::instance().getRootLogger().log(LogLevel::Info, "plain console line");
            captured = capture.text();
        }

        EXPECT_TRUE(contains(captured, "plain console line")) << captured;
        EXPECT_EQ(captured.find('\x1b'), std::string::npos) << "关闭彩色后不应出现 ANSI 转义序列：" << captured;
    }

    TEST_F(LoggerConfigLoaderTest, ConsoleSinkWithColorStillWritesMessage)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: console
          color: true
)");

        applyLogging();

        std::string captured;
        {
            const ConsoleCapture capture;
            LoggerRegistry::instance().getRootLogger().log(LogLevel::Info, "colored console line");
            captured = capture.text();
        }

        // 是否真的着色取决于终端能力，这里只断言消息一定可达
        EXPECT_TRUE(contains(captured, "colored console line")) << captured;
    }

    TEST_F(LoggerConfigLoaderTest, ConsoleSinkColorDefaultsToEnabled)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: console
)");

        applyLogging();

        std::string captured;
        {
            const ConsoleCapture capture;
            LoggerRegistry::instance().getRootLogger().log(LogLevel::Info, "color omitted");
            captured = capture.text();
        }
        EXPECT_TRUE(contains(captured, "color omitted")) << captured;
    }

    // ============================================================================
    // file sink
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, FileSinkRelativePathResolvesAgainstBaseDirectory)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: base_relative.log
)");

        applyLogging();

        ASSERT_TRUE(std::filesystem::exists(temporaryPath("base_relative.log")));

        logAndFlush("root", LogLevel::Info, "relative base directory line");

        EXPECT_TRUE(contains(readTemporaryFile("base_relative.log"), "relative base directory line"));
    }

    TEST_F(LoggerConfigLoaderTest, FileSinkCreatesMissingParentDirectories)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: nested/deep/nested_app.log
)");

        applyLogging();

        ASSERT_TRUE(std::filesystem::exists(temporaryPath("nested/deep/nested_app.log")));

        logAndFlush("root", LogLevel::Info, "nested directory line");

        EXPECT_TRUE(contains(readTemporaryFile("nested/deep/nested_app.log"), "nested directory line"));
    }

    TEST_F(LoggerConfigLoaderTest, FileSinkAppendsToExistingFileByDefault)
    {
        ASSERT_TRUE(m_temporaryDirectory.writeFile("append.log", "previous run marker\n"));

        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: append.log
)");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "appended line");

        const std::string content = readTemporaryFile("append.log");
        EXPECT_TRUE(contains(content, "previous run marker")) << content;
        EXPECT_TRUE(contains(content, "appended line")) << content;
    }

    TEST_F(LoggerConfigLoaderTest, FileSinkWithExplicitAppendKeepsExistingFile)
    {
        ASSERT_TRUE(m_temporaryDirectory.writeFile("append_explicit.log", "previous run marker\n"));

        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: append_explicit.log
          truncate: false
)");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "second run line");

        const std::string content = readTemporaryFile("append_explicit.log");
        EXPECT_TRUE(contains(content, "previous run marker")) << content;
        EXPECT_TRUE(contains(content, "second run line")) << content;
    }

    TEST_F(LoggerConfigLoaderTest, FileSinkWithTruncateClearsExistingContent)
    {
        ASSERT_TRUE(m_temporaryDirectory.writeFile("truncated.log", "previous run marker\n"));

        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: truncated.log
          truncate: true
)");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "fresh line");

        const std::string content = readTemporaryFile("truncated.log");
        EXPECT_FALSE(contains(content, "previous run marker")) << content;
        EXPECT_TRUE(contains(content, "fresh line")) << content;
    }

    TEST_F(LoggerConfigLoaderTest, FileSinkLevelFiltersBeforeWritingToFile)
    {
        loadConfiguration(R"(logging:
  global_level: TRACE
  loggers:
    root:
      level: TRACE
      sinks:
        - type: file
          path: sink_level.log
          level: WARN
)");

        applyLogging();

        logAndFlush("root", LogLevel::Info, "below sink level");
        logAndFlush("root", LogLevel::Error, "above sink level");

        const std::string content = readTemporaryFile("sink_level.log");
        EXPECT_FALSE(contains(content, "below sink level")) << content;
        EXPECT_TRUE(contains(content, "above sink level")) << content;
    }

    // ============================================================================
    // rolling_file sink
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkWithSizePolicyUsesPlainBaseFilename)
    {
        loadRollingFileConfiguration("size", "roll.log");

        applyLogging();

        ASSERT_TRUE(std::filesystem::exists(temporaryPath("rolling/roll.log")));

        logAndFlush("root", LogLevel::Info, "rolling size line");

        EXPECT_TRUE(contains(readTemporaryFile("rolling/roll.log"), "rolling size line"));

        const std::vector<std::string> names = listFilesIn(temporaryPath("rolling"));
        EXPECT_EQ(names, std::vector<std::string>{"roll.log"});
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkDirectoryDefaultsToLogsSubdirectory)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          base_filename: default_directory.log
)");

        applyLogging();

        EXPECT_TRUE(std::filesystem::exists(temporaryPath("logs/default_directory.log")));
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkWithDailyPolicyUsesDateSuffix)
    {
        loadRollingFileConfiguration("daily", "daily.log");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "daily policy line");

        const std::regex dailyPattern(R"(^daily\.\d{4}-\d{2}-\d{2}\.log$)");
        EXPECT_TRUE(anyNameMatches(listFilesIn(temporaryPath("rolling")), dailyPattern));
        EXPECT_TRUE(anyFileContainsIn(temporaryPath("rolling"), "daily policy line"));
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkWithHourlyPolicyUsesHourSuffix)
    {
        loadRollingFileConfiguration("hourly", "hourly.log");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "hourly policy line");

        const std::regex hourlyPattern(R"(^hourly\.\d{4}-\d{2}-\d{2}_\d{2}\.log$)");
        EXPECT_TRUE(anyNameMatches(listFilesIn(temporaryPath("rolling")), hourlyPattern));
        EXPECT_TRUE(anyFileContainsIn(temporaryPath("rolling"), "hourly policy line"));
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkWithUnknownPolicyFallsBackToSize)
    {
        loadRollingFileConfiguration("weekly", "fallback.log");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "unknown policy line");

        // 未知策略退回 size：活动文件名即基础文件名，不带时间后缀
        EXPECT_TRUE(std::filesystem::exists(temporaryPath("rolling/fallback.log")));
        EXPECT_TRUE(contains(readTemporaryFile("rolling/fallback.log"), "unknown policy line"));
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkSizeThresholdAndBackupLimitComeFromConfig)
    {
        // 阈值 1 MB、上限 1 份备份：五条 700 KB 日志写入后活动文件长度
        // 0 -> 700KB -> 1.4MB（第 3 条前滚动）-> 700KB -> 1.4MB（第 5 条前滚动），
        // 共两次滚动但只保留一份最新备份，据此同时验证 max_size_mb 与 max_backup
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          base_filename: big.log
          directory: big
          policy: size
          max_size_mb: 1
          max_backup: 1
)");

        applyLogging();

        const std::string oversizedTail(700 * 1024, 'x');
        for (int index = 0; index < 5; ++index)
        {
            logAndFlush("root", LogLevel::Info, "marker" + std::to_string(index) + oversizedTail);
        }

        const std::regex               backupPattern(R"(^big\.\d+\.log$)");
        const std::vector<std::string> names = listFilesIn(temporaryPath("big"));
        EXPECT_EQ(countMatchingNames(names, backupPattern), 1u) << "两次滚动应被 max_backup 压到一份备份";

        const std::string activeContent = readTemporaryFile("big/big.log");
        EXPECT_TRUE(contains(activeContent, "marker4"));
        EXPECT_FALSE(contains(activeContent, "marker2")) << "滚动后活动文件只应留下最新一条，marker2 已进备份";
        EXPECT_FALSE(contains(activeContent, "marker0"));
    }

    // ============================================================================
    // async sink
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, AsyncSinkWrapsFileSinkAndDeliversEvents)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 512
          overflow_policy: block
          wrapped:
            type: file
            path: async_wrapped.log
)");

        applyLogging();

        logAndFlush("root", LogLevel::Info, "async wrapped line");

        const bool delivered = TestSupport::waitForCondition(
                [this]
                {
                    return contains(readTemporaryFile("async_wrapped.log"), "async wrapped line");
                },
                10000);
        EXPECT_TRUE(delivered) << readTemporaryFile("async_wrapped.log");
    }

    TEST_F(LoggerConfigLoaderTest, AsyncSinkAcceptsEveryOverflowPolicy)
    {
        const std::vector<std::string_view> policyNames{
                "block",
                "drop",
                "drop_oldest",
                "not_a_policy",
        };

        for (const std::string_view policyName: policyNames)
        {
            const std::string logName = "async_" + std::string(policyName) + ".log";
            loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 4
          overflow_policy: )" + std::string(policyName) + R"(
          wrapped:
            type: file
            path: )" + logName + R"(
)");

            LoggerRegistry::instance().clear();
            EXPECT_NO_THROW(applyLogging()) << policyName;

            Logger &root = LoggerRegistry::instance().getRootLogger();
            for (int index = 0; index < 200; ++index)
            {
                root.log(LogLevel::Info, "burst " + std::to_string(index));
            }
            EXPECT_NO_THROW(root.flush()) << policyName;

            EXPECT_TRUE(std::filesystem::exists(temporaryPath(logName))) << logName;
        }
    }

    TEST_F(LoggerConfigLoaderTest, AsyncSinkClampsZeroQueueSizeAndReportsIt)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 0
          overflow_policy: block
          wrapped:
            type: file
            path: clamped_queue.log
)");

        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        // 配置边界必须给出可见中文诊断，说明非法取值与钳制结果
        EXPECT_TRUE(contains(diagnostic, "queue_size=0")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "钳制")) << diagnostic;

        // 钳到 1 后 Block 策略仍应完整投递；若仍按 0 处理，第二条日志会永久阻塞在这里
        for (int index = 0; index < 8; ++index)
        {
            logAndFlush("root", LogLevel::Info, "clamped_" + std::to_string(index));
        }

        const bool delivered = TestSupport::waitForCondition(
                [this]
                {
                    return contains(readTemporaryFile("clamped_queue.log"), "clamped_7");
                },
                10000);
        EXPECT_TRUE(delivered) << readTemporaryFile("clamped_queue.log");
    }

    TEST_F(LoggerConfigLoaderTest, AsyncSinkNestedInsideAsyncSinkDeliversEvents)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 64
          wrapped:
            type: async
            queue_size: 64
            wrapped:
              type: file
              path: doubly_async.log
)");

        applyLogging();
        logAndFlush("root", LogLevel::Info, "nested async line");

        // flush() 返回即代表事件已穿过两层异步队列并交给文件 Sink 刷新，首次轮询就应读到
        const bool delivered = TestSupport::waitForCondition(
                [this]
                {
                    return contains(readTemporaryFile("doubly_async.log"), "nested async line");
                },
                10000);

        EXPECT_TRUE(delivered) << readTemporaryFile("doubly_async.log");
    }

    // ============================================================================
    // 容错：缺失字段与未知类型只跳过单个 Sink
    // ============================================================================

    TEST_F(LoggerConfigLoaderTest, SinkWithoutTypeIsSkippedAndRestStillApply)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - path: missing_type.log
          level: INFO
        - type: file
          path: kept_after_missing_type.log
)");

        EXPECT_NO_THROW(applyLogging());

        logAndFlush("root", LogLevel::Info, "kept after missing type");

        EXPECT_FALSE(std::filesystem::exists(temporaryPath("missing_type.log")));
        EXPECT_TRUE(contains(readTemporaryFile("kept_after_missing_type.log"), "kept after missing type"));

        const std::vector<std::string> expected{"kept_after_missing_type.log", kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    TEST_F(LoggerConfigLoaderTest, FileSinkWithoutPathIsSkippedAndRestStillApply)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          truncate: true
        - type: console
          color: false
)");

        EXPECT_NO_THROW(applyLogging());

        std::string captured;
        {
            const ConsoleCapture capture;
            logAndFlush("root", LogLevel::Info, "console survives missing path");
            captured = capture.text();
        }

        EXPECT_TRUE(contains(captured, "console survives missing path")) << captured;

        const std::vector<std::string> expected{kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkWithoutBaseFilenameIsSkipped)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          directory: never_created
          policy: size
)");

        EXPECT_NO_THROW(applyLogging());
        logAndFlush("root", LogLevel::Info, "nothing should be written");

        EXPECT_FALSE(std::filesystem::exists(temporaryPath("never_created")));

        const std::vector<std::string> expected{kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    TEST_F(LoggerConfigLoaderTest, AsyncSinkWithoutWrappedSinkIsSkipped)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 128
          overflow_policy: drop
        - type: file
          path: kept_after_missing_wrapped.log
)");

        EXPECT_NO_THROW(applyLogging());
        logAndFlush("root", LogLevel::Info, "kept after missing wrapped");

        EXPECT_TRUE(contains(readTemporaryFile("kept_after_missing_wrapped.log"), "kept after missing wrapped"));

        const std::vector<std::string> expected{"kept_after_missing_wrapped.log", kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    TEST_F(LoggerConfigLoaderTest, AsyncSinkWithBrokenWrappedSinkIsSkipped)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 128
          wrapped:
            type: file
)");

        EXPECT_NO_THROW(applyLogging());
        logAndFlush("root", LogLevel::Info, "async with broken wrapped");

        const std::vector<std::string> expected{kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    TEST_F(LoggerConfigLoaderTest, UnknownSinkTypeIsSkippedAndRestStillApply)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: syslog
          facility: daemon
        - type: file
          path: kept_after_unknown_type.log
)");

        EXPECT_NO_THROW(applyLogging());
        logAndFlush("root", LogLevel::Info, "kept after unknown type");

        EXPECT_TRUE(contains(readTemporaryFile("kept_after_unknown_type.log"), "kept after unknown type"));

        const std::vector<std::string> expected{"kept_after_unknown_type.log", kconfigurationFileName};
        EXPECT_EQ(listTemporaryFiles(), expected);
    }

    TEST_F(LoggerConfigLoaderTest, SinkEntryThatIsNotAnObjectIsSkipped)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - just_a_scalar
        - type: file
          path: kept_after_scalar.log
)");

        EXPECT_NO_THROW(applyLogging());
        logAndFlush("root", LogLevel::Info, "kept after scalar entry");

        EXPECT_TRUE(contains(readTemporaryFile("kept_after_scalar.log"), "kept after scalar entry"));
    }

    // ============================================================================
    // 重复加载
    // ============================================================================

    /**
     * @brief sinks 字段类型不符（漏写每条的 `-`）时保留原有 sink，而不是把该 logger 清空
     * @details 诊断写的是「已忽略该字段」，行为就得是忽略：清空会让这个 logger 此后静默丢掉
     *          所有日志，而运维只看到一行标准错误
     */
    TEST_F(LoggerConfigLoaderTest, KeepsExistingSinksWhenSinksFieldHasWrongType)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: console
)");
        applyLogging();

        // 第二次：sinks 写成了映射（每条前面的 `-` 漏了）
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        type: console
)");
        applyLogging();

        std::string captured;
        {
            const ConsoleCapture capture;
            logAndFlush("root", LogLevel::Info, "still has its sink");
            captured = capture.text();
        }

        EXPECT_TRUE(contains(captured, "still has its sink"))
                << "sinks 类型不符时说好「已忽略该字段」，实际却把原有 sink 清空了：" << captured;
    }

    TEST_F(LoggerConfigLoaderTest, ReloadingConfigurationReplacesSinksInsteadOfDuplicating)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: ERROR
      sinks:
        - type: file
          path: reload.log
          truncate: true
)");

        applyLogging();
        logAndFlush("root", LogLevel::Error, "first run line");

        // 第二次加载：先 clearSinks（旧 FileSink 析构并落盘），再以 truncate 打开同一文件
        applyLogging();
        logAndFlush("root", LogLevel::Error, "second run line");

        const std::string content = readTemporaryFile("reload.log");
        EXPECT_FALSE(contains(content, "first run line")) << content;
        EXPECT_TRUE(contains(content, "second run line")) << content;
    }
} // namespace AsynGyanis::Base
