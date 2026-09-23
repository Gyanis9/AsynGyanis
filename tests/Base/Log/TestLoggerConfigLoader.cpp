// LoggerConfigLoader 单元测试：从 YAML 配置构建日志器与各类 Sink 及容错跳过

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"

#include "Base/Config/ConfigLoadResult.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerConfigLoader.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/RollingFileSink.h"
#include "BaseTestSupport.h"
#include "Platform/FileSystem/FileSystem.h"

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

    /**
     * @brief max_backup 超出上限时钳到上限并给出诊断
     * @details 与下界那条对称。这个值决定每次滚动要顺移多少个序号（逐个查存在性），
     *          天文数字等于让滚动握着本 Sink 的锁做上千万次目录项查询，日志系统反过来拖垮进程。
     *          判据取 RollingFileSink 里的同一个常量，避免诊断文案与生效值各说一遍
     */
    TEST_F(LoggerConfigLoaderTest, OversizedMaximumBackupIsClampedWithDiagnostic)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      sinks:
        - type: rolling_file
          base_filename: capped_backup.log
          directory: rolling
          max_size_mb: 1
          max_backup: 999999999999
)");

        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "max_backup=999999999999")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "已钳制为 " + std::to_string(RollingFileSink::kMaximumBackupFileCount)))
                << diagnostic;

        // 钳到上限之后仍应正常写出：非法取值不该让整个 sink 失效
        logAndFlush("root", LogLevel::Info, "capped_backup_line");
        EXPECT_TRUE(contains(readTemporaryFile("rolling/capped_backup.log"), "capped_backup_line"))
                << readTemporaryFile("rolling/capped_backup.log");
    }

    /**
     * @brief max_size_mb 大到换算成字节会回绕时，必须钳住而不是退化成「每写一行就滚一次」
     * @details 2^44 MiB 乘 1024*1024 在 size_t 里正好回绕成 0，于是「已写字节 >= 阈值」恒真，
     *          症状与填 0 一模一样，而配置里报出来的数却大得离谱——这条挡的是另一半
     */
    TEST_F(LoggerConfigLoaderTest, MaximumSizeThatWouldWrapIsClampedInsteadOfRollingEveryLine)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      sinks:
        - type: rolling_file
          base_filename: capped_size.log
          directory: rolling
          max_size_mb: 17592186044416
          max_backup: 5
)");

        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }
        EXPECT_TRUE(contains(diagnostic, "max_size_mb=17592186044416")) << diagnostic;

        logAndFlush("root", LogLevel::Info, "capped_size_line_one");
        logAndFlush("root", LogLevel::Info, "capped_size_line_two");
        logAndFlush("root", LogLevel::Info, "capped_size_line_three");

        const std::string activeContent = readTemporaryFile("rolling/capped_size.log");
        EXPECT_TRUE(contains(activeContent, "capped_size_line_one")) << activeContent;
        EXPECT_TRUE(contains(activeContent, "capped_size_line_three")) << activeContent;
        EXPECT_EQ(countMatchingNames(listFilesIn(temporaryPath("rolling")), std::regex(R"(capped_size\.\d+\.log)")), 0u)
                << "阈值被换算回绕成了 0：每写一行就滚动一次";
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

    /**
     * @brief 等级名的小写写法在三处配置点上都按同一等级生效
     * @details 钉住一次契约变更（旧语义：等级名大小写敏感，"error" 判成不认识并回落为 INFO）。
     *          这份 YAML 其余取值全是小写（file/console/size/drop_oldest），所以小写等级是最自然的
     *          写法；旧实现下「只留错误日志」的配置实际在打全量 INFO——多出来的不只是噪音，还可能
     *          把正文里不该外泄的字段写进日志文件。三处配置点各配一条独立判据，回落发生在哪一处
     *          就红在哪一处
     */
    TEST_F(LoggerConfigLoaderTest, LowercaseLevelNamesApplyAtEveryConfigurationPoint)
    {
        loadConfiguration(R"(logging:
  global_level: trace
  loggers:
    root:
      level: error
      sinks:
        - type: file
          path: lowercase_root.log
    app:
      sinks:
        - type: file
          path: lowercase_sink.log
          level: warn
)");

        applyLogging();

        // global_level：app 没有写 level，因此它的等级只能来自这份小写的全局值
        EXPECT_EQ(LoggerRegistry::instance().getLogger("app").getLevel(), LogLevel::Trace);

        // logger 的 level：回落成 INFO 时这条 WARN 会落地
        logAndFlush("root", LogLevel::Warn, "lowercase root warn line");
        logAndFlush("root", LogLevel::Error, "lowercase root error line");
        const std::string rootContent = readTemporaryFile("lowercase_root.log");
        EXPECT_FALSE(contains(rootContent, "lowercase root warn line")) << rootContent;
        EXPECT_TRUE(contains(rootContent, "lowercase root error line")) << rootContent;

        // sink 的 level：回落成 INFO 时这条 INFO 会落地
        logAndFlush("app", LogLevel::Info, "lowercase sink info line");
        logAndFlush("app", LogLevel::Warn, "lowercase sink warn line");
        const std::string sinkContent = readTemporaryFile("lowercase_sink.log");
        EXPECT_FALSE(contains(sinkContent, "lowercase sink info line")) << sinkContent;
        EXPECT_TRUE(contains(sinkContent, "lowercase sink warn line")) << sinkContent;
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

    // ---- 文件名文本的编码口径：file 与 rolling_file 成对，两处都是「UTF-8 配置文本 -> 原生刻度」 ----
    // POSIX 上窄串与原生刻度逐字节相同，因此这两条只有 Windows 侧能证伪（那里窄串要过本地代码页，
    // 代码页外的字符会变成 '?'，日志就写到另一个名字上，而进程一切正常、无人报警）

    TEST_F(LoggerConfigLoaderTest, FileSinkKeepsPathTextOutsideLocalCodePage)
    {
        static constexpr std::string_view kPathTextUtf8 = "日志-🐳.log";

        loadConfiguration(std::string{"logging:\n"
                                      "  global_level: INFO\n"
                                      "  loggers:\n"
                                      "    root:\n"
                                      "      level: INFO\n"
                                      "      sinks:\n"
                                      "        - type: file\n"
                                      "          path: "}
                          + std::string{kPathTextUtf8} + "\n");

        applyLogging();

        const std::filesystem::path expectedPath =
                m_temporaryDirectory.path() / AsynGyanis::Platform::FileSystem::pathFromUtf8(std::string{kPathTextUtf8});
        ASSERT_TRUE(std::filesystem::exists(expectedPath));

        logAndFlush("root", LogLevel::Info, "file sink code page line");

        EXPECT_TRUE(contains(readTextFile(expectedPath), "file sink code page line"));
    }

    TEST_F(LoggerConfigLoaderTest, RollingFileSinkKeepsBaseFilenameOutsideLocalCodePage)
    {
        static constexpr std::string_view kBaseFilenameUtf8 = "应用-🐳.log";

        loadConfiguration(std::string{"logging:\n"
                                      "  global_level: INFO\n"
                                      "  loggers:\n"
                                      "    root:\n"
                                      "      level: INFO\n"
                                      "      sinks:\n"
                                      "        - type: rolling_file\n"
                                      "          base_filename: "}
                          + std::string{kBaseFilenameUtf8} + R"(
          directory: rolling
          policy: size
          max_size_mb: 2
          max_backup: 3
)");

        applyLogging();

        // 目录侧已经是 path 刻度（这一半早就修好了），落空的只会是文件名那一段
        const std::filesystem::path expectedPath =
                temporaryPath("rolling") / AsynGyanis::Platform::FileSystem::pathFromUtf8(std::string{kBaseFilenameUtf8});
        ASSERT_TRUE(std::filesystem::exists(expectedPath));

        logAndFlush("root", LogLevel::Info, "rolling sink code page line");

        EXPECT_TRUE(contains(readTextFile(expectedPath), "rolling sink code page line"));
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

    /**
     * @brief queue_size 填成天文数字时钳到上限并给出诊断，而不是照单建队列
     * @details 一条事件在队列里占 sizeof(LogEvent) 字节，容量乘过去就是下游卡住时最多占住的内存：
     *          「100000 多打几个 0」会把本该按策略丢弃/阻塞的背压变成 OOM。钳的是上限、不是拒绝，
     *          因此钳后仍要正常投递（下面写 8 条并等最后一条落地）。
     */
    TEST_F(LoggerConfigLoaderTest, AsyncSinkClampsOversizedQueueSizeAndReportsIt)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 999999999999
          overflow_policy: block
          wrapped:
            type: file
            path: oversized_queue.log
)");

        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "queue_size=999999999999")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "已钳制为 " + std::to_string(AsyncSink::kMaximumQueueSize))) << diagnostic;

        for (int index = 0; index < 8; ++index)
        {
            logAndFlush("root", LogLevel::Info, "oversized_" + std::to_string(index));
        }

        const bool delivered = TestSupport::waitForCondition(
                [this]
                {
                    return contains(readTemporaryFile("oversized_queue.log"), "oversized_7");
                },
                10000);
        EXPECT_TRUE(delivered) << readTemporaryFile("oversized_queue.log");
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

        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            EXPECT_NO_THROW(applyLogging());
            diagnostic = capture.text();
        }
        logAndFlush("root", LogLevel::Info, "kept after scalar entry");

        EXPECT_TRUE(contains(readTemporaryFile("kept_after_scalar.log"), "kept after scalar entry"));

        // 跳过可以，但不能无声：这份配置「写了两条 sink、实际只挂上一条」必须被说出口
        EXPECT_TRUE(contains(diagnostic, "sink 配置不是对象")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "已跳过该 sink")) << diagnostic;
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

    /**
     * @brief 重复装配同一份配置不换掉日志器实例，退休表因此不随重载次数增长
     * @details 重新装配走的是「取出现有实例 + 换 sink」这条路。若哪天改成用 registerLogger
     *          覆盖，每次热重载都会往退休表留下一份日志器外壳（实测 sizeof(Logger) 80 字节
     *          加共享计数块），而退休表按约定只能由调用方确认无在途裸引用时才清。
     */
    TEST_F(LoggerConfigLoaderTest, ReloadingConfigurationKeepsTheSameLoggerInstances)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    worker:
      level: INFO
      sinks:
        - type: file
          path: identity_worker.log
    app:
      level: INFO
      sinks:
        - type: file
          path: identity_app.log
)");
        applyLogging();

        Logger &workerFirst = LoggerRegistry::instance().getLogger("worker");
        Logger &appFirst    = LoggerRegistry::instance().getLogger("app");
        Logger &rootFirst   = LoggerRegistry::instance().getRootLogger();

        for (int round = 0; round < 3; ++round)
        {
            applyLogging();
        }

        EXPECT_EQ(&LoggerRegistry::instance().getLogger("worker"), &workerFirst) << "重复装配换掉了具名日志器实例";
        EXPECT_EQ(&LoggerRegistry::instance().getLogger("app"), &appFirst);
        EXPECT_EQ(&LoggerRegistry::instance().getRootLogger(), &rootFirst) << "重复装配换掉了 root 实例";
    }

    /**
     * @brief 端到端：改文件 → reload() → 重新装配 → 日志换到新文件、旧文件不再增长
     * @details 三段各自都有单测（热重载把新值装进快照、提交路径执行 schema 校验、装配按新值换
     *          sink），但没有任何一条把「磁盘上的改动」一路传到「落在文件里的日志」。断了任何一段
     *          的现场都是「改了配置没生效」，而那正是各测一段时看不见的形状。
     * @note 用显式 reload() 而不是等文件监视器：等事件会把断言交给调度运气
     */
    TEST_F(LoggerConfigLoaderTest, ReloadFromDiskThenReapplyMovesLogsToTheNewFile)
    {
        // 只往磁盘写、不加载：让「新值进快照」这一步只能由 loadFromDirectory/reload 完成，
        // 否则夹具的 loadConfiguration 会顺手把它读进来，reload() 就成了走过场
        const auto writeYaml = [this](const std::string &logFileName)
        { return m_temporaryDirectory.writeFile(kconfigurationFileName, R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: )" + logFileName + R"()");
        };

        ASSERT_TRUE(writeYaml("chain_stage_one.log"));
        ASSERT_TRUE(ConfigManager::instance().loadFromDirectory(m_temporaryDirectory.path()).success);
        applyLogging();
        logAndFlush("root", LogLevel::Info, "line before the change");

        ASSERT_TRUE(contains(readTemporaryFile("chain_stage_one.log"), "line before the change"));

        // 改的是同一份文件：sink 指向换到第二个文件，磁盘之外什么都不做
        ASSERT_TRUE(writeYaml("chain_stage_two.log"));
        const ConfigLoadResult reloaded = ConfigManager::instance().reload();
        ASSERT_TRUE(reloaded.success) << (reloaded.errors.empty() ? std::string{} : reloaded.errors.front());
        // 中间那道检查别省：先确认「新值确实进了快照」，末端红时才分得清是配置没到还是装配没接
        const auto sinksSnapshot = ConfigManager::instance().getOptional("logging.loggers.root.sinks");
        ASSERT_TRUE(sinksSnapshot.has_value()) << "reload() 之后快照里连 sinks 键都没有";
        EXPECT_NE(sinksSnapshot->dump().find("chain_stage_two.log"), std::string::npos)
                << "reload() 之后快照里还是旧 sink 配置";

        applyLogging();
        logAndFlush("root", LogLevel::Info, "line after the change");

        EXPECT_TRUE(contains(readTemporaryFile("chain_stage_two.log"), "line after the change"));
        EXPECT_FALSE(contains(readTemporaryFile("chain_stage_one.log"), "line after the change"))
                << "旧 sink 没被换掉：装配没有按新快照清过 sink";
    }

    /**
     * @brief loggers 段不是对象时报诊断，并且一个日志器都不动
     * @details 装配被拒绝时最坏的走法是把已有 sink 清空：那之后该日志器的日志全丢，而现场只有一行
     *          标准错误。这里同时钉两点——报得出形态、原有 sink 还活着。
     */
    TEST_F(LoggerConfigLoaderTest, LoggersSectionThatIsNotAnObjectLeavesExistingSinksAlone)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: kept_when_section_bad.log
)");
        applyLogging();
        logAndFlush("root", LogLevel::Info, "applied while well-formed");
        ASSERT_TRUE(contains(readTemporaryFile("kept_when_section_bad.log"), "applied while well-formed"));

        // 同一份文件把 loggers 写成了一个标量（少写了下面那层结构）
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers: root
)");
        std::string diagnostic;
        {
            const ConsoleCapture capture;
            EXPECT_NO_THROW(applyLogging());
            diagnostic = capture.text();
        }
        EXPECT_TRUE(contains(diagnostic, ".loggers 不是对象")) << diagnostic;

        logAndFlush("root", LogLevel::Info, "still writing after a rejected section");
        EXPECT_TRUE(contains(readTemporaryFile("kept_when_section_bad.log"), "still writing after a rejected section"));
    }

    /**
     * @brief 单个日志器的配置写成标量时报诊断，并且不动这个日志器
     * @details 'loggers: {app: "on"}' 这种手写原先两头不着：字段读不出来因而无处报错，而取 logger
     *          那一步会顺手把级别改成 global_level——一行写错的字段换来一次没人报告的级别变更。
     *          形态判据因此排在取 logger 之前。级别是否被改写只看「低于原级别的那条不落盘」，
     *          这一侧才分得开 ERROR 与被改成的 INFO。
     */
    TEST_F(LoggerConfigLoaderTest, LoggerEntryThatIsNotAnObjectIsReportedAndLeftUntouched)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
    app:
      level: ERROR
      sinks:
        - type: file
          path: kept_when_entry_bad.log
)");
        applyLogging();
        logAndFlush("app", LogLevel::Error, "kept before the bad entry");
        ASSERT_TRUE(contains(readTemporaryFile("kept_when_entry_bad.log"), "kept before the bad entry"));

        // 同一份配置把 app 那一段写成了一个标量（root 仍是合法对象）
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
    app: "on"
)");
        std::string diagnostic;
        {
            const ConsoleCapture capture;
            EXPECT_NO_THROW(applyLogging());
            diagnostic = capture.text();
        }
        EXPECT_TRUE(contains(diagnostic, "日志器 'app' 的配置不是对象")) << diagnostic;

        logAndFlush("app", LogLevel::Error, "kept after the bad entry");
        EXPECT_TRUE(contains(readTemporaryFile("kept_when_entry_bad.log"), "kept after the bad entry"));
        logAndFlush("app", LogLevel::Info, "info must stay filtered");
        EXPECT_FALSE(contains(readTemporaryFile("kept_when_entry_bad.log"), "info must stay filtered"))
                << "级别被 global_level 改写了：一份读不出字段的配置不该动这个日志器";
    }

    /**
     * @brief 用 setValue 把整个 loggers 段改写成标量时，装配报出来、不抛也不动日志器
     * @details 这一段先由配置文件摊成 logging.loggers.root.* 那些键，再用 setValue 写一个同名标量：
     *          定形之后段里那些键一起让位，因此装配看到的是「loggers 是个标量」而不是「既是值又是表」
     *          ——两条通道合起来才让这个形状根本不出现。装配路径的既有口径是「不让异常逃出配置加载」，
     *          所以这里要就地报出并把已有的 sink 原样留着。
     */
    TEST_F(LoggerConfigLoaderTest, ScalarOverrideOfTheLoggersSectionIsRejectedWithoutThrowing)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: kept_when_section_contradicts.log
)");
        applyLogging();
        logAndFlush("root", LogLevel::Info, "applied while consistent");
        ASSERT_TRUE(contains(readTemporaryFile("kept_when_section_contradicts.log"), "applied while consistent"));

        // logging.loggers 被写成标量，而 logging.loggers.root.level 已经带着它作为第一段
        ASSERT_TRUE(ConfigManager::instance().setValue("logging.loggers", ConfigValue(std::string("scalar_and_also_a_prefix"))));

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            EXPECT_NO_THROW(applyLogging());
            diagnostic = capture.text();
        }
        EXPECT_TRUE(contains(diagnostic, "本次不改动任何日志器")) << diagnostic;

        logAndFlush("root", LogLevel::Info, "still writing after a contradictory section");
        EXPECT_TRUE(contains(readTemporaryFile("kept_when_section_contradicts.log"), "still writing after a contradictory section"));
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

    /**
     * @brief 可选字段类型不符时必须报出「实际类型 vs 期望类型」，不能静默按默认值生效
     * @details YAML 里给数字加了引号（queue_size: "8192"）是本仓最常见的配错方式：原先
     *          `.value_or(默认值)` 会让 8192 静默变成 1024，配置与生效值长期不一致而无人知道，
     *          而同一个文件对 max_size_mb、overflow_policy 的非法取值都会报。
     *          这条钉的是「容错必须可见」：值仍回落默认、sink 仍要建成，但必须说清原因。
     */
    TEST_F(LoggerConfigLoaderTest, WrongTypedOptionalFieldDiagnosesInsteadOfSilentDefault)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: async
          queue_size: "8192"
          wrapped:
            type: file
            path: typed_queue.log
            truncate: true
)");
        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "queue_size 类型是 string")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "要求 int")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "1024")) << diagnostic;

        // 类型不符只影响这一项取值，不能顺手把整条 sink 判废
        logAndFlush("root", LogLevel::Info, "typed_queue_line");
        EXPECT_TRUE(contains(readTemporaryFile("typed_queue.log"), "typed_queue_line"))
                << readTemporaryFile("typed_queue.log");
    }

    /**
     * @brief 非法 policy 要报出非法值与支持的取值，并说明按 size 处理
     * @details 与 overflow_policy 那条同口径：回退本身可以，「想写 daily 却拼错」不能没人说。
     *          行为半边（按 size 滚动、活动文件就是 base_filename）改动前后都成立，
     *          新钉住的是诊断半边——把 else 分支改回静默赋值本用例即红。
     */
    TEST_F(LoggerConfigLoaderTest, UnknownRollingPolicyIsDiagnosedAndFallsBackToSize)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: rolling_file
          base_filename: typed_policy.log
          directory: rolling
          policy: dailyy
          max_size_mb: 1
)");
        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "policy='dailyy'")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "size / daily / hourly")) << diagnostic;

        logAndFlush("root", LogLevel::Info, "typed_policy_line");
        EXPECT_TRUE(contains(readTemporaryFile("rolling/typed_policy.log"), "typed_policy_line"))
                << readTemporaryFile("rolling/typed_policy.log");
    }

    /**
     * @brief 必填键「存在但类型不符」不能报成「缺少字段」
     * @details 原先 type / path / base_filename 三条都走同一个 optional 判空，`type: 42`
     *          这样的配置会被报成「缺少 'type' 字段」——字段明明在，运维照着提示补键反而补不对。
     */
    TEST_F(LoggerConfigLoaderTest, WrongTypedRequiredKeyReportsActualTypeNotMissing)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - type: 42
)");
        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "'type' 类型是 uint")) << diagnostic;
        EXPECT_FALSE(contains(diagnostic, "缺少 'type'"))
                << "键在而类型不符，报成缺少会把人引向错误的修法：" << diagnostic;
    }

    /**
     * @brief 必填键真的缺失时仍要说「缺少」：两种失败共用一条文案就都说不准
     */
    TEST_F(LoggerConfigLoaderTest, MissingRequiredKeyStillReportsAsMissing)
    {
        loadConfiguration(R"(logging:
  global_level: INFO
  loggers:
    root:
      level: TRACE
      sinks:
        - path: without_type.log
)");
        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "缺少 'type'")) << diagnostic;
        EXPECT_FALSE(contains(diagnostic, "'type' 类型是")) << diagnostic;
    }

    /**
     * @brief global_level 类型不符也要报出来：它决定所有 logger 的缺省等级
     * @details 原先经 `get<std::string>(key, "INFO")` 取值，非字符串的取值与「没配」走同一条
     *          回落路径，一声不响——「明明配了等级却没生效」是这类配置里最难查的现场。
     */
    TEST_F(LoggerConfigLoaderTest, WrongTypedGlobalLevelDiagnosesAndFallsBackToInfo)
    {
        loadConfiguration(R"(logging:
  global_level: 42
  loggers:
    root:
      sinks:
        - type: console
)");
        LoggerRegistry::instance().clear();

        std::string diagnostic;
        {
            const ConsoleCapture capture;
            applyLogging();
            logAndFlush("root", LogLevel::Info, "global_level_fallback_line");
            diagnostic = capture.text();
        }

        EXPECT_TRUE(contains(diagnostic, "global_level 类型是 uint")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "已按 INFO 处理")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "global_level_fallback_line")) << diagnostic;
    }
} // namespace AsynGyanis::Base
