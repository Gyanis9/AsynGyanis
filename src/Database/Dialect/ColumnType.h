/**
 * @file ColumnType.h
 * @brief 列的逻辑类型 —— 方言之间共享的类型词汇表
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details ORM 只知道逻辑类型（64 位整数 / 浮点 / 文本…），而各引擎的物理类型名差异很大
 *          （SQLite 只有 5 个存储类，MySQL 的整数按位宽分家），因此把「逻辑类型」抽成本枚举，
 *          由各 SqlDialect 用 columnTypeName() 翻译成自己的物理类型名。
 *          枚举只保留 ORM 当前能映射的成员类型所对应的取值，每个取值都有成员类型能产生它；
 *          日期时间、定点小数等类型等 ORM 支持了再往这里追加。
 *
 * @note 本枚举只是「类型名翻译」的输入，不参与值的编解码：值一律以 DatabaseValue
 *       绑定给驱动（见 RowMapper.h 的双向映射规则）。
 */
#pragma once

namespace AsynGyanis::Database
{
    /**
     * @brief 列的逻辑类型（与具体数据库无关的类型词汇）
     *
     * @details 由 ORM 侧按结构体成员类型推导（见 SchemaMigrator 的列类型推导），
     *          再交给 SqlDialect::columnTypeName() 换成目标引擎的物理类型名。
     */
    enum class ColumnType
    {
        Int64,  ///< 有符号 64 位整数（C++ 的 int8/16/32/64 一律映射到这里）
        UInt64, ///< 无符号 64 位整数（SQLite 无无符号类型，见 SqliteDialect::columnTypeName 的范围说明）
        Double, ///< 双精度浮点（C++ 的 float 也走这里：引擎侧没有更窄的小数类型值得单列）
        Bool,   ///< 布尔（两个引擎都没有独立的布尔物理类型，用整数 0/1 表达）
        Text,   ///< 变长文本（C++ 的 std::string）
        Blob    ///< 二进制大对象（C++ 的 std::vector<std::uint8_t> / std::vector<std::byte>，按原始字节存取，不做字符集解释）
    };

} // namespace AsynGyanis::Database
