// ConfigManager 单元测试：目录与文件加载、类型安全访问、同目录按文件名升序覆盖、
// schema 校验、热加载开关状态机与并发读取。
// 文件监听器本身的行为由 tests/Platform/FileSystem/TestFileWatcher.cpp 覆盖；本文件中
// 「接力」「回调抛异常」两条用例依赖真实监听事件，平台监听器不可用时用例跳过。
// 另有一条用例把 root 日志器的写入当闸口用（校验错误日志与写锁的先后关系），收尾时整份换掉 root。

#include "Base/Config/ConfigManager.h"

#include "TestHelpers.h"
#include "BaseTestSupport.h"
#include "Base/Config/ConfigLoadResult.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Platform/FileSystem/FileSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
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

        /**
         * @brief 提交路径落日志时的闸口：把「校验正在写日志」变成用例可等待、可放行的状态
         * @details 报到时只拦第一次写入（一条违规对应一条日志），放行之后的写入直接过去。
         *          两类谓词共用一条 CV：两处唤醒一律用 notify_all，配合各自的谓词重查不会吞唤醒。
         */
        class LogWriteGate
        {
        public:
            /// Sink 侧：报到后停在这里，直到用例放行
            void enter()
            {
                std::unique_lock lock(m_mutex);
                m_isEntered = true;
                m_condition.notify_all();
                m_condition.wait(lock, [this] { return m_isReleased; });
            }

            /**
             * @brief 用例侧：等到确实有日志落进闸口
             * @param timeout 最长等待时间
             * @return true 闸口已被进入
             */
            [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds &timeout)
            {
                std::unique_lock lock(m_mutex);
                return m_condition.wait_for(lock, timeout, [this] { return m_isEntered; });
            }

            /// 用例侧：放行本次与之后的所有写入
            void release()
            {
                {
                    const std::lock_guard lock(m_mutex);
                    m_isReleased = true;
                }
                m_condition.notify_all();
            }

        private:
            std::mutex              m_mutex;            ///< 保护下面两个标记
            std::condition_variable m_condition;        ///< 报到与放行的唤醒通道
            bool                    m_isEntered{false}; ///< 是否已有写入停在闸口里
            bool                    m_isReleased{false};///< 是否已放行
        };

        /**
         * @brief 把一次日志写入接到闸口上的 Sink：只用来卡住线程，不落地任何内容
         */
        class GatedSink final : public LogSink
        {
        public:
            /**
             * @brief 构造指向闸口的 Sink
             * @param gate 报到的闸口，存在期由用例保证（清理闸口前 root 已被换掉）
             */
            explicit GatedSink(LogWriteGate &gate) : m_gate(gate)
            {
            }

            /**
             * @brief 交出一条日志并停在闸口里
             * @param event 日志事件，本 Sink 不看内容只数次数
             */
            void write(const LogEvent & /*event*/) override
            {
                m_gate.enter();
            }

            /// 不落盘，没有缓冲需要刷新
            void flush() override
            {
            }

        private:
            LogWriteGate &m_gate; ///< 写入报到并等待放行的闸口
        };

        /**
         * @brief 用例收尾：放行闸口、接回两条线程，再把挂了闸口 Sink 的 root 日志器换掉
         * @details 断言失败会让用例直接返回，而可接合状态的 std::thread 析构即 std::terminate，
         *          因此放行与接合都放进析构，任何退出路径都走同一套。
         */
        class GateCleanup
        {
        public:
            /**
             * @brief 登记要收尾的闸口与线程
             * @param gate 本用例的闸口
             * @param loaderThread 跑加载的线程（可以尚未启动）
             * @param setterThread 跑 setValue 的线程（可以尚未启动）
             */
            GateCleanup(LogWriteGate &gate, std::thread &loaderThread, std::thread &setterThread) :
                m_gate(gate), m_loaderThread(loaderThread), m_setterThread(setterThread)
            {
            }

            ~GateCleanup()
            {
                m_gate.release();
                if (m_loaderThread.joinable())
                {
                    m_loaderThread.join();
                }
                if (m_setterThread.joinable())
                {
                    m_setterThread.join();
                }
                // 闸口 Sink 挂在 root 上，而 Logger 只有 clearSinks() 没有「摘掉单个 Sink」：
                // 整份换掉 root，后面的用例拿到的是一棵没有闸口的新 root（退休表兜住在途引用）
                LoggerRegistry::instance().clear();
            }

            GateCleanup(const GateCleanup &)            = delete;
            GateCleanup &operator=(const GateCleanup &) = delete;

        private:
            LogWriteGate &m_gate;          ///< 要放行的闸口
            std::thread  &m_loaderThread;  ///< 要接回的加载线程
            std::thread  &m_setterThread;  ///< 要接回的写入线程
        };

        /**
         * @brief 把 root 日志器收到的消息原文收进一张表的 Sink，用于断言「这条日志到底报了没有」
         * @details 消息表按 shared_ptr 与 Sink 共享：Sink 挂在 root 上、随用例收尾被换掉，
         *          而用例手里的句柄仍要能读到已经收下的那些消息，因此表不能随 Sink 一起销毁
         */
        class RecordingSink final : public LogSink
        {
        public:
            RecordingSink() : m_messages(std::make_shared<std::vector<std::string>>())
            {
            }

            /**
             * @brief 记下一条消息的原文
             * @param event 日志事件，只取它的 message
             */
            void write(const LogEvent &event) override
            {
                const std::lock_guard lock(m_mutex);
                m_messages->push_back(event.message);
            }

            /// 不落盘，没有缓冲需要刷新
            void flush() override
            {
            }

            /**
             * @brief 取共享的消息表句柄，存在期独立于本 Sink
             * @return std::shared_ptr<std::vector<std::string>> 用例侧可一直读到收尾
             */
            [[nodiscard]] std::shared_ptr<std::vector<std::string>> messages() const noexcept
            {
                return m_messages;
            }

            /**
             * @brief 取截至此刻的消息副本
             * @details 要在别的线程上轮询「这条日志报了没有」时只能走这里：直接读共享表会与
             *          写入线程撞同一个 vector，副本是在消息锁内拷出来的
             * @return std::vector<std::string> 已收下的消息原文
             */
            [[nodiscard]] std::vector<std::string> snapshot() const
            {
                const std::lock_guard lock(m_mutex);
                return *m_messages;
            }

        private:
            std::shared_ptr<std::vector<std::string>> m_messages; ///< 与用例共享的消息表
            mutable std::mutex                        m_mutex;    ///< 保护消息表：Sink 可能被多个线程写
        };

        /**
         * @brief 作用域结束时换掉整棵 root 日志器，摘掉本用例挂上去的 Sink
         * @details Logger 只有 clearSinks() 而没有「摘掉单个 Sink」的口，沿用本文件既有的
         *          「整份换掉 root」做法；退休表兜住在途引用，因此后面的用例拿不到这份 Sink
         */
        class RootSinkScope
        {
        public:
            RootSinkScope()                        = default;
            RootSinkScope(const RootSinkScope &)   = delete;
            RootSinkScope &operator=(const RootSinkScope &) = delete;

            ~RootSinkScope()
            {
                LoggerRegistry::instance().clear();
            }
        };
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

    /**
     * @brief 键里带点号的配置直接判加载失败：它与嵌套写法落成同一个扁平路径，会互相覆盖
     */
    TEST_F(ConfigManagerTest, DottedKeyIsRejectedInsteadOfSilentlyOverwriting)
    {
        // 这份文件里 "server.port" 与 server.port 是同一个扁平键：不拦住的话谁后写谁赢，
        // 调用方拿到的配置与文件里写的对不上，而且毫无提示
        const std::filesystem::path filePathWithDottedKey = writeFile("dotted.yaml", R"("server.port": 8080
server:
  port: 9090
)");

        const ConfigLoadResult result = configuration().loadFromDirectory(filePathWithDottedKey.parent_path());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "server.port")) << "报错里没有指出是哪个键";
        EXPECT_TRUE(anyEntryContains(result.errors, "分隔符")) << "报错里没有说明拒绝的原因";
    }

    /**
     * @brief 空键段也判加载失败：它让嵌套与平铺写法塌缩成同一个扁平键，后写者静默覆盖前者
     * @details 带点号的键已经被上面那条拦住了，但空键段绕过了那道判定：递归进空键那一层时
     *          前缀缓冲仍是空串（`prefixLength != 0` 才补点号），于是子层的 `port` 与顶层的
     *          `port` 落成同一个键。配置文件按 `std::map` 的键序展开，空键排在最前，
     *          因此「9090 覆盖 8080」是确定结果，而这份覆盖毫无提示
     */
    TEST_F(ConfigManagerTest, EmptyKeySegmentIsRejectedInsteadOfSilentlyCollapsing)
    {
        writeFile("empty-key.yaml", R"("":
  port: 8080
port: 9090
)");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_TRUE(anyEntryContains(result.errors, "空")) << "报错里没有指出这是空键段的问题";
        EXPECT_TRUE(configuration().keys().empty()) << "失败的那份配置不该留下一半";
    }

    /**
     * @brief 只有空键本身时同样拒绝：空键进表后就再也改不动
     * @details `{"": 5}` 会往扁平表里塞一个空键，`has("")` 报有值；而 `setValue("")` 拒绝空键，
     *          于是这条记录既取不到默认值路径也永远无法被改写——一份「在配置里但不可写」的键
     */
    TEST_F(ConfigManagerTest, BareEmptyKeyIsRejectedAndNeverEntersTheTable)
    {
        writeFile("bare-empty.json", R"({"": 5, "kept": 1})");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_FALSE(configuration().has("")) << "空键进了生效的快照";
        EXPECT_TRUE(configuration().keys().empty()) << "失败的那份配置不该留下一半";
    }

    /**
     * @brief 解析中途失败的文件：它已经展开的那半份键不能跟着提交进快照
     * @details 钉住 ConfigLoadResult 写明的「失败文件里的键从快照中消失」：半份配置被一起
     *          提交时，运维只看到失败提示，实际却已应用了一半新配置
     */
    TEST_F(ConfigManagerTest, FailedFileDoesNotCommitItsAlreadyFlattenedKeys)
    {
        writeFile("a.yaml", "kept: true\n");
        // 对象按键名排序展开：alpha 先落进扁平表，随后 "bad.key" 让这次加载失败
        writeFile("z.yaml", "alpha: 1\n\"bad.key\": 2\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success) << "带点号的键必须让这次加载失败";
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_TRUE(configuration().getBool("kept", false)) << "没问题的那份文件应当照常提交";
        EXPECT_EQ(configuration().getInt("alpha", -1), -1)
                << "失败文件里「已经展开」的那半份键跟着提交了：契约说失败的键要从快照中消失";
    }

    /**
     * @brief 显式文件列表那条入口同样不能提交失败文件的半截键
     * @details 与上一条钉同一份契约（ConfigLoadResult 写的「失败文件里的键从快照中消失」），
     *          但走的是 loadFiles：它原先把整份累计表直接交给每份文件摊，失败文件已经展开的
     *          键就留在里面被一起提交了。两份文件各用一种格式，顺带覆盖跨格式的这一对入口。
     */
    TEST_F(ConfigManagerTest, LoadFilesDoesNotCommitHalfFlattenedFailedFile)
    {
        const std::filesystem::path goodFile   = writeFile("good.yaml", "kept: true\n");
        // 对象按键名排序展开：alpha 先落进临时表，随后 "bad.key" 才让这份文件判失败
        const std::filesystem::path brokenFile = writeFile("broken.json", R"({"alpha": 1, "bad.key": 2})");

        const ConfigLoadResult result = configuration().loadFiles({goodFile, brokenFile});

        EXPECT_FALSE(result.success) << "带点号的键必须让这次加载失败";
        EXPECT_EQ(result.failedFiles.size(), 1U);
        EXPECT_EQ(result.loadedFiles.size(), 1U);
        EXPECT_TRUE(configuration().getBool("kept", false)) << "没问题的那份文件应当照常提交";
        EXPECT_EQ(configuration().getInt("alpha", -1), -1)
                << "loadFiles 把失败文件已展开的那半份键跟着提交了";
    }

    /**
     * @brief 后一份文件把一段表写成单个值时，先前摊开的那些叶子键要一起让位并报到
     * @details 扁平键模型下「server: 9090」会与前一份文件留下的 server.port / server.host 同时留在快照里，
     *          而这个形态根本读不回来：`getSection` 对「同一个键既是值又是表」一律抛异常，装配整段停摆，
     *          加载却报成功。冲突只可能来自多份文件（单份文件自己展开出来的键互不重叠），因此按
     *          「后写的文件说话」定形，并把每一条丢弃报出来——静默删键与静默留幽灵同样不可接受。
     */
    TEST_F(ConfigManagerTest, ScalarOverrideInLaterFilePrunesTheShadowedSubtree)
    {
        const std::filesystem::path baseFile     = writeFile("base.yaml", "server:\n  port: 8080\n  host: 0.0.0.0\nkept: true\n");
        const std::filesystem::path overrideFile = writeFile("override.yaml", "server: 9090\n");

        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        const ConfigLoadResult result = configuration().loadFiles({baseFile, overrideFile});

        EXPECT_TRUE(result.success);
        EXPECT_FALSE(configuration().has("server.port")) << "旧的一段没让位：快照里 server 既是值又是表";
        EXPECT_FALSE(configuration().has("server.host"));
        EXPECT_TRUE(configuration().has("kept")) << "清理过头，把不相干的键也带走了";
        EXPECT_EQ(configuration().getInt("server", 0), 9090);
        EXPECT_EQ(configuration().keys().size(), 2U);

        ASSERT_EQ(recorded->size(), 1U) << "一次取代报一条：两处丢弃各报一条就成了噪声";
        EXPECT_TRUE(anyEntryContains(*recorded, "server.port"));
        EXPECT_TRUE(anyEntryContains(*recorded, "server.host"));
    }

    /**
     * @brief 反方向同理：后一份文件把单个值写成一段表，先前那个值也要让位并报到
     * @details 同一形状的冲突，只是被淘汰的一方换成了叶子。此时快照必须能被 getSection 正常读出，
     *          这条断言就是「定形之后确实可读」的证据。
     */
    TEST_F(ConfigManagerTest, TableOverrideInLaterFilePrunesTheShadowedLeaf)
    {
        const std::filesystem::path baseFile     = writeFile("base.yaml", "server: 9090\nkept: true\n");
        const std::filesystem::path overrideFile = writeFile("override.yaml", "server:\n  port: 8080\n");

        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        const ConfigLoadResult result = configuration().loadFiles({baseFile, overrideFile});

        EXPECT_TRUE(result.success);
        EXPECT_FALSE(configuration().has("server")) << "旧的值还赖在表的位置上";
        EXPECT_EQ(configuration().getInt("server.port", 0), 8080);
        EXPECT_EQ(configuration().keys().size(), 2U);
        const ConfigValue section = configuration().getSection("server");
        ASSERT_TRUE(section.is_object());
        EXPECT_EQ(section.size(), 1U);

        ASSERT_EQ(recorded->size(), 1U);
        EXPECT_TRUE(anyEntryContains(*recorded, "server"));
    }

    /**
     * @brief 文件名落在本地代码页外时，加载失败要「报出来」而不是把异常抛给调用方
     * @details 本模块把路径写进结果列表与错误文案用的是 `path::string()`，Windows 上它按本地代码页
     *          转换，落在代码页外的字符**直接抛出**，而四处抛出点里有三处就在 catch 块内——
     *          「构造一条报错」于是把整次加载变成异常。POSIX 上窄串等于原生刻度，
     *          这一条只有 Windows 侧能证伪
     */
    TEST_F(ConfigManagerTest, FileNamesOutsideLocalCodePageAreReportedInsteadOfThrowing)
    {
        static constexpr std::string_view brokenNameUtf8  = "坏配置-🐳.yaml";
        static constexpr std::string_view missingNameUtf8 = "没这个文件-🐳.yaml";

        // 绕开夹具的 writeFile：它按窄串拼路径，创建与断言会同过一次代码页而互相掩护
        const std::filesystem::path brokenPath =
                directory() / AsynGyanis::Platform::FileSystem::pathFromUtf8(std::string{brokenNameUtf8});
        {
            std::ofstream output(brokenPath);
            ASSERT_TRUE(output.is_open());
            output << "server:\n  port: 1\n   bad-indent: [unclosed\n";
        }
        const std::filesystem::path missingPath =
                directory() / AsynGyanis::Platform::FileSystem::pathFromUtf8(std::string{missingNameUtf8});

        ConfigLoadResult broken;
        ASSERT_NO_THROW(broken = configuration().loadFiles({brokenPath}));
        EXPECT_FALSE(broken.success);
        // 文件名要原样出现在文案里：变形或替换过的名字会让运维照着提示找不到那个文件
        EXPECT_TRUE(anyEntryContains(broken.errors, std::string{brokenNameUtf8}));

        ConfigLoadResult missing;
        ASSERT_NO_THROW(missing = configuration().loadFiles({missingPath}));
        EXPECT_FALSE(missing.success);
        EXPECT_TRUE(anyEntryContains(missing.failedFiles, std::string{missingNameUtf8}));
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

    /**
     * @brief 非递归加载之后，reload() 不得自己改成递归口径
     * @details 钉的是「没人改过文件却换了配置」：loadFromDirectory(dir, false) 明确只看顶层，
     *          而 reload() 若强行按递归重扫，子目录那份文件的键就凭空多出来。上一条只验首次
     *          加载不含子目录，验不到 reload 这一跳——改前这一跳把 recursive 写死成 true
     */
    TEST_F(ConfigManagerTest, ReloadKeepsTheNonRecursiveScopeOfTheOriginalLoad)
    {
        writeFile("root.yaml", "root: true\n");
        writeFile("group/nested.yaml", "group:\n  depth: 2\n");

        const ConfigLoadResult firstLoad = configuration().loadFromDirectory(directory(), false);
        ASSERT_TRUE(firstLoad.success);
        ASSERT_FALSE(configuration().has("group.depth"));

        const ConfigLoadResult reloaded = configuration().reload();

        EXPECT_TRUE(reloaded.success);
        EXPECT_TRUE(configuration().has("root")) << "顶层的键不该丢";
        EXPECT_FALSE(configuration().has("group.depth"))
                << "reload() 把非递归的加载改成了递归：凭空多出子目录里的键";
    }

    /**
     * @brief loadFiles() 复用既有锚点目录时，必须沿用该目录原本的递归口径
     * @details 钉的是「锚点没换、口径却换了」：显式列表推出的公共父目录正好等于快照已有的锚点时，
     *          把口径写死成 true 会让先前 loadFromDirectory(dir, false) 的顶层限定失效——
     *          此后一次 reload() 凭空多出子目录里的键，而没人改过任何文件
     */
    TEST_F(ConfigManagerTest, LoadFilesReusingTheAnchorDirectoryKeepsItsRecursionScope)
    {
        writeFile("root.yaml", "root: true\n");
        writeFile("group/nested.yaml", "group:\n  depth: 2\n");

        ASSERT_TRUE(configuration().loadFromDirectory(directory(), false).success);
        ASSERT_FALSE(configuration().has("group.depth"));

        // 单份文件的父目录就是 directory() 本身：锚点未换，口径应当原样保留
        ASSERT_TRUE(configuration().loadFiles({filePath("root.yaml")}).success);

        const ConfigLoadResult reloaded = configuration().reload();

        EXPECT_TRUE(reloaded.success);
        EXPECT_TRUE(configuration().has("root")) << "顶层的键不该丢";
        EXPECT_FALSE(configuration().has("group.depth"))
                << "loadFiles() 复用了同一个锚点目录，却把它的递归口径改成了 true";
    }

    /**
     * @brief loadFiles() 落下一个新锚点目录时，重扫按默认的递归口径
     * @details 与上一条互为对照：旧口径属于另一棵树，跟不过来，因此新锚点按递归取默认值。
     *          刻意放在非递归加载之后——若实现无条件沿用快照口径，这条会红
     */
    TEST_F(ConfigManagerTest, LoadFilesWithANewAnchorDirectoryRescansRecursively)
    {
        writeFile("root.yaml", "root: true\n");
        writeFile("other/top.yaml", "other:\n  name: sidecar\n");
        writeFile("other/deep/inner.yaml", "deep:\n  value: 7\n");

        // 先建立「非递归 + 锚点为 directory()」的快照，作为新锚点必须被换掉的起点
        ASSERT_TRUE(configuration().loadFromDirectory(directory(), false).success);

        ASSERT_TRUE(configuration().loadFiles({filePath("other/top.yaml")}).success);

        const ConfigLoadResult reloaded = configuration().reload();

        EXPECT_TRUE(reloaded.success);
        EXPECT_TRUE(configuration().has("other.name")) << "新锚点目录自己的文件要读得到";
        EXPECT_TRUE(configuration().has("deep.value"))
                << "新锚点没有历史口径可沿用，应当按默认的递归重扫，把子目录一起收进来";
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

    namespace
    {
        /**
         * @brief 造一份嵌套指定层数的 JSON 文本：{"k":{"k":…{"k":1}…}}
         * @param nestingDepth 嵌套层数
         * @return std::string 文档文本，长度随层数线性增长
         */
        [[nodiscard]] std::string makeDeeplyNestedJson(const int nestingDepth)
        {
            std::string text;
            text.reserve(static_cast<std::size_t>(nestingDepth) * 6U + 8U);
            for (int level = 0; level < nestingDepth; ++level)
            {
                text += R"({"k":)";
            }
            text += "1";
            text.append(static_cast<std::size_t>(nestingDepth), '}');
            return text;
        }

        /**
         * @brief 造一份嵌套指定层数的 YAML 流式映射文本：k: {k: {…k: 1…}}
         * @details 刻意用流式写法而不是块式缩进：块式每多一层就多 2×层数个空格，文本长度按层数平方
         *          增长，两万层就是几百 MB，量的就不再是解析器而是内存了。
         *          层数按「摊平后的点分键有几段」计：3 层就是 k: {k: {k: 1}}，读出来是 k.k.k。
         * @param nestingDepth 嵌套层数，1 表示一层普通映射
         * @return std::string 文档文本
         */
        [[nodiscard]] std::string makeDeeplyNestedYaml(const int nestingDepth)
        {
            std::string text;
            text.reserve(static_cast<std::size_t>(std::max(nestingDepth - 1, 0)) * 6U + 16U);
            for (int level = 0; level + 1 < nestingDepth; ++level)
            {
                text += "k: {";
            }
            text += "k: 1";
            text.append(static_cast<std::size_t>(std::max(nestingDepth - 1, 0)), '}');
            text += "\n";
            return text;
        }
    } // namespace

    /**
     * @brief 极深嵌套的 JSON 判「这一份文件失败」，而不是把调用栈吃光
     * @details 递归下降的解析器对上无界嵌套只有两条路：要么自己有深度上限，要么当场倒下。
     *          配置文件是可以被仓库里别人提交的一份畸形文件污染的面，因此这条契约要钉住：
     *          超限的那份以中文原因报失败、同目录别份照常生效、进程不死。
     */
    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsDeeplyNestedJsonWithReason)
    {
        writeFile("shallow.json", makeDeeplyNestedJson(3));
        writeFile("abyss.json", makeDeeplyNestedJson(20000));

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.failedFiles.empty()) << "深嵌套的 JSON 没被当成一次失败的加载";
        // failedFiles 里给的是路径，按名字片段核对即可（报错点名要准：坏的只有 abyss.json 那一份）
        const bool onlyTheAbyssFileFailed = std::ranges::any_of(result.failedFiles,
                                                                [](const std::string &failedFile)
                                                                {
                                                                    return failedFile.find("abyss.json") != std::string::npos;
                                                                })
                                            && result.failedFiles.size() == 1U;
        EXPECT_TRUE(onlyTheAbyssFileFailed);
        // 失败原因里要能看出是哪份文件，调用方才知道去哪儿修
        const bool mentionsTheOffendingFile = std::ranges::any_of(result.errors,
                                                                  [](const std::string &error)
                                                                  {
                                                                      return error.find("abyss.json") != std::string::npos;
                                                                  });
        EXPECT_TRUE(mentionsTheOffendingFile) << "错误文案没点名那份文件，全中文也白搭";

        // 拒绝而不是一起带走：同目录那份正常文件的关键照常可读
        EXPECT_TRUE(configuration().has("k.k.k"));
        EXPECT_EQ(configuration().getInt("k.k.k", 0), 1);
    }

    /**
     * @brief 极深嵌套的 YAML 同样判失败而不是崩溃
     * @details 本模块自带的 128 层换算上限拦的是「DOM 转成配置值」那一跳，而 yaml-cpp 的流式
     *          解析本身也是递归下降的——这一例钉的是「解析器先倒下还是我们的闸门先拦住」这个缝隙：
     *          不管哪一侧先拦，交回调用方的都必须是一轮失败的加载。
     */
    TEST_F(ConfigManagerTest, LoadFromDirectoryRejectsDeeplyNestedYamlWithReason)
    {
        // 同目录再放一份同样写法、只是层数正常的流式映射：万一「深嵌套被拒」其实是「我造的文本
        // 本身不合法」，这一份会跟着失败，下面的 keys 断言当场就红
        writeFile("shallow.yaml", makeDeeplyNestedYaml(3));
        writeFile("abyss.yaml", makeDeeplyNestedYaml(20000));

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.failedFiles.empty()) << "深嵌套的 YAML 被当成了成功";
        EXPECT_TRUE(configuration().has("k.k.k")) << "同目录那份正常文件没读进来，说明失败的原因不是嵌套深度";
        EXPECT_EQ(configuration().getInt("k.k.k", 0), 1);
        const bool mentionsTheOffendingFile = std::ranges::any_of(result.errors,
                                                                  [](const std::string &error)
                                                                  {
                                                                      return error.find("abyss.yaml") != std::string::npos;
                                                                  });
        EXPECT_TRUE(mentionsTheOffendingFile) << "错误文案没点名那份文件";
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

    /**
     * @brief 只有注释行的 YAML 与空文件同义：不产出键，也不算坏文件
     * @details 解析出来是「无内容」，与逐字节为空的文档是一回事；而「放一份只写说明的
     *          overrides.yaml」是常见的发布做法，判它失败会把整轮加载报成失败，等于逼运维
     *          删掉那份说明。显式写成 null 文档（`~`）同理。
     * @note 拒绝面同时钉住：标量根仍按「根节点必须是映射」报错，不能顺手一并放过。
     */
    TEST_F(ConfigManagerTest, LoadFromDirectoryTreatsCommentOnlyFileAsEmptyConfig)
    {
        writeFile("app.yaml", "key: value\n");
        writeFile("overrides.yaml", "# 本环境不覆盖任何键\n# 说明性占位文件\n");
        writeFile("explicitNull.yaml", "~\n");

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success);
        EXPECT_TRUE(result.failedFiles.empty()) << "只写注释的文件被判成了坏文件";
        EXPECT_TRUE(result.errors.empty());
        EXPECT_EQ(result.loadedFiles.size(), 3U);
        // 占位文件既不贡献键，也不该把别的文件读进来的键带走
        EXPECT_EQ(configuration().keys().size(), 1U);
        EXPECT_EQ(configuration().getString("key"), "value");
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

    /**
     * @brief 只含 BOM 的 .json 必须与真·空文件同一口径：接受且不产出键
     * @details Windows 记事本把「空」文件存成三个 BOM 字节。它过不了「全是空白」那道判定，
     *          于是带着空正文去解析，被报成 JSON 语法错误——同一份内容在 YAML 侧被当空文档接受，
     *          每次热重载还会把这个幽灵错误重复一遍
     */
    TEST_F(ConfigManagerTest, LoadFromDirectoryAcceptsByteOrderMarkOnlyJsonAsEmpty)
    {
        writeFile("empty-bom.json", std::string("\xEF\xBB\xBF", 3));

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());

        EXPECT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
        EXPECT_TRUE(result.failedFiles.empty()) << "只有 BOM 的文件被当成了解析失败";
        EXPECT_TRUE(configuration().keys().empty()) << "接受为空文档时不该产出任何键";
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

    /**
     * @brief 多线程反复启停热加载，结束时状态必须自洽且不崩
     * @details 启停两条路径都要碰 m_fileWatcher（普通 unique_ptr），二者之间的互斥只由控制面锁
     *          m_hotReloadControlMutex 表达——那个原子布尔护得住开关，护不住监视器对象本身。
     *          本例从并发起停的形状把「终态自洽、关掉后还能再开起来」钉住。
     * @note 这条钉得到的是「不崩、收尾状态一致」这一下界；撕裂指针那类竞态只有容器里的 TSan
     *       看得见（本机 MSVC 不提供 TSan），别把这条跑绿当成「无竞态」的证据
     * @note 每一对启停都要建并销毁一个监视器线程，因此轮数刻意少（3 线程 × 6 轮），并把等待做成
     *       有界的：卡住时报失败并放手，不让这一例把后面的用例一起挂死
     */
    TEST_F(ConfigManagerTest, ConcurrentEnableAndDisableHotReloadEndsInOneConsistentState)
    {
        writeFile("app.yaml", "app:\n  name: demo\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        constexpr int kThreadCount = 3;
        constexpr int kRoundCount  = 6;
        constexpr std::chrono::milliseconds kDebounceMilliseconds{1};
        // 整例的硬上界：启停一对正常是亚毫秒级。这一例曾在并行负载下把套件拖到几百秒——失败要被
        // 报出来，而不是把排在后面的用例一起挂死，所以等待本身必须是有界的
        constexpr std::chrono::milliseconds kMaximumTotalMilliseconds{10000};

        std::atomic<bool> stopRequested{false};
        std::atomic<int>  returnedWorkerCount{0};

        std::vector<std::thread> workers;
        workers.reserve(kThreadCount);
        for (int workerIndex = 0; workerIndex < kThreadCount; ++workerIndex)
        {
            // 每个线程各起各的监视器再各关各的：两条控制路径会在同一时刻撞 m_fileWatcher
            workers.emplace_back([&stopRequested, &returnedWorkerCount, this]
            {
                for (int round = 0; round < kRoundCount && !stopRequested.load(std::memory_order_acquire); ++round)
                {
                    static_cast<void>(configuration().enableHotReload(nullptr, kDebounceMilliseconds));
                    configuration().disableHotReload();
                }
                returnedWorkerCount.fetch_add(1, std::memory_order_release);
            });
        }

        const auto beganAt  = std::chrono::steady_clock::now();
        const auto deadline = beganAt + kMaximumTotalMilliseconds;

        bool allWorkersReturned = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (returnedWorkerCount.load(std::memory_order_acquire) == kThreadCount)
            {
                allWorkersReturned = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - beganAt).count();
        // 标签走 ASCII：控制台代码页会把中文读数弄成乱码，取不到数就白跑一轮
        std::printf("hot-reload churn threads=%d rounds=%d elapsed=%lldms\n",
                    kThreadCount, kRoundCount, static_cast<long long>(elapsedMilliseconds));

        stopRequested.store(true, std::memory_order_release);
        if (allWorkersReturned)
        {
            for (auto &worker: workers)
            {
                worker.join();
            }
        } else
        {
            // 超时就不 join：卡在锁里的线程叫不醒，等它等于等挂。结论先记下来，再把结论冲出去，
            // 然后立刻了断本进程——留在场外的线程还会碰单例与测试互斥量，拖到退出时就是第二次挂死。
            // ctest 把每条用例当成独立进程，因此这一手只影响本例的判定，不会带走别的用例
            ADD_FAILURE() << "有 worker 没能在规定上界内从启停里回来，这就是那条长时间挂死的形状";
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(1);
        }

        configuration().disableHotReload();
        EXPECT_FALSE(configuration().isHotReloadEnabled());
        // 关掉之后必须还能再开起来：指针被撕坏时这一步会崩或返回 false
        EXPECT_TRUE(configuration().enableHotReload());
        EXPECT_TRUE(configuration().isHotReloadEnabled());
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

    /**
     * @brief setValue 改掉一个键的形态时，被它盖住的旧键要一起让位
     * @details 装载路径按「后写的说话」定形，setValue 原先只是往快照里塞一个新键，于是同一个名字
     *          可以既是值又是表。这样的快照读起来是骗人的：getSection() 只扫 `<段名>.` 前缀下的那些键，
     *          刚设进去的那个值整段看不见，而 keys()/has()/get() 又都报它存在——两边各自都「对」，
     *          合起来没有一句真话。反向（在旧叶子下面写出键）同理。
     */
    TEST_F(ConfigManagerTest, SetValueCollapsingASectionDropsTheKeysItShadows)
    {
        const std::filesystem::path baseFile = writeFile("base.yaml", "server:\n  port: 8080\n  host: 0.0.0.0\nkept: true\n");
        ASSERT_TRUE(configuration().loadFiles({baseFile}).success);

        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        ASSERT_TRUE(configuration().setValue("server", ConfigValue(std::int64_t{9090})));

        EXPECT_EQ(configuration().getInt("server", 0), 9090);
        EXPECT_FALSE(configuration().has("server.port")) << "旧叶子没让位：getSection(\"server\") 会把刚设进去的值吞掉";
        EXPECT_FALSE(configuration().has("server.host"));
        EXPECT_TRUE(configuration().has("kept")) << "清理过头，把不相干的键也带走了";
        // 消费方真正看得见的形状：段空了，值在段名自己身上
        EXPECT_TRUE(configuration().getSection("server").empty()) << "getSection 还在报出已被废掉的段内键";
        ASSERT_EQ(recorded->size(), 1U) << "一次取代只报一条";
        EXPECT_TRUE(anyEntryContains(*recorded, "server.port"));
        EXPECT_TRUE(anyEntryContains(*recorded, "setValue")) << "报错里认不出这一次是谁写的";

        // 反向：在旧叶子下面写出一个键，那个旧值同样要让位
        ASSERT_TRUE(configuration().setValue("logging", ConfigValue(std::int64_t{3})));
        ASSERT_TRUE(configuration().setValue("logging.level", ConfigValue(std::string("DEBUG"))));

        EXPECT_EQ(configuration().getString("logging.level", ""), "DEBUG");
        EXPECT_FALSE(configuration().has("logging")) << "旧值留着：logging 同时是 3 又是一张表";
        EXPECT_TRUE(anyEntryContains(*recorded, "既是值又是表"));
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

    /**
     * @brief 相对路径不得被当成配置目录锚点：锚点是此后每次重读时按当前目录解析的
     * @details 注释与头文件都写着「含相对路径时保留既有配置目录不覆盖」，实现此前只挡
     *          「没有目录成分」那一种（"sub/settings.json" 的 parent 是 "sub"，非空即被收下）。
     *          锚点会被 enableHotReload 与之后每一次 reload() 按**当时的**工作目录重新解析，
     *          进程换过工作目录（daemonize 等）后重读的就是另一个目录，而配置目录看着「已设置」
     */
    TEST_F(ConfigManagerTest, LoadFilesWithRelativePathKeepsTheConfigDirectoryUnset)
    {
        struct ScopedWorkingDirectory
        {
            std::filesystem::path previous;
            explicit ScopedWorkingDirectory(const std::filesystem::path &target) : previous(std::filesystem::current_path())
            {
                std::filesystem::current_path(target);
            }
            ~ScopedWorkingDirectory()
            {
                std::filesystem::current_path(previous);
            }
        };

        writeFile("relative-anchor/settings.json", R"({"port": 9090})");
        const ScopedWorkingDirectory chdirToSandbox(directory());

        const ConfigLoadResult result = configuration().loadFiles({std::filesystem::path("relative-anchor/settings.json")});

        ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
        EXPECT_EQ(configuration().getInt("port", 0), 9090) << "文件本身要照常加载";
        EXPECT_TRUE(configuration().configDirectory().empty())
                << "相对路径被当成了锚点：换工作目录之后 reload() 读的就是别处";
        EXPECT_FALSE(configuration().reload().success) << "没有锚点时 reload 要如实失败，而不是悄悄换个目录";
    }

    /**
     * @brief 「尚未设置配置目录」这条失败出口也要交出确定值的回执
     * @details 两条早退出口直接返回默认构造的 ConfigLoadResult，而 timestamp 一度没有初值：
     *          steady_clock::time_point 的默认构造底层是未初始化的整型，读它即未定义行为，
     *          而 Debug 下 MSVC 会把栈填成 0xCDCDCDCD……，断言因此能稳定抓到
     */
    TEST_F(ConfigManagerTest, ReloadWithoutAnchorReportsADeterministicallyInitializedTimestamp)
    {
        configuration().clear();

        const ConfigLoadResult result = configuration().reload();

        ASSERT_FALSE(result.success);
        EXPECT_EQ(result.timestamp.time_since_epoch().count(), 0)
                << "这条出口没给 timestamp 赋值，交回的是不确定的栈内容";
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

    TEST_F(ConfigManagerTest, SchemaViolationLoggingDoesNotHoldTheWriterLock)
    {
        writeFile("cfg.yaml", "port: 8080\n");
        // 一个缺失的必需键：提交快照时正好产生一条校验错误日志
        static_cast<void>(configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"must.exist", ConfigValueType::string, true, std::nullopt, std::nullopt},
        }));

        LogWriteGate gate;
        std::thread  loader;
        std::thread  setter;
        const GateCleanup cleanup(gate, loader, setter);
        bool              isLoadSucceeded = false;
        std::promise<void> setterFinished;
        auto              setterDone = setterFinished.get_future();

        LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<GatedSink>(gate));

        loader = std::thread([this, &isLoadSucceeded]
        {
            isLoadSucceeded = configuration().loadFromDirectory(directory()).success;
        });

        // 先确认提交线程确实停在「写那条校验错误日志」上，否则下面的绿只是没撞上有锁的那段
        ASSERT_TRUE(gate.waitUntilEntered(std::chrono::seconds{5})) << "闸口没等到日志写入，用例没有构造出重叠";

        // 闸口进去之后再放竞争者：先跑完的 setValue 会让这条判据假绿
        setter = std::thread([this, &setterFinished]
        {
            static_cast<void>(configuration().setValue("runtime.flag", ConfigValue(std::string("on"))));
            setterFinished.set_value();
        });
        const bool didSetWhileGated = setterDone.wait_for(std::chrono::milliseconds{500}) == std::future_status::ready;

        gate.release();
        loader.join();
        setter.join();

        EXPECT_TRUE(didSetWhileGated) << "校验期间的日志写入把写锁占住了：setValue 只能等一轮日志写完";
        EXPECT_TRUE(isLoadSucceeded);
        // 两条路都真的写了快照：提交换掉的键与 setValue 补的键同时在场
        EXPECT_EQ(configuration().getInt("port", 0), 8080);
        EXPECT_EQ(configuration().getString("runtime.flag"), "on");
    }

    TEST_F(ConfigManagerTest, LoadFromEmptyDirectoryStillRunsTheRegisteredSchema)
    {
        static_cast<void>(configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"must.exist", ConfigValueType::string, true, std::nullopt, std::nullopt},
        }));

        bool isLoadSucceeded = false;
        {
            LogWriteGate gate;
            std::thread  loader;
            std::thread  idleSetter;
            const GateCleanup cleanup(gate, loader, idleSetter);

            LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<GatedSink>(gate));
            loader = std::thread([this, &isLoadSucceeded]
            {
                isLoadSucceeded = configuration().loadFromDirectory(directory()).success;
            });

            // 目录里一份配置文件也没有：这条提交路径同样要过已注册 schema（它以前自己换快照，绕开了这道暴露）
            EXPECT_TRUE(gate.waitUntilEntered(std::chrono::seconds{5})) << "空目录提交没有执行已注册 schema 的校验";
        }

        EXPECT_TRUE(isLoadSucceeded);
        EXPECT_TRUE(configuration().keys().empty());
    }

    TEST_F(ConfigManagerTest, SetValueIsAppliedEvenWhenRegisteredSchemaRejectsItsType)
    {
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        const ConfigSchema schema = {ConfigSchemaEntry{"app.size", ConfigValueType::number_integer, true, std::nullopt, std::nullopt}};

        // 注册时即对当前（空）快照校验一次：缺失必需键被报告，但不阻断后续写入
        const ConfigValidationResult registration = configuration().setSchema(schema);
        EXPECT_FALSE(registration.valid);
        EXPECT_TRUE(anyEntryContains(registration.errors, "缺少必需配置键"));

        // schema 为建议性约束：setValue 不拦下违规值，但违规要与文件加载同一口径报进日志（见下面两条用例）
        EXPECT_TRUE(configuration().setValue("app.size", ConfigValue(std::string("not-a-number"))));
        EXPECT_EQ(configuration().getOptional("app.size")->type(), ConfigValueType::string);

        const ConfigValidationResult result = configuration().validateSchema(schema);
        EXPECT_FALSE(result.valid);
        EXPECT_TRUE(anyEntryContains(result.errors, "类型不符"));
    }

    TEST_F(ConfigManagerTest, SetValueReportsSchemaViolationOfTheKeyItWrites)
    {
        // 钉住的是可观测性：类型不符的写入此后每次 getInt 都静默回落到默认值，
        // 同样的值写在文件里会报一条 ERROR，写在程序里却什么都不报
        static_cast<void>(configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"app.size", ConfigValueType::number_integer, false, std::nullopt, std::nullopt},
        }));

        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        EXPECT_TRUE(configuration().setValue("app.size", ConfigValue(std::string("not-a-number"))));

        ASSERT_EQ(recorded->size(), 1U) << "违规写入没有按文件加载的同一口径上报";
        EXPECT_TRUE(anyEntryContains(*recorded, "类型不符"));
        EXPECT_TRUE(anyEntryContains(*recorded, "app.size"));
    }

    TEST_F(ConfigManagerTest, SetValueStaysSilentForKeysItShouldNotComplainAbout)
    {
        // 必需键此刻是缺的：那是文件那边的事，不该由每次无关写入重复播报
        static_cast<void>(configuration().setSchema(ConfigSchema{
                ConfigSchemaEntry{"app.size", ConfigValueType::number_integer, true, 1.0, 65535.0},
        }));

        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        // schema 里没有这个键：写什么都不报
        EXPECT_TRUE(configuration().setValue("runtime.flag", ConfigValue(std::string("on"))));
        // 类型与区间都合规：也不报
        EXPECT_TRUE(configuration().setValue("app.size", ConfigValue(42)));

        EXPECT_TRUE(recorded->empty()) << "每次 setValue 都重播一遍别人的欠账，等于把这条通道变成噪声";
        EXPECT_EQ(configuration().getInt("app.size", 0), 42);
    }

    // ============================================================================
    // 热加载开关与状态机
    // ============================================================================

    /**
     * @brief 没设锚点时 enableHotReload 回 false，并且要说清是哪一条失败路
     * @details 四条失败路（没锚点、FileWatcher::create() 返回空、addWatch 失败、start 失败、装配抛异常）
     *          原先都只回一个 false——「改了配置没反应」在这种现场下根本无从下手，而「没锚点」与
     *          「监视器起不来」的修法完全不同。现在每一条都报明原因，能带目录的带上目录。
     */
    TEST_F(ConfigManagerTest, EnableHotReloadWithoutConfigDirectoryFailsAndStaysDisabled)
    {
        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        EXPECT_FALSE(configuration().isHotReloadEnabled());

        EXPECT_FALSE(configuration().enableHotReload());

        EXPECT_FALSE(configuration().isHotReloadEnabled());
        EXPECT_NO_THROW(configuration().disableHotReload());

        ASSERT_EQ(recorded->size(), 1U) << "只回 false 不说原因，运维分不出「没设锚点」与「监视器起不来」";
        EXPECT_TRUE(anyEntryContains(*recorded, "配置目录"));
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

    /**
     * @brief 已在监听时再启用一次：这一轮的新回调必须接上
     * @details 这条走的是「已经启用」那条早退：早退此前把这一轮传入的回调整份丢掉，调用方拿到
     *          true 却永远等不到自己新给的接线，而没有任何返回值暗示得先 disable 一次。
     * @note 平台监听器不可用时跳过（与其余热重载用例同一口径）；先等第一次接线出声，
     *        否则「换过去没出声」可能是根本没武装监听，而不是换回调失败。
     */
    TEST_F(ConfigManagerTest, RepeatedEnableHotReloadTakesTheNewCallback)
    {
        writeFile("cfg.yaml", "value: first\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        std::atomic<int> firstCallbackCount{0};
        std::atomic<int> secondCallbackCount{0};

        const bool enabled = configuration().enableHotReload(
                [&firstCallbackCount](const ConfigLoadResult &)
                {
                    firstCallbackCount.fetch_add(1);
                },
                std::chrono::milliseconds(50));
        if (!enabled)
        {
            GTEST_SKIP() << "本平台的文件监听器不可用，热重载用例跳过";
        }

        // 监听线程要先把读请求投出去，紧跟着 enableHotReload 就写文件会落在武装之前
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        writeFile("cfg.yaml", "value: second\n");
        ASSERT_TRUE(TestSupport::waitForCondition([&firstCallbackCount]
                                                  {
                                                      return firstCallbackCount.load() >= 1;
                                                  },
                                                  std::chrono::seconds(8)))
                << "第一次接线就没出声，换回调的判据无从谈起";

        EXPECT_TRUE(configuration().enableHotReload(
                [&secondCallbackCount](const ConfigLoadResult &)
                {
                    secondCallbackCount.fetch_add(1);
                },
                std::chrono::milliseconds(50)));

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        writeFile("cfg.yaml", "value: third\n");
        EXPECT_TRUE(TestSupport::waitForCondition([&secondCallbackCount]
                                                  {
                                                      return secondCallbackCount.load() >= 1;
                                                  },
                                                  std::chrono::seconds(8)))
                << "重复 enableHotReload 把新回调丢了：返回 true 却没人接线";

        configuration().disableHotReload();
    }

    /**
     * @brief 在重载回调里关掉热重载，不得把进程带走
     * @details 「连续几轮失败就别再监视」是合法用法，可本轮任务此刻正跑在回调所在的线程上：
     *          disableHotReload() 原先把登记表里的任务全部销毁，~jthread 于是去 join 自己
     *          那条线程——异常从析构里出来即 std::terminate。回调先睡一小段，确保任务已经
     *          进了登记表，这条路径就是确定命中而不是赌调度
     */
    TEST_F(ConfigManagerTest, DisablingHotReloadFromWithinItsOwnCallbackDoesNotAbort)
    {
        writeFile("cfg.yaml", "value: first\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        std::atomic<bool> isDisableReturned{false};
        const bool        enabled = configuration().enableHotReload(
                [&isDisableReturned](const ConfigLoadResult &)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    ConfigManager::instance().disableHotReload();
                    isDisableReturned.store(true, std::memory_order_release);
                },
                std::chrono::milliseconds(50));
        if (!enabled)
        {
            GTEST_SKIP() << "本平台的文件监听器不可用，热重载用例跳过";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        writeFile("cfg.yaml", "value: second\n");

        ASSERT_TRUE(TestSupport::waitForCondition([&isDisableReturned]
                                                  {
                                                      return isDisableReturned.load(std::memory_order_acquire);
                                                  },
                                                  std::chrono::seconds(10)))
                << "回调里的 disableHotReload() 没有返回：它在 join 自己那条重载线程";
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

    /**
     * @brief 钉住：把配置文件改名移走也要触发热重载
     * @details 消费方接受的事件种类是 Modified/Created/Deleted 三种。Linux 的 `IN_MOVED_FROM` 被映射成
     *          Deleted，正好落在其中；Windows 的 `FILE_ACTION_RENAMED_OLD_NAME` 映射成 Moved，于是
     *          「把 config.yaml 改名挪走」这个下线动作在 Windows 上一条通知都不算数：旧名被 Moved 滤掉，
     *          新名因不是配置后缀被扩展名滤掉，配置停在已经消失的那份上直到下次改动。
     *          先做一次普通改写当对照：确认监视通道活着，后面的判据才只指向「移走」这一条路。
     */
    TEST_F(ConfigManagerTest, RenamingConfigFileAwayTriggersHotReload)
    {
        writeFile("cfg.yaml", "value: first\n");
        writeFile("control.yaml", "control: 1\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        std::atomic<int> callbackCount{0};
        const bool       enabled = configuration().enableHotReload(
                [&callbackCount](const ConfigLoadResult &)
                {
                    callbackCount.fetch_add(1, std::memory_order_release);
                },
                std::chrono::milliseconds(50));
        if (!enabled)
        {
            GTEST_SKIP() << "本平台的文件监听器不可用，热重载用例跳过";
        }

        const auto waitsForMoreCallbacks = [&callbackCount](const int expectedCount)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (callbackCount.load(std::memory_order_acquire) >= expectedCount)
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // 对照走的是另一个文件：同一条路径在监听器的防抖窗口内被再次上报会被压掉，
        // 用同一个文件做对照就会把「移走那一步到底通不通」和「防抖恰好吃掉了它」混在一起
        writeFile("control.yaml", "control: 2\n");
        const bool sawControlReload = waitsForMoreCallbacks(1);
        EXPECT_TRUE(sawControlReload) << "普通改写就没触发重载：监视通道没建立，下面的判据无从谈起";
        if (!sawControlReload)
        {
            configuration().disableHotReload();
            return;
        }

        const int countBeforeRename = callbackCount.load(std::memory_order_acquire);
        std::error_code renameError;
        std::filesystem::rename(directory() / "cfg.yaml", directory() / "cfg.retired", renameError);
        ASSERT_FALSE(static_cast<bool>(renameError)) << "改名移走配置文件失败：" << renameError.message();

        EXPECT_TRUE(waitsForMoreCallbacks(countBeforeRename + 1)) << "改名移走配置文件没有触发热重载：Moved 事件被消费方的种类判据滤掉了";

        configuration().disableHotReload();
    }

    /**
     * @brief 没挂回调时，热重载那一轮的失败要自己落到日志上
     * @details enableHotReload(nullptr) 是默认用法（「重载照跑」），而重载结果原本只有一个去处——
     *          那个回调。没有它，一个文件解析不了就从新快照里整份缺席，开关照旧是开的，现场只看得见
     *          「改了配置没反应」。判据走日志：先换一条路径写合法内容并等新值进快照（证明监视通道是活的，
     *          否则下面的红只是「监听没起来」），再把另一个文件写成非法 YAML，等有内容的错误行出现。
     *          两步分开在不同文件上做：同一条路径在防抖窗口内被再次上报会被压掉
     */
    TEST_F(ConfigManagerTest, FailedHotReloadWithoutCallbackIsLogged)
    {
        writeFile("cfg.yaml", "value: first\n");
        writeFile("other.yaml", "other: 1\n");
        ASSERT_TRUE(configuration().loadFromDirectory(directory()).success);

        auto recorder = std::make_unique<RecordingSink>();
        RecordingSink *const recorderPointer = recorder.get();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        if (!configuration().enableHotReload(nullptr, std::chrono::milliseconds(50)))
        {
            configuration().disableHotReload();
            GTEST_SKIP() << "本平台的文件监听器不可用，热重载用例跳过";
        }

        // 有界轮询：构造不出「日志已落地」时用例该报失败，而不是赌调度
        const auto waitsFor = [](const auto &condition)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };

        writeFile("cfg.yaml", "value: second\n");
        const bool watcherAlive = waitsFor([]
        {
            return ConfigManager::instance().getString("value") == "second";
        });
        EXPECT_TRUE(watcherAlive) << "普通改写没触发热重载：监视通道没建立，后面的判据无从谈起";

        bool sawFailureLogged = false;
        if (watcherAlive)
        {
            // 保留字符 @ 不能作为标量开头：这条改动会让那一轮重载整轮失败
            writeFile("other.yaml", "other: 1\nbroken: @invalid\n");
            sawFailureLogged = waitsFor([recorderPointer]
            {
                const std::vector<std::string> recorded = recorderPointer->snapshot();
                return anyEntryContains(recorded, "热重载本轮失败") && anyEntryContains(recorded, "YAML 语法错误");
            });
            EXPECT_TRUE(sawFailureLogged) << "热重载失败且没有回调时，一条诊断都没落到日志上";
        }

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

    /**
     * @brief 后一份文件把先前的标量叶子写成表时，加载阶段就定形并报到
     * @details 旧断言是「两份文件各自合法、加载不报错，冲突留给 getSection 抛」——那等于把矛盾留给读的人，
     *          加载报成功而这段永远读不回来。语义改为「后写的说话 + 淘汰时报一条」，依据是配置模块
     *          既定的「容错必须可见」，与 setValue 走的同一条定形规矩。
     */
    TEST_F(ConfigManagerTest, LaterFileTableWinsOverEarlierScalarLeafAndReportsIt)
    {
        writeFile("base.yaml", "server:\n  port: 8080\n");
        writeFile("extra.yaml", "server:\n  port:\n    forwarded: true\n  host: 0.0.0.0\n");
        writeFile("cache.yaml", "cache:\n  ttl: 60\n");

        auto recorder = std::make_unique<RecordingSink>();
        auto recorded = recorder->messages();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        const ConfigLoadResult result = configuration().loadFromDirectory(directory());
        ASSERT_TRUE(result.success);

        // 后一份文件在 server.port 下面写出了键，先前的标量叶子就该让位；这段现在是可读的表
        EXPECT_FALSE(configuration().has("server.port")) << "旧的值没让位，getSection 仍会在这一段抛";
        EXPECT_TRUE(configuration().has("server.port.forwarded"));
        EXPECT_EQ(configuration().getString("server.host"), "0.0.0.0") << "host 被连带丢弃了：清理过头";
        const ConfigValue serverSection = configuration().getSection("server");
        ASSERT_TRUE(serverSection.is_object());
        EXPECT_TRUE(serverSection.contains("port"));

        // 冲突只报一条，且点名到被淘汰的那个键；不相干的段落不受影响
        ASSERT_EQ(recorded->size(), 1U);
        EXPECT_TRUE(anyEntryContains(*recorded, "server.port"));
        const ConfigValue cacheSection = configuration().getSection("cache");
        ASSERT_TRUE(cacheSection.is_object());
        EXPECT_EQ(cacheSection.at("ttl").get<int>(), 60);
    }

    /**
     * @brief setValue 沿一条已有标量的路径往下写键时，旧标量让位、整段照样读得回来
     * @details 旧断言是「程序侧写入照样能造出『同一个键既是值又是表』，而 getSection 必须拒绝这种快照」——
     *          那等于把矛盾留给读的人：刚写进去的值整段看不见，keys()/has() 又都报它存在。语义改成与装载
     *          路径同一口径（后写的说话 + 淘汰时报一条），依据是本模块既定的「容错必须可见」。
     *          读侧 buildNestedObject 那道守卫因此再没有公开入口能触发，留着当不变式自检。
     */
    TEST_F(ConfigManagerTest, SetValueWritingBelowAScalarRebuildsTheSectionInsteadOfThrowing)
    {
        auto recorder = std::make_unique<RecordingSink>();
        RecordingSink *const recorderPointer = recorder.get();
        LoggerRegistry::instance().getRootLogger().addSink(std::move(recorder));
        const RootSinkScope detachSink;

        ASSERT_TRUE(configuration().setValue("server.port", ConfigValue(8080)));
        ASSERT_TRUE(configuration().setValue("server.port.forwarded", ConfigValue(true)));

        EXPECT_FALSE(configuration().has("server.port")) << "旧标量没让位：这一段会被判成「既是值又是表」";
        EXPECT_TRUE(configuration().has("server.port.forwarded"));

        ConfigValue serverSection;
        try
        {
            serverSection = configuration().getSection("server");
        } catch (const ConfigValidationException &error)
        {
            FAIL() << "getSection 仍拒绝这种快照：" << error.what();
        }
        ASSERT_TRUE(serverSection.is_object());
        ASSERT_TRUE(serverSection.contains("port"));
        EXPECT_TRUE(serverSection.at("port").at("forwarded").get<bool>()) << "定形做对了，段落还原却漏了嵌套层";
        EXPECT_TRUE(anyEntryContains(recorderPointer->snapshot(), "既是值又是表"));
    }

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
