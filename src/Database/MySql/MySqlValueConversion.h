/**
 * @file MySqlValueConversion.h
 * @brief MySQL 列文本值到 DatabaseValue 的类型映射（驱动内部共用，不对外暴露）
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 文本协议（mysql_store_result）与二进制协议（mysql_stmt_* 预处理语句）
 *          都把列值按「指针 + 长度」交给客户端，本头文件把「按列声明类型解析成 DatabaseValue」
 *          这件事收敛成一份实现：两条执行路径的取值语义因此必然一致——
 *          SQL NULL→monostate、整数列→int64_t、浮点列→double、DECIMAL 与其余类型→std::string。
 *
 * @note 本头只在 MySql 驱动内部的 .cpp 里包含（它必须看到真实的 mysql.h 才能拿到
 *       enum_field_types 常量），因此不会把第三方 C 头传染给使用方。转换函数声明为
 *       inline 是为了不额外增加一个编译单元，它们都很小且与调用点同在一个静态库里。
 */
#pragma once

#include "Database/Common/DatabaseValue.h"

// 两种发行布局（顶层 mysql.h / mysql/ 子目录 mysql.h）的兼容写法，说明见 MySqlConnection.cpp
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace AsynGyanis::Database::Detail
{
    /// 服务端文本协议给出的整数列恒为十进制
    inline constexpr int kDecimalNumberBase = 10;

    /**
     * @brief 把一段整数文本解析为 64 位有符号整数
     * @param rawValue 列值首地址，调用方保证非空
     * @param byteLength 列值字节长度
     * @return std::optional<std::int64_t> 解析结果；非数字、有余文或超出范围时返回空值
     */
    inline std::optional<std::int64_t> parseIntegerText(const char *rawValue, const std::size_t byteLength)
    {
        // 行缓冲里相邻字段首尾相接，不保证每个字段都以 '\0' 收尾；落一份 std::string 副本
        // 才有可靠的终止符，std::strtoll 的 endptr 判定也因此才成立（副本同时保住内嵌 '\0' 之后的字节）
        const std::string numericText(rawValue, byteLength);

        // errno 只反映最后一次 C 库调用的结果、成功时不会自清：不清残留就可能把上一次的 ERANGE 当成本次的
        errno = 0;

        char             *endPointer  = nullptr;
        const long long   parsedValue = std::strtoll(numericText.c_str(), &endPointer, kDecimalNumberBase);

        // 三种失败各自判掉：没消费任何字符（不是数字文本）、数值溢出 long long（ERANGE）、尾部仍有余文（如 "12abc"）
        if (errno == ERANGE || endPointer == numericText.c_str() || *endPointer != '\0')
        {
            return std::nullopt;
        }

        // long long 比 int64_t 更宽的平台（现实中没有，但标准允许）还要单独判一次收窄是否无损；
        // 等宽时 ERANGE 已经覆盖了溢出情形，再写这个比较会触发「恒假比较」的编译器告警
        if constexpr (sizeof(long long) > sizeof(std::int64_t))
        {
            if (parsedValue < static_cast<long long>(std::numeric_limits<std::int64_t>::min()) ||
                parsedValue > static_cast<long long>(std::numeric_limits<std::int64_t>::max()))
            {
                return std::nullopt;
            }
        }

        return static_cast<std::int64_t>(parsedValue);
    }

    /**
     * @brief 把一段浮点文本解析为 double
     * @param rawValue 列值首地址，调用方保证非空
     * @param byteLength 列值字节长度
     * @return std::optional<double> 解析结果；非数字、有余文或上/下溢时返回空值
     */
    inline std::optional<double> parseDoubleText(const char *rawValue, const std::size_t byteLength)
    {
        // 同 parseIntegerText：先拿到可靠的零终止符，endptr 判定才有意义
        const std::string numericText(rawValue, byteLength);

        errno = 0;

        char        *endPointer  = nullptr;
        const double parsedValue = std::strtod(numericText.c_str(), &endPointer);

        // strtod 的 ERANGE 同时涵盖上溢（HUGE_VAL）与下溢到 0，两者都说明这份数据落在 double 之外，
        // 一律判失败由调用方退回原始十进制文本，至少不凭空造数
        if (errno == ERANGE || endPointer == numericText.c_str() || *endPointer != '\0')
        {
            return std::nullopt;
        }

        // 十进制点依赖进程的 C 数值环境：MySQL 协议文本恒用 '.'，若上层改过 LC_NUMERIC，
        // 这里不会解析出错误数值，而是被上面的 endptr 判定挡成失败并退回文本
        return parsedValue;
    }

    /**
     * @brief 按列的声明类型把一段 (指针, 长度) 的原始字节转换成统一的 DatabaseValue
     * @details 纯函数：只读入参与列类型，不涉及任何句柄状态，因此两条执行路径都能安全调用。
     *          文本协议下所有列都以字符串送达（二进制协议也按本函数统一按文本缓冲读取），
     *          因此这里按声明类型决定「解析成什么」而不是「怎么取字节」——字节始终按 (指针, 长度) 拿。
     * @param columnType 列的声明类型（MYSQL_FIELD::type，取 int 是为了不在签名里暴露第三方枚举）
     * @param rawValue 列值首地址，调用方保证非空
     * @param byteLength 列值字节长度，可为 0（空串，与 SQL NULL 是两件事）
     * @return DatabaseValue 映射后的值
     */
    inline DatabaseValue convertColumnText(const int columnType, const char *rawValue, const std::size_t byteLength)
    {
        switch (columnType)
        {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_YEAR:
            {
                if (const std::optional<std::int64_t> parsedValue = parseIntegerText(rawValue, byteLength); parsedValue.has_value())
                {
                    return *parsedValue;
                }

                // 解析失败或数值超出 int64（BIGINT UNSIGNED 的上界是 2^64-1）时按原始十进制文本交出：
                // 钳到 LLONG_MAX 会凭空造出一个错误数值，返回 monostate 又等于直接丢数据
                return std::string(rawValue, byteLength);
            }

            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
            {
                if (const std::optional<double> parsedValue = parseDoubleText(rawValue, byteLength); parsedValue.has_value())
                {
                    return *parsedValue;
                }

                // 服务端的 NaN / Inf 文本由 strtod 正常识别，走到这里说明文本确实不是浮点数，退回原文
                return std::string(rawValue, byteLength);
            }

            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                // DECIMAL 是精确定小数（金额列的常规选择），转 double 会在末位丢精度且不可逆，
                // 因此原样交出十进制文本，由调用方决定用字符串还是本地高精度类型承接
                return std::string(rawValue, byteLength);

            default:
                // 日期时间、字符、二进制、BIT、SET、几何等其余类型在 DatabaseValue 里都只能用 std::string 承载：
                // 按 (指针, 长度) 原样拷贝，TEXT/BLOB 内嵌的 '\0' 因此不丢，也不依赖零终止符
                return std::string(rawValue, byteLength);
        }
    }

} // namespace AsynGyanis::Database::Detail
