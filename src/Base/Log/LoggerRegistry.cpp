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
        {
            // 快路径：日志器已存在时只持读锁，避免所有线程在写锁上串行
            std::shared_lock readerLock(m_mutex);
            if (const auto iterator = m_loggers.find(name); iterator != m_loggers.end())
            {
                return *iterator->second;
            }
        }

        std::unique_lock writerLock(m_mutex);

        // 双检：升锁间隙可能已被其它线程创建
        if (const auto iterator = m_loggers.find(name); iterator != m_loggers.end())
        {
            return *iterator->second;
        }

        auto    logger    = std::make_unique<Logger>(name);
        Logger &reference = *logger;
        m_loggers.emplace(name, std::move(logger));

        // 新建的很可能正是根日志器，一律失效缓存，由下次读取重新回填
        m_cachedRootLogger.store(nullptr, std::memory_order_release);
        return reference;
    }

    Logger &LoggerRegistry::getRootLogger()
    {
        // 每条 LOG_* 宏都走这里，命中缓存时完全不加锁
        if (auto *cachedLogger = m_cachedRootLogger.load(std::memory_order_acquire); cachedLogger != nullptr)
        {
            return *cachedLogger;
        }

        Logger &rootLogger = getLogger(std::string(kRootLoggerName));
        m_cachedRootLogger.store(&rootLogger, std::memory_order_release);
        return rootLogger;
    }

    void LoggerRegistry::registerLogger(std::unique_ptr<Logger> logger)
    {
        if (!logger)
        {
            return;
        }

        std::unique_lock lock(m_mutex);

        // 覆盖会销毁旧实例，先置空缓存再替换，避免他人在窗口内解引用已释放指针
        m_cachedRootLogger.store(nullptr, std::memory_order_release);

        Logger *   registeredPointer         = logger.get();
        const bool isRootLogger              = registeredPointer->name() == kRootLoggerName;
        m_loggers[registeredPointer->name()] = std::move(logger);

        if (isRootLogger)
        {
            m_cachedRootLogger.store(registeredPointer, std::memory_order_release);
        }
    }

    void LoggerRegistry::unregisterLogger(const std::string &name)
    {
        std::unique_lock lock(m_mutex);

        // 先失效缓存再擦除，避免其他线程取到即将析构的指针
        if (name == kRootLoggerName)
        {
            m_cachedRootLogger.store(nullptr, std::memory_order_release);
        }
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
        m_cachedRootLogger.store(nullptr, std::memory_order_release);
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
