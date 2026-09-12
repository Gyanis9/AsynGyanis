/**
 * @file JsonPatch.h
 * @brief JSON Patch（RFC 6902）：六种操作的解析、校验与原子应用
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Value/FormatValue.h"

namespace AsynGyanis::Base
{
    /**
     * @brief JSON Patch
     *
     * @details 补丁本身就是一个 JSON 数组，每项是含 `op`、`path`（部分操作另需 `from`/`value`）
     *          的操作对象（RFC 6902 §3）。支持 add、remove、replace、move、copy、test 六种操作，
     *          逐条语义与条款对应关系见实现文件中的 `// RFC 6902 §x.y` 注释。
     *
     * 关键语义取舍（调用方必须知晓）：
     *   - **原子性**：`apply()` 先在文档副本上逐条落地，全部成功后才返回；
     *     任一条失败即抛异常，入参文档与补丁**都不会**被改动，不存在「改了一半」的中间态，
     *     满足 RFC 6902 §5「应用整个补丁文档不得被视为成功」的要求。
     *   - **add 的下标规则**：数组下标必须满足 `0 <= index <= size`，等于 size 等价于追加；
     *     大于 size 属于非法（RFC 6902 §4.1「指定的下标不得大于数组元素个数」）。
     *     数组 token `-` 表示追加到末尾。
     *   - **test 的相等判定**：数值按其**数值**比较——`1`、`1.0`、`1`（UInt）三者互等，
     *     与 RFC 6902 §4.6「numbers: are considered equal if their values are numerically equal」
     *     一致；其余类型先比类型再比内容，数组与对象递归比较。相等判定由 valuesEqual() 暴露。
     *   - **根路径 `""`**：add/replace 表示整体替换文档；remove 视为错误（文档必须总有根值）；
     *     test 比较整份文档；move 的 from 为空必然构成 path 的真前缀而非法。
     *
     * @note 入参文档不会被修改；需要就地更新请用 applyInPlace()，它同样是「先算后赋值」的原子过程。
     */
    class JsonPatch
    {
    public:
        /**
         * @brief 把补丁应用到文档上并返回新文档
         * @details 先在文档副本上逐条应用，全部成功后才返回副本；失败抛异常且不改动入参。
         * @param document 原始文档（不被修改）
         * @param patch JSON Patch 文档，必须是操作对象组成的数组
         * @return FormatValue 应用补丁后的新文档
         * @throws FormatError 补丁不是数组、操作对象缺字段或字段类型不符、操作名未知、
         *                     path/from 不是合法 JSON Pointer、目标不存在、
         *                     数组下标越界或非法、test 不通过
         */
        [[nodiscard]] static FormatValue apply(const FormatValue &document, const FormatValue &patch);

        /**
         * @brief 把补丁就地应用到文档上
         * @details 语义与 apply() 完全一致，只是把结果写回 document；实现为
         *          `document = apply(document, patch)`，因此失败时 document 保持原样。
         * @param document 目标文档，成功后被替换为应用补丁后的结果
         * @param patch JSON Patch 文档
         * @throws FormatError 与 apply() 相同的失败条件
         */
        static void applyInPlace(FormatValue &document, const FormatValue &patch);

        /**
         * @brief JSON 值的 Patch 语义相等判定
         * @details 与 test 操作使用同一套规则（RFC 6902 §4.6）：
         *          - 数值（Int/UInt/Double）只要数值相等即相等，`1` == `1.0` == `1u`；
         *            整数族之间按整数比较以避免超过 2^53 后的浮点精度丢失，
         *            整数与浮点混比时退化为 double 比较；含 NaN 恒不相等；
         *          - null、bool、string 按类型与值比较；
         *          - 数组要求长度相同且逐元素相等；对象要求成员集合相同且逐成员值相等；
         *          - 数值与非数值（例如 `1` 与 `"1"`）永不相等。
         * @param left 左值
         * @param right 右值
         * @return bool 按上述规则相等时返回 true
         */
        [[nodiscard]] static bool valuesEqual(const FormatValue &left, const FormatValue &right) noexcept;
    };
} // namespace AsynGyanis::Base
