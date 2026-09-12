/**
 * @file JsonMergePatch.h
 * @brief JSON Merge Patch（RFC 7396）的递归合并
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
     * @brief JSON Merge Patch
     *
     * @details 与 RFC 6902 的 JSON Patch 是**两套互不兼容**的补丁格式：Merge Patch 不使用
     *          操作数组与路径表达式，而是直接把「目标文档形状」当作补丁描述（RFC 7396 §1）。
     *          因此单独成类而不是塞进 JsonPatch，避免调用方误以为两者可以互换。
     *
     *          递归规则（RFC 7396 §2）：
     *          - 补丁是对象：目标不是对象时先视为空对象，然后逐成员处理；
     *            成员值为 `null` 表示**删除**该成员；否则以「目标同名成员（可能不存在）」为
     *            目标递归合并；目标缺失同名成员时该成员被整体写入；
     *          - 补丁不是对象（标量、数组或 `null`）：**整体替换**目标，
     *            注意数组没有逐元素合并语义，一个数组补丁会把目标数组整个换掉；
     *          - `null` 补丁把目标替换为 `null`。
     *
     * @note 合并过程不修改入参，返回新文档；需要就地更新请用 applyInPlace()。
     */
    class JsonMergePatch
    {
    public:
        /**
         * @brief 把 Merge Patch 合并到目标文档上并返回新文档
         * @details 按 RFC 7396 §2 递归合并，入参 target 与 patch 都不会被修改。
         * @param target 原始文档
         * @param patch Merge Patch 文档（可以是任意 JSON 值）
         * @return FormatValue 合并后的新文档
         */
        [[nodiscard]] static FormatValue apply(const FormatValue &target, const FormatValue &patch);

        /**
         * @brief 把 Merge Patch 就地合并到目标文档上
         * @details 实现为 `target = apply(target, patch)`，语义与 apply() 完全一致。
         * @param target 目标文档，成功后被替换为合并结果
         * @param patch Merge Patch 文档
         */
        static void applyInPlace(FormatValue &target, const FormatValue &patch);
    };
} // namespace AsynGyanis::Base
