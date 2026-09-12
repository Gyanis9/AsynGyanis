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
     * @details 默认值即「严格 YAML 1.2 + 安全上限」：类型按 1.2 核心 schema 判定，
     *          重复键视为错误，无法识别的保留指令（§6.8）按规范忽略并告警。
     *          maximumAliasCount 是**别名炸弹**（billion laughs）的核心防线，缺它则指数级
     *          展开会在解析期耗尽内存；取值 0 表示显式关闭该项保护，风险交由调用方承担。
     */
    struct YamlParseOptions
    {
        std::size_t maximumInputLength{64ULL * 1024ULL * 1024ULL};  ///< 输入文本字节数上限（64 MiB），0 表示不限制
        std::size_t maximumDepth{128};                              ///< 节点嵌套深度上限，0 表示不限制
        std::size_t maximumDocumentCount{256};                      ///< 多文档流中的文档份数上限，0 表示不限制
        std::size_t maximumAliasCount{10000};                       ///< 别名引用次数上限（防别名炸弹），0 表示不限制
        std::size_t maximumScalarLength{16ULL * 1024ULL * 1024ULL}; ///< 单个标量字节数上限（16 MiB），0 表示不限制

        bool allowDuplicateKeys{false};      ///< 允许同一映射内出现重复键（后者覆盖前者）；默认关闭，重复键几乎总是笔误
        bool rejectUnknownDirectives{false}; ///< 无法识别的保留指令（非 %YAML/%TAG，§6.8）按规范忽略并告警（false）还是直接报错（true）
    };
} // namespace AsynGyanis::Base
