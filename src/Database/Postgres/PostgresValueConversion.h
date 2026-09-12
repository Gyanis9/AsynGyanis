/**
 * @file PostgresValueConversion.h
 * @brief PostgreSQL 参数文本化与列值到 DatabaseValue 的类型映射（驱动内部共用，不对外暴露）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本头把驱动与 libpq 之间的两件事各收敛成一份实现，两个方向都只吃入参、不碰任何句柄：
 *          - 送出方向 toPostgresTextParameter()：DatabaseValue → libpq 文本格式参数
 *            （SQL NULL 用空指针表达、bool 用 "true"/"false"、数值用十进制文本、文本原样）；
 *          - 取回方向 convertColumnText()：PGresult 的单元格文本 → DatabaseValue
 *            （按列的 OID 决定映射，PQftype 的取值由调用方传入）。
 *
 * ## 为什么全文件都包在 DATABASE_HAS_POSTGRES 里
 * 本头依赖 <libpq-fe.h>（需要用 Oid 常量与 BOOLOID 之类类型标识），而桩构建（CMake 未找到 libpq）
 * 下这个第三方头根本不存在。把声明留在保护外会让每一个包含它的翻译单元在桩构建里编译失败，
 * 而桩分支按设计必须能单独编译通过，因此整个头内容都放进保护内：桩构建里它就是一个空头。
 * 这也是与 MySqlValueConversion.h 的唯一差异——那边靠「只在 .cpp 的保护分支内包含」达成同样效果，
 * 这里额外加一层自身保护，包含点就不必再关心顺序。
 *
 * @note 本头只在 Postgres 驱动内部的 .cpp 里包含（它必须看到真实的 <libpq-fe.h>），因此不会把
 *       第三方 C 头传染给使用方。两个转换函数声明为 inline 是为了不额外增加一个编译单元。
 */
#pragma once

#ifdef DATABASE_HAS_POSTGRES

#include "Database/Common/DatabaseValue.h"

#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

namespace AsynGyanis::Database::Detail
{
    /// 64 位有符号十进制的最大字符数：19 位数字 + 1 个负号，这里再留出余量便于一眼看出不会溢出
    inline constexpr std::size_t kIntegerTextBufferBytes = 24U;

    /// double 最短往返表示的字符数上限：最长形态是带指数的定点写法，40 字节远大于实测上界
    inline constexpr std::size_t kRealTextBufferBytes = 40U;

    /**
     * @brief 本驱动用到的 PostgreSQL 类型 OID（结果集按它决定每个列值怎么映射）
     *
     * @details 这些取值来自服务端系统表 pg_type 里**固定不变**的 OID（PostgreSQL 源码里的
     *          生成文件 catalog/pg_type_d.h）。这里之所以自己定义而不是直接包含官方头：
     *          那些 BOOLOID / INT4OID 之类的常量只出现在**服务端**头文件
     *          <postgresql/server/catalog/pg_type_d.h> 里，而多数发行版的 libpq 开发包
     *          （如 Debian 的 libpq-dev / 本仓库经 Conan 引入的包）只装客户端头，
     *          根本没有 server 目录——依赖它会让驱动在正常的客户端开发环境里编不过。
     *          这些 OID 是协议与系统表的既定事实（变动会让所有已编译客户端失效），
     *          因此以常量写死是安全的，也是客户端解析结果类型的常规做法。
     *          常量名保留 PostgreSQL 的原始语义名，便于与官方文档逐条对照。
     */
    namespace TypeOid
    {
        inline constexpr Oid kBoolean = 16U;   ///< BOOLOID：BOOLEAN，文本形态恒为 't' / 'f'
        inline constexpr Oid kInt8    = 20U;   ///< INT8OID：BIGINT
        inline constexpr Oid kInt2    = 21U;   ///< INT2OID：SMALLINT
        inline constexpr Oid kInt4    = 23U;   ///< INT4OID：INTEGER
        inline constexpr Oid kFloat4  = 700U;  ///< FLOAT4OID：REAL
        inline constexpr Oid kFloat8  = 701U;  ///< FLOAT8OID：DOUBLE PRECISION
        inline constexpr Oid kNumeric = 1700U; ///< NUMERICOID：NUMERIC / DECIMAL（精确定小数）
    } // namespace TypeOid

    /**
     * @brief 送往服务端的单个参数：libpq 文本格式参数所需的 (指针, 长度, 是否 NULL)
     *
     * @details 文本由本对象持有，因此把本对象放进一个「放好之后不再改动」的稳定容器
     *          （如先 reserve 再逐个 push_back 的 std::vector）之后，交给 libpq 的指针
     *          在整个执行期间都有效。若容器中途扩容，元素会被搬移，SSO 短字符串的
     *          内部缓冲随之失效——调用方必须先把参数收集齐、再取指针。
     */
    struct PostgresTextParameter
    {
        std::string text;          ///< 参数文本；内嵌 '\0' 完整保留（长度由调用方另行交给 libpq）
        bool        isNull{false}; ///< true 表示 SQL NULL：libpq 用空指针表达，text 不参与语义
    };

    /**
     * @brief 把一个 64 位有符号整数格式化成十进制文本
     * @details 用 std::to_chars 而不是 std::to_string：前者按最短往返表示输出且**不受 locale 影响**
     *          （std::to_string 走 sprintf 一类路径，受 LC_NUMERIC 影响），也不会抛异常。
     * @param value 待格式化的整数
     * @return std::string 十进制文本，例如 "-9223372036854775808"
     */
    inline std::string toDecimalText(const std::int64_t value)
    {
        // 缓冲区大小是编译期常量且远大于 int64 的最长十进制表示，因此 to_chars 不可能失败
        std::array<char, kIntegerTextBufferBytes> buffer{};
        const std::to_chars_result                result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

        // 按写入结束位置构造字符串（不使用 data() 的零终止形式，避免把多余字节带进参数）
        return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
    }

    /**
     * @brief 把一个 double 格式化成文本
     * @details 用 std::to_chars 的最短往返表示：任意 double 转出成文本再解析回来都能得到原值，
     *          比 std::to_string 的固定六位小数既更精确也更短；同样不受 locale 影响，
     *          小数点恒为 '.'（PostgreSQL 的数值输入语法无法接受逗号小数点）。
     *          Infinity / NaN 会被写成 "inf" / "-inf" / "nan"，PostgreSQL 的浮点输入
     *          大小写不敏感地接受这几种写法，因此不需要另行映射成 "Infinity"/"NaN"。
     * @param value 待格式化的浮点数
     * @return std::string 文本形式，例如 "1234.5"、"1e+30"
     */
    inline std::string toDecimalText(const double value)
    {
        std::array<char, kRealTextBufferBytes> buffer{};
        const std::to_chars_result             result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

        // 同 toDecimalText(int64)：缓冲区上界是编译期常量，写入必然成功
        return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
    }

    /**
     * @brief 把统一的 DatabaseValue 转成 libpq 文本格式参数
     *
     * @details 纯函数：只吃入参、不写任何状态，因此可以在任意路径上安全调用。映射规则：
     *          - std::monostate → isNull，由调用方交出空指针（SQL NULL 的唯一表达方式。
     *            若绑成空串，"IS NULL" 不再成立，NULL 与空串也会被混淆）；
     *          - bool → **"true" / "false" 两个字面量**：PostgreSQL 的 BOOLEAN 列不接受
     *            '0'/'1' 文本（那是 MySQL 的 TINYINT(1) 约定），写错会在服务端报
     *            "invalid input syntax for type boolean"；
     *          - std::int64_t / double → 十进制文本（见 toDecimalText 的两条说明）；
     *          - std::string → 原样，不转义也不改字节：SQL 结构由 $n 占位符固定，
     *            取值以参数送出，单引号、"--"、分号都只是普通字符（注入面因此消失）。
     *
     * @param value 待送出的统一值
     * @return std::optional<PostgresTextParameter> 可送出的参数
     * @return std::nullopt 容器类型（List / Hash）无法作为单个标量参数绑定：调用方必须据此
     *         给出中文错误，而不是静默绑成 NULL——静默会让调用方以为条件生效了
     */
    inline std::optional<PostgresTextParameter> toPostgresTextParameter(const DatabaseValue &value)
    {
        // NULL 先判：monostate 是「无值」，既不是数字也不是文本
        if (std::holds_alternative<std::monostate>(value))
        {
            return PostgresTextParameter{.text = {}, .isNull = true};
        }

        if (const auto *booleanValue = std::get_if<bool>(&value))
        {
            // 必须是 "true"/"false"：PostgreSQL 的布尔输入语法只认这几个字面量（及 t/f 等别名），
            // 传 "1"/"0" 会被服务端直接拒绝，因此这里与 MySQL 驱动的 0/1 约定刻意不同
            return PostgresTextParameter{.text = *booleanValue ? "true" : "false"};
        }

        if (const auto *integerValue = std::get_if<std::int64_t>(&value))
        {
            return PostgresTextParameter{.text = toDecimalText(*integerValue)};
        }

        if (const auto *realValue = std::get_if<double>(&value))
        {
            return PostgresTextParameter{.text = toDecimalText(*realValue)};
        }

        if (const auto *textValue = std::get_if<std::string>(&value))
        {
            // 原样交出字节：内嵌 '\0' 由调用方通过 libpq 的「指针 + 长度」形式保全，
            // 只要长度数组非空，libpq 就按长度取字节而不是找零终止符
            return PostgresTextParameter{.text = *textValue};
        }

        // 容器的正确用法是展开成多个标量参数（IN 列表由方言展开），这里明确拒绝并交回调用方报错
        return std::nullopt;
    }

    /**
     * @brief 把一段整数文本解析为 64 位有符号整数
     * @details 用 std::from_chars：不跳过前导空白、不接受余文、按 C locale 解析，且不抛异常
     *          （std::stoll 会抛、按进程 locale 解析，语义都比这里宽）。
     * @param rawValue 列值首地址，调用方保证在 [rawValue, rawValue + byteLength) 内可读
     * @param byteLength 列值字节长度，可为 0（空串）
     * @return std::optional<std::int64_t> 解析结果；非数字、有余文或超出 int64 范围时返回空值
     */
    inline std::optional<std::int64_t> parseIntegerText(const char *rawValue, const std::size_t byteLength)
    {
        std::int64_t parsedValue = 0;

        const std::from_chars_result result = std::from_chars(rawValue, rawValue + byteLength, parsedValue);

        // 三种失败各自判掉：一个字符都没消费（不是数字）、数值越界（result_out_of_range）、
        // 尾部仍有余文（如 "12abc"）。后者对应「文本形态不是纯整数」，一并退回原文更诚实
        if (result.ec != std::errc{} || result.ptr != rawValue + byteLength)
        {
            return std::nullopt;
        }

        return parsedValue;
    }

    /**
     * @brief 把一段浮点文本解析为 double
     * @param rawValue 列值首地址，调用方保证在 [rawValue, rawValue + byteLength) 内可读
     * @param byteLength 列值字节长度，可为 0（空串）
     * @return std::optional<double> 解析结果；非数字、有余文或超出 double 范围时返回空值
     * @note 标准规定 std::from_chars 的浮点解析**不识别 inf / nan**，而 PostgreSQL 的
     *       float8 输出在溢出情形下正是 "Infinity" / "NaN"：这类取值会解析失败并退回原始文本
     *       （见 convertColumnText 的兜底），不会凭空造出一个数值
     */
    inline std::optional<double> parseDoubleText(const char *rawValue, const std::size_t byteLength)
    {
        double parsedValue = 0.0;

        // 不指定 chars_format：默认 general，定点与科学计数法（服务端可能给出 '1e+30'）都接受
        const std::from_chars_result result = std::from_chars(rawValue, rawValue + byteLength, parsedValue);

        if (result.ec != std::errc{} || result.ptr != rawValue + byteLength)
        {
            return std::nullopt;
        }

        return parsedValue;
    }

    /**
     * @brief 按列的 OID 把一段单元格文本转换成统一的 DatabaseValue
     *
     * @details 纯函数：只读入参与列类型，不涉及任何句柄状态。列值一律按「指针 + 长度」交出，
     *          因此 BYTEA 的十六进制文本与包含任何字节的文本列都不会被截断。
     *
     * 映射规则（OID 常量取自本头的 Detail::TypeOid，来源与理由见那里的说明）：
     * - BOOLOID → bool：文本形态恒为单字节 't' / 'f'；
     * - INT2OID / INT4OID / INT8OID → std::int64_t；
     * - FLOAT4OID / FLOAT8OID → double；
     * - NUMERICOID → std::string：NUMERIC 是精确定小数（金额列、无符号 64 位的承载类型），
     *   转 double 会在末位丢精度且不可逆，取舍与 MySQL 对 DECIMAL / NEWDECIMAL 的处理一致；
     * - 其余（TEXT / VARCHAR / NAME / BYTEA / 日期时间 / JSON / UUID …）→ std::string 原样。
     *
     * @param columnType 列的 OID，来自 PQftype()
     * @param rawValue 列值首地址，调用方保证在 [rawValue, rawValue + byteLength) 内可读
     * @param byteLength 列值字节长度，可为 0（空串，与 SQL NULL 是两件事）
     * @return DatabaseValue 映射后的值；解析失败时退回原始文本（绝不钳位造数）
     */
    inline DatabaseValue convertColumnText(const Oid columnType, const char *rawValue, const std::size_t byteLength)
    {
        switch (columnType)
        {
            case TypeOid::kBoolean:
            {
                // PostgreSQL 的布尔文本输出只有 't' / 'f' 两个单字节形态（不是 "true"/"false"）；
                // 其它形态说明服务端给出的不是布尔列，退回原始文本而不是猜一个布尔值
                if (byteLength == 1U)
                {
                    if (rawValue[0] == 't')
                    {
                        return true;
                    }
                    if (rawValue[0] == 'f')
                    {
                        return false;
                    }
                }

                return std::string(rawValue, byteLength);
            }

            case TypeOid::kInt2:
            case TypeOid::kInt4:
            case TypeOid::kInt8:
            {
                if (const std::optional<std::int64_t> parsedValue = parseIntegerText(rawValue, byteLength);
                    parsedValue.has_value())
                {
                    return *parsedValue;
                }

                // int8 恒能放进 int64，走到这里说明服务端给出的文本形态与预期不符（被视图或
                // 自定义输出函数改写成带单位、带空格的字符串等），按原文交出比钳一个数值更诚实
                return std::string(rawValue, byteLength);
            }

            case TypeOid::kFloat4:
            case TypeOid::kFloat8:
            {
                if (const std::optional<double> parsedValue = parseDoubleText(rawValue, byteLength); parsedValue.has_value())
                {
                    return *parsedValue;
                }

                // "Infinity" / "NaN" 会走到这条兜底（from_chars 不识别它们），退回原文保住信息
                return std::string(rawValue, byteLength);
            }

            case TypeOid::kNumeric:
                // 精确定小数不做任何转换：转 double 丢精度不可逆，转整数会截断小数位
                return std::string(rawValue, byteLength);

            default:
                // 日期时间、字符、二进制、JSON、UUID 等其余类型在 DatabaseValue 里都只能用
                // std::string 承载：按 (指针, 长度) 原样拷贝，不依赖零终止符、不丢任何字节
                return std::string(rawValue, byteLength);
        }
    }

} // namespace AsynGyanis::Database::Detail

#endif // DATABASE_HAS_POSTGRES
