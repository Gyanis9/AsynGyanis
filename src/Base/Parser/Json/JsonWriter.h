/**
 * @file JsonWriter.h
 * @brief 把配置值序列化为 JSON 文本
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Parser/Value/ParserValue.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 序列化器
     *
     * @details 与 JsonParser 严格互逆：任何 parse 成功得到的 ParserValue 再 write 后
     *          都能被重新 parse 回等价结果。浮点使用 std::to_chars 的最短往返表示，
     *          不会出现 ostream 默认精度造成的数值截断。
     */
    class JsonWriter
    {
    public:
        /**
         * @brief 序列化配置值为 JSON 文本
         * @param value 待序列化的配置值
         * @param indented 是否按两空格缩进换行输出，便于人工阅读
         * @return std::string 合法 JSON 文本
         * @note 非有限的浮点值（NaN、±Inf）没有合法的 JSON 数字表示，输出为 null。
         */
        [[nodiscard]] static std::string write(const ParserValue &value, bool indented = false);

    private:
        /**
         * @brief 递归追加一个值的 JSON 片段
         * @param output 输出缓冲
         * @param value 当前值
         * @param indentationLevel 当前缩进层级
         * @param indented 是否启用缩进格式
         */
        static void appendValue(std::string &output, const ParserValue &value, std::size_t indentationLevel, bool indented);

        /**
         * @brief 追加带引号并转义后的字符串
         * @param output 输出缓冲
         * @param text 原始文本
         */
        static void appendQuotedString(std::string &output, const std::string &text);

        /**
         * @brief 追加缩进空白
         * @param output 输出缓冲
         * @param indentationLevel 缩进层级
         * @param indented 是否启用缩进格式
         */
        static void appendIndentation(std::string &output, std::size_t indentationLevel, bool indented);
    };
} // namespace AsynGyanis::Base
