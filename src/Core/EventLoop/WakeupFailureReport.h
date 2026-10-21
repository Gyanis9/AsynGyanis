/**
 * @file WakeupFailureReport.h
 * @brief 唤醒写失败的限频报告：两个事件循环入口共用一条
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Log/LogMacros.h"

#include <atomic>

namespace AsynGyanis::Core
{
    /**
     * @brief 唤醒写失败只报首条
     * @details 坏掉的唤醒器会让每次投递都失败，逐条打等于把日志刷满；进程内只留第一条，
     *          后续失败仍按各自的返回路径处理。
     * @param message 首条日志正文（静态文本）
     */
    inline void reportWakeupWriteFailureOnce(const char *message) noexcept
    {
        static std::atomic<bool> hasWarned{false};
        if (hasWarned.exchange(true, std::memory_order_relaxed))
        {
            return;
        }

        // 日志器本身可能正处在坏状态，报告失败不能再把异常带出 noexcept 边界
        try
        {
            LOG_WARN_FMT("{}", message);
        } catch (...)
        {
        }
    }
} // namespace AsynGyanis::Core
