/**
 * @file TextEscapes.h
 * @brief JSON 与 YAML 共用的转义与 UTF-8 编码原语
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/TextPosition.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 解析期文本原语
     *
     * @details JSON 与 YAML 的双引号标量共用同一套转义规则，集中在此避免两处实现漂移。
     *          所有失败路径抛出 FormatError 并带上调用方传入的位置。
     */
    class TextEscapes
    {
    public:
        /**
         * @brief 按 UTF-8 追加一个码位
         * @param output 目标字符串
         * @param codePoint 待编码的 Unicode 码位
         */
        static void appendUtf8(std::string &output, std::uint32_t codePoint);

        /**
         * @brief 解析定长十六进制转义序列
         * @param text 输入全文，用于错误定位
         * @param index 输入输出参数，指向十六进制首字符并在成功后前进
         * @param digitCount 需要读取的十六进制位数
         * @param position 出错时上报的位置基准
         * @return std::uint32_t 转换得到的数值
         * @throws FormatError 位数不足或含非十六进制字符
         */
        static std::uint32_t decodeHex(std::string_view text, std::size_t &index, std::size_t digitCount, const TextPosition &position);

        /**
         * @brief 解析 JSON 风格转义序列（不含 \\u 本体）
         * @details 支持 \" \\ \/ \b \f \n \r \t 六种控制符与引号；
         *          \\u 由调用方识别后另行处理，本函数遇到 u 视为非法。
         * @param text 输入全文，用于错误定位
         * @param index 输入输出参数，指向转义引导字符之后的类型字符并在成功后前进
         * @param position 出错时上报的位置基准
         * @return char 转义代表的字符
         * @throws FormatError 出现未知转义字符
         */
        static char decodeSimpleEscape(std::string_view text, std::size_t &index, const TextPosition &position);

        /**
         * @brief 解码双引号字符串的正文部分
         * @details 输入为不含首尾引号的原文，内部处理 \\uXXXX 与代理对合并，
         *          代理对不完整或 \\u 后不足四位时按错误抛出。
         * @param body 引号内的原始文本
         * @param position 该字符串起始位置，用于错误定位
         * @return std::string 解码后的 UTF-8 文本
         * @throws FormatError 转义非法或代理对不完整
         */
        static std::string decodeQuotedBody(std::string_view body, const TextPosition &position);
    };
} // namespace AsynGyanis::Base
