/**
 * @file YamlWriteOptions.h
 * @brief YAML 序列化选项：缩进、多行风格、行宽、flow 阈值与深度守卫
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace AsynGyanis::Base
{
    /**
     * @brief 多行字符串的标量风格策略
     *
     * @details 只影响「含换行符的字符串」如何落地：Literal 用块标量 `|`（§8.1.1），
     *          逐字节保留每一行与换行，唯一无损；Folded 用 `>`（§8.1.3）更省行数但
     *          **折叠本身有损**，仅在可无损折叠时启用，否则退回 Literal；Quoted 用
     *          双引号标量（§7.3.2）配 `\n` 转义，单行输出。
     */
    enum class ScalarStylePolicy : std::uint8_t
    {
        Literal, ///< 块标量 `|`：保留换行（默认，round-trip 无损）
        Folded,  ///< 块标量 `>`：可无损折叠时用折叠风格，否则回退为字面风格
        Quoted   ///< 双引号标量：换行写成 `\n` 转义，整体仍是单行
    };

    /**
     * @brief YAML 序列化选项
     *
     * @details 默认值即「人类可读 + 严格 YAML 1.2 + 可原样解析回等价值」：两空格缩进、块风格
     *          容器、多行字符串用字面块标量、形如 `true` 的字符串加引号。所有开关的默认值都保证
     *          `parse(write(value)) == value`，改动其中任何一项都可能产生有损输出。
     */
    struct YamlWriteOptions
    {
        std::size_t indentWidth{2}; ///< 每层缩进空格数；取 0 会被按 1 处理（§6.1 禁止 tab 缩进，且块结构至少需一个空格），默认 2 与主流 YAML 工具一致

        std::size_t lineWidth{80}; ///< 裸标量折行的建议行宽（列数）；0 表示绝不折行。默认 80 兼顾可读性与 diff 友好，折行点只选「单个空格」以保证折叠后文本不变（§7.3.3）

        ScalarStylePolicy multiLineStyle{ScalarStylePolicy::Literal}; ///< 含换行字符串的风格策略；默认 Literal 是唯一完全无损的多行表示

        bool useFlowForEmptyContainers{true}; ///< 空容器是否写成 `[]` / `{}`。默认 true：YAML 1.2 没有空容器的块风格写法（§8.2.2 / §8.2.3），置 false 只能退化为隐式 null（类型会变），仅在「空即无」的下游语义下使用

        std::size_t flowThreshold{0}; ///< 元素数不大于该值且元素全为标量时改用 flow 风格 `[a, b]` / `{k: v}`；0（默认）表示一律用块风格。小集合用 flow 更紧凑，大集合用块更易 diff

        bool emitDocumentStart{false}; ///< 是否在文档开头输出 `---`（§9.1.3）；默认 false，单文档流无需起始标记，置 true 便于把多份输出拼成多文档流

        bool quoteAmbiguousStrings{true}; ///< 形如 `"true"`、`"null"`、`"123"`、`"0x1F"`、`"~"`、`"yes"` 的字符串是否强制加引号。默认 true：不加引号会被核心 schema（§10.2.1）判成 bool/null/int，字符串类型当场丢失

        bool reuseAnchors{false}; ///< 是否为结构相同的重复子树输出锚点/别名（§3.2.2）；默认 false，因为等价性检测需要一次额外的全树遍历与哈希

        std::size_t maximumDepth{128}; ///< 嵌套深度上限（按容器层数计），0 表示不限制；默认 128 与 YamlParseOptions::maximumDepth 对齐，保证解析得出来的值必然写得回去
    };
} // namespace AsynGyanis::Base
