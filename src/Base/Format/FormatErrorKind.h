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
     * @details 分类只描述「失败的性质」，不携带位置与文案；位置与文案仍由 FormatError
     *          提供。这样调用方可以用 kind() 做分支（例如把超限类错误降级为警告、
     *          把语法类错误直接判为配置非法），而不必匹配易变的中文错误文本。
     *
     *          分类与各格式的对应关系（JSON 侧）：
     *          - 输入为空 → EmptyInput
     *          - 非法起始字符、缺分隔符等「此处不应出现该字节」→ UnexpectedByte
     *          - 字符串扫描到文档末尾仍未见收尾引号 → UnterminatedString
     *          - 容器扫描到文档末尾仍未见收尾括号 → UnterminatedContainer
     *          - 宽松模式下块注释未闭合 → UnterminatedComment
     *          - 转义序列非法、\u 十六进制位数不足或字符非法 → InvalidEscape
     *          - 数字语法非法（前导零、缺小数位、缺指数位）→ InvalidNumber
     *          - 数字语法合法但超出可表示范围 → NumberOutOfRange
     *          - true/false/null 拼写或后缀非法 → InvalidKeyword
     *          - 对象内出现重复键 → DuplicateKey
     *          - 文档根值结束后仍有非空白内容 → TrailingContent
     *          - 嵌套层级超限 → DepthExceeded
     *          - 输入长度、字符串长度或容器元素数超限 → SizeExceeded
     *          - 字符串内出现原始控制字符（U+0000..U+001F）→ ControlCharacter
     *          - 字符串内出现非法 UTF-8 字节序列 → InvalidUtf8
     *          - 转义中的代理项孤立或配对非法 → SurrogatePairError
     */
    enum class FormatErrorKind : std::uint8_t
    {
        None,                  ///< 未分类：旧构造路径与跨格式共用原语（TextEscapes）抛出的错误
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
        SurrogatePairError     ///< \u 转义中的代理项非法（孤立或配对错误）
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
            default:
                return "unknown";
        }
    }
} // namespace AsynGyanis::Base
