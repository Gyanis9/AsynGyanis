/**
 * @file LoggerConfigLoader.h
 * @brief 从配置系统加载并应用日志配置
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

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
     */
    class ASYN_BASE_API LoggerConfigLoader
    {
    public:
        /**
         * @brief 从配置系统读取日志器配置并应用到注册表
         * @details 整段配置按一份快照取回后再装配（不是一键一读），因此并发改掉配置也不会
         *          拼出「等级来自上一版、sinks 来自这一版」的半新半旧结果。原始段不是对象
         *          （写成标量或数组）、getSection 因段落形状自相矛盾而失败、或 loggers 段不是
         *          对象时，报诊断并一个日志器都不动。配置形态与 sink 构造的失败都在本函数内
         *          收口，不会作为异常逃出；内存分配失败仍按分配错误向调用方传播。
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
         * @details 构造期的失败（目标文件打不开、句柄耗尽等）在这一层收口：报一行诊断后按
         *          nullptr 返回，异常不逃出配置装配路径。逐字段的形态诊断与具体构造见
         *          createSinkFromConfigImpl。
         * @param sinkConfiguration Sink 配置对象
         * @param baseDirectory 相对日志路径的基准目录
         * @return std::unique_ptr<LogSink> 构造成功返回 Sink，失败返回 nullptr
         */
        static std::unique_ptr<LogSink> createSinkFromConfig(const ConfigValue &sinkConfiguration, const std::filesystem::path &baseDirectory);

        /**
         * @brief createSinkFromConfig 的未检查实现，允许构造期异常外抛
         * @details 配置形态不可用时仍按原有诊断返回 nullptr；Sink 自身构造抛出的异常不在这里
         *          拦截，由 createSinkFromConfig 统一收口。
         * @param sinkConfiguration Sink 配置对象
         * @param baseDirectory 相对日志路径的基准目录
         * @return std::unique_ptr<LogSink> 构造成功返回 Sink，配置不可用时返回 nullptr
         */
        [[nodiscard]] static std::unique_ptr<LogSink> createSinkFromConfigImpl(const ConfigValue &sinkConfiguration, const std::filesystem::path &baseDirectory);
    };
} // namespace AsynGyanis::Base
