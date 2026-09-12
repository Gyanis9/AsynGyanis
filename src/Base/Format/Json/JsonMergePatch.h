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
     * @details 与 RFC 6902 的 JSON Patch 是**两套互不兼容**的补丁格式：本类不用操作数组与路径
     *          表达式，而是直接把「目标文档形状」当作补丁描述（RFC 7396 §1），故单独成类，
     *          避免调用方误以为两者可以互换。
     *          递归规则（RFC 7396 §2）：补丁成员的值为 `null` 表示**删除**该成员；补丁不是对象时
     *          **整体替换**目标（数组没有逐元素合并语义）；`null` 补丁把目标替换为 `null`。
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
