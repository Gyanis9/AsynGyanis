/**
 * @file YamlParser.h
 * @brief 严格 YAML 1.2 解析器（DOM 前端）：在流式扫描器之上组装配置值模型
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Yaml/YamlParseOptions.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 严格 YAML 1.2 解析器
     *
     * @details 本类是 YamlReader 扫描器之上的 **DOM 前端**：扫描器负责词法与块结构
     *          （缩进、标量风格与折叠、块标量、跨行 flow、指令、标签、锚点），
     *          本类只消费 YamlEvent 并组装 FormatValue，因此两个前端共享同一份解析逻辑，
     *          不存在实现漂移。
     *
     *          覆盖的 YAML 1.2 全量特性：
     *          - 标量风格：plain（含多行折叠与空行）、单引号（`''` 转义）、双引号（§5.7 完整转义表、
     *            跨行折叠与转义换行）、块标量 `|`/`>`（chomping `-`/`+`、显式缩进指示、§8.1.3 折叠规则）；
     *          - 键：隐式键、显式复杂键 `? `、`: ` 空值（null）、引号与转义键；
     *          - 结构：块映射、块序列（含零缩进序列 `ports:` 换行 `- 80`）、任意嵌套、
     *            跨行 flow `[a,\n b]` 与 `{k: v,\n k2: v2}`、flow 内注释、空容器；
     *          - 多文档：`---`、`...`、指令段；无分隔符的后续文档按规范报错；
     *          - 指令：`%YAML 1.2`（版本校验）、`%TAG`（句柄展开）、未知指令默认忽略并告警；
     *          - 标签：`!!str/!!int/!!float/!!bool/!!null/!!seq/!!map/!!binary/!!merge`、
     *            本地标签 `!foo`、全局标签与 `!<verbatim>`；
     *          - 锚点/别名/合并键：`&a`、`*a`（未定义别名报错）、`<<: *base` 与 `<<: [*a, *b]`
     *            （显式键优先于合并结果，靠前的合并源优先）。
     *
     *          类型解析严格遵循 1.2 核心 schema（见 resolveScalar）：`yes/no/on/off/y/n` 是**字符串**；
     *          `0755` 是十进制 755；`0o755` 才是八进制；`1:30` 是字符串；
     *          十进制整数依次尝试 int64 → uint64 → double。
     *
     *          键的落地方式：FormatValueObject 的键必须是字符串，因此标量键按核心 schema 解析后
     *          转成**规范文本**（null → "null"，bool → "true"/"false"，整数 → 十进制文本，
     *          浮点 → 最短往返文本，字符串 → 原文）；**非标量键（映射/序列）明确报错**，
     *          因为把集合压成字符串键没有无损且无歧义的约定。
     *
     *          别名语义：别名解析为被引用子树的**深拷贝**（FormatValue 是值语义，不共享存储），
     *          展开总量受 YamlParseOptions::maximumAliasCount 约束，用于抵御别名炸弹。
     *          锚点可重名，别名总是指向**在它之前最近一次**声明的锚点。
     */
    class YamlParser
    {
    public:
        /**
         * @brief 按默认选项解析首份文档
         * @param text UTF-8 编码的 YAML 文本
         * @return FormatValue 首份文档的根值；输入为空或只有注释时返回 null（YAML 1.2 §9.1.1）
         * @throws FormatError 语法非法或超出默认上限
         * @note 多文档输入只返回首份文档；需要全部文档请用 parseAll。
         */
        [[nodiscard]] static FormatValue parse(std::string_view text);

        /**
         * @brief 按指定选项解析首份文档
         * @param text UTF-8 编码的 YAML 文本
         * @param options 解析选项（安全上限与宽容度开关）
         * @return FormatValue 首份文档的根值；无文档时返回 null
         * @throws FormatError 语法非法或超出 options 中给定的上限
         */
        [[nodiscard]] static FormatValue parse(std::string_view text, const YamlParseOptions &options);

        /**
         * @brief 按默认选项解析全部文档
         * @param text UTF-8 编码的 YAML 文本
         * @return std::vector<FormatValue> 各文档根值，按出现顺序
         * @throws FormatError 语法非法或超出默认上限
         * @note 与 parse 的差异：流中不含任何文档（空输入或只有注释）时返回**空 vector**，
         *       忠实反映「零文档」；而 parse 把零文档视为一份 null 文档。
         */
        [[nodiscard]] static std::vector<FormatValue> parseAll(std::string_view text);

        /**
         * @brief 按指定选项解析全部文档
         * @param text UTF-8 编码的 YAML 文本
         * @param options 解析选项
         * @return std::vector<FormatValue> 各文档根值，按出现顺序
         * @throws FormatError 语法非法或超出 options 中给定的上限
         */
        [[nodiscard]] static std::vector<FormatValue> parseAll(std::string_view text, const YamlParseOptions &options);
    };
} // namespace AsynGyanis::Base
