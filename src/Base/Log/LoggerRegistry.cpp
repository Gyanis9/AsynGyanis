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

    void LoggerRegistry::storeCachedRootLogger(const std::shared_ptr<Logger> &logger) noexcept
    {
        // 两个缓存成对更新：强引用先落（对象的所有权先到位），裸指针后发布（热路径只读它）
        m_cachedRootLogger.store(logger, std::memory_order_release);
        m_cachedRootLoggerPointer.store(logger.get(), std::memory_order_release);
    }

    void LoggerRegistry::clearCachedRootLogger() noexcept
    {
        // 与发布顺序相反：先断裸指针让新读者走慢路径，再放掉强引用
        m_cachedRootLoggerPointer.store(nullptr, std::memory_order_release);
        m_cachedRootLogger.store(nullptr, std::memory_order_release);
    }

    Logger &LoggerRegistry::getLogger(const std::string &name)
    {
        {
            // 快路径：日志器已存在时只持读锁，避免所有线程在写锁上串行。
            // 若查询的正是 root，顺带把强引用回填进缓存：这里仍持有读锁，
            // 任何会删改 root 的写者（register/unregister/clear）都无法插入到
            // 「查到迭代器」与「写入缓存」之间，因此不会把已被移除的旧实例复活成缓存内容
            std::shared_lock readerLock(m_mutex);
            if (const auto iterator = m_loggers.find(name); iterator != m_loggers.end())
            {
                if (name == kRootLoggerName)
                {
                    storeCachedRootLogger(iterator->second);
                }
                return *iterator->second;
            }
        }

        std::unique_lock writerLock(m_mutex);

        // 双检：升锁间隙可能已被其它线程创建
        if (const auto iterator = m_loggers.find(name); iterator != m_loggers.end())
        {
            if (name == kRootLoggerName)
            {
                storeCachedRootLogger(iterator->second);
            }
            return *iterator->second;
        }

        auto    logger    = std::make_shared<Logger>(name);
        Logger &reference = *logger;
        if (name == kRootLoggerName)
        {
            // 缓存与 map 共享同一份所有权：缓存命中期间 root 不会被析构
            storeCachedRootLogger(logger);
        }
        m_loggers.emplace(name, std::move(logger));
        return reference;
    }

    Logger &LoggerRegistry::getRootLogger()
    {
        // 每条 LOG_* 宏都走这里：命中缓存时只有一次原子读，不加锁、也不碰 shared_ptr
        //（atomic<shared_ptr> 的 load 在 MSVC/libstdc++ 上要走内部自旋锁）。
        // 指针的生命期由本类的退休约定保证：被替换/注销/清理的日志器都移入退休表而不是销毁
        if (Logger *const cachedLogger = m_cachedRootLoggerPointer.load(std::memory_order_acquire))
        {
            return *cachedLogger;
        }

        // 未命中：getLogger 在取出或创建 root 时会回填缓存，这里直接复用其返回值
        return getLogger(std::string(kRootLoggerName));
    }

    void LoggerRegistry::registerLogger(std::unique_ptr<Logger> logger)
    {
        if (!logger)
        {
            return;
        }

        std::unique_lock lock(m_mutex);

        std::shared_ptr<Logger> registeredLogger = std::move(logger);
        const bool              isRootLogger     = registeredLogger->name() == kRootLoggerName;
        if (isRootLogger)
        {
            // 覆盖会替换旧 root，先置空缓存再替换，避免他人在窗口内继续使用旧实例
            clearCachedRootLogger();
        }
        // 同名替换：旧对象移入退休表而不是就地销毁——正在使用它的裸引用（LOG_* 宏）可能跨过这一刻
        if (const auto existing = m_loggers.find(registeredLogger->name()); existing != m_loggers.end())
        {
            m_retiredLoggers.push_back(std::move(existing->second));
        }
        m_loggers[registeredLogger->name()] = registeredLogger;

        if (isRootLogger)
        {
            storeCachedRootLogger(registeredLogger);
        }
    }

    void LoggerRegistry::unregisterLogger(const std::string &name)
    {
        std::unique_lock lock(m_mutex);

        // 先失效缓存再擦除，避免缓存继续指向已被移除的 root
        if (name == kRootLoggerName)
        {
            clearCachedRootLogger();
        }
        // 摘出而非销毁：getLogger() 给出的是裸引用，使用中的调用方可能还没走完
        if (const auto existing = m_loggers.find(name); existing != m_loggers.end())
        {
            m_retiredLoggers.push_back(std::move(existing->second));
            m_loggers.erase(existing);
        }
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
        clearCachedRootLogger();
        // 与 unregisterLogger 同一处置：全部移入退休表，不就地销毁使用中的对象
        for (auto &entry: m_loggers)
        {
            m_retiredLoggers.push_back(std::move(entry.second));
        }
        m_loggers.clear();
    }

    void LoggerRegistry::purgeRetiredLoggers()
    {
        // 换出到局部变量、在锁外析构：AsyncSink 的析构要 join 工作线程、FileSink 要关文件句柄，
        // 持锁期间做这些会把其他线程的日志调用一起卡住
        std::vector<std::shared_ptr<Logger> > retiredLoggers;
        {
            const std::unique_lock lock(m_mutex);
            retiredLoggers.swap(m_retiredLoggers);
        }
    }

    void LoggerRegistry::forEachLogger(const std::function<void(Logger &)> &function) const
    {
        // 锁内只做一件事：把当前日志器的强引用复制成快照。
        // 回调必须在锁外执行——回调里再调 getLogger/registerLogger/clear 会重复获取
        // shared_mutex（不可重入）从而自死锁；同时快照持有 shared_ptr，
        // 遍历期间他人注销日志器也不会让正在回调的对象被销毁
        std::vector<std::shared_ptr<Logger> > snapshot;
        {
            std::shared_lock lock(m_mutex);
            snapshot.reserve(m_loggers.size());
            for (const auto &value: m_loggers | std::views::values)
            {
                if (value)
                {
                    snapshot.push_back(value);
                }
            }
        }

        for (const auto &logger: snapshot)
        {
            function(*logger);
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
