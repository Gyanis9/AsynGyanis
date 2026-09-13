/**
 * @file JsonFormatter.h
 * @brief 把日志事件渲染成单行 JSON（JSON Lines）的格式化器
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Formatters/LogFormatter.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 单行 JSON（JSON Lines）日志格式化器，供日志采集端直接解析
     *
     * @details 一条日志渲染成一个 JSON 对象、不含换行：采集端按行切分后按键取值即可，不必再写
     *          正则解析人读格式。键固定为 timestamp、level、logger、thread、message，Debug 构建
     *          下追加 file、line、function（与 DefaultFormatter 的 Debug/Release 差异口径一致）；
     *          logger 名为空时省略该键，避免给每条根日志塞一个空字段。
     *          转义、控制字符与数字格式化一律交给 JsonWriter，不在这里另写一份。
     * @note 文本按 UTF-8 原样写出（JsonWriteOptions::ensureAscii 为默认的 false）：中文日志不会被
     *       膨胀成 \uXXXX 转义，代价是消息必须是合法 UTF-8，含非法字节的消息会写出非法 JSON——
     *       这与 JsonWriter 自身的约定一致，需要严格的场景请改用 ensureAscii 的自定义实现。
     * @note 无共享可变状态，可被多个 Sink 并发调用（见 LogFormatter 的约定）。
     * @see LogFormatter, JsonWriter
     */
    class JsonFormatter : public LogFormatter
    {
    public:
        /**
         * @brief 把一条日志事件渲染成单行 JSON 对象
         * @param event 日志事件
         * @return std::string 不含换行的 JSON 文本
         */
        std::string format(const LogEvent &event) override;
    };
} // namespace AsynGyanis::Base
