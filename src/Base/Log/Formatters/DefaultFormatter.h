/**
 * @file DefaultFormatter.h
 * @brief 默认纯文本日志格式化器
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
     * @brief 默认纯文本格式化器
     *
     * @details 输出不带任何转义序列的日志行，供文件类 Sink 与不支持 ANSI 的控制台使用。
     *          Debug 构建（ASYN_DEBUG）额外输出线程号与「文件:行号」，Release 构建只保留
     *          定位问题必需的字段以压缩体积。
     */
    class DefaultFormatter : public LogFormatter
    {
    public:
        /**
         * @brief 将日志事件格式化为纯文本日志行
         * @details 重写 LogFormatter::format()：不添加任何 ANSI 转义序列；
         *          Debug 构建为「时间 线程号 [等级] [日志器] 文件:行号 消息」，
         *          Release 构建精简为「时间 [等级] [日志器] 消息」。
         * @param event 日志事件
         * @return std::string 格式化后的日志行
         */
        std::string format(const LogEvent &event) override;
    };
} // namespace AsynGyanis::Base
