/**
 * @file LoggerConfigLoader.h
 * @brief 从配置系统加载并应用日志配置
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/LogSink.h"

#include <filesystem>
#include <memory>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 日志配置加载器
     *
     * @details 从 ConfigManager 读取配置并初始化日志系统，支持配置多个日志器与 Sink。
     * @note 必须在单线程环境（通常是 main() 启动阶段）调用 loadFromConfig()。
     *
     * 配置示例 (YAML)：
     * @code
     * logging:
     *   global_level: INFO
     *   loggers:
     *     root:
     *       level: DEBUG
     *       sinks:
     *         - type: console
     *           color: true
     *         - type: file
     *           path: logs/app.log
     *     database:
     *       level: INFO
     *       sinks:
     *         - type: async
     *           queue_size: 2048
     *           wrapped:
     *             type: file
     *             path: logs/db.log
     * @endcode
     */
    class LoggerConfigLoader
    {
    public:
        /**
         * @brief 从配置系统读取日志器配置并应用到注册表
         * @param configurationPrefix 日志配置根键前缀
         * @param baseDirectory 相对日志路径的基准目录（为空时使用可执行文件目录）
         */
        static void loadFromConfig(const std::string &configurationPrefix = "logging", const std::filesystem::path &baseDirectory = {});

    private:
        /**
         * @brief 将单个日志器配置应用到指定日志器实例
         * @param logger 目标日志器
         * @param loggerConfiguration 日志器配置对象，包含 level 与 sinks 等键
         * @param baseDirectory 相对日志路径的基准目录
         */
        static void applyLoggerConfig(Logger &logger, const ConfigValue &loggerConfiguration, const std::filesystem::path &baseDirectory);

        /**
         * @brief 根据配置对象创建具体日志 Sink 实例
         * @param sinkConfiguration Sink 配置对象
         * @param baseDirectory 相对日志路径的基准目录
         * @return std::unique_ptr<LogSink> 构造成功返回 Sink，失败返回 nullptr
         */
        static std::unique_ptr<LogSink> createSinkFromConfig(const ConfigValue &sinkConfiguration, const std::filesystem::path &baseDirectory);
    };
} // namespace AsynGyanis::Base
