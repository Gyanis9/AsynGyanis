#include "Base/Log/LoggerConfigLoader.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/FileSink.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/RollingFileSink.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/ProcessInfo.h"

#include <algorithm>
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

        /**
         * @brief 取带默认值的可选字段；键在而类型不符时先报再回落
         * @details 「键不存在」走默认值是正常路径，不报；「键存在而类型不符」多半是 YAML 里给
         *          数字或布尔加了引号，静默按默认值生效会让配置与生效值长期不一致而无人知道。
         *          与本文件对 overflow_policy、max_size_mb 的口径一致：容错必须可见。
         * @tparam ValueType 期望取值类型
         * @param configuration 承载该键的配置对象
         * @param key 键名
         * @param defaultValue 类型不符或键缺失时的回落值
         * @param ownerDescription 诊断里指认这是哪个 sink 的字段
         * @return ValueType 取到的值或回落值
         */
        template<typename ValueType>
        [[nodiscard]] ValueType optionalFieldWithDiagnosis(const ConfigValue &configuration, const std::string_view key,
                                                           ValueType defaultValue, const std::string_view ownerDescription)
        {
            const auto iterator = configuration.find(key);
            if (iterator == configuration.end())
            {
                return defaultValue;
            }
            const std::optional<ValueType> parsed = configValueAs<ValueType>(*iterator);
            if (!parsed.has_value())
            {
                std::cerr << "LoggerConfig：" << ownerDescription << " 的 " << key << " 类型是 "
                        << typeName((*iterator).type()) << "，要求 " << configTypeNameOf<ValueType>()
                        << "，已按默认值 " << std::boolalpha << defaultValue << std::noboolalpha << " 处理" << '\n';
                return defaultValue;
            }
            return *parsed;
        }

        /**
         * @brief 取必填的字符串字段，并区分「键缺失」与「类型不符」两种失败
         * @details 两者的处置相同（跳过这个 sink），但原因必须分开说：原先一律报「缺少字段」，
         *          而把 `path: 2026.log` 这类写成不带引号的数字时，字段明明在、报的却是缺失，
         *          运维照着提示补键反而补不对。
         * @param configuration 承载该键的配置对象
         * @param key 键名
         * @param ownerDescription 诊断里指认这是哪个 sink 的字段
         * @return std::optional<std::string> 取到的文本，失败时为空（原因已报出）
         */
        [[nodiscard]] std::optional<std::string> requiredStringField(const ConfigValue &configuration, const std::string_view key,
                                                                     const std::string_view ownerDescription)
        {
            const auto iterator = configuration.find(key);
            if (iterator == configuration.end())
            {
                std::cerr << "LoggerConfig：" << ownerDescription << " 缺少 '" << key << "' 字段，已跳过该 sink" << '\n';
                return std::nullopt;
            }
            const std::optional<std::string> value = configValueAs<std::string>(*iterator);
            if (!value.has_value())
            {
                std::cerr << "LoggerConfig：" << ownerDescription << " 的 '" << key << "' 类型是 "
                        << typeName((*iterator).type()) << "，要求 " << configTypeNameOf<std::string>() << "，已跳过该 sink" << '\n';
            }
            return value;
        }

        /**
         * @brief 由 UTF-8 文本构造路径对象，并让相对路径落在基准目录下
         * @details 基准目录为空时回落到可执行文件所在目录，避免依赖进程工作目录。
         *          全程走 path 对象：中途 `.string()` 会经过本地代码页，落在代码页外的字符
         *          （emoji、非本机文字）会被换成 '?'，日志就此写到改了名的文件上。
         * @param utf8Path 配置里读到的 UTF-8 路径文本
         * @param baseDirectory 相对路径的基准目录，可为空
         * @return std::filesystem::path 解析后的路径
         */
        [[nodiscard]] std::filesystem::path resolveLogPath(const std::string &utf8Path, const std::filesystem::path &baseDirectory)
        {
            const std::filesystem::path basePath = baseDirectory.empty()
                                                       ? AsynGyanis::Platform::ProcessInfo::applicationDirectory()
                                                       : baseDirectory;
            std::filesystem::path resolvedPath = AsynGyanis::Platform::FileSystem::pathFromUtf8(utf8Path);
            if (resolvedPath.is_relative())
            {
                resolvedPath = basePath / resolvedPath;
            }
            return resolvedPath;
        }
    } // namespace

    void LoggerConfigLoader::loadFromConfig(const std::string &configurationPrefix, const std::filesystem::path &baseDirectory)
    {
        const auto &configuration = ConfigManager::instance();

        const std::string globalLevelKey = configurationPrefix + ".global_level";

        // 键不存在时按 INFO 是正常路径；存在但不是字符串（YAML 里写成不带引号的数字、或整段漏了
        // 缩进被解析成列表）原先会静默按 INFO 生效——「明明配了等级却没生效」是这里最难查的一类
        // 现场，因此按 sinks 各字段的同一口径报出实际类型再回落
        const std::optional<ConfigValue> globalLevelValue = configuration.getOptional(globalLevelKey);
        LogLevel                        defaultLevel      = LogLevel::Info;
        if (globalLevelValue.has_value())
        {
            if (const auto globalLevel = configValueAs<std::string>(*globalLevelValue); globalLevel.has_value())
            {
                defaultLevel = logLevelFromString(*globalLevel);
            } else
            {
                std::cerr << "LoggerConfig：" << globalLevelKey << " 类型是 " << typeName(globalLevelValue->type())
                        << "，要求 " << configTypeNameOf<std::string>() << "，已按 INFO 处理" << '\n';
            }
        }

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
            // 本文件的口径是「容错必须可见」：少写一个 '-'（`- type: file` 变成 `- console`，或整条
            // 被写成标量）时这个 sink 就是不成立了，但一声不吭地跳过后，运维看到的是一份
            // 「配置里有两条 sink、实际只挂上一条」的现场——与其余拒绝路径同样报出形态与原因
            std::cerr << "LoggerConfig：sink 配置不是对象（实际是 " << typeName(sinkConfiguration.type())
                    << "），已跳过该 sink；每条 sink 要写成 '- type: ...' 加它的字段" << '\n';
            return nullptr;
        }

        // 必填键先按「键缺失」与「类型不符」分别报出，再由下面统一跳过：加载器不让任何异常
        // 逃出这条路径去打断整份配置，也不能把「类型不符」说成「缺少字段」
        const std::optional<std::string> typeOptional = requiredStringField(sinkConfiguration, "type", "sink");
        if (!typeOptional.has_value())
        {
            return nullptr;
        }
        const std::string &type = *typeOptional;

        std::unique_ptr<LogSink> sink;

        if (type == "console")
        {
            const bool color = optionalFieldWithDiagnosis(sinkConfiguration, "color", true, "console sink");
            sink             = std::make_unique<ConsoleSink>(color);
        } else if (type == "file")
        {
            const std::optional<std::string> pathOptional = requiredStringField(sinkConfiguration, "path", "file sink");
            if (!pathOptional.has_value())
            {
                return nullptr;
            }
            // 相对路径基于基准目录解析，避免依赖进程工作目录
            const std::filesystem::path filePath = resolveLogPath(*pathOptional, baseDirectory);
            const bool                  truncate = optionalFieldWithDiagnosis(sinkConfiguration, "truncate", false, "file sink");
            // 路径文本按 UTF-8 报：`path::string()` 走本地代码页，代码页外的字符直接抛出，
            // 一条「日志写到哪了」的说明不该把整次装配打断
            LOG_INFO_FMT("文件日志输出：{}", AsynGyanis::Platform::FileSystem::utf8FromPath(filePath));
            sink = std::make_unique<FileSink>(filePath, truncate);
        } else if (type == "rolling_file")
        {
            const std::optional<std::string> baseOptional = requiredStringField(sinkConfiguration, "base_filename", "rolling_file sink");
            if (!baseOptional.has_value())
            {
                return nullptr;
            }
            // 目录全程按 path 传递：中途落成 std::string 会经过本地代码页，落在代码页之外的字符
            // （emoji、非本机文字）会被换成 '?'，日志就此写到改了名的文件上
            const std::filesystem::path logDirectory
                    = resolveLogPath(optionalFieldWithDiagnosis<std::string>(sinkConfiguration, "directory", std::string{"logs"},
                                                                            "rolling_file sink"),
                                     baseDirectory);
            const std::string policyName = optionalFieldWithDiagnosis<std::string>(sinkConfiguration, "policy", std::string{"size"},
                                                                                   "rolling_file sink");

            RollingPolicy policy;
            if (policyName == "size")
                policy = RollingPolicy::Size;
            else if (policyName == "daily")
                policy = RollingPolicy::Daily;
            else if (policyName == "hourly")
                policy = RollingPolicy::Hourly;
            else
            {
                // 与 overflow_policy 同一口径：回退本身可以，但必须说得出「拼错了」
                std::cerr << "LoggerConfig：rolling_file sink 的 policy='" << policyName
                        << "' 非法（只支持 size / daily / hourly），已按 size 处理" << '\n';
                policy = RollingPolicy::Size;
            }

            const int64_t configuredMaximumSizeMb = optionalFieldWithDiagnosis(sinkConfiguration, "max_size_mb", int64_t{10},
                                                                               "rolling_file sink");
            // 边界钳制：0（或负数）会让「已写字节 >= 上限」恒真，退化成每写一行就滚动一次——
            // 每次滚动都要重开文件并整目录扫描备份，日志系统会反过来把进程拖垮。这里钳到 1 MB 并给出诊断。
            // 上界 1 TiB 挡的是另一半：MB 数乘 1024*1024 时若在 size_t 里回绕，会得到一个极小的
            // 阈值，症状与「填 0」完全相同，而报出来的配置却大得离谱
            constexpr int64_t    maximumSizeMbLimit = 1024LL * 1024LL;
            const int64_t        clampedMaximumSizeMb
                    = std::clamp(configuredMaximumSizeMb, int64_t{1}, maximumSizeMbLimit);
            if (clampedMaximumSizeMb != configuredMaximumSizeMb)
            {
                std::cerr << "LoggerConfig：rolling sink 的 max_size_mb=" << configuredMaximumSizeMb
                        << " 非法（要求 1~" << maximumSizeMbLimit << "），已钳制为 " << clampedMaximumSizeMb
                        << "；单个日志文件按钳制后的大小滚动" << '\n';
            }
            const size_t maximumSizeBytes = static_cast<size_t>(clampedMaximumSizeMb) * 1024 * 1024;

            // 边界钳制到 [0, kMaximumBackupFileCount]，与 RollingFileSink 构造里的同一道夹取共用
            // 一个常量（两处解析必须一致，否则诊断报的上限与实际生效的上限会各说一遍）。
            // 下界：负数强转成 size_t 会成为 SIZE_MAX，等于「备份一个都不删」——与「限制备份数量」
            // 的意图正好相反，日志目录无界增长。上界：这个值决定每次滚动要顺移多少个序号，
            // 填成天文数字就是让滚动握着本 Sink 的锁做上千万次目录项查询，日志系统反过来拖垮进程。
            // 0 是合法值：不保留任何备份
            const int64_t     configuredMaximumBackupCount = optionalFieldWithDiagnosis(sinkConfiguration, "max_backup", int64_t{10},
                                                                                        "rolling_file sink");
            constexpr int64_t maximumBackupLimit           = static_cast<int64_t>(RollingFileSink::kMaximumBackupFileCount);
            const int64_t     clampedMaximumBackupCount    = std::clamp(configuredMaximumBackupCount, int64_t{0}, maximumBackupLimit);
            if (clampedMaximumBackupCount != configuredMaximumBackupCount)
            {
                std::cerr << "LoggerConfig：rolling sink 的 max_backup=" << configuredMaximumBackupCount
                        << " 非法（要求 0~" << maximumBackupLimit << "），已钳制为 " << clampedMaximumBackupCount
                        << "；请检查该键是否多打了一位数字，日志保留份数按钳制后的值生效" << '\n';
            }
            const size_t maximumBackupCount = static_cast<size_t>(clampedMaximumBackupCount);

            // 基础文件名同样按 UTF-8 解成 path 再交给 Sink：直接交窄串，Sink 内部与目录拼接时
            // 会过一遍本地代码页，代码页外的字符变成 '?'，滚动日志就此写到改了名的文件上
            sink = std::make_unique<RollingFileSink>(AsynGyanis::Platform::FileSystem::pathFromUtf8(*baseOptional), logDirectory, policy,
                                                     maximumSizeBytes, maximumBackupCount);
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

            const int64_t configuredQueueSize = optionalFieldWithDiagnosis(sinkConfiguration, "queue_size", int64_t{1024},
                                                                           "async sink");
            // 边界钳制两端都要，与 max_size_mb / max_backup 同一口径：
            // 下界——queue_size 为 0（或负数）会让 AsyncSink 的三种策略全部退化（Drop 全丢、
            // DropOldest 对空队列 pop 是未定义行为、Block 永久阻塞）；
            // 上界——一条事件在队列里占 sizeof(LogEvent) 字节，容量乘过去就是下游卡住时最多占住的
            // 内存，多打几个 0 会把「按策略丢弃或阻塞」的背压变成 OOM。
            // AsyncSink 构造里还有同样一对兜底钳制，两处共用它声明的这两个常量
            constexpr int64_t minimumQueueSizeLimit = static_cast<int64_t>(AsyncSink::kMinimumQueueSize);
            constexpr int64_t maximumQueueSizeLimit = static_cast<int64_t>(AsyncSink::kMaximumQueueSize);
            const int64_t     clampedQueueSize      = std::clamp(configuredQueueSize, minimumQueueSizeLimit, maximumQueueSizeLimit);
            if (clampedQueueSize != configuredQueueSize)
            {
                std::cerr << "LoggerConfig：async sink 的 queue_size=" << configuredQueueSize
                        << " 非法（要求 " << minimumQueueSizeLimit << "~" << maximumQueueSizeLimit
                        << "），已钳制为 " << clampedQueueSize << "；队列容量按钳制后的值生效" << '\n';
            }
            const std::string overflowPolicyName = optionalFieldWithDiagnosis<std::string>(sinkConfiguration, "overflow_policy",
                                                                                            std::string{"block"}, "async sink");
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

            sink = std::make_unique<AsyncSink>(std::move(wrappedSink), static_cast<size_t>(clampedQueueSize), overflowPolicy);
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
