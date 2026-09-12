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
     * @details 默认值即「严格模式 + 安全上限」：
     *          - 三个宽松开关全部关闭，行为与 RFC 8259 完全一致（注释、尾逗号、单引号字符串
     *            都不接受），因此既有严格行为与相关断言不受影响；
     *          - 四个上限给出远高于正常配置文件、又足以抵挡滥用输入的取值，详见各字段注释；
     *          - 上限字段取 0 表示显式关闭该档保护（不限制），便于受控场景处理超大数据。
     *
     *          默认值理由：
     *          - maximumInputLength = 1 GiB：单份配置文件达到 GiB 量级已属异常，
     *            而正常配置文本通常在 KiB 到 MiB 之间，1 GiB 留出足够余量的同时
     *            使「把整个输入读进内存」这一前提不至于被无限放大。
     *          - maximumStringLength = 64 MiB：单个字符串超过 64 MiB 基本可以判定为
     *            数据而非配置；限制它可避免一个字符串就吃掉整个输入预算。
     *          - maximumDepth = 256：递归下降解析器每层消耗一个栈帧，256 层在主流平台
     *            （含 1 MiB 线程栈）上都留有充足余量，同时远高于真实配置的嵌套深度。
     *          - maximumContainerElements = 1e6：单个容器 100 万个元素已超出配置语义的
     *            合理范围，同时把单容器内存占用限制在可预估的量级。
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
