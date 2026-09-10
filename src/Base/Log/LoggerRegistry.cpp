#include "Base/Log/LoggerRegistry.h"

#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    LoggerRegistry &LoggerRegistry::instance()
    {
        static LoggerRegistry instance;
        return instance;
    }

    Logger &LoggerRegistry::getLogger(const std::string &name)
    {
        std::unique_lock lock(m_mutex);
        if (const auto iterator = m_loggers.find(name); iterator != m_loggers.end())
        {
            return *iterator->second;
        }
        auto    logger    = std::make_unique<Logger>(name);
        Logger &reference = *logger;
        m_loggers[name]   = std::move(logger);
        return reference;
    }

    Logger &LoggerRegistry::getRootLogger()
    {
        return getLogger("root");
    }

    void LoggerRegistry::registerLogger(std::unique_ptr<Logger> logger)
    {
        if (!logger)
        {
            return;
        }
        std::unique_lock lock(m_mutex);
        m_loggers[logger->name()] = std::move(logger);
    }

    void LoggerRegistry::unregisterLogger(const std::string &name)
    {
        std::unique_lock lock(m_mutex);
        m_loggers.erase(name);
    }

    std::vector<std::string> LoggerRegistry::getLoggerNames() const
    {
        std::shared_lock         lock(m_mutex);
        std::vector<std::string> names;
        names.reserve(m_loggers.size());
        for (const auto &key: m_loggers | std::views::keys)
        {
            names.push_back(key);
        }
        return names;
    }

    void LoggerRegistry::clear()
    {
        std::unique_lock lock(m_mutex);
        m_loggers.clear();
    }

    void LoggerRegistry::forEachLogger(const std::function<void(Logger &)> &function) const
    {
        std::shared_lock lock(m_mutex);
        for (const auto &value: m_loggers | std::views::values)
        {
            if (value)
            {
                function(*value);
            }
        }
    }

    void LoggerRegistry::setLoggerLevel(const std::string &name, const LogLevel level)
    {
        // getLogger 在缺失时创建日志器，保证诊断开关对新日志器同样生效
        getLogger(name).setLevel(level);
    }

    std::optional<LogLevel> LoggerRegistry::loggerLevel(const std::string &name) const
    {
        const std::shared_lock lock(m_mutex);
        if (const auto iterator = m_loggers.find(name); iterator != m_loggers.end() && iterator->second)
        {
            return iterator->second->getLevel();
        }
        return std::nullopt;
    }

    void LoggerRegistry::setGlobalLevel(const LogLevel level) const
    {
        forEachLogger([level](Logger &logger)
        {
            logger.setLevel(level);
        });
    }
} // namespace AsynGyanis::Base
