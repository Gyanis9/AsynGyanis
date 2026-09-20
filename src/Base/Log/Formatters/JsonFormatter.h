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
     * @details 由 nlohmann::json 生成紧凑单行对象，字段为 timestamp、level、logger、thread、message；
     *          logger 名为空时省略，Debug 构建追加 file、line、function，与 DefaultFormatter 保持一致。
     *          字符串中的 NUL 完整保留并转义，合法 UTF-8 原样写出，不膨胀成 `\uXXXX`。
     * @note 非法 UTF-8 拒绝输出并抛出 Base::Exception，不替换字节，以免日志内容静默变形。
     * @note 无共享可变状态，可被多个 Sink 并发调用（见 LogFormatter 的约定）。
     * @see LogFormatter
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
