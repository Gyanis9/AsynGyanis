/**
 * @file JsonParseOptions.h
 * @brief JSON 解析选项：安全上限与三个宽松语法开关
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 解析选项
     *
     * @details 默认值即「严格模式 + 安全上限」：三个宽松开关全部关闭，故默认行为与 RFC 8259 完全一致；
     *          四个上限的取值理由见各自字段注释。任一上限字段取 0 表示显式关闭该档保护，而不是「安全地
     *          不限」——尤其 maximumDepth，详见下方 @warning。
     *
     * @warning maximumDepth 取 0 是「把栈深风险交给调用方」，不是「安全地不限」：
     *          递归下降的每层嵌套都会真实压入一个栈帧，因此不限深度时，一份足够深的输入
     *          （几百到几千层，取决于平台的栈大小与编译选项——开启 AddressSanitizer 后
     *          每帧开销显著变大，这个阈值会明显下降）会直接耗尽栈并崩溃，而这不是一个
     *          可被捕获的解析错误。只在输入可信或已自行限制层数的场景下关闭它。
     */
    struct JsonParseOptions
    {
        std::size_t maximumInputLength{1024ULL * 1024ULL * 1024ULL}; ///< 输入文本字节数上限（1 GiB），0 表示不限制
        std::size_t maximumStringLength{64ULL * 1024ULL * 1024ULL};  ///< 单个字符串字节数上限（64 MiB，按原文含转义序列计），0 表示不限制
        std::size_t maximumDepth{256};                               ///< 容器嵌套深度上限（越深越易耗尽栈），0 表示不限制
        std::size_t maximumContainerElements{1000000};               ///< 单个数组/对象的元素个数上限（1e6），0 表示不限制

        bool allowComments{false};            ///< 允许 `//` 行注释与 `/* */` 块注释（JSON 扩展，默认关闭）
        bool allowTrailingCommas{false};      ///< 允许数组与对象末尾出现多余逗号（JSON5 风格，默认关闭）
        bool allowSingleQuotedStrings{false}; ///< 允许单引号字符串（仅支持与 JSON 相同的简单转义，默认关闭）
        bool skipUtf8Bom{true};               ///< 跳过输入起始的 UTF-8 BOM；默认开启以便直接消费 Windows 工具生成的文件
    };
} // namespace AsynGyanis::Base
