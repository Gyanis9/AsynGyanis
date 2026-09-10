/**
 * @file LogColor.h
 * @brief 终端 ANSI 颜色码常量与日志等级到颜色的映射
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogLevel.h"

namespace AsynGyanis::Base
{
    /**
     * @brief 终端 ANSI 颜色码集合
     *
     * @details 只提供转义序列常量与等级到颜色的映射，不判断终端是否支持颜色；
     *          输出能力探测属于操作系统交互，统一由 Platform::Console::supportsAnsiEscapeCodes()
     *          负责，调用方据此决定使用彩色还是纯文本格式化器。
     */
    class LogColor
    {
    public:
        static constexpr auto kReset         = "\033[0m";  ///< 重置全部属性
        static constexpr auto kRed           = "\033[31m"; ///< 红色
        static constexpr auto kGreen         = "\033[32m"; ///< 绿色
        static constexpr auto kYellow        = "\033[33m"; ///< 黄色
        static constexpr auto kBlue          = "\033[34m"; ///< 蓝色
        static constexpr auto kMagenta       = "\033[35m"; ///< 品红
        static constexpr auto kCyan          = "\033[36m"; ///< 青色
        static constexpr auto kWhite         = "\033[37m"; ///< 白色
        static constexpr auto kBrightBlack   = "\033[90m"; ///< 亮黑（灰色）
        static constexpr auto kBrightRed     = "\033[91m"; ///< 亮红
        static constexpr auto kBrightGreen   = "\033[92m"; ///< 亮绿
        static constexpr auto kBrightYellow  = "\033[93m"; ///< 亮黄
        static constexpr auto kBrightBlue    = "\033[94m"; ///< 亮蓝
        static constexpr auto kBrightMagenta = "\033[95m"; ///< 亮品红
        static constexpr auto kBrightCyan    = "\033[96m"; ///< 亮青
        static constexpr auto kBrightWhite   = "\033[97m"; ///< 亮白

        /**
         * @brief 获取日志等级对应的 ANSI 颜色码
         * @param level 日志等级
         * @return const char* 颜色转义序列，未知等级返回重置码
         */
        static const char *colorForLevel(const LogLevel level) noexcept
        {
            switch (level)
            {
                case LogLevel::Trace:
                    return kBrightBlack;
                case LogLevel::Debug:
                    return kCyan;
                case LogLevel::Info:
                    return kGreen;
                case LogLevel::Warn:
                    return kYellow;
                case LogLevel::Error:
                    return kRed;
                case LogLevel::Fatal:
                    return kBrightRed;
                default:
                    return kReset;
            }
        }
    };
} // namespace AsynGyanis::Base
