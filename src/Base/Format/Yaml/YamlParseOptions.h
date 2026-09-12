/**
 * @file YamlParseOptions.h
 * @brief YAML 解析选项：安全上限、重复键策略与指令宽容度
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
     * @brief YAML 解析选项
     *
     * @details 默认值即「严格 YAML 1.2 + 安全上限」：解析出的类型一律按 1.2 核心
     *          schema 判定，重复键默认视为错误，未知指令默认按规范忽略。
     *
     *          默认值理由（安全值与真实配置规模之间留出足够余量）：
     *          - maximumInputLength = 64 MiB：单份手写配置达到 MiB 级已属异常，
     *            64 MiB 足以容纳机器生成的大配置，同时让「整份读进内存」的前提可控。
     *          - maximumDepth = 128：递归下降每层消耗一个栈帧，128 层在 1 MiB 线程栈
     *            上留有充足余量，也远高于真实配置的嵌套深度。
     *          - maximumDocumentCount = 256：多文档流通常用于日志或清单，256 份已很宽裕，
     *            同时避免无限追加文档耗尽内存。
     *          - maximumAliasCount = 10000：这是**别名炸弹**（billion laughs）的核心防线；
     *            别名展开按引用子树计一次，1 万次引用足以覆盖正常配置，又使指数级展开
     *            在解析期即被截断。
     *          - maximumScalarLength = 16 MiB：单个标量超过 16 MiB 基本可判定为数据而非配置，
     *            限制它可避免一个块标量就吃掉整个输入预算。
     *
     *          取值 0 表示显式关闭该项保护（不限制），供受控场景处理超大数据。
     */
    struct YamlParseOptions
    {
        std::size_t maximumInputLength{64ULL * 1024ULL * 1024ULL};  ///< 输入文本字节数上限（64 MiB），0 表示不限制
        std::size_t maximumDepth{128};                              ///< 节点嵌套深度上限，0 表示不限制
        std::size_t maximumDocumentCount{256};                      ///< 多文档流中的文档份数上限，0 表示不限制
        std::size_t maximumAliasCount{10000};                       ///< 别名引用次数上限（防别名炸弹），0 表示不限制
        std::size_t maximumScalarLength{16ULL * 1024ULL * 1024ULL}; ///< 单个标量字节数上限（16 MiB），0 表示不限制

        bool allowDuplicateKeys{false};        ///< 允许同一映射内出现重复键（后者覆盖前者）；默认关闭，重复键几乎总是笔误
        bool rejectUnknownDirectives{false};   ///< 未知指令（非 %YAML/%TAG）按规范忽略（false）还是报错（true）
    };
} // namespace AsynGyanis::Base
