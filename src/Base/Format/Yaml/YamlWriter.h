/**
 * @file YamlWriter.h
 * @brief 把配置值序列化为 YAML 1.2 文本
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Yaml/YamlWriteOptions.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief YAML 序列化器
     *
     * @details 与 YamlParser 严格互逆：`YamlParser::parse(YamlWriter::write(value))`
     *          必须得到与 value **相等**（FormatValue::operator==）的值，这是本类的第一目标，
     *          其余格式美学（缩进、flow 风格、折行）都让位于它。
     *
     *          无损性由三组规则共同保证：
     *          - 类型保真：null/bool/整数/浮点各自写成核心 schema（§10.2.1）中唯一的写法，
     *            整数值的浮点补 `.0`，NaN 与无穷写成 `.nan` / `.inf` / `-.inf`，
     *            形如 `true`、`123`、`null`、`~` 的**字符串**一律加引号（否则类型当场改变）；
     *          - 文本保真：裸标量只在安全时使用（§7.3.3 的 `: ` 与 ` #` 禁区、YAML 指示符
     *            首字符、首尾空白等一律排除），需要转义时优先单引号（只需 `''` 折叠），
     *            含控制字符时改用双引号（`\xXX` / `\uXXXX`），含换行时优先块标量（§8.1）；
     *          - 结构保真：块风格统一按「键行 + 更深缩进」展开，块标量显式给出缩进指示符，
     *            chomping 按尾部换行个数三态落地——0 个用 `-`（strip）、1 个用默认的 clip、
     *            多个用 `+`（keep）并只补 N-1 个空行，因为 keep 的「保留全部」里已经含有
     *            正文末行自身的那个换行（§7.3.1、§8.1.1.2）。
     *
     * @note 输出是**单份文档**：多文档流请用 emitDocumentStart 拼装，或在外层自行加 `---`。
     * @note reuseAnchors 打开后，重复子树的第二处及以后写成别名 `*a1`；解析时别名按深拷贝
     *       展开，因此 round-trip 结果仍是相等的值，但会占用解析侧的别名展开预算。
     * @note 两处**无法用文本消除**的不等：其一，核心 schema 只有一种整数类型，因此
     *       `UInt` 落在 int64 正区间时会回读为 `Int`（数值相同、类型不同），只有大于
     *       INT64_MAX 的 `UInt` 才能原样回读；其二，NaN 按 IEEE 754 与自身不相等，
     *       含 NaN 的值需要用 isNan 之类的按位判断验证回读结果。
     */
    class YamlWriter
    {
    public:
        /**
         * @brief 按默认选项序列化配置值
         * @param value 待序列化的配置值
         * @return std::string YAML 1.2 文本，末尾带一个换行
         * @throws FormatError 嵌套层数超出默认上限（kind 为 DepthExceeded）
         */
        [[nodiscard]] static std::string write(const FormatValue &value);

        /**
         * @brief 按指定选项序列化配置值
         * @param value 待序列化的配置值
         * @param options 序列化选项
         * @return std::string YAML 1.2 文本，末尾带一个换行
         * @throws FormatError 嵌套层数超出 options.maximumDepth（kind 为 DepthExceeded），
         *                     或 reuseAnchors 的检测过程遇到非法 UTF-8（kind 为 InvalidUtf8）
         * @details 选项默认值下解析得出来的任何值都能被写回；
         *          非默认值（如 useFlowForEmptyContainers = false、quoteAmbiguousStrings = false、
         *          multiLineStyle = Quoted）会主动选择有损表示，取舍见 YamlWriteOptions 各字段说明。
         */
        [[nodiscard]] static std::string write(const FormatValue &value, const YamlWriteOptions &options);
    };
} // namespace AsynGyanis::Base
