/**
 * @file TestLoggerRegistry.cpp
 * @brief LoggerRegistry 单元测试：单例、按名获取与注册、遍历与运行时等级开关
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"

#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        /**
         * @brief 按字典序排序日志器名称列表，便于与期望列表整体比较
         * @param names 待排序的名称列表（按值接收）
         * @return std::vector<std::string> 排序后的名称列表
         */
        std::vector<std::string> sortedNames(std::vector<std::string> names)
        {
            std::ranges::sort(names);
            return names;
        }

        /**
         * @brief 判断注册表中是否存在指定名称的日志器
         * @param name 待查找的日志器名称
         * @return true 存在
         */
        bool isRegistered(const std::string &name)
        {
            const std::vector<std::string> names = LoggerRegistry::instance().getLoggerNames();
            return std::ranges::find(names, name) != names.end();
        }
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
