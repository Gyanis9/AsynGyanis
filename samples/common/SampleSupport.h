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
#include <cstdint>
#include <cstdlib>
#include <format>
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
    inline void setupConsoleLogging(const bool useJsonFormatter = false, const Base::LogLevel level = Base::LogLevel::Info)
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
     * @brief 把一行面向操作者的启动期报错写到标准错误
     * @details 不用 std::print：`<print>` 要 GCC 14+，而本仓库当作泄漏/未定义行为门禁的容器是
     *          GCC 13，一条命令行提示不值得让整批示例在那台工具链上构建不过。文案仍由
     *          std::format 拼（`<format>` 两侧都有），这里只负责输出与换行。
     * @param message 已经拼好的整行（不含行尾换行）
     */
    inline void printStartupError(const std::string_view message)
    {
        std::cerr << message << '\n';
    }

    /**
     * @brief 取「--选项 值」型选项的取值，选项在末尾却没有值时当场终止
     * @param argc 实参个数
     * @param argv 实参表
     * @param optionIndex 选项在实参表中的下标，取值位于 optionIndex + 1
     * @param optionName 选项名，只用于报错文案
     * @param expectedValueHint 该选项期望的取值写法，进报错文案
     * @return const char * 取值字符串（指向实参表，生命周期随进程）
     * @details 「选项排在末尾却没有取值」不当成「没给这个选项」：静默回落会让脚本以为程序按某个
     *          地址/某份配置在跑，实际跑的是默认值，结论行照旧是 PASS。
     */
    inline const char *readOptionValue(const int argc, char **argv, const int optionIndex, const std::string_view optionName, const std::string_view expectedValueHint)
    {
        if (optionIndex + 1 >= argc)
        {
            printStartupError(std::format("启动参数非法：{} 后面缺少取值。请补上{}，或整个去掉该选项让程序按默认值运行", optionName, expectedValueHint));
            std::exit(2);
        }
        return argv[optionIndex + 1];
    }

    /**
     * @brief 取一个「--选项 数值」型命令行选项的取值，缺失或非法时当场终止
     * @param argc 实参个数
     * @param argv 实参表
     * @param optionIndex 选项在实参表中的下标，取值位于 optionIndex + 1
     * @param optionName 选项名，只用于报错文案
     * @param minimumValue 允许的最小值（含）
     * @param maximumValue 允许的最大值（含）
     * @return std::uint64_t 区间内的取值
     * @details 三条硬判据各挡一种静默变形：整串必须是纯十进制无符号数（负数因此判非法，不会绕回大数）、
     *          必须落在区间内（99999 不会绕回 34463）、选项后面必须有值（漏写取值不当成「没给这个选项」）。
     *          非法一律以退出码 2 终止而不回落默认值：脚本分发了哪个端口与程序实际听在哪个端口必须是
     *          同一件事，否则「另一个配置跑成功」也会留下一行 PASS。
     */
    inline std::uint64_t readNumericOption(const int argc, char **argv, const int optionIndex, const std::string_view optionName, const std::uint64_t minimumValue,
                                           const std::uint64_t maximumValue)
    {
        const std::string      valueRangeText    = std::format("{}-{}", minimumValue, maximumValue);
        const std::string      expectedValueHint = std::format("一个 {} 之间的十进制整数", valueRangeText);
        const auto *const      valuePointer      = readOptionValue(argc, argv, optionIndex, optionName, expectedValueHint);
        const std::string_view valueText(valuePointer);
        std::uint64_t          parsedValue = 0;
        const auto             parseResult = std::from_chars(valueText.data(), valueText.data() + valueText.size(), parsedValue);
        if (parseResult.ec != std::errc{} || parseResult.ptr != valueText.data() + valueText.size() || parsedValue < minimumValue || parsedValue > maximumValue)
        {
            printStartupError(std::format("启动参数非法：{} 的值「{}」不是一个 {} 之间的十进制整数。"
                                          "请改成区间内的取值，或整个去掉该选项让程序按默认值运行",
                                          optionName, valueText, valueRangeText));
            std::exit(2);
        }
        return parsedValue;
    }

    /// 端口号上限：`uint16_t` 里最大的那个可用值，也是 65535 在这一份代码里的唯一出处
    constexpr std::uint16_t kMaximumPortNumber = 65535U;

    /**
     * @brief 命令行里取 --port：脚本会给每个示例分发一个明确端口
     * @param argc 实参个数
     * @param argv 实参表
     * @param offset 没给 --port 时用的默认端口偏移
     * @return std::uint16_t 端口
     * @details 取值非法（含选项在末尾却没有值）时当场以退出码 2 终止而不回落到默认端口：示例的客户端
     *          与服务端共用这一个值，回落会让整套自检照样全绿，脚本里的端口笔误就成了假证据。
     * @note 只保证基准值本身合法。一条示例若还要占相邻端口，另需 requirePortHeadroom 声明余量。
     */
    inline std::uint16_t readPortArgument(const int argc, char **argv, const std::uint16_t offset)
    {
        for (int index = 1; index < argc; ++index)
        {
            if (std::string_view(argv[index]) == "--port")
            {
                return static_cast<std::uint16_t>(readNumericOption(argc, argv, index, "--port", 1U, kMaximumPortNumber));
            }
        }
        return samplePort(offset);
    }

    /**
     * @brief 声明「本示例会从基准端口往后最多占用 highestDelta 个端口」，越出端口号上限时当场终止
     * @param basePort 基准端口（一般就是 readPortArgument 的返回值）
     * @param highestDelta 相对基准端口最多要加到几（0 表示只用基准端口本身）
     * @details readPortArgument 只保证基准值落在 1..65535，而一条示例往往还要占相邻的几个端口
     *          （分发与 worker 各一台、明文与 TLS 各一台）。派生端口是 `uint16_t` 上的加法，
     *          基准取到 65533 这类值时第 4 个端口会绕回 0——而 bind 到 0 是「让内核随便挑」，
     *          客户端却照旧去连 0，最后看到的是一条与端口毫无关系的失败。宁可在起跑前说清楚，
     *          也不静默回绕（与 readNumericOption 挡「99999 绕回 34463」是同一条判据）。
     */
    inline void requirePortHeadroom(const std::uint16_t basePort, const std::uint16_t highestDelta)
    {
        const std::uint32_t highestUsedPort = static_cast<std::uint32_t>(basePort) + highestDelta;
        if (highestUsedPort <= kMaximumPortNumber)
        {
            return;
        }
        printStartupError(std::format("启动参数非法：基准端口 {} 太靠上，本示例最多要用到端口 {}，而端口号上限是 {}。"
                                      "请把 --port 降到 {} 或以下",
                                      basePort, highestUsedPort, static_cast<std::uint32_t>(kMaximumPortNumber), static_cast<std::uint32_t>(kMaximumPortNumber) - highestDelta));
        std::exit(2);
    }

    /**
     * @brief 有界地等到某个条件成立
     * @tparam Predicate 返回 true 表示等到了
     * @param predicate 判定体（会在本线程反复调用）
     * @param timeout 最长等待时长；到点仍未成立即返回 false
     * @param interval 两次判定之间的间隔
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool waitUntil(const Predicate &predicate, const std::chrono::milliseconds timeout, const std::chrono::milliseconds interval = std::chrono::milliseconds{10})
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
        std::cout << "RESULT " << sampleName << ' ' << (hasEvidence ? "PASS " : "FAIL ") << steps.stepCount() << " gated " << steps.gatedCount() << std::endl;
        if (hasEvidence)
        {
            LOG_INFO_FMT("示例 {} 自检通过：{} 步全绿，另有 {} 步因环境不齐备跳过", sampleName, steps.stepCount(), steps.gatedCount());
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
