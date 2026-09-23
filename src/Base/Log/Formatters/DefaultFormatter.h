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

        /**
         * @brief 把纯文本日志行追加进调用方的缓冲
         * @details 重写 LogFormatter::formatInto()：版式与 format() 逐字一致，区别只在写法——
         *          字段逐个 memcpy 进调用方留有容量的缓冲（一次 reserve），稳态下整行不取堆；
         *          format() 就是本函数加一个空串。字段顺序定义在 Detail/PlainTextLogLine.h，
         *          与 ColorFormatter 共用，改版式只改那一处。
         * @param out 目标缓冲；不清空，本次文本追加在其现有内容之后
         * @param event 日志事件
         */
        void formatInto(std::string &out, const LogEvent &event) override;
    };
} // namespace AsynGyanis::Base
