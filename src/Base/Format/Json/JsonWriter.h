/**
 * @file JsonWriter.h
 * @brief 把配置值序列化为 JSON 文本
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Json/JsonWriteOptions.h"
#include "Base/Format/Value/FormatValue.h"

#include <cstddef>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 序列化器
     *
     * @details 与 JsonParser 严格互逆：任何 parse 成功得到的 FormatValue 再 write 后
     *          都能被重新 parse 回等价结果。浮点使用 std::to_chars 的最短往返表示，
     *          不会出现 ostream 默认精度造成的数值截断；整数同样走 std::to_chars。
     *
     * 与既有格式的兼容性：
     *   - write(value, bool) 重载保留，indented = true 时输出与该重载历史行为逐字节一致
     *     （两空格缩进、键升序、3.0 保留小数点、非有限数输出 null、非 ASCII 原样透传）；
     *   - write(value, options) 通过 JsonWriteOptions 控制缩进宽度、ASCII 转义、键序与深度守护。
     *
     * @note 文本原样透传（ensureAscii = false）时不校验输入字符串的 UTF-8 合法性，
     *       非法字节会按原样写出；ensureAscii = true 时必须解码码位，此时遇到非法序列会抛
     *       FormatError（kind 为 InvalidUtf8）。
     */
    class JsonWriter
    {
    public:
        /**
         * @brief 序列化配置值为 JSON 文本（兼容重载）
         * @param value 待序列化的配置值
         * @param indented 是否按两空格缩进换行输出，便于人工阅读
         * @return std::string 合法 JSON 文本
         * @throws FormatError 嵌套层数达到 JsonWriteOptions::maximumDepth 默认上限
         * @note 非有限的浮点值（NaN、±Inf）没有合法的 JSON 数字表示，输出为 null。
         */
        [[nodiscard]] static std::string write(const FormatValue &value, bool indented = false);

        /**
         * @brief 序列化配置值为 JSON 文本（选项重载）
         * @param value 待序列化的配置值
         * @param options 序列化选项
         * @return std::string 合法 JSON 文本
         * @throws FormatError 嵌套层数达到 options.maximumDepth（kind 为 DepthExceeded）
         * @throws FormatError ensureAscii 为真且字符串含非法 UTF-8 序列（kind 为 InvalidUtf8）
         * @details 深度守护只作用于数组与对象：与解析侧含义一致（解析时以容器所在层数判定），
         *          因此用默认选项解析成功过的值一定能用默认选项写回。
         */
        [[nodiscard]] static std::string write(const FormatValue &value, const JsonWriteOptions &options);

    private:
        /**
         * @brief 递归追加一个值的 JSON 片段
         * @param output 输出缓冲
         * @param value 当前值
         * @param indentationLevel 当前缩进层级（容器层数，根值为 0）
         * @param options 序列化选项
         * @throws FormatError 容器层数达到 options.maximumDepth
         */
        static void appendValue(std::string &output, const FormatValue &value, std::size_t indentationLevel, const JsonWriteOptions &options);

        /**
         * @brief 追加带引号并转义后的字符串
         * @details 成段拷贝普通字符，只在遇到需转义字符或（ensureAscii 时的）非 ASCII 码位时
         *          才切换为逐字符处理，避免每个字符都做一次 push_back。
         * @param output 输出缓冲
         * @param text 原始文本
         * @param options 序列化选项（关键字段为 ensureAscii）
         * @throws FormatError ensureAscii 为真且遇到非法 UTF-8 序列
         */
        static void appendQuotedString(std::string &output, const std::string &text, const JsonWriteOptions &options);

        /**
         * @brief 追加缩进空白
         * @param output 输出缓冲
         * @param indentationLevel 缩进层级
         * @param options 序列化选项（indentWidth 为 0 时不产生任何空白）
         */
        static void appendIndentation(std::string &output, std::size_t indentationLevel, const JsonWriteOptions &options);

        /**
         * @brief 粗估输出文本的字节规模，用于一次性 reserve
         * @details 用显式栈遍历，避免为预估再引入一通递归；估算只求量级正确（宁大勿小），
         *          不参与任何正确性判断。
         * @param value 待序列化的配置值
         * @return std::size_t 估算字节数
         */
        [[nodiscard]] static std::size_t estimateOutputSize(const FormatValue &value);
    };
} // namespace AsynGyanis::Base
