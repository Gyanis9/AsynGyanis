/**
 * @file StackTrace.h
 * @brief 调用栈捕获与格式化 —— 异常携带抛出点栈，日志在输出端解析
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 捕获与解析刻意分成两步：捕获只记原始帧（微秒级，异常构造时做），符号解析要读
 *          PDB/调试信息（文件 IO，首次毫秒级）而推迟到真正输出时才做——因此事件循环线程上
 *          「记录一条带栈的日志」不会触发磁盘访问，解析发生在 Sink 那一侧。
 * @note 平台/标准库不提供 std::stacktrace 时（构建期探测，见 ASYN_HAS_STACKTRACE）退化为
 *       空实现：捕获得到空栈，日志与异常照常工作，只是不带栈。
 */

#pragma once

#include <cstddef>
#include <string>

#if defined(ASYN_HAS_STACKTRACE)
    #include <stacktrace>
#endif

namespace AsynGyanis::Base
{
    /// 单次捕获的最大帧数：覆盖框架里最深的调用链，同时把捕获成本压在一微秒量级
    inline constexpr std::size_t kMaximumStackTraceDepth = 32;

#if defined(ASYN_HAS_STACKTRACE)
    /// 捕获到的调用栈（只含原始帧）；符号解析在 formatStackTrace() 中进行
    using CapturedStackTrace = std::stacktrace;
#else
    /**
     * @brief 无 std::stacktrace 时的降级替身：永远为空栈
     */
    class CapturedStackTrace
    {
    public:
        [[nodiscard]] bool empty() const noexcept
        {
            return true;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return 0;
        }
    };
#endif

#if defined(ASYN_HAS_STACKTRACE)
    /**
     * @brief 捕获当前调用栈（只记原始帧，不做符号解析）
     * @param framesToSkip 自本函数的调用者起额外跳过的帧数（用于隐去框架自身的包装帧）
     * @param maximumDepth 最多保留的帧数
     * @return CapturedStackTrace 原始帧集合
     */
    [[nodiscard]] CapturedStackTrace captureStackTrace(std::size_t framesToSkip = 0,
                                                       std::size_t maximumDepth = kMaximumStackTraceDepth);

    /**
     * @brief 把原始帧解析成多行文本
     * @details 符号解析（含首次的调试信息加载）在此发生，调用方留意自己所在的线程。
     * @param stackTrace 待解析的调用栈
     * @return std::string 每帧一行；空栈返回空串
     */
    [[nodiscard]] std::string formatStackTrace(const CapturedStackTrace &stackTrace);
#else
    /// 降级：不捕获任何帧
    [[nodiscard]] inline CapturedStackTrace captureStackTrace(const std::size_t = 0,
                                                              const std::size_t = kMaximumStackTraceDepth) noexcept
    {
        return {};
    }

    /// 降级：空栈渲染为空文本
    [[nodiscard]] inline std::string formatStackTrace(const CapturedStackTrace &)
    {
        return {};
    }
#endif
} // namespace AsynGyanis::Base
