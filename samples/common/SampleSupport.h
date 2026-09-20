/**
 * @file SampleSupport.h
 * @brief 示例程序共用的底座：日志装配、错开的端口、有界等待与自检收尾
 * @author Gyanis
 * @date 2026-09-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 每个示例都只做一件事，但都要能「自己判断成没成」：跑完打印一步一行的结论，
 *          有任何一步失败就以非零码退出，脚本据此把整套示例当成系统完整性检查来跑。
 *
 * @note 这里的等待一律带时限：示例挂在某个未唤醒的协程上时，必须表现为失败而不是无限等待。
 */

#pragma once

#include "Base/Log/Formatters/JsonFormatter.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Platform/System/ProcessInfo.h"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

namespace AsynGyanis::Samples
{
    /**
     * @brief 一个示例的自检清单：逐步记录成败，收尾时据此决定退出码
     */
    class SampleChecklist
    {
    public:
        /**
         * @brief 记一步的结果并打印出来
         * @param isPassed 这一步是否通过
         * @param step 步骤说明（中文，直接进日志）
         */
        void check(const bool isPassed, const std::string_view step)
        {
            ++m_stepCount;
            if (!isPassed)
            {
                ++m_failureCount;
                LOG_ERROR_FMT("✗ {}（第 {} 步）", step, m_stepCount);
                return;
            }
            LOG_INFO_FMT("✓ {}", step);
        }

        /// @return std::size_t 失败的步数；0 表示全部通过
        [[nodiscard]] std::size_t failureCount() const noexcept
        {
            return m_failureCount;
        }

        /// @return std::size_t 总共记下的步数（含通过的）
        [[nodiscard]] std::size_t stepCount() const noexcept
        {
            return m_stepCount;
        }

    private:
        std::size_t m_stepCount{0};    ///< 已记下的步数
        std::size_t m_failureCount{0}; ///< 其中失败的步数
    };

    /// @return SampleChecklist & 本进程的自检清单（单实例，示例都是单线程主干）
    inline SampleChecklist &checklist()
    {
        static SampleChecklist instance;
        return instance;
    }

    /**
     * @brief 装配控制台日志：示例的所有输出都走日志器，便于按级别与格式核对
     * @param useJsonFormatter true 时按 JSON Lines 输出（供采集端解析）
     * @param level 根记录器级别；示例默认 Info
     */
    inline void setupConsoleLogging(const bool useJsonFormatter = false,
                                    const Base::LogLevel level    = Base::LogLevel::Info)
    {
        auto &rootLogger  = Base::LoggerRegistry::instance().getRootLogger();
        auto  consoleSink = std::make_unique<Base::ConsoleSink>();
        if (useJsonFormatter)
        {
            consoleSink->setFormatter(std::make_unique<Base::JsonFormatter>());
        }
        rootLogger.addSink(std::move(consoleSink));
        rootLogger.setLevel(level);
    }

    /**
     * @brief 取一个本示例专用的端口：由进程号错开，多个示例并行跑也不互撞
     * @param offset 每个示例固定一个偏移（0..99），同一次运行里也彼此不同
     * @return std::uint16_t 20000..60000 之间的端口
     */
    inline std::uint16_t samplePort(const std::uint16_t offset)
    {
        const std::uint64_t processIdentifier = static_cast<std::uint64_t>(Platform::ProcessInfo::currentProcessId());
        return static_cast<std::uint16_t>(20000U + (offset % 100U) * 100U + processIdentifier % 97U);
    }

    /**
     * @brief 命令行里取 --port：脚本会给每个示例分发一个明确端口
     * @param argc 实参个数
     * @param argv 实参表
     * @param offset 没给 --port 时用的默认端口偏移
     * @return std::uint16_t 端口
     */
    inline std::uint16_t readPortArgument(const int argc, char **argv, const std::uint16_t offset)
    {
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (std::string_view(argv[index]) == "--port")
            {
                return static_cast<std::uint16_t>(std::atoi(argv[index + 1]));
            }
        }
        return samplePort(offset);
    }

    /**
     * @brief 有界地等到某个条件成立
     * @tparam Predicate 返回 true 表示等到了
     * @param predicate 判定体（会在本线程反复调用）
     * @param timeout 最长等待时长；到点仍未成立即返回 false
     * @param interval 两次判定之间的间隔
     * @return true 条件在时限内成立
     */
    template <typename Predicate>
    bool waitUntil(const Predicate &predicate, const std::chrono::milliseconds timeout,
                   const std::chrono::milliseconds interval = std::chrono::milliseconds{10})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(interval);
        }
        return predicate();
    }

    /**
     * @brief 打印结论并给出退出码：有任何一步失败即非零
     * @param sampleName 示例名（出现在结论里，脚本按它对齐清单）
     * @return int 0 表示全部通过，1 表示有失败步
     */
    inline int finishSample(const std::string_view sampleName)
    {
        const SampleChecklist &steps = checklist();
        // 结论同时走 stdout：脚本读这一行判定，级别与 sink 被示例自己改掉也不影响（本示例就有这一步）
        std::cout << "RESULT " << sampleName << ' ' << (steps.failureCount() == 0 ? "PASS " : "FAIL ") << steps.stepCount()
                  << std::endl;
        if (steps.failureCount() == 0)
        {
            LOG_INFO_FMT("示例 {} 自检通过：{} 步全绿", sampleName, steps.stepCount());
            return 0;
        }
        LOG_ERROR_FMT("示例 {} 自检失败：{} 步里有 {} 步没过", sampleName, steps.stepCount(), steps.failureCount());
        return 1;
    }
} // namespace AsynGyanis::Samples
