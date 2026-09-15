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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 按键取配置值，键缺失或类型不符时返回空
         * @details 与 ConfigManager 的类型化取值同一口径（见 configValueAs）：不做跨类型转换，
         *          字符串 "true" 不会当布尔用、数字不会当字符串取。
         * @tparam ValueType 目标类型
         * @param configuration 承载该键的对象
         * @param key 键名
         * @return std::optional<ValueType> 取值或空
         */
        template<typename ValueType>
        [[nodiscard]] std::optional<ValueType> configValueAt(const ConfigValue &configuration, const std::string_view key)
        {
            const auto iterator = configuration.find(key);
            if (iterator == configuration.end())
            {
                return std::nullopt;
            }
            return configValueAs<ValueType>(*iterator);
        }
    } // namespace

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

        if (loggerConfiguration.contains("level"))
        {
            const auto levelString = loggerConfiguration.at("level").get<std::string>();
            logger.setLevel(logLevelFromString(levelString));
        }

        if (loggerConfiguration.contains("sinks"))
        {
            for (const auto &sinksArray = loggerConfiguration.at("sinks").get<ConfigArray>(); const auto &sinkConfiguration: sinksArray)
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
        if (!sinkConfiguration.is_object())
        {
            return nullptr;
        }

        // 安全获取 type 字段，避免取缺失键抛出异常中断整个配置加载
        const auto typeOptional = configValueAt<std::string>(sinkConfiguration, "type");
        if (!typeOptional.has_value())
        {
            std::cerr << "LoggerConfig：sink 缺少 'type' 字段，已跳过" << '\n';
            return nullptr;
        }
        const std::string &type = typeOptional.value();

        std::unique_ptr<LogSink> sink;

        if (type == "console")
        {
            const bool color = configValueAt<bool>(sinkConfiguration, "color").value_or(true);
            sink             = std::make_unique<ConsoleSink>(color);
        } else if (type == "file")
        {
            const auto pathOptional = configValueAt<std::string>(sinkConfiguration, "path");
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
            const bool truncate = configValueAt<bool>(sinkConfiguration, "truncate").value_or(false);
            LOG_INFO_FMT("文件日志输出：{}", filePath.string());
            sink = std::make_unique<FileSink>(filePath, truncate);
        } else if (type == "rolling_file")
        {
            const auto baseOptional = configValueAt<std::string>(sinkConfiguration, "base_filename");
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
                    configValueAt<std::string>(sinkConfiguration, "directory").value_or("logs"));
            if (logDirectory.is_relative())
            {
                logDirectory = basePath / logDirectory;
            }
            const std::string directory  = logDirectory.string();
            const std::string policyName = configValueAt<std::string>(sinkConfiguration, "policy").value_or("size");

            RollingPolicy policy;
            if (policyName == "size")
                policy = RollingPolicy::Size;
            else if (policyName == "daily")
                policy = RollingPolicy::Daily;
            else if (policyName == "hourly")
                policy = RollingPolicy::Hourly;
            else
                policy = RollingPolicy::Size;

            const size_t maximumSizeBytes   = configValueAt<int64_t>(sinkConfiguration, "max_size_mb").value_or(10) * 1024 * 1024;
            const size_t maximumBackupCount = configValueAt<int64_t>(sinkConfiguration, "max_backup").value_or(10);

            sink = std::make_unique<RollingFileSink>(baseOptional.value(), directory, policy, maximumSizeBytes, maximumBackupCount);
        } else if (type == "async")
        {
            if (!sinkConfiguration.contains("wrapped"))
            {
                std::cerr << "LoggerConfig：async sink 缺少 'wrapped'，已跳过" << '\n';
                return nullptr;
            }
            auto wrappedSink = createSinkFromConfig(sinkConfiguration.at("wrapped"), baseDirectory);
            if (!wrappedSink)
                return nullptr;

            const int64_t configuredQueueSize = configValueAt<int64_t>(sinkConfiguration, "queue_size").value_or(1024);
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
            const std::string overflowPolicyName = configValueAt<std::string>(sinkConfiguration, "overflow_policy").value_or("block");
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
            const auto levelString = sinkConfiguration.at("level").get<std::string>();
            sink->setLevel(logLevelFromString(levelString));
        }

        return sink;
    }
} // namespace AsynGyanis::Base
