// LoggerRegistry 单元测试：单例、按名获取与注册、遍历与运行时等级开关

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"

#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /** @brief 按字典序排序日志器名称列表，便于与期望列表整体比较 */
        std::vector<std::string> sortedNames(std::vector<std::string> names)
        {
            std::ranges::sort(names);
            return names;
        }

        /** @brief 判断注册表中是否存在指定名称的日志器 */
        bool isRegistered(const std::string &name)
        {
            const std::vector<std::string> names = LoggerRegistry::instance().getLoggerNames();
            return std::ranges::find(names, name) != names.end();
        }

        /**
         * @brief 观察自身销毁的 Sink：退休表是否真的释放了对象，靠它计数
         */
        class CountingSink final : public LogSink
        {
        public:
            CountingSink()
            {
                s_aliveCount.fetch_add(1);
            }

            ~CountingSink() override
            {
                s_aliveCount.fetch_sub(1);
            }

            void write(const LogEvent &) override
            {
            }

            void flush() override
            {
            }

            /// 当前存活实例数；用例结束前必须回到 0，否则说明对象仍被别处持有
            static std::atomic<int> s_aliveCount;
        };

        std::atomic<int> CountingSink::s_aliveCount{0};

        /**
         * @brief 在 write() 内部等外部放行的 Sink：把「写入者已经进到 Sink 里」变成可观测事实
         */
        class BlockingSink final : public LogSink
        {
        public:
            BlockingSink()
            {
                s_aliveCount.fetch_add(1);
            }

            ~BlockingSink() override
            {
                s_aliveCount.fetch_sub(1);
            }

            void write(const LogEvent &) override
            {
                // 先进到 Sink 里报个到，再停在这里模拟一次慢落盘：这段窗口就是要测的重叠点
                m_entered.set_value();
                m_release.get_future().wait();
            }

            void flush() override
            {
            }

            /// 等到 write() 真的开始执行；返回 false 说明用例没构造出重叠，不该继续往下断言
            [[nodiscard]] bool waitForEnter()
            {
                return m_entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
            }

            /// 放行写入者，write() 随即返回
            void release()
            {
                m_release.set_value();
            }

            /// 当前存活实例数
            static std::atomic<int> s_aliveCount;

        private:
            std::promise<void> m_entered; ///< 第一次进入 write() 时兑现
            std::promise<void> m_release; ///< 测试侧用它放行
        };

        std::atomic<int> BlockingSink::s_aliveCount{0};
    } // namespace

    /**
     * @brief LoggerRegistry 测试夹具
     * @details 注册表是进程级单例，SetUp/TearDown 均清空，避免用例之间互相串味。
     */
    class LoggerRegistryTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            LoggerRegistry::instance().clear();
        }

        void TearDown() override
        {
            LoggerRegistry::instance().clear();
        }
    };

    // ============================================================================
    // 单例与获取语义
    // ============================================================================

    TEST_F(LoggerRegistryTest, InstanceReturnsTheSameObjectOnEveryCall)
    {
        LoggerRegistry &first  = LoggerRegistry::instance();
        LoggerRegistry &second = LoggerRegistry::instance();

        EXPECT_EQ(&first, &second);
    }

    TEST_F(LoggerRegistryTest, GetLoggerCreatesNamedLoggerOnFirstAccess)
    {
        const Logger &logger = LoggerRegistry::instance().getLogger("custom");

        EXPECT_EQ(logger.name(), "custom");
        EXPECT_EQ(logger.getLevel(), LogLevel::Trace);
    }

    TEST_F(LoggerRegistryTest, GetLoggerReturnsSameInstanceForRepeatedCalls)
    {
        Logger &first = LoggerRegistry::instance().getLogger("reused");

        Logger &second = LoggerRegistry::instance().getLogger("reused");

        EXPECT_EQ(&first, &second);
    }

    TEST_F(LoggerRegistryTest, GetLoggerKeepsStateOfFirstCallVisible)
    {
        LoggerRegistry::instance().getLogger("stateful").setLevel(LogLevel::Error);

        const Logger &retrieved = LoggerRegistry::instance().getLogger("stateful");

        EXPECT_EQ(retrieved.getLevel(), LogLevel::Error);
    }

    TEST_F(LoggerRegistryTest, GetLoggerCreatesIndependentLoggerForEachName)
    {
        Logger &first  = LoggerRegistry::instance().getLogger("logger_a");
        Logger &second = LoggerRegistry::instance().getLogger("logger_b");

        EXPECT_NE(&first, &second);
        EXPECT_EQ(first.name(), "logger_a");
        EXPECT_EQ(second.name(), "logger_b");

        first.setLevel(LogLevel::Fatal);
        EXPECT_EQ(second.getLevel(), LogLevel::Trace);
    }

    TEST_F(LoggerRegistryTest, GetRootLoggerIsNamedRoot)
    {
        Logger &root = LoggerRegistry::instance().getRootLogger();

        EXPECT_EQ(root.name(), "root");
    }

    TEST_F(LoggerRegistryTest, GetRootLoggerSharesInstanceWithExplicitRootName)
    {
        Logger &root = LoggerRegistry::instance().getRootLogger();

        Logger &byName = LoggerRegistry::instance().getLogger("root");

        EXPECT_EQ(&root, &byName);
    }

    TEST_F(LoggerRegistryTest, CachedRootLoggerFollowsRegisterOverwrite)
    {
        // 先预热 root 缓存，再用 registerLogger 覆盖同一名字：缓存必须在覆盖时失效并
        // 指向新实例，否则取到的是已被替换的旧对象
        LoggerRegistry::instance().getRootLogger().setLevel(LogLevel::Error);

        auto    replacement         = std::make_unique<Logger>("root");
        Logger *replacementInstance = replacement.get();
        replacement->setLevel(LogLevel::Debug);
        LoggerRegistry::instance().registerLogger(std::move(replacement));

        Logger &root = LoggerRegistry::instance().getRootLogger();
        EXPECT_EQ(&root, replacementInstance);
        EXPECT_EQ(root.getLevel(), LogLevel::Debug);
    }

    TEST_F(LoggerRegistryTest, CachedRootLoggerIsRebuiltAfterUnregister)
    {
        LoggerRegistry::instance().getRootLogger().setLevel(LogLevel::Fatal);

        LoggerRegistry::instance().unregisterLogger("root");

        Logger &rebuilt = LoggerRegistry::instance().getRootLogger();
        EXPECT_EQ(&rebuilt, &LoggerRegistry::instance().getLogger("root"));
        EXPECT_EQ(rebuilt.getLevel(), LogLevel::Trace);
    }

    TEST_F(LoggerRegistryTest, CachedRootLoggerIsRebuiltAfterClear)
    {
        LoggerRegistry::instance().getRootLogger().setLevel(LogLevel::Fatal);

        LoggerRegistry::instance().clear();

        Logger &rebuilt = LoggerRegistry::instance().getRootLogger();
        EXPECT_EQ(rebuilt.name(), "root");
        EXPECT_EQ(rebuilt.getLevel(), LogLevel::Trace);
    }

    TEST_F(LoggerRegistryTest, RootLoggerLookupStaysConsistentWhileOtherLoggersChurn)
    {
        LoggerRegistry::instance().getRootLogger().setLevel(LogLevel::Warn);

        std::atomic<bool>        stopChurning{false};
        std::atomic<int>         mismatches{0};
        std::vector<std::thread> churners;
        std::vector<std::thread> readers;

        // 变更线程只增删「非 root」日志器：root 缓存持有强引用且不因其它名字的增删失效，
        // 读取线程因此始终命中缓存，既不会取到被销毁的对象，也不会被写锁串行化
        churners.emplace_back([&stopChurning]
        {
            int sequence = 0;
            while (!stopChurning.load(std::memory_order_relaxed))
            {
                const std::string name = "churn_" + std::to_string(sequence++);
                LoggerRegistry::instance().registerLogger(std::make_unique<Logger>(name));
                LoggerRegistry::instance().unregisterLogger(name);
            }
        });
        for (int index = 0; index < 3; ++index)
        {
            readers.emplace_back([&mismatches, &stopChurning]
            {
                while (!stopChurning.load(std::memory_order_relaxed))
                {
                    const Logger &root = LoggerRegistry::instance().getRootLogger();
                    if (root.name() != "root" || root.getLevel() != LogLevel::Warn)
                    {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stopChurning.store(true, std::memory_order_relaxed);
        for (std::thread &churner: churners)
        {
            churner.join();
        }
        for (std::thread &reader: readers)
        {
            reader.join();
        }

        EXPECT_EQ(mismatches.load(), 0);
        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().name(), "root");
    }

    // ============================================================================
    // 注册与注销
    // ============================================================================

    TEST_F(LoggerRegistryTest, RegisterLoggerAdoptsExternallyCreatedInstance)
    {
        auto    external         = std::make_unique<Logger>("external");
        Logger *externalInstance = external.get();
        external->setLevel(LogLevel::Debug);

        LoggerRegistry::instance().registerLogger(std::move(external));

        Logger &registered = LoggerRegistry::instance().getLogger("external");
        EXPECT_EQ(&registered, externalInstance);
        EXPECT_EQ(registered.getLevel(), LogLevel::Debug);
    }

    TEST_F(LoggerRegistryTest, RegisterLoggerReplacesLoggerOfSameName)
    {
        LoggerRegistry::instance().getLogger("overwrite").setLevel(LogLevel::Info);

        auto    replacement      = std::make_unique<Logger>("overwrite");
        Logger *replacementEntry = replacement.get();
        replacement->setLevel(LogLevel::Error);

        LoggerRegistry::instance().registerLogger(std::move(replacement));

        Logger &current = LoggerRegistry::instance().getLogger("overwrite");
        EXPECT_EQ(&current, replacementEntry);
        EXPECT_EQ(current.getLevel(), LogLevel::Error);
    }

    TEST_F(LoggerRegistryTest, RegisterLoggerIgnoresNullPointer)
    {
        LoggerRegistry::instance().getLogger("keeper");

        LoggerRegistry::instance().registerLogger(nullptr);

        const std::vector<std::string> expected{"keeper"};
        EXPECT_EQ(sortedNames(LoggerRegistry::instance().getLoggerNames()), expected);
    }

    TEST_F(LoggerRegistryTest, UnregisterLoggerRemovesRegisteredLogger)
    {
        LoggerRegistry::instance().getLogger("to_remove").setLevel(LogLevel::Error);

        LoggerRegistry::instance().unregisterLogger("to_remove");

        EXPECT_FALSE(isRegistered("to_remove"));
    }

    TEST_F(LoggerRegistryTest, UnregisterLoggerMakesNextAccessCreateFreshLogger)
    {
        LoggerRegistry::instance().getLogger("recycled").setLevel(LogLevel::Fatal);

        LoggerRegistry::instance().unregisterLogger("recycled");

        const Logger &rebuilt = LoggerRegistry::instance().getLogger("recycled");
        EXPECT_EQ(rebuilt.name(), "recycled");
        EXPECT_EQ(rebuilt.getLevel(), LogLevel::Trace);
    }

    TEST_F(LoggerRegistryTest, UnregisterLoggerIgnoresUnknownName)
    {
        LoggerRegistry::instance().getLogger("untouched");

        EXPECT_NO_THROW(LoggerRegistry::instance().unregisterLogger("never_registered"));

        const std::vector<std::string> expected{"untouched"};
        EXPECT_EQ(sortedNames(LoggerRegistry::instance().getLoggerNames()), expected);
    }

    // ============================================================================
    // 查询与遍历
    // ============================================================================

    TEST_F(LoggerRegistryTest, GetLoggerNamesIsEmptyForFreshRegistry)
    {
        EXPECT_TRUE(LoggerRegistry::instance().getLoggerNames().empty());
    }

    TEST_F(LoggerRegistryTest, GetLoggerNamesListsEveryRegisteredName)
    {
        LoggerRegistry::instance().getLogger("alpha");
        LoggerRegistry::instance().getLogger("beta");
        LoggerRegistry::instance().getRootLogger();
        LoggerRegistry::instance().registerLogger(std::make_unique<Logger>("gamma"));

        const std::vector<std::string> expected{"alpha", "beta", "gamma", "root"};

        EXPECT_EQ(sortedNames(LoggerRegistry::instance().getLoggerNames()), expected);
    }

    TEST_F(LoggerRegistryTest, ClearRemovesEveryLogger)
    {
        LoggerRegistry::instance().getLogger("temp_one");
        LoggerRegistry::instance().getLogger("temp_two");

        LoggerRegistry::instance().clear();

        EXPECT_TRUE(LoggerRegistry::instance().getLoggerNames().empty());
    }

    /**
     * @brief 退休只保留「裸引用不悬垂」这一项保证：Sink 当场交还，不必等 purge
     * @details 退休表存在的理由是 getLogger() 给出裸引用；而 Sink 带着文件句柄与后台线程，
     *          在退休表里多留一秒就多占一秒。在途写入者靠 Sink 快照的强引用自保，与退休表无关。
     * @note 这条取代旧断言「clear() 之后 Sink 仍存活、要 purge 才释放」——那条把「日志器还活着」
     *       与「Sink 还活着」用同一个计数钉住了，而契约需要的只有前者。
     */
    TEST_F(LoggerRegistryTest, RetiringALoggerReleasesItsSinksImmediately)
    {
        auto logger = std::make_unique<Logger>("retire_probe");
        logger->addSink(std::make_unique<CountingSink>());
        LoggerRegistry::instance().registerLogger(std::move(logger));

        Logger &inFlightReference = LoggerRegistry::instance().getLogger("retire_probe");
        ASSERT_EQ(CountingSink::s_aliveCount.load(), 1);

        LoggerRegistry::instance().unregisterLogger("retire_probe");
        EXPECT_EQ(CountingSink::s_aliveCount.load(), 0) << "Sink 该在退休那一刻交还：句柄与线程不该等 purge";

        // 旧裸引用照样能用（对象还活在退休表里），只是这份日志到不了任何 Sink
        EXPECT_NO_THROW(inFlightReference.log(LogLevel::Info, "written after retirement"));
        EXPECT_EQ(CountingSink::s_aliveCount.load(), 0);

        LoggerRegistry::instance().purgeRetiredLoggers();
    }

    /**
     * @brief 写入者已经进到 Sink 里时，替换不能把那份 Sink 从脚下抽走
     * @details 交还 Sink 只是丢掉注册表这一份强引用；写入者栈上的快照引用必须活到 write() 返回。
     * @note 重叠由 promise/future 自己造出来：等不到「已进入 write()」就判失败，不赌调度运气。
     */
    TEST_F(LoggerRegistryTest, ReplacingLoggerKeepsTheSinkAliveUntilInFlightWriteReturns)
    {
        auto    blockingSink     = std::make_unique<BlockingSink>();
        auto *const blockingSinkHandle = blockingSink.get();

        auto logger = std::make_unique<Logger>("in_flight");
        logger->addSink(std::move(blockingSink));
        LoggerRegistry::instance().registerLogger(std::move(logger));

        Logger &firstLogger = LoggerRegistry::instance().getLogger("in_flight");
        std::thread writer([&firstLogger]
        {
            firstLogger.log(LogLevel::Info, "slow write in progress");
        });

        if (!blockingSinkHandle->waitForEnter())
        {
            // 没造出重叠：放行（万一它其实进了）再收线程，别让用例带着一个卡死的线程返回
            blockingSinkHandle->release();
            writer.join();
            FAIL() << "写入者没有在时限内进到 Sink 里，本用例的重叠点没构造出来";
        }
        ASSERT_EQ(BlockingSink::s_aliveCount.load(), 1);

        // 同名覆盖：注册表丢掉旧日志器并把它的 Sink 当场交还，但写入者还站在里面
        LoggerRegistry::instance().registerLogger(std::make_unique<Logger>("in_flight"));
        EXPECT_EQ(BlockingSink::s_aliveCount.load(), 1) << "在途写入者持有的快照引用必须顶住这一次交还";

        blockingSinkHandle->release();
        writer.join();
        EXPECT_EQ(BlockingSink::s_aliveCount.load(), 0) << "写入者收尾之后 Sink 才该真正销毁";

        LoggerRegistry::instance().purgeRetiredLoggers();
    }

    TEST_F(LoggerRegistryTest, ForEachLoggerVisitsEveryRegisteredLogger)
    {
        LoggerRegistry::instance().getLogger("iter_a");
        LoggerRegistry::instance().getLogger("iter_b");
        LoggerRegistry::instance().getRootLogger();

        std::vector<std::string> visited;
        LoggerRegistry::instance().forEachLogger([&visited](Logger &logger)
        {
            visited.push_back(logger.name());
        });

        const std::vector<std::string> expected{"iter_a", "iter_b", "root"};
        EXPECT_EQ(sortedNames(visited), expected);
    }

    TEST_F(LoggerRegistryTest, ForEachLoggerVisitsNothingOnEmptyRegistry)
    {
        size_t visitCount = 0;

        LoggerRegistry::instance().forEachLogger([&visitCount](Logger &)
        {
            ++visitCount;
        });

        EXPECT_EQ(visitCount, 0u);
    }

    TEST_F(LoggerRegistryTest, ForEachLoggerMutatesEveryVisitedLogger)
    {
        LoggerRegistry::instance().getLogger("mutate_a");
        LoggerRegistry::instance().getLogger("mutate_b");

        LoggerRegistry::instance().forEachLogger([](Logger &logger)
        {
            logger.setLevel(LogLevel::Warn);
        });

        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("mutate_a"), LogLevel::Warn);
        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("mutate_b"), LogLevel::Warn);
    }

    TEST_F(LoggerRegistryTest, ForEachLoggerAllowsReentrantRegistryAccess)
    {
        LoggerRegistry::instance().getLogger("reentrant_a");
        LoggerRegistry::instance().getLogger("reentrant_b");

        int visitedCount = 0;
        // 回调内再次访问注册表：遍历若持共享锁回调，shared_mutex 不可重入会自死锁
        EXPECT_NO_THROW(LoggerRegistry::instance().forEachLogger([&visitedCount](Logger &logger)
        {
            ++visitedCount;
            Logger &createdInCallback = LoggerRegistry::instance().getLogger("created_in_callback");
            createdInCallback.setLevel(LogLevel::Warn);
            logger.setLevel(LogLevel::Error);
        }));

        EXPECT_EQ(visitedCount, 2);
        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("reentrant_a"), LogLevel::Error);
        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("reentrant_b"), LogLevel::Error);
        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("created_in_callback"), LogLevel::Warn);
    }

    TEST_F(LoggerRegistryTest, ForEachLoggerSurvivesCallbackClearingTheRegistry)
    {
        LoggerRegistry::instance().getLogger("clear_victim_a");
        LoggerRegistry::instance().getLogger("clear_victim_b");

        // 最极端的重入：回调内清空注册表。快照持有强引用，因此遍历中的对象不会被销毁
        int visitedCount = 0;
        EXPECT_NO_THROW(LoggerRegistry::instance().forEachLogger([&visitedCount](Logger &logger)
        {
            ++visitedCount;
            logger.setLevel(LogLevel::Fatal);
            LoggerRegistry::instance().clear();
        }));

        EXPECT_EQ(visitedCount, 2);
        EXPECT_TRUE(LoggerRegistry::instance().getLoggerNames().empty());
    }

    // ============================================================================
    // 运行时等级开关：setLoggerLevel / loggerLevel / setGlobalLevel
    // ============================================================================

    TEST_F(LoggerRegistryTest, SetLoggerLevelChangesExistingLoggerLevel)
    {
        LoggerRegistry::instance().getLogger("runtime").setLevel(LogLevel::Trace);

        LoggerRegistry::instance().setLoggerLevel("runtime", LogLevel::Debug);

        EXPECT_EQ(LoggerRegistry::instance().getLogger("runtime").getLevel(), LogLevel::Debug);
    }

    TEST_F(LoggerRegistryTest, SetLoggerLevelCreatesLoggerWhenNameIsUnknown)
    {
        EXPECT_FALSE(LoggerRegistry::instance().loggerLevel("created_by_switch").has_value());

        LoggerRegistry::instance().setLoggerLevel("created_by_switch", LogLevel::Error);

        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("created_by_switch"), LogLevel::Error);
        EXPECT_EQ(LoggerRegistry::instance().getLogger("created_by_switch").name(), "created_by_switch");
    }

    TEST_F(LoggerRegistryTest, SetLoggerLevelTargetsRootLoggerByName)
    {
        LoggerRegistry::instance().setLoggerLevel("root", LogLevel::Fatal);

        EXPECT_EQ(LoggerRegistry::instance().getRootLogger().getLevel(), LogLevel::Fatal);
    }

    TEST_F(LoggerRegistryTest, LoggerLevelReturnsLevelOfRegisteredLogger)
    {
        LoggerRegistry::instance().getLogger("probe").setLevel(LogLevel::Warn);

        const std::optional<LogLevel> level = LoggerRegistry::instance().loggerLevel("probe");

        ASSERT_TRUE(level.has_value());
        EXPECT_EQ(level.value(), LogLevel::Warn);
    }

    TEST_F(LoggerRegistryTest, LoggerLevelReturnsNulloptForUnknownName)
    {
        LoggerRegistry::instance().getLogger("existing");

        EXPECT_FALSE(LoggerRegistry::instance().loggerLevel("missing").has_value());

        const std::vector<std::string> expected{"existing"};
        EXPECT_EQ(sortedNames(LoggerRegistry::instance().getLoggerNames()), expected);
    }

    TEST_F(LoggerRegistryTest, LoggerLevelReflectsLoggerRegisteredExternally)
    {
        auto external = std::make_unique<Logger>("adopted");
        external->setLevel(LogLevel::Debug);
        LoggerRegistry::instance().registerLogger(std::move(external));

        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("adopted"), LogLevel::Debug);
    }

    TEST_F(LoggerRegistryTest, SetGlobalLevelAppliesToEveryRegisteredLogger)
    {
        LoggerRegistry::instance().getLogger("global_a").setLevel(LogLevel::Trace);
        LoggerRegistry::instance().getLogger("global_b").setLevel(LogLevel::Fatal);
        LoggerRegistry::instance().getRootLogger();

        LoggerRegistry::instance().setGlobalLevel(LogLevel::Error);

        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("global_a"), LogLevel::Error);
        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("global_b"), LogLevel::Error);
        EXPECT_EQ(LoggerRegistry::instance().loggerLevel("root"), LogLevel::Error);
    }

    TEST_F(LoggerRegistryTest, SetGlobalLevelDoesNotCreateLoggerOnEmptyRegistry)
    {
        EXPECT_NO_THROW(LoggerRegistry::instance().setGlobalLevel(LogLevel::Warn));

        EXPECT_TRUE(LoggerRegistry::instance().getLoggerNames().empty());
    }

    TEST_F(LoggerRegistryTest, SetGlobalLevelSuppressesLowerLevelEvents)
    {
        Logger &logger = LoggerRegistry::instance().getLogger("filtered_by_global");
        logger.setLevel(LogLevel::Trace);
        ASSERT_TRUE(logger.shouldLog(LogLevel::Debug));

        LoggerRegistry::instance().setGlobalLevel(LogLevel::Error);

        EXPECT_FALSE(logger.shouldLog(LogLevel::Debug));
        EXPECT_TRUE(logger.shouldLog(LogLevel::Error));
    }

    // ============================================================================
    // 并发与压力
    // ============================================================================

    TEST_F(LoggerRegistryTest, ConcurrentGetLoggerCreatesOneLoggerPerName)
    {
        constexpr int            kthreadCount = 8;
        constexpr int            kiterations  = 100;
        std::vector<std::thread> workers;
        workers.reserve(kthreadCount);

        for (int threadIndex = 0; threadIndex < kthreadCount; ++threadIndex)
        {
            workers.emplace_back([kiterations]
            {
                for (int iteration = 0; iteration < kiterations; ++iteration)
                {
                    LoggerRegistry::instance().getLogger("shared_name").log(LogLevel::Debug, "concurrent probe");
                }
            });
        }
        for (std::thread &worker: workers)
        {
            worker.join();
        }

        const std::vector<std::string> expected{"shared_name"};
        EXPECT_EQ(sortedNames(LoggerRegistry::instance().getLoggerNames()), expected);
    }

    TEST_F(LoggerRegistryTest, RapidRegisterAndUnregisterCyclesKeepRegistryConsistent)
    {
        for (int cycle = 0; cycle < 1000; ++cycle)
        {
            const std::string name = "cycle_" + std::to_string(cycle);
            LoggerRegistry::instance().registerLogger(std::make_unique<Logger>(name));
            LoggerRegistry::instance().unregisterLogger(name);
        }

        EXPECT_TRUE(LoggerRegistry::instance().getLoggerNames().empty());
    }
} // namespace AsynGyanis::Base
