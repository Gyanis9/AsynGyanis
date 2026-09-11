/**
 * @file YamlParser.h
 * @brief 手写 YAML 解析器，覆盖配置文件常用子集
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
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief YAML 解析器（配置子集）
     *
     * @details 以「逻辑行 + 缩进」驱动的递归下降实现，支持：块映射、块序列、
     *          序列项内嵌映射、单行流式风格 `[a, b]` 与 `{k: v}`、双引号与单引号标量、
     *          整行与行尾注释、空行、以及 bool/int64/double/string/null 的类型推断。
     * @note 明确**不支持**并会给出可读错误提示的 YAML 特性：锚点与别名（& *）、
     *       多文档标记（`---` 与 `...`）、块标量（`|` 与 `>`）、显式类型标签（!!）、
     *       指令行（%YAML/%TAG）、制表符缩进、跨行的流式容器。
     *       配置场景用不到这些能力，拒绝而非静默误解更符合「配置文件出错要响」的原则。
     */
    class YamlParser
    {
    public:
        /**
         * @brief 解析一份 YAML 文本
         * @param text UTF-8 编码的 YAML 内容
         * @return ConfigValue 根节点值；输入为空或全为注释时返回空对象
         * @throws ParserError 缩进非法、语法不完整或使用了指明的不支持特性
         */
        [[nodiscard]] static ConfigValue parse(std::string_view text);

    private:
        /// 嵌套深度上限，避免损坏或恶意输入耗尽调用栈
        static constexpr std::size_t kMaximumNestingDepth = 32;

        /**
         * @brief 一条参与解析的逻辑行
         */
        struct Line
        {
            std::string_view content; ///< 去掉缩进与行尾注释后的正文
            std::size_t indent{0};    ///< 行首空格数
            std::size_t number{1};    ///< 原始行号，用于错误定位
        };

        /**
         * @brief 绑定输入
         * @param text 待解析文本
         */
        explicit YamlParser(std::string_view text);

        /**
         * @brief 把输入切分为逻辑行，同时校验缩进字符与注释边界
         * @throws ParserError 制表符参与缩进，或引号未闭合
         */
        void splitIntoLines();

        /**
         * @brief 解析一个块（映射或序列由首行形态决定）
         * @param index 输入输出参数，当前行下标
         * @param parentIndent 父块缩进，只有更深的行才属于本块
         * @param nestingDepth 当前嵌套深度
         * @return ConfigValue 块值；没有更深行时返回空对象
         */
        [[nodiscard]] ConfigValue parseBlock(std::size_t &index, std::size_t parentIndent, std::size_t nestingDepth);

        /**
         * @brief 解析同缩进的映射块
         * @param index 输入输出参数，当前行下标
         * @param blockIndent 本块缩进
         * @param nestingDepth 当前嵌套深度
         * @return ConfigValue 映射值
         */
        [[nodiscard]] ConfigValue parseMapping(std::size_t &index, std::size_t blockIndent, std::size_t nestingDepth);

        /**
         * @brief 解析同缩进的序列块
         * @param index 输入输出参数，当前行下标
         * @param blockIndent 本块缩进
         * @param nestingDepth 当前嵌套深度
         * @return ConfigValue 序列值
         */
        [[nodiscard]] ConfigValue parseSequence(std::size_t &index, std::size_t blockIndent, std::size_t nestingDepth);

        /**
         * @brief 解析出现在某一行内的值
         * @param text 值文本（已去除键与前导空白）
         * @param number 所在行号
         * @param column 值在行内的列号
         * @return ConfigValue 标量或流式容器值
         */
        [[nodiscard]] ConfigValue parseInlineValue(std::string_view text, std::size_t number, std::size_t column);

        /**
         * @brief 解析流式序列 `[a, b]`
         * @param text 所在行的剩余正文
         * @param cursor 输入输出参数，text 内的偏移
         * @param number 所在行号
         * @return ConfigValue 序列值
         */
        [[nodiscard]] ConfigValue parseFlowSequence(std::string_view text, std::size_t &cursor, std::size_t number);

        /**
         * @brief 解析流式容器内的一个值并把游标推进到其后
         * @param text 所在行的剩余正文
         * @param cursor 输入输出参数，text 内的偏移
         * @param number 所在行号
         * @return ConfigValue 该位置的值
         */
        [[nodiscard]] ConfigValue parseFlowValue(std::string_view text, std::size_t &cursor, std::size_t number);

        /**
         * @brief 解析流式映射 `{k: v}`
         * @param text 所在行的剩余正文
         * @param cursor 输入输出参数，text 内的偏移
         * @param number 所在行号
         * @return ConfigValue 映射值
         */
        [[nodiscard]] ConfigValue parseFlowMapping(std::string_view text, std::size_t &cursor, std::size_t number);

        /**
         * @brief 按 YAML 规则推断裸标量的类型
         * @param text 标量文本
         * @return ConfigValue 推断得到的值
         */
        [[nodiscard]] static ConfigValue inferScalar(std::string_view text);

        /**
         * @brief 判断正文是否以序列项标记开头
         * @param content 行正文
         * @return true 形如 `- xxx` 或单独一个 `-`
         */
        [[nodiscard]] static bool startsSequenceEntry(std::string_view content) noexcept;

        /**
         * @brief 在引号之外定位映射键的分隔冒号
         * @param content 行正文
         * @return std::size_t 冒号位置，未找到返回 npos
         */
        [[nodiscard]] static std::size_t findKeyValueSeparator(std::string_view content) noexcept;

        std::string_view m_text;      ///< 原始输入
        std::vector<Line> m_lines;     ///< 切分后的逻辑行
    };
} // namespace AsynGyanis::Base
