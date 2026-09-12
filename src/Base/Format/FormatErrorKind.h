/**
 * @file FormatErrorKind.h
 * @brief 文本解析失败的错误分类枚举，供上层按类别分流处理
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>

namespace AsynGyanis::Base
{
    /**
     * @brief 解析错误分类
     *
     * @details 分类只描述「失败的性质」，不携带位置与文案（位置与文案由 FormatError 提供），
     *          调用方可据 kind() 分支而不必匹配易变的中文错误文本；各分类的适用场景见下方
     *          枚举值注释。其中 JSON Pointer / Patch 一族都是「语法合法但语义不成立」，
     *          与字节级语法错误严格区分，因此一律不借用 UnexpectedByte。
     */
    enum class FormatErrorKind : std::uint8_t
    {
        None,                  ///< 未分类：尚无分类的构造路径与跨格式共用原语（TextEscapes）抛出的错误
        EmptyInput,            ///< 输入为空或只有空白/BOM
        UnexpectedByte,        ///< 出现了语法不允许的字节（起始字符、分隔符、对象键位置等）
        UnterminatedString,    ///< 字符串未闭合
        UnterminatedContainer, ///< 对象或数组未闭合
        UnterminatedComment,   ///< 宽松模式下块注释未闭合
        InvalidEscape,         ///< 转义序列非法（未知转义、\u 位数或十六进制字符非法）
        InvalidNumber,         ///< 数字语法非法（前导零、小数点后缺位、指数缺位）
        NumberOutOfRange,      ///< 数字语法合法但越界到连 double 都无法表示
        InvalidKeyword,        ///< true、false、null 字面量拼写或后缀非法
        DuplicateKey,          ///< 对象内出现重复键
        TrailingContent,       ///< 文档根值结束后出现意外内容
        DepthExceeded,         ///< 嵌套深度超出上限
        SizeExceeded,          ///< 输入长度、字符串长度或容器元素数超出上限
        ControlCharacter,      ///< 字符串内出现原始控制字符
        InvalidUtf8,           ///< 字符串内出现非法 UTF-8 字节序列
        SurrogatePairError,    ///< \u 转义中的代理项非法（孤立或配对错误）
        InvalidPointer,        ///< JSON Pointer 语法非法（缺前导 '/'、孤立 '~'、'~' 后非 0/1）
        InvalidPatchOperation, ///< JSON Patch 结构或操作非法（非数组、项非对象、缺字段、op 未知、数组下标文本非法、remove 根、move 的 from 是祖先）
        PatchTargetMissing,    ///< 补丁的 path/from 定位不到值（目标缺失、父级非容器、add 下标越界、数组下标越界）
        PatchTestFailed        ///< test 操作目标存在但与给定值不相等
    };

    /**
     * @brief 将解析错误分类转换为稳定的英文名称
     * @details 名称用于日志与测试断言，一经发布不再变化；不得用于面向用户的提示文本
     *          （用户可见文本由 FormatError 的中文 reason 承担）。
     * @param kind 解析错误分类
     * @return const char* 分类名称；未知值返回 "unknown"
     */
    [[nodiscard]] constexpr const char *errorKindName(const FormatErrorKind kind) noexcept
    {
        switch (kind)
        {
            case FormatErrorKind::None:
                return "none";
            case FormatErrorKind::EmptyInput:
                return "empty-input";
            case FormatErrorKind::UnexpectedByte:
                return "unexpected-byte";
            case FormatErrorKind::UnterminatedString:
                return "unterminated-string";
            case FormatErrorKind::UnterminatedContainer:
                return "unterminated-container";
            case FormatErrorKind::UnterminatedComment:
                return "unterminated-comment";
            case FormatErrorKind::InvalidEscape:
                return "invalid-escape";
            case FormatErrorKind::InvalidNumber:
                return "invalid-number";
            case FormatErrorKind::NumberOutOfRange:
                return "number-out-of-range";
            case FormatErrorKind::InvalidKeyword:
                return "invalid-keyword";
            case FormatErrorKind::DuplicateKey:
                return "duplicate-key";
            case FormatErrorKind::TrailingContent:
                return "trailing-content";
            case FormatErrorKind::DepthExceeded:
                return "depth-exceeded";
            case FormatErrorKind::SizeExceeded:
                return "size-exceeded";
            case FormatErrorKind::ControlCharacter:
                return "control-character";
            case FormatErrorKind::InvalidUtf8:
                return "invalid-utf8";
            case FormatErrorKind::SurrogatePairError:
                return "surrogate-pair-error";
            case FormatErrorKind::InvalidPointer:
                return "invalid-pointer";
            case FormatErrorKind::InvalidPatchOperation:
                return "invalid-patch-operation";
            case FormatErrorKind::PatchTargetMissing:
                return "patch-target-missing";
            case FormatErrorKind::PatchTestFailed:
                return "patch-test-failed";
            default:
                return "unknown";
        }
    }
} // namespace AsynGyanis::Base
