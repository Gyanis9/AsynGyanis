/**
 * @file YamlEvent.h
 * @brief YAML 流式解析事件模型：事件类型、标量风格与单条事件载体
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/TextPosition.h"

#include <cstdint>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief YAML 事件类型
     *
     * @details 与 YAML 1.2 规范 §3.5「事件流」的节点事件一一对应，另按本项目需要
     *          把锚点与别名显式化为独立事件。消费方按「开始/结束」成对入栈即可重建
     *          文档结构，无需关心缩进与标量风格的差异。
     */
    enum class YamlEventType : std::uint8_t
    {
        StreamStart,   ///< 流开始：一次 feed 后首个事件
        StreamEnd,     ///< 流结束：之后不再有事件
        DocumentStart, ///< 文档开始：对应 `---` 或隐式文档头
        DocumentEnd,   ///< 文档结束：对应 `...` 或隐式文档尾
        MappingStart,  ///< 映射开始：进入一个键值对集合
        MappingEnd,    ///< 映射结束
        SequenceStart, ///< 序列开始：进入一个条目集合
        SequenceEnd,   ///< 序列结束
        Scalar,        ///< 标量：携带已解码文本、风格与显式标签
        Alias,         ///< 别名：指向此前某个锚点
        Anchor         ///< 锚点声明：紧随其后的节点将绑定该锚点名
    };

    /**
     * @brief YAML 标量风格
     *
     * @details 风格决定标量的解码方式与类型解析规则：引号风格一律解析为字符串，
     *          plain 风格参与核心 schema 类型推断，块风格保留换行结构。
     */
    enum class YamlScalarStyle : std::uint8_t
    {
        Plain,        ///< 裸标量：参与核心 schema 的类型推断（null/bool/int/float）
        SingleQuoted, ///< 单引号：仅 `''` 转义，结果恒为字符串
        DoubleQuoted, ///< 双引号：完整转义表，结果恒为字符串
        Literal,      ///< 块标量 `|`：保留换行
        Folded        ///< 块标量 `>`：折叠换行
    };

    /**
     * @brief 单条 YAML 解析事件
     *
     * @details 事件自持标量文本（std::string），因此可以脱离源缓冲独立存放——
     *          YamlReader::readAll() 能在读取器析构后安全返回事件序列。扫描器只做
     *          **一次**文本构造：无折叠、无转义的裸标量直接从源视图构造，有折叠或转义时
     *          才在内部缓冲上追加；DOM 前端随后用 std::move 取走文本，不再二次拷贝。
     */
    struct YamlEvent
    {
        YamlEventType   type{YamlEventType::StreamEnd}; ///< 事件类型
        YamlScalarStyle style{YamlScalarStyle::Plain};  ///< 标量风格；非 Scalar 事件无意义

        std::string text; ///< 标量解码/折叠后的最终文本；非 Scalar 事件为空

        std::string tag;    ///< 显式标签；空表示无显式标签（如 `!!str` 展开为 `tag:yaml.org,2002:str`）
        std::string anchor; ///< 锚点名；Anchor 事件或带锚点的节点事件非空
        std::string alias;  ///< 别名指向的锚点名；仅 Alias 事件非空

        TextPosition position; ///< 事件在原始输入中的起始位置

        /**
         * @brief 判断是否为集合开始事件
         * @return true 事件为 MappingStart 或 SequenceStart
         */
        [[nodiscard]] bool isContainerStart() const noexcept
        {
            return type == YamlEventType::MappingStart || type == YamlEventType::SequenceStart;
        }

        /**
         * @brief 判断是否为集合结束事件
         * @return true 事件为 MappingEnd 或 SequenceEnd
         */
        [[nodiscard]] bool isContainerEnd() const noexcept
        {
            return type == YamlEventType::MappingEnd || type == YamlEventType::SequenceEnd;
        }
    };
} // namespace AsynGyanis::Base
