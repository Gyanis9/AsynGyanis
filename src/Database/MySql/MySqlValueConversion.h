/**
 * @file MySqlValueConversion.h
 * @brief MySQL 列文本值到 DatabaseValue 的类型映射（驱动内部共用，不对外暴露）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 文本协议（mysql_store_result）与二进制协议（mysql_stmt_* 预处理语句）都把列值按
 *          「指针 + 长度」交给客户端，本头文件把「按列声明类型解析成 DatabaseValue」这件事收敛成
 *          一份实现：SQL NULL→monostate、整数列与 BIT 列→int64_t、浮点列→double、
 *          二进制列→BinaryBytes、DECIMAL 与其余类型→std::string。
 *
 * @note 二进制列**不能只看类型码**：MySQL 的 BLOB 与 TEXT 共用 MYSQL_TYPE_BLOB，VARBINARY 与
 *       VARCHAR 共用 MYSQL_TYPE_VAR_STRING，唯一的区分依据是列的字符集是否为 binary(63)，
 *       因此转换函数必须同时接收字符集号，否则会把 TEXT 列读成字节、或把 BLOB 读成文本。
 * @note 本头只在 MySql 驱动内部的 .cpp 里包含（它必须看到真实的 mysql.h 才能拿到 enum_field_types
 *       常量），不会把第三方 C 头传染给使用方；转换函数声明为 inline 是为了不额外增加编译单元。
 */
#pragma once

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseValue.h"

// mysql.h 顺着 <windows.h> 把 min/max 带成函数式宏，消费方一写 std::numeric_limits<T>::min() 就炸成
// C4003 + C2589（Linux 侧没有这对宏，容器门禁看不见）。在本头里挡一次：凡是经本头拿
// enum_field_types 的翻译单元都自动受保护，不必各自再记一句写在文件顶部的定义
#ifndef NOMINMAX
#define NOMINMAX
#endif

// 两种发行布局（顶层 mysql.h / mysql/ 子目录 mysql.h）的兼容写法，说明见 MySqlConnection.cpp
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace AsynGyanis::Database::Detail
{
    /**
     * @brief 把一段整数文本解析为 64 位有符号整数
     * @param rawValue 列值首地址，调用方保证非空
     * @param byteLength 列值字节长度
     * @return std::optional<std::int64_t> 解析结果；非数字、有余文或超出范围时返回空值
     */
    inline std::optional<std::int64_t> parseIntegerText(const char *rawValue, const std::size_t byteLength)
    {
        std::int64_t parsedValue = 0;

        // from_chars 直接吃「指针 + 长度」：不必先落一份 std::string 副本，
        // 也天然要求消费完整个区间——小数点、科学计数法、余文一律失败
        const auto parseResult = std::from_chars(rawValue, rawValue + byteLength, parsedValue);
        if (parseResult.ec != std::errc{} || parseResult.ptr != rawValue + byteLength)
        {
            return std::nullopt;
        }

        return parsedValue;
    }

    /**
     * @brief 把一段浮点文本解析为 double
     * @details 判定与拒绝面全部由 std::from_chars 的浮点重载给出，两点值得写清：它接受
     *          inf / infinity / nan（含大小写变体与服务端的 -nan），因此服务端的非有限值
     *          在这里就能还原成对应的 double，不需要额外拼写表；它**不接受前导 '+'**
     *          （"+7" 判失败），而 MySQL 印数时也不会带正号，所以这条放宽没有必要。
     * @param rawValue 列值首地址，调用方保证非空
     * @param byteLength 列值字节长度
     * @return std::optional<double> 解析结果；非数字、有余文或超出表示范围时返回空值
     */
    inline std::optional<double> parseDoubleText(const char *rawValue, const std::size_t byteLength)
    {
        double parsedValue = 0;

        // 与整数路径同一份理由：不落副本、不受 LC_NUMERIC 影响、必须消费完整个区间；
        // 上溢由 result_out_of_range 报出，调用方据此退回原始十进制文本，不凭空造数
        const auto parseResult = std::from_chars(rawValue, rawValue + byteLength, parsedValue);
        if (parseResult.ec != std::errc{} || parseResult.ptr != rawValue + byteLength)
        {
            return std::nullopt;
        }

        return parsedValue;
    }

    /// MySQL 的二进制字符集编号（information_schema.CHARACTER_SETS 中 binary 的 id 即 63）
    inline constexpr unsigned int kBinaryCharacterSetNumber = 63U;

    /**
     * @brief 判定一列是否为真正的二进制列
     *
     * @details MySQL 在协议层**不区分** BLOB 与 TEXT：两者都是 MYSQL_TYPE_BLOB（TINY/MEDIUM/LONG
     *          同理），VARBINARY 与 VARCHAR 都是 MYSQL_TYPE_VAR_STRING。唯一的判据是列字符集
     *          是否为 binary——因此只看类型码会把 TEXT 列读成字节。
     *
     * @param columnType 列的声明类型（MYSQL_FIELD::type）
     * @param characterSetNumber 列的字符集编号（MYSQL_FIELD::charsetnr）
     * @return true 该列是 BLOB / BINARY / VARBINARY 等真正的二进制列
     */
    inline bool isBinaryColumn(const int columnType, const unsigned int characterSetNumber) noexcept
    {
        // 字符集不是 binary 就是文本列，后面不必再看类型码
        if (characterSetNumber != kBinaryCharacterSetNumber)
        {
            return false;
        }

        switch (columnType)
        {
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_VARCHAR:
                return true;
            default:
                // 名单之外（几何、BIT 等）即便字符集是 binary 也不进 BinaryBytes：BIT 由 convertColumnText
                // 按大端整数还原，几何则按 std::string 交出服务端的 WKB 字节（DatabaseValue 没有几何备选，
                // 本驱动的方言也不建空间列）——两者都不当成「一段二进制载荷」原样交给调用方
                return false;
        }
    }

    /**
     * @brief 按列的声明类型把一段 (指针, 长度) 的原始字节转换成统一的 DatabaseValue
     * @details 纯函数：只读入参与列类型，不涉及任何句柄状态，因此两条执行路径都能安全调用。
     *          文本协议下所有列都以字符串送达（二进制协议也按本函数统一按文本缓冲读取），
     *          因此这里按声明类型决定「解析成什么」而不是「怎么取字节」——字节始终按 (指针, 长度) 拿。
     * @param columnType 列的声明类型（MYSQL_FIELD::type，取 int 是为了不在签名里暴露第三方枚举）
     * @param characterSetNumber 列的字符集编号（MYSQL_FIELD::charsetnr），二进制与文本的唯一判据
     * @param rawValue 列值首地址，调用方保证非空
     * @param byteLength 列值字节长度，可为 0（空值，与 SQL NULL 是两件事）
     * @return DatabaseValue 映射后的值
     */
    inline DatabaseValue convertColumnText(const int columnType, const unsigned int characterSetNumber, const char *rawValue, const std::size_t byteLength)
    {
        // 二进制先判：它的类型码与 TEXT 重叠，必须靠字符集把两类分开，
        // 否则下面的 switch 一定把 BLOB 当文本交出去
        if (isBinaryColumn(columnType, characterSetNumber))
        {
            BinaryBytes bytes;
            if (byteLength > 0)
            {
                // 按 (指针, 长度) 拷贝原始字节，内嵌 '\0' 不丢；不做任何字符集解释
                const auto *rawBytes = reinterpret_cast<const std::uint8_t *>(rawValue);
                bytes.assign(rawBytes, rawBytes + byteLength);
            }
            // 零长度 BLOB 与 SQL NULL 是两回事：给空序列而不是 monostate
            return bytes;
        }

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

                // 非有限值（inf / -inf / nan / -nan）由 std::from_chars 的浮点文法认出，
                // 到这里已经是个正常的 double；走到这条兜底说明文本连那套文法都不匹配，退回原文
                return std::string(rawValue, byteLength);
            }

            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                // DECIMAL 是精确定小数（金额列的常规选择），转 double 会在末位丢精度且不可逆，
                // 因此原样交出十进制文本，由调用方决定用字符串还是本地高精度类型承接
                return std::string(rawValue, byteLength);

            case MYSQL_TYPE_BIT:
            {
                // 协议把 BIT(M) 送成 ⌈M/8⌉ 字节的**大端无符号整数**（实测 BIT(8)=200 回 0xC8、
                // BIT(1)=0 回一个长度为 1 的 NUL 字节）。按文本交出等于把一串控制字节冒充成文本列，
                // 因此还原成数值：装得进 int64 给整数，装不进（BIT(64) 上界 2^64-1）按十进制文本交出
                // ——与 MYSQL_TYPE_*UNSIGNED 同一套约定，RowMapper 的文本支路能原样读回。
                // 空载荷与超过 8 字节的意外宽度一律不猜，退回原文交出去
                if (byteLength == 0 || byteLength > sizeof(std::uint64_t))
                {
                    return std::string(rawValue, byteLength);
                }

                std::uint64_t bitFieldValue = 0;
                for (size_t byteIndex = 0; byteIndex < byteLength; ++byteIndex)
                {
                    // 大端拼装：先到的字节是高位。unsigned char 转换不能省，
                    // 否则 char 为有符号的平台会把 0x80 以上的字节符号扩展成负数污染高位
                    bitFieldValue = (bitFieldValue << 8U) | static_cast<unsigned char>(rawValue[byteIndex]);
                }

                if (bitFieldValue <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                {
                    return static_cast<std::int64_t>(bitFieldValue);
                }
                return std::to_string(bitFieldValue);
            }

            default:
                // 日期时间、字符集文本、SET、几何等其余类型在 DatabaseValue 里都只能用 std::string 承载：
                // 按 (指针, 长度) 原样拷贝，内嵌的 '\0' 因此不丢，也不依赖零终止符。
                // （二进制列已在函数开头按字符集分流，不会走到这里；GEOMETRY 落在这里，
                // 交出的是服务端给的 WKB 字节，不是 WKT 文本）
                return std::string(rawValue, byteLength);
        }
    }

} // namespace AsynGyanis::Database::Detail
