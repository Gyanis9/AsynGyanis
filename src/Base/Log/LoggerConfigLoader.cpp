#include "Base/Log/LoggerConfigLoader.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/FileSink.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/RollingFileSink.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/ProcessInfo.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    void LoggerConfigLoader::loadFromConfig(const std::string &configurationPrefix, const std::filesystem::path &baseDirectory)
    {
        const auto &configuration = ConfigManager::instance();

        const std::string globalLevelKey = configurationPrefix + ".global_level";

        const auto globalLevel  = configuration.get<std::string>(globalLevelKey, "INFO");
        const auto defaultLevel = logLevelFromString(globalLevel);

        // ConfigManager 使用扁平化键存储，此处从 key 前缀提取 logger 名。
        const std::string     loggerPrefix = configurationPrefix + ".loggers.";
        std::set<std::string> loggerNames;
        for (const auto &key: configuration.keys())
        {
            // C++20 起用 starts_with 表达「前缀匹配」，语义比 rfind(..., 0) == 0 直白
            if (!key.starts_with(loggerPrefix))
            {
                continue;
            }

            const auto nameStart   = loggerPrefix.size();
            const auto dotPosition = key.find('.', nameStart);
            if (const auto name = key.substr(nameStart, dotPosition == std::string::npos ? std::string::npos : dotPosition - nameStart); !name.empty())
            {
                loggerNames.insert(name);
            }
        }

        if (loggerNames.empty())
        {
            // 无 loggers 配置，创建默认 root logger
            auto &root = LoggerRegistry::instance().getRootLogger();
            root.clearSinks();
            root.setLevel(defaultLevel);
            root.addSink(std::make_unique<ConsoleSink>(true));
            return;
        }

        for (const auto &name: loggerNames)
        {
            auto &logger = LoggerRegistry::instance().getLogger(name);

            ConfigObject      loggerConfigurationObject;
            const std::string loggerConfigurationKey = loggerPrefix + name;
            if (const auto levelOptional = configuration.getOptional(loggerConfigurationKey + ".level"); levelOptional.has_value())
            {
                loggerConfigurationObject.emplace("level", *levelOptional);
            }
            if (const auto sinksOptional = configuration.getOptional(loggerConfigurationKey + ".sinks"); sinksOptional.has_value())
            {
                loggerConfigurationObject.emplace("sinks", *sinksOptional);
            }

            logger.setLevel(defaultLevel);
            applyLoggerConfig(logger, ConfigValue(std::move(loggerConfigurationObject)), baseDirectory);
        }
    }

    void LoggerConfigLoader::applyLoggerConfig(Logger &logger, const ConfigValue &loggerConfiguration, const std::filesystem::path &baseDirectory)
    {
        logger.clearSinks();

        // 设置等级
        if (loggerConfiguration.contains("level"))
        {
            const auto levelString = loggerConfiguration["level"].as<std::string>();
            logger.setLevel(logLevelFromString(levelString));
        }

        // 设置 Sinks
        if (loggerConfiguration.contains("sinks"))
        {
            for (const auto &sinksArray = loggerConfiguration["sinks"].as<ConfigArray>(); const auto &sinkConfiguration: sinksArray)
            {
                if (auto sink = createSinkFromConfig(sinkConfiguration, baseDirectory))
                {
                    logger.addSink(std::move(sink));
                }
            }
        }
    }

    std::unique_ptr<LogSink> LoggerConfigLoader::createSinkFromConfig(const ConfigValue &sinkConfiguration, const std::filesystem::path &baseDirectory)
    {
        if (!sinkConfiguration.is<ConfigObject>())
        {
            return nullptr;
        }

        // 安全获取 type 字段，避免 operator[] 抛出异常中断整个配置加载
        const auto typeOptional = sinkConfiguration.get<std::string>("type");
        if (!typeOptional.has_value())
        {
            std::cerr << "LoggerConfig：sink 缺少 'type' 字段，已跳过" << '\n';
            return nullptr;
        }
        const std::string &type = typeOptional.value();

        std::unique_ptr<LogSink> sink;

        if (type == "console")
        {
            const bool color = sinkConfiguration.get<bool>("color").value_or(true);
            sink             = std::make_unique<ConsoleSink>(color);
        } else if (type == "file")
        {
            const auto pathOptional = sinkConfiguration.get<std::string>("path");
            if (!pathOptional.has_value())
            {
                std::cerr << "LoggerConfig：file sink 缺少 'path'，已跳过" << '\n';
                return nullptr;
            }
            // 相对路径基于基准目录解析，避免依赖进程工作目录
            const std::filesystem::path basePath = baseDirectory.empty()
                                                       ? AsynGyanis::Platform::ProcessInfo::applicationDirectory()
                                                       : baseDirectory;
            std::filesystem::path filePath = AsynGyanis::Platform::FileSystem::pathFromUtf8(pathOptional.value());
            if (filePath.is_relative())
            {
                filePath = basePath / filePath;
            }
            const bool truncate = sinkConfiguration.get<bool>("truncate").value_or(false);
            LOG_INFO_FMT("文件日志输出：{}", filePath.string());
            sink = std::make_unique<FileSink>(filePath, truncate);
        } else if (type == "rolling_file")
        {
            const auto baseOptional = sinkConfiguration.get<std::string>("base_filename");
            if (!baseOptional.has_value())
            {
                std::cerr << "LoggerConfig：rolling_file sink 缺少 'base_filename'，已跳过" << '\n';
                return nullptr;
            }
            // 相对目录基于基准目录解析，避免依赖进程工作目录
            const std::filesystem::path basePath = baseDirectory.empty()
                                                       ? AsynGyanis::Platform::ProcessInfo::applicationDirectory()
                                                       : baseDirectory;
            std::filesystem::path logDirectory = AsynGyanis::Platform::FileSystem::pathFromUtf8(
                    sinkConfiguration.get<std::string>("directory").value_or("logs"));
            if (logDirectory.is_relative())
            {
                logDirectory = basePath / logDirectory;
            }
            const std::string directory  = logDirectory.string();
            const std::string policyName = sinkConfiguration.get<std::string>("policy").value_or("size");

            RollingPolicy policy;
            if (policyName == "size")
                policy = RollingPolicy::Size;
            else if (policyName == "daily")
                policy = RollingPolicy::Daily;
            else if (policyName == "hourly")
                policy = RollingPolicy::Hourly;
            else
                policy = RollingPolicy::Size;

            const size_t maximumSizeBytes   = sinkConfiguration.get<int64_t>("max_size_mb").value_or(10) * 1024 * 1024;
            const size_t maximumBackupCount = sinkConfiguration.get<int64_t>("max_backup").value_or(10);

            sink = std::make_unique<RollingFileSink>(baseOptional.value(), directory, policy, maximumSizeBytes, maximumBackupCount);
        } else if (type == "async")
        {
            if (!sinkConfiguration.contains("wrapped"))
            {
                std::cerr << "LoggerConfig：async sink 缺少 'wrapped'，已跳过" << '\n';
                return nullptr;
            }
            auto wrappedSink = createSinkFromConfig(sinkConfiguration["wrapped"], baseDirectory);
            if (!wrappedSink)
                return nullptr;

            const int64_t configuredQueueSize = sinkConfiguration.get<int64_t>("queue_size").value_or(1024);
            // 配置边界钳制：queue_size 为 0（或负数）会让 AsyncSink 的三种策略全部退化——
            // Drop 丢弃全部事件、DropOldest 对空队列 pop（未定义行为）、Block 永久阻塞。
            // 这里钳到 AsyncSink 声明的最小容量并给出可见诊断，AsyncSink 内部还有一次兜底钳制
            auto          queueSize           = static_cast<size_t>(configuredQueueSize);
            if (configuredQueueSize < static_cast<int64_t>(AsyncSink::kMinimumQueueSize))
            {
                std::cerr << "LoggerConfig：async sink 的 queue_size=" << configuredQueueSize
                        << " 非法（要求 >= " << AsyncSink::kMinimumQueueSize << "），已钳制为 "
                        << AsyncSink::kMinimumQueueSize << '\n';
                queueSize = AsyncSink::kMinimumQueueSize;
            }
            const std::string overflowPolicyName = sinkConfiguration.get<std::string>("overflow_policy").value_or("block");
            // overflow_policy 支持 block / drop / drop_oldest 三种取值，非法值回退为 block
            auto              overflowPolicy     = AsyncSink::OverflowPolicy::Block;
            if (overflowPolicyName == "drop")
            {
                overflowPolicy = AsyncSink::OverflowPolicy::Drop;
            } else if (overflowPolicyName == "drop_oldest")
            {
                overflowPolicy = AsyncSink::OverflowPolicy::DropOldest;
            }

            sink = std::make_unique<AsyncSink>(std::move(wrappedSink), queueSize, overflowPolicy);
        } else
        {
            // 模块初始化阶段日志系统可能尚未就绪，使用 std::cerr
            std::cerr << "LoggerConfig：未知的 sink 类型 '" << type << "'，已跳过" << '\n';
            return nullptr;
        }

        if (sink && sinkConfiguration.contains("level"))
        {
            const auto levelString = sinkConfiguration["level"].as<std::string>();
            sink->setLevel(logLevelFromString(levelString));
        }

        return sink;
    }
} // namespace AsynGyanis::Base
