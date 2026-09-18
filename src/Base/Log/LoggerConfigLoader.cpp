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
         * @details 与 configValueAs 同一口径：不做跨类型转换，"true" 不会当布尔用、数字不会当字符串取。
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

        // 配置以扁平键存储，具名 logger 由前缀下的键推导
        const std::string     loggerPrefix = configurationPrefix + ".loggers.";
        std::set<std::string> loggerNames;
        for (const auto &key: configuration.keys())
        {
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

        // 没人显式配 root 时也要按 global_level 装上控制台 sink：只配了具名 logger 的部署里，
        // 框架自身那些走 root 的日志会停在「无 sink」的默认状态上被静默丢掉，而那正是最需要
        // 看到的诊断。显式配了 root 的走下面具名 logger 那条路
        if (!loggerNames.contains("root"))
        {
            auto &root = LoggerRegistry::instance().getRootLogger();
            root.clearSinks();
            root.setLevel(defaultLevel);
            root.addSink(std::make_unique<ConsoleSink>(true));
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
        // 先校验完取值形态再动 sink：类型不符时当场诊断并保留原有 sink（清空后才发现不符
        // 会让该 logger 此后静默丢日志）
        const auto levelText     = configValueAt<std::string>(loggerConfiguration, "level");
        const auto sinksArrayOpt = configValueAt<ConfigArray>(loggerConfiguration, "sinks");
        if (loggerConfiguration.contains("level") && !levelText.has_value())
        {
            std::cerr << "LoggerConfig：logger 的 level 不是字符串，已忽略该字段" << '\n';
        }
        if (loggerConfiguration.contains("sinks") && !sinksArrayOpt.has_value())
        {
            std::cerr << "LoggerConfig：logger 的 sinks 不是列表（YAML 里每条前要加 '-'），已忽略该字段" << '\n';
        }

        if (levelText.has_value())
        {
            logger.setLevel(logLevelFromString(*levelText));
        }

        // 清空只发生在「这份配置确实带了一份可用的 sinks 列表」时：字段缺失或类型不符都算
        // 「没说要换 sink」，此时清空会让该 logger 此后静默丢日志，而诊断却说「已忽略该字段」
        if (sinksArrayOpt.has_value())
        {
            logger.clearSinks();
            for (const auto &sinkConfiguration: *sinksArrayOpt)
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

            const int64_t configuredMaximumSizeMb = configValueAt<int64_t>(sinkConfiguration, "max_size_mb").value_or(10);
            // 边界钳制：0（或负数）会让「已写字节 >= 上限」恒真，退化成每写一行就滚动一次——
            // 每次滚动都要重开文件并整目录扫描备份，日志系统会反过来把进程拖垮。这里钳到 1 MB 并给出诊断
            if (configuredMaximumSizeMb < 1)
            {
                std::cerr << "LoggerConfig：rolling sink 的 max_size_mb=" << configuredMaximumSizeMb
                        << " 非法（要求 >= 1），已钳制为 1" << '\n';
            }
            const size_t maximumSizeBytes = static_cast<size_t>(configuredMaximumSizeMb < 1 ? 1 : configuredMaximumSizeMb) * 1024 * 1024;

            // 边界钳制：负数强转成 size_t 会成为 SIZE_MAX，等于「备份一个都不删」——
            // 与「限制备份数量」的意图正好相反，日志目录会无界增长。与 max_size_mb、queue_size
            // 两处同口径钳到合法下界并给出诊断（0 是合法值：不保留任何备份）
            const int64_t configuredMaximumBackupCount = configValueAt<int64_t>(sinkConfiguration, "max_backup").value_or(10);
            if (configuredMaximumBackupCount < 0)
            {
                std::cerr << "LoggerConfig：rolling sink 的 max_backup=" << configuredMaximumBackupCount
                        << " 非法（要求 >= 0），已钳制为 0" << '\n';
            }
            const size_t maximumBackupCount = static_cast<size_t>(configuredMaximumBackupCount < 0 ? 0 : configuredMaximumBackupCount);

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
            // overflow_policy 支持 block / drop / drop_oldest 三种取值，非法值回退为 block。
            // 回退是明示的（std::cerr 诊断）：静默回退会让「想写 drop 却拼错」的配置在高负载下
            // 阻塞调用线程，而运维以为它在丢日志
            auto              overflowPolicy     = AsyncSink::OverflowPolicy::Block;
            if (overflowPolicyName == "drop")
            {
                overflowPolicy = AsyncSink::OverflowPolicy::Drop;
            } else if (overflowPolicyName == "drop_oldest")
            {
                overflowPolicy = AsyncSink::OverflowPolicy::DropOldest;
            } else if (overflowPolicyName != "block")
            {
                std::cerr << "LoggerConfig：async sink 的 overflow_policy='" << overflowPolicyName
                        << "' 非法（只支持 block / drop / drop_oldest），已按 block 处理" << '\n';
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
            // 与上面同口径：形态不符只诊断并忽略，不让未声明的异常逃出配置加载
            if (const auto sinkLevelText = configValueAt<std::string>(sinkConfiguration, "level"); sinkLevelText.has_value())
            {
                sink->setLevel(logLevelFromString(*sinkLevelText));
            } else
            {
                std::cerr << "LoggerConfig：sink 的 level 不是字符串，已忽略该字段" << '\n';
            }
        }

        return sink;
    }
} // namespace AsynGyanis::Base
