/**
 * @file ColorFormatter.h
 * @brief 彩色终端日志格式化器
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Formatters/LogFormatter.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 彩色终端格式化器
     *
     * @details 在等级字段前后插入 LogColor 提供的 ANSI 转义序列，并按等级着色；
     *          仅在确认输出目标支持 ANSI 序列时启用，否则应改用 DefaultFormatter。
     */
    class ColorFormatter : public LogFormatter
    {
    public:
        /**
         * @brief 将日志事件格式化为带颜色的终端输出文本
         * @details 重写 LogFormatter::format()：等级字段被包成「颜色码 + 等级 + 重置码」；
         *          Debug 构建（ASYN_DEBUG）额外输出线程号与「文件:行号」，
         *          Release 构建精简为「时间 [着色等级] [日志器] 消息」。
         * @param event 日志事件
         * @return std::string 含 ANSI 转义序列的日志行
         */
        std::string format(const LogEvent &event) override;
    };
} // namespace AsynGyanis::Base
