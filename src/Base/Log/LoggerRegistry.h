/**
 * @file LoggerRegistry.h
 * @brief 全局日志器注册表（单例）
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogLevel.h"
#include "Base/Log/Logger.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 全局日志器注册表
     *
     * @details 管理所有命名的 Logger 实例，提供获取或创建日志器的接口，
     *          并支持默认根日志器（名称为 "root"）。
     * @note 单例不可拷贝；全部查询与创建接口以读写锁保护，可在多线程下并发调用。
     * @note 引用契约：getLogger()/getRootLogger() 返回 Logger&，该引用仅在
     *       「注册表对象存活 + 期间没有他人对该日志器做 registerLogger/unregisterLogger/clear」
     *       的前提下有效。注册表用 shared_ptr 持有 Logger，被替换或注销的还会移入退休表而不销毁，
     *       因此注册表自身的读路径不会解引用已销毁对象；但调用方跨过注册表变更继续使用旧引用
     *       仍属未定义行为——这是保留 `Logger&` 返回类型所必须明示的边界。
     */
    class LoggerRegistry
    {
    public:
        /**
         * @brief 获取全局日志器注册表单例
         * @return LoggerRegistry& 注册表实例引用
         */
        static LoggerRegistry &instance();

        LoggerRegistry(const LoggerRegistry &) = delete;

        LoggerRegistry &operator=(const LoggerRegistry &) = delete;

        /**
         * @brief 按名称获取日志器，不存在时自动创建
         * @param name 日志器名称
         * @return Logger& 日志器引用（生命周期契约见类注释）
         */
        Logger &getLogger(const std::string &name);

        /**
         * @brief 获取默认根日志器
         * @return Logger& 根日志器引用（生命周期契约见类注释）
         */
        Logger &getRootLogger();

        /**
         * @brief 注册外部创建的日志器，若同名则覆盖
         * @details 被替换的那一份移入退休表（护住仍在用它裸引用的调用方），但它携带的 Sink
         *          当场交还：文件句柄与后台线程不等退休表，也不靠 purgeRetiredLoggers()。
         * @param logger 待注册日志器对象
         */
        void registerLogger(std::unique_ptr<Logger> logger);

        /**
         * @brief 注销指定名称日志器
         * @details 对象移入退休表而 Sink 当场交还（与 registerLogger 同名覆盖同一处置），因此
         *          注销之后再经旧裸引用写入的日志不落地——「注销即停止输出」正是调用方要的结果。
         * @param name 日志器名称
         */
        void unregisterLogger(const std::string &name);

        /**
         * @brief 返回当前所有已注册日志器名称
         * @return std::vector<std::string> 日志器名称列表
         */
        [[nodiscard]] std::vector<std::string> getLoggerNames() const;

        /**
         * @brief 清空注册表中的所有日志器
         * @details 日志器移入退休表而非就地销毁（保护在途裸引用，见 m_retiredLoggers 的说明），
         *          它们携带的 Sink 则当场交还——删临时目录这类操作不再需要先 purge 一次。
         */
        void clear();

        /**
         * @brief 丢弃退休表里的日志器外壳，回收注册/注销 churn 攒下的内存
         * @details Sink 已在退休那一刻交还，本方法剩下的是对象本身：调用方须**确知**已无在途
         *          裸引用（正在使用 `Logger&` 的 LOG_* 宏）——进程收尾，或测试夹具删临时目录之前。
         * @warning 运行期不要调用：退休表的存在前提就是「对象仍可达」，销毁会把在途引用变成悬垂。
         */
        void purgeRetiredLoggers();

        /**
         * @brief 对每个已注册日志器执行回调
         * @details 锁内只复制一份「日志器强引用快照」，回调在锁外执行：
         *          回调里再调 getLogger/registerLogger/clear 不会与这里的锁自死锁
         *          （std::shared_mutex 不可重入），快照持有的 shared_ptr 也保证
         *          遍历期间日志器不会被并发注销摧毁。
         * @param function 作用于日志器的回调函数
         */
        void forEachLogger(const std::function<void(Logger &)> &function) const;

        /**
         * @brief 运行时设置指定日志器的日志级别（不存在时自动创建）
         * @details 供诊断界面/远程排障在不停机的前提下临时放开或收敛日志。
         * @param name 日志器名称
         * @param level 目标日志级别
         */
        void setLoggerLevel(const std::string &name, LogLevel level);

        /**
         * @brief 查询指定日志器当前级别
         * @param name 日志器名称
         * @return std::optional<LogLevel> 存在时返回级别，否则为空
         */
        [[nodiscard]] std::optional<LogLevel> loggerLevel(const std::string &name) const;

        /**
         * @brief 运行时统一调整全部已注册日志器的级别
         * @details 仅影响内存中现有日志器；配置热重载会以 logging 段重新覆盖。
         * @param level 目标日志级别
         */
        void setGlobalLevel(LogLevel level) const;

    private:
        /// 根日志器的固定名称
        static constexpr std::string_view kRootLoggerName{"root"};

        /**
         * @brief 构造函数私有化，仅由 instance() 创建唯一实例
         */
        LoggerRegistry() = default;

        /// 成对更新根日志器缓存：强引用 + 热路径裸指针（实现见 .cpp）
        void storeCachedRootLogger(const std::shared_ptr<Logger> &logger) noexcept;

        /// 成对失效根日志器缓存（实现见 .cpp）
        void clearCachedRootLogger() noexcept;

        mutable std::shared_mutex                                m_mutex{};   ///< 保护 m_loggers 的读写锁
        std::unordered_map<std::string, std::shared_ptr<Logger>> m_loggers{}; ///< 日志器名称到 Logger 实例的映射表（共享所有权，便于快照延长生命周期）

        /// 已注销/被替换日志器的退休表：注销不销毁对象，只是移到这里，同时把它们的 Sink 当场交还。
        /// getLogger() 返回的是裸引用，使用它的调用方（LOG_* 宏）可能正跨过注销点继续用；
        /// 就地销毁会让那些引用悬垂。留下来的外壳只有名字、等级与一份空快照，而日志器数量少、
        /// 注册/注销罕见，保留到进程退出是可接受的代价（对象仍可达，LeakSanitizer 不会报告）
        std::vector<std::shared_ptr<Logger>> m_retiredLoggers{};

        /// 根日志器的热路径缓存（裸指针）：每条 LOG_* 宏都先读它，命中时只需一次原子读。
        /// 用裸指针而不是 shared_ptr 的原子量，是因为后者的 load 在 MSVC/libstdc++ 上要走内部
        /// 自旋锁，而这条路径每次日志调用都会被走到。
        /// **安全前提是本类的退休约定**：任何被替换/注销/清理的日志器都移入 m_retiredLoggers
        /// 而不是销毁，因此这里拿到的指针在注册表存活期内一直有效
        std::atomic<Logger *> m_cachedRootLoggerPointer{nullptr};
    };
} // namespace AsynGyanis::Base
