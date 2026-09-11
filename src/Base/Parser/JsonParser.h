/**
 * @file JsonParser.h
 * @brief 手写 JSON 解析器，产出配置值模型
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Base/Parser/ParserPosition.h"

#include <cstddef>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 解析器
     *
     * @details 覆盖 RFC 8259 的常用子集：对象、数组、字符串（含 \\uXXXX 与代理对）、
     *          数字、true/false/null。与 YAML 侧共用 ParserText 的转义实现，
     *          解析结果直接落到 ConfigValue，不引入第二套值类型。
     * @note 明确拒绝：注释、单引号字符串、前导零、尾逗号、`NaN`/`Infinity`、
     *          文档尾部的多余内容；对象内重复键同样视为错误（配置场景下几乎总是笔误）。
     */
    class JsonParser
    {
    public:
        /**
         * @brief 解析一份完整的 JSON 文档
         * @param text UTF-8 编码的 JSON 文本
         * @return ConfigValue 解析得到的配置值
         * @throws ParserError 语法非法、位置越界或嵌套超过 kMaximumNestingDepth 层
         */
        [[nodiscard]] static ConfigValue parse(std::string_view text);

    private:
        /// 容器嵌套深度上限，防止恶意或损坏的输入耗尽调用栈
        static constexpr std::size_t kMaximumNestingDepth = 32;

        /**
         * @brief 绑定输入并初始化扫描游标
         * @param text 待解析文本
         */
        explicit JsonParser(std::string_view text);

        /**
         * @brief 解析入口值并确认尾部无残留内容
         * @return ConfigValue 文档根值
         */
        [[nodiscard]] ConfigValue parseDocument();

        /**
         * @brief 解析单个值
         * @param nestingDepth 当前嵌套深度
         * @return ConfigValue 解析结果
         */
        [[nodiscard]] ConfigValue parseValue(std::size_t nestingDepth);

        /**
         * @brief 解析对象
         * @param nestingDepth 当前嵌套深度
         * @return ConfigValue 承载 ConfigObject 的值
         */
        [[nodiscard]] ConfigValue parseObject(std::size_t nestingDepth);

        /**
         * @brief 解析数组
         * @param nestingDepth 当前嵌套深度
         * @return ConfigValue 承载 ConfigArray 的值
         */
        [[nodiscard]] ConfigValue parseArray(std::size_t nestingDepth);

        /**
         * @brief 解析字符串（含首尾双引号）
         * @return ConfigValue 字符串值
         */
        [[nodiscard]] ConfigValue parseString();

        /**
         * @brief 解析数字并判定为整数或浮点
         * @return ConfigValue 数值
         */
        [[nodiscard]] ConfigValue parseNumber();

        /**
         * @brief 解析 true/false/null 字面量
         * @param leadCharacter 首字符，用于区分具体字面量
         * @return ConfigValue 对应的标量值
         */
        [[nodiscard]] ConfigValue parseLiteral(char leadCharacter);

        /**
         * @brief 跳过空白字符并同步行列号
         */
        void skipWhitespace();

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
         * @throws ParserError 当前字符不匹配
         */
        void expect(char expected);

        /**
         * @brief 构造当前位置描述
         * @return ParserPosition 行列与字节偏移
         */
        [[nodiscard]] ParserPosition currentPosition() const noexcept;

        std::string_view m_text;   ///< 待解析全文
        std::size_t m_index{0};    ///< 当前扫描偏移
        std::size_t m_line{1};     ///< 当前行号
        std::size_t m_column{1};   ///< 当前列号
    };
} // namespace AsynGyanis::Base
