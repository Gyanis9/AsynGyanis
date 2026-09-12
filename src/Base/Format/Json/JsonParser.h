/**
 * @file JsonParser.h
 * @brief 手写 JSON 解析器，产出配置值模型
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Json/JsonParseOptions.h"
#include "Base/Format/TextPosition.h"
#include "Base/Format/Value/FormatValue.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 解析器
     *
     * @details 覆盖 RFC 8259 的常用子集：对象、数组、字符串（含 \\uXXXX 与代理对）、
     *          数字、true/false/null。与 YAML 侧共用 TextEscapes 的转义实现，
     *          解析结果直接落到 FormatValue，不引入第二套值类型。
     *
     *          默认（严格模式）明确拒绝：注释、单引号字符串、前导零、尾逗号、
     *          `NaN`/`Infinity`、文档尾部的多余内容；对象内重复键同样视为错误
     *          （配置场景下几乎总是笔误）。上述三类扩展语法可在 JsonParseOptions 中
     *          逐个打开。
     *
     *          数值取舍（不凭空造数）：按 RFC 8259 §6 的语法扫描后，非负整数依次尝试
     *          int64_t（得到 Int）→ uint64_t（得到 UInt）→ double（得到 Double）；
     *          负整数依次尝试 int64_t → double。超出 double 可表示范围时按
     *          FormatErrorKind::NumberOutOfRange 报错。
     *
     *          UTF-8 校验：字符串正文按 RFC 3629 校验字节序列（拒绝过长编码、截断序列、
     *          以及以 UTF-8 编码的代理项）；U+007F（DEL）在 JSON 中合法（RFC 8259 §7
     *          只禁止 U+0000..U+001F 的裸控制字符），因此原样保留而不报错。
     */
    class JsonParser
    {
    public:
        /**
         * @brief 按默认（严格）选项解析一份完整的 JSON 文档
         * @param text UTF-8 编码的 JSON 文本
         * @return FormatValue 解析得到的配置值
         * @throws FormatError 语法非法、越界或超出 JsonParseOptions 的默认上限
         */
        [[nodiscard]] static FormatValue parse(std::string_view text);

        /**
         * @brief 按指定选项解析一份完整的 JSON 文档
         * @param text UTF-8 编码的 JSON 文本
         * @param options 解析选项（安全上限与宽松语法开关）
         * @return FormatValue 解析得到的配置值
         * @throws FormatError 语法非法、越界或超出 options 中给定的上限
         */
        [[nodiscard]] static FormatValue parse(std::string_view text, const JsonParseOptions &options);

    private:
        /**
         * @brief 绑定输入与选项并初始化扫描游标
         * @param text 待解析文本
         * @param options 解析选项
         */
        explicit JsonParser(std::string_view text, const JsonParseOptions &options);

        /**
         * @brief 解析入口值并确认尾部无残留内容
         * @return FormatValue 文档根值
         * @throws FormatError 输入为空、超出长度上限或根值之后仍有内容
         */
        [[nodiscard]] FormatValue parseDocument();

        /**
         * @brief 解析单个值
         * @param nestingDepth 当前嵌套深度
         * @return FormatValue 解析结果
         */
        [[nodiscard]] FormatValue parseValue(std::size_t nestingDepth);

        /**
         * @brief 解析对象
         * @param nestingDepth 当前嵌套深度
         * @return FormatValue 承载 FormatValueObject 的值
         */
        [[nodiscard]] FormatValue parseObject(std::size_t nestingDepth);

        /**
         * @brief 解析数组
         * @param nestingDepth 当前嵌套深度
         * @return FormatValue 承载 FormatValueArray 的值
         */
        [[nodiscard]] FormatValue parseArray(std::size_t nestingDepth);

        /**
         * @brief 解析字符串（含首尾引号，宽松模式下也接受单引号）
         * @return FormatValue 字符串值
         */
        [[nodiscard]] FormatValue parseString();

        /**
         * @brief 解析字符串并直接返回其文本
         * @details 对象键只需要文本本身，不必先包一层 FormatValue：
         *          省掉一次 variant 构造与一次字符串分配，也让调用方能用一次
         *          emplace 的返回值同时完成插入与重复键判定。
         * @return std::string 解码后的字符串文本
         */
        [[nodiscard]] std::string parseStringText();

        /**
         * @brief 解析数字并判定为 Int、UInt 或 Double
         * @return FormatValue 数值
         */
        [[nodiscard]] FormatValue parseNumber();

        /**
         * @brief 解析 true/false/null 字面量
         * @param leadCharacter 首字符，用于区分具体字面量
         * @return FormatValue 对应的标量值
         */
        [[nodiscard]] FormatValue parseLiteral(char leadCharacter);

        /**
         * @brief 跳过空白字符与（可选）注释并同步行列号
         */
        void skipWhitespace();

        /**
         * @brief 跳过一个注释（调用方已确认注释引导符合法）
         * @throws FormatError 块注释未闭合
         */
        void skipComment();

        /**
         * @brief 跳过输入起始的 UTF-8 BOM（受 skipUtf8Bom 控制）
         */
        void skipUtf8Bom() noexcept;

        /**
         * @brief 校验字符串正文中的 UTF-8 字节序列
         * @details 依 RFC 3629 逐序列校验：拒绝过长编码、越界码位、UTF-8 形式的代理项
         *          以及被截断的序列
         * @param bodyStart 正文起始字节偏移
         * @param bodyEnd 正文结束字节偏移（不含）
         * @param bodyStartPosition 正文起点位置，用于按字节偏移换算出错列号
         * @throws FormatError 出现非法或截断的 UTF-8 序列（kind 为 InvalidUtf8）
         */
        void validateStringUtf8(std::size_t bodyStart, std::size_t bodyEnd, const TextPosition &bodyStartPosition) const;

        /**
         * @brief 判断当前位置是否为合法的字符串起始引号
         * @return bool 双引号恒合法；单引号仅在 allowSingleQuotedStrings 打开时合法
         */
        [[nodiscard]] bool atStringOpeningQuote() const noexcept;

        /**
         * @brief 读取当前字符但不前进
         * @return char 当前字符，已到达末尾时返回 '\0'
         */
        [[nodiscard]] char peekCurrent() const noexcept;

        /**
         * @brief 前进一个字符并维护行列号
         */
        void advance() noexcept;

        /**
         * @brief 要求当前字符为指定字符并前进
         * @param expected 期望字符
         * @throws FormatError 当前字符不匹配
         */
        void expect(char expected);

        /**
         * @brief 构造当前位置描述
         * @return TextPosition 行列与字节偏移
         */
        [[nodiscard]] TextPosition currentPosition() const noexcept;

        std::string_view m_text;      ///< 待解析全文
        JsonParseOptions m_options;   ///< 解析选项快照
        std::size_t      m_index{0};  ///< 当前扫描偏移
        std::size_t      m_line{1};   ///< 当前行号
        std::size_t      m_column{1}; ///< 当前列号
    };
} // namespace AsynGyanis::Base
