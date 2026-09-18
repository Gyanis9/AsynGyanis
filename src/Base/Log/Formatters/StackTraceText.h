/**
 * @file StackTraceText.h
 * @brief 调用栈文本渲染的共用工具（符号解析在此处发生）
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/StackTrace.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /// 文本日志里调用栈的引导行（JSON 格式化器不用它：栈在那里是独立字段）
    inline constexpr std::string_view kStackTraceHeading = "\n调用栈:\n";

    /**
     * @brief 把事件的调用栈追加到已格式化文本末尾
     * @details 符号解析在这里发生（见 StackTrace.h），调用方是 Sink 所在的写入线程；
     *          空栈时不追加任何内容。
     * @param text 目标文本（就地追加）
     * @param stackTrace 事件的调用栈原始帧
     */
    inline void appendStackTraceText(std::string &text, const CapturedStackTrace &stackTrace)
    {
        if (stackTrace.empty())
        {
            return;
        }
        text += kStackTraceHeading;
        text += formatStackTrace(stackTrace);
    }
} // namespace AsynGyanis::Base
