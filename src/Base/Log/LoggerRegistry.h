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
         * @return Logger& 日志器引用
         */
        Logger &getLogger(const std::string &name);

        /**
         * @brief 获取默认根日志器
         * @return Logger& 根日志器引用
         */
        Logger &getRootLogger();

        /**
         * @brief 注册外部创建的日志器，若同名则覆盖
         * @param logger 待注册日志器对象
         */
        void registerLogger(std::unique_ptr<Logger> logger);

        /**
         * @brief 注销指定名称日志器
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
         */
        void clear();

        /**
         * @brief 对每个已注册日志器执行回调
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

        mutable std::shared_mutex                                 m_mutex{};   ///< 保护 m_loggers 的读写锁
        std::unordered_map<std::string, std::unique_ptr<Logger> > m_loggers{}; ///< 日志器名称到 Logger 实例的映射表

        /// 根日志器指针缓存：所有增删日志器的入口都会将其置空，读取时无锁命中缓存
        std::atomic<Logger *> m_cachedRootLogger{nullptr};
    };
} // namespace AsynGyanis::Base
