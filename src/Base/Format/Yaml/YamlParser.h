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

#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 严格 YAML 1.2 解析器
     *
     * @details 本类是 YamlReader 扫描器之上的 DOM 前端：只消费 YamlEvent 组装 FormatValue，与扫描器
     *          共享同一份解析逻辑。覆盖 1.2 全量特性（双引号 §5.7 转义表、块标量 §8.1.3 折叠规则、
     *          保留指令 §6.8 参数个数不受约束故零参数同样忽略）；标量按核心 schema 解析（yes/no/on/off/y/n
     *          是字符串、`0755` 十进制、`0o755` 八进制）；非标量键报错，别名深拷贝且受 maximumAliasCount 约束。
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
