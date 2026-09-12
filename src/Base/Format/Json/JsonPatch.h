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
     * @details 补丁是 JSON 数组，每项为含 `op`、`path`（部分操作另需 `from`/`value`）的操作对象
     *          （RFC 6902 §3），支持 add/remove/replace/move/copy/test 六种操作。原子性：先在副本上
     *          逐条落地，全部成功才返回，失败则入参文档与补丁都不被改动（RFC 6902 §5）。
     *          add 的下标须满足 `0 <= index <= size`、`-` 表示追加（RFC 6902 §4.1）；test 按数值比较（§4.6）。
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
         * @details 与 test 操作同一套规则（RFC 6902 §4.6）：数值只要数值相等即相等（`1` == `1.0` == `1u`），
         *          整数族之间按整数比较以避免超过 2^53 后的浮点精度丢失，整数与浮点混比退化为 double
         *          比较，含 NaN 恒不相等；容器按结构与内容逐层比较，数值与非数值（`1` 与 `"1"`）永不相等。
         * @param left 左值
         * @param right 右值
         * @return bool 按上述规则相等时返回 true
         */
        [[nodiscard]] static bool valuesEqual(const FormatValue &left, const FormatValue &right) noexcept;
    };
} // namespace AsynGyanis::Base
