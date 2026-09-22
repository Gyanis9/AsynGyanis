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

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <print>
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

        /**
         * @brief 记一步「因环境不齐备而没执行」，并在结论里单独列出
         * @details 这类步骤既不算通过也不算失败：把它混进步数会让「真机跑过」与「真机没跑」两种
         *          运行给出同一条结论行，步数一致性检查也就看不出差别。单独计数才留得下证据。
         * @param step 步骤说明（中文，直接进日志）
         * @param reason 跳过原因，只写缺什么（如环境变量名），绝不带上取值
         */
        void skip(const std::string_view step, const std::string_view reason)
        {
            ++m_gatedCount;
            LOG_INFO_FMT("– {}：{}（因环境不齐备跳过，不计入通过也不计入失败）", step, reason);
        }

        /// @return std::size_t 因环境不齐备而跳过的步骤数
        [[nodiscard]] std::size_t gatedCount() const noexcept
        {
            return m_gatedCount;
        }

    private:
        std::size_t m_stepCount{0};    ///< 已记下的步数
        std::size_t m_failureCount{0}; ///< 其中失败的步数
        std::size_t m_gatedCount{0};   ///< 因环境不齐备而跳过的步数，单列不进 m_stepCount
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
     * @return std::uint16_t 20000..29996 之间的端口
     * @details 偏移各占 100 宽的一段、段内按进程号取模错开，取模除数 97 小于段宽，因此不同示例
     *          的端口永不重叠。整段刻意落在系统动态端口区之下，避开本机主动外连时临时占用的端口。
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
     * @details 取值非法时当场以退出码 2 终止而不回落到默认端口：示例的客户端与服务端共用这一个值，
     *          回落会让整套自检照样全绿，脚本里的端口笔误就成了假证据。
     */
    inline std::uint16_t readPortArgument(const int argc, char **argv, const std::uint16_t offset)
    {
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (std::string_view(argv[index]) != "--port")
            {
                continue;
            }

            // 整串必须是 1-65535 的十进制数。这里刻意不用 atoi：它把 "abc" 折成 0、把 99999 交给
            // uint16 强转回绕成 34463，两种都会让示例「在另一个端口上跑成功」
            const std::string_view portText(argv[index + 1]);
            std::uint32_t          parsedPort = 0;
            const auto             parseResult =
                    std::from_chars(portText.data(), portText.data() + portText.size(), parsedPort);
            if (parseResult.ec != std::errc{} || parseResult.ptr != portText.data() + portText.size()
                || parsedPort == 0U || parsedPort > 65535U)
            {
                std::print(stderr, "示例启动参数非法：--port 的值「{}」不是一个 1-65535 的十进制端口号。"
                                   "请改成合法端口，或整个去掉 --port 让示例自行取一个专用端口\n", portText);
                std::exit(2);
            }
            return static_cast<std::uint16_t>(parsedPort);
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
     * @brief 打印结论并给出退出码：有任何一步失败、或一步都没执行，都算不通过
     * @details 结论行固定带 `gated <m>`：跳过的步数不混进 <步数>，但必须留在结论里，
     *          否则「真机跑过」与「真机没跑」两种运行给出的是同一行，重复跑的步数一致性
     *          检查也就无从区分。
     * @param sampleName 示例名（出现在结论里，脚本按它对齐清单）
     * @return int 0 表示全绿且至少有一步证据，1 表示有失败步或零步
     */
    inline int finishSample(const std::string_view sampleName)
    {
        const SampleChecklist &steps = checklist();
        // 零步不算通过：示例在登记任何检查之前就 return（平台分支整块被 #if 挡掉、初始化抛异常后被
        // 吞掉）留下的正是「没有失败也没有证据」的结论行，只判 failureCount()==0 会把它报成绿
        const bool hasEvidence = steps.failureCount() == 0 && steps.stepCount() > 0;
        // 结论同时走 stdout：脚本读这一行判定，级别与 sink 被示例自己改掉也不影响（本示例就有这一步）
        std::cout << "RESULT " << sampleName << ' ' << (hasEvidence ? "PASS " : "FAIL ") << steps.stepCount()
                  << " gated " << steps.gatedCount() << std::endl;
        if (hasEvidence)
        {
            LOG_INFO_FMT("示例 {} 自检通过：{} 步全绿，另有 {} 步因环境不齐备跳过",
                         sampleName, steps.stepCount(), steps.gatedCount());
            return 0;
        }
        if (steps.stepCount() == 0)
        {
            LOG_ERROR_FMT("示例 {} 一步都没执行：没有证据的 PASS 不算通过", sampleName);
            return 1;
        }
        LOG_ERROR_FMT("示例 {} 自检失败：{} 步里有 {} 步没过", sampleName, steps.stepCount(), steps.failureCount());
        return 1;
    }
} // namespace AsynGyanis::Samples
