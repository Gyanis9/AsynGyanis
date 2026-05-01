#include "LoggerConfig.h"

#include <iostream>
#include <set>

namespace Base
{
    void LoggerConfigLoader::loadFromConfig(const std::string &config_prefix)
    {
        const auto &cfg = ConfigManager::instance();

        const std::string global_level_key = config_prefix + ".global_level";

        const auto global_level  = cfg.get<std::string>(global_level_key, "INFO");
        const auto default_level = logLevelFromString(global_level);

        // ConfigManager 使用扁平化键存储，此处从 key 前缀提取 logger 名。
        const std::string     logger_prefix = config_prefix + ".loggers.";
        std::set<std::string> logger_names;
        for (const auto &key: cfg.keys())
        {
            if (key.rfind(logger_prefix, 0) != 0)
            {
                continue;
            }

            const auto name_start = logger_prefix.size();
            const auto dot_pos    = key.find('.', name_start);
            if (const auto name = key.substr(name_start, dot_pos == std::string::npos ? std::string::npos : dot_pos - name_start); !name.empty())
            {
                logger_names.insert(name);
            }
        }

        if (logger_names.empty())
        {
            // 无 loggers 配置，创建默认 root logger
            auto &root = LoggerRegistry::instance().getRootLogger();
            root.clearSinks();
            root.setLevel(default_level);
            root.addSink(std::make_unique<ConsoleSink>(true));
            return;
        }

        for (const auto &name: logger_names)
        {
            auto &logger = LoggerRegistry::instance().getLogger(name);

            ConfigObject logger_cfg_obj;
            const auto   base = logger_prefix + name;
            if (const auto level_opt = cfg.getOptional(base + ".level"); level_opt.has_value())
            {
                logger_cfg_obj.emplace("level", *level_opt);
            }
            if (const auto sinks_opt = cfg.getOptional(base + ".sinks"); sinks_opt.has_value())
            {
                logger_cfg_obj.emplace("sinks", *sinks_opt);
            }

            logger.setLevel(default_level);
            applyLoggerConfig(logger, ConfigValue(std::move(logger_cfg_obj)));
        }
    }

    void LoggerConfigLoader::applyLoggerConfig(Logger &logger, const ConfigValue &logger_cfg)
    {
        logger.clearSinks();

        // 设置等级
        if (logger_cfg.contains("level"))
        {
            const auto level_str = logger_cfg["level"].as<std::string>();
            logger.setLevel(logLevelFromString(level_str));
        }

        // 设置 Sinks
        if (logger_cfg.contains("sinks"))
        {
            for (const auto &sinks_arr = logger_cfg["sinks"].as<ConfigArray>(); const auto &sink_cfg: sinks_arr)
            {
                if (auto sink = createSinkFromConfig(sink_cfg))
                {
                    logger.addSink(std::move(sink));
                }
            }
        }
    }

    std::unique_ptr<LogSink> LoggerConfigLoader::createSinkFromConfig(const ConfigValue &sink_cfg)
    {
        if (!sink_cfg.is<ConfigObject>())
        {
            return nullptr;
        }

        // 安全获取 type 字段，避免 operator[] 抛出异常中断整个配置加载
        const auto type_opt = sink_cfg.get<std::string>("type");
        if (!type_opt.has_value())
        {
            std::cerr << "LoggerConfig: sink missing 'type' field, skipping" << '\n';
            return nullptr;
        }
        const std::string type = type_opt.value();

        std::unique_ptr<LogSink> sink;

        if (type == "console")
        {
            bool color = sink_cfg.get<bool>("color").value_or(true);
            sink       = std::make_unique<ConsoleSink>(color);
        } else if (type == "file")
        {
            const auto path_opt = sink_cfg.get<std::string>("path");
            if (!path_opt.has_value())
            {
                std::cerr << "LoggerConfig: file sink missing 'path', skipping" << '\n';
                return nullptr;
            }
            bool truncate = sink_cfg.get<bool>("truncate").value_or(false);
            sink          = std::make_unique<FileSink>(path_opt.value(), truncate);
        } else if (type == "rolling_file")
        {
            const auto base_opt = sink_cfg.get<std::string>("base_filename");
            if (!base_opt.has_value())
            {
                std::cerr << "LoggerConfig: rolling_file sink missing 'base_filename', skipping" << '\n';
                return nullptr;
            }
            const std::string dir        = sink_cfg.get<std::string>("directory").value_or("logs");
            const std::string policy_str = sink_cfg.get<std::string>("policy").value_or("size");

            RollingPolicy policy;
            if (policy_str == "size")
                policy = RollingPolicy::Size;
            else if (policy_str == "daily")
                policy = RollingPolicy::Daily;
            else if (policy_str == "hourly")
                policy = RollingPolicy::Hourly;
            else
                policy = RollingPolicy::Size;

            size_t max_size   = sink_cfg.get<int64_t>("max_size_mb").value_or(10) * 1024 * 1024;
            size_t max_backup = sink_cfg.get<int64_t>("max_backup").value_or(10);

            sink = std::make_unique<RollingFileSink>(base_opt.value(), dir, policy, max_size, max_backup);
        } else if (type == "async")
        {
            if (!sink_cfg.contains("wrapped"))
            {
                std::cerr << "LoggerConfig: async sink missing 'wrapped', skipping" << '\n';
                return nullptr;
            }
            auto wrapped = createSinkFromConfig(sink_cfg["wrapped"]);
            if (!wrapped)
                return nullptr;

            size_t                    queue_size = sink_cfg.get<int64_t>("queue_size").value_or(1024);
            const std::string         overflow   = sink_cfg.get<std::string>("overflow_policy").value_or("block");
            AsyncSink::OverflowPolicy policy     = (overflow == "drop") ? AsyncSink::OverflowPolicy::Drop : AsyncSink::OverflowPolicy::Block;

            sink = std::make_unique<AsyncSink>(std::move(wrapped), queue_size, policy);
        } else
        {
            // 模块初始化阶段日志系统可能尚未就绪，使用 std::cerr
            std::cerr << "LoggerConfig: unknown sink type '" << type << "', skipping" << '\n';
            return nullptr;
        }

        if (sink && sink_cfg.contains("level"))
        {
            const auto level_str = sink_cfg["level"].as<std::string>();
            sink->setLevel(logLevelFromString(level_str));
        }

        return sink;
    }
}
