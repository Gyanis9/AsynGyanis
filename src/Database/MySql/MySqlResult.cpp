/**
 * @file MySqlResult.cpp
 * @brief MySQL / MariaDB 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/MySql/MySqlResult.h"

#ifdef DATABASE_HAS_MYSQL

// MySQL C API 头只在本实现文件里包含，前置声明见 MySqlConnection.h 的全局作用域
#include <mysql/mysql.h>

#include <cerrno>
#include <cstdlib>
#include <limits>

#endif // DATABASE_HAS_MYSQL

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_MYSQL

    namespace
    {
        // 用 constexpr 常量取代宏：进制与错误判定基准集中在此，类型安全且作用域受控
        constexpr int kDecimalNumberBase = 10; ///< 服务端文本协议给出的整数列恒为十进制

        /**
         * @brief 把一段整数文本解析为 64 位有符号整数
         * @param rawValue 列值首地址，调用方保证非空
         * @param byteLength 列值字节长度
         * @return std::optional<std::int64_t> 解析结果；非数字、有余文或超出范围时返回空值
         */
        std::optional<std::int64_t> parseIntegerText(const char *rawValue, const size_t byteLength)
        {
            // 行缓冲里相邻字段首尾相接，不保证每个字段都以 '\0' 收尾；落一份 std::string 副本
            // 才有可靠的终止符，std::strtoll 的 endptr 判定也因此才成立（副本同时保住内嵌 '\0' 之后的字节）
            const std::string numericText(rawValue, byteLength);

            // errno 只反映最后一次 C 库调用的结果、成功时不会自清：不清残留就可能把上一次的 ERANGE 当成本次的
            errno = 0;

            char *endPointer = nullptr;
            const long long parsedValue = std::strtoll(numericText.c_str(), &endPointer, kDecimalNumberBase);

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
        std::optional<double> parseDoubleText(const char *rawValue, const size_t byteLength)
        {
            // 同 parseIntegerText：先拿到可靠的零终止符，endptr 判定才有意义
            const std::string numericText(rawValue, byteLength);

            errno = 0;

            char *endPointer = nullptr;
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
    } // namespace

    MySqlResult::MySqlResult(MYSQL_RES *const ownedResult) : m_result(ownedResult)
    {
        // 空句柄即「写操作的成功回执」：没有列也没有行，两个计数保持默认 0，isEmpty() 因此恒为 true
        if (m_result == nullptr)
        {
            return;
        }

        // 数据已由 mysql_store_result 完整读进客户端内存，这两个计数此后不会再变，
        // 构造时一次快照，之后所有 const 接口都只读缓存（预读成功的结果不会给出「行数未知」的哨兵值）
        m_rowCount    = static_cast<size_t>(mysql_num_rows(m_result));
        m_columnCount = static_cast<size_t>(mysql_num_fields(m_result));
    }

    MySqlResult::~MySqlResult()
    {
        // 结果集由本对象独占所有权：mysql_free_result 一次性回收行缓冲与列元数据。
        // 该接口没有返回码，析构路径也无从向调用方报错，因此这里不做额外检查。
        // m_currentRow 指向的正是这份内部缓冲，随之一起失效，绝不能再单独解引用
        if (m_result != nullptr)
        {
            mysql_free_result(m_result);
            m_result = nullptr;
        }
    }

    bool MySqlResult::next()
    {
        // 写回执没有游标，永远「没有下一行」；顺手把游标置空，保证 getValue() 的判定与之一致
        if (m_result == nullptr)
        {
            m_currentRow = nullptr;
            return false;
        }

        // 预读结果集的行都在客户端内存里，返回空指针只可能是已到末尾，不会再有「读取出错」这一分支。
        // 按基类契约本方法属只读路径，不改写 m_lastError
        m_currentRow = mysql_fetch_row(m_result);
        return m_currentRow != nullptr;
    }

    std::optional<std::string> MySqlResult::columnName(const size_t index) const
    {
        // 先用缓存的无符号列数判界：越界索引直接强转成 unsigned int 形参会回绕成另一个合法列号
        if (m_result == nullptr || index >= m_columnCount)
        {
            return std::nullopt;
        }

        // 元数据数组由 MYSQL_RES 自己持有，生命周期到 mysql_free_result 为止；
        // 这里立刻拷成 std::string 交出去，不把内部指针泄漏给调用方
        const MYSQL_FIELD *currentField = mysql_fetch_field_direct(m_result, static_cast<unsigned int>(index));
        if (currentField == nullptr || currentField->name == nullptr)
        {
            return std::nullopt;
        }

        return std::string(currentField->name);
    }

    std::optional<size_t> MySqlResult::columnIndex(const std::string_view name) const
    {
        if (m_result == nullptr || name.empty())
        {
            return std::nullopt;
        }

        // mysql_fetch_fields 一次给出整张元数据数组（长度即列数）；
        // 无列或元数据读取失败时它返回空指针，旧实现直接 fields[i] 解引用，这里必须先判空
        const MYSQL_FIELD *fields = mysql_fetch_fields(m_result);
        if (fields == nullptr)
        {
            return std::nullopt;
        }

        for (size_t index = 0; index < m_columnCount; ++index)
        {
            const char *rawName = fields[index].name;
            // 表达式列可能没有名字，跳过而不是当成匹配，避免把空名误认成一次命中
            if (rawName == nullptr)
            {
                continue;
            }

            // 显式构造 std::string_view 而不是依赖 char* 的隐式转换：比较语义写明白，也确保是逐字节比较。
            // 元数据里的列名是客户端库另建的一份 C 字符串（与行值不同，它保证零终止），因此可以只给首地址。
            // 列标识符的大小写敏感性由服务端排序规则决定，本方法按原文精确匹配（区分大小写）；
            // 同名列先到先得，与 MySQL 自身按名取列的规则一致
            if (name == std::string_view(rawName))
            {
                return index;
            }
        }

        return std::nullopt;
    }

    DatabaseValue MySqlResult::getValue(const size_t index) const
    {
        // 游标没停在有效行上（未 next()、已走完、刚 reset()）时行指针与长度表都不可信，
        // 旧实现会读出上一行的残值；统一按「无值」返回，与读到 NULL 列的表现一致
        if (m_result == nullptr || m_currentRow == nullptr || index >= m_columnCount)
        {
            return std::monostate{};
        }

        const char *rawValue = m_currentRow[index];
        // 列值为 SQL NULL：与「空串」「0」是三件不同的事，只有 NULL 才映射成 monostate
        if (rawValue == nullptr)
        {
            return std::monostate{};
        }

        // 长度表与行指针同批产出，只到下一次 mysql_fetch_row 之前有效，因此必须在推进游标前取。
        // 它是唯一能正确界定 TEXT/BLOB 边界的依据：行缓冲里字段首尾相接，不保证每个都以 '\0' 结束
        const unsigned long *columnLengths = mysql_fetch_lengths(m_result);
        if (columnLengths == nullptr)
        {
            // 拿不到长度就没有可信的取值边界，宁缺毋滥：按 C 字符串猜边界会截断二进制数据
            return std::monostate{};
        }

        return convertValue(rawValue, static_cast<size_t>(columnLengths[index]), index);
    }

    void MySqlResult::reset()
    {
        // 写回执本来就是空集，reset 是安全的空操作
        if (m_result == nullptr)
        {
            return;
        }

        // 先清掉上一轮的错误：本函数代表一次新的尝试，不能让历史文本冒充本次结果
        m_lastError.clear();

        // 预读结果集才支持随机定位（本驱动一律用 mysql_store_result，所以恒满足该前提）。
        // mysql_data_seek 是 void 接口，失败也无从知晓，这是它与 sqlite3_reset 的差别
        mysql_data_seek(m_result, 0);

        // 游标退回首行之前，当前行随之失效：不清这个指针会让 getValue() 继续读上一行的缓冲
        m_currentRow = nullptr;
    }

    DatabaseValue MySqlResult::convertValue(const char *const rawValue, const size_t byteLength, const size_t index) const
    {
        // 类型信息只能来自列元数据；取不到（索引判界已在调用方做过）时唯一安全的映射是按文本交出字节，
        // 至少不丢数据，也比猜一个类型更可靠
        const MYSQL_FIELD *currentField = (m_result != nullptr && index < m_columnCount)
                                              ? mysql_fetch_field_direct(m_result, static_cast<unsigned int>(index))
                                              : nullptr;
        if (currentField == nullptr)
        {
            return std::string(rawValue, byteLength);
        }

        // 文本协议下所有列都以字符串送达（MySQL 只在预处理语句的二进制协议里给原始字节），
        // 因此这里按声明类型决定「解析成什么」而不是「怎么取字节」——字节始终按 (指针, 长度) 拿
        switch (currentField->type)
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

#else // DATABASE_HAS_MYSQL —— 桩实现：没有客户端库，结果集退化成永远为空的只读对象

    // 桩构建里不可能有 MYSQL_RES，构造函数刻意不使用参数值（也就无需 mysql_free_result），
    // 全部状态保持默认：0 行 0 列、isEmpty() 为 true
    MySqlResult::MySqlResult(MYSQL_RES *)
    {
    }

    MySqlResult::~MySqlResult()
    {
        // 桩构建里没有 MYSQL_RES，也就没有 mysql_free_result 要做的事；
        // 虚析构只能在类内首次声明处 = default，类外定义必须给出函数体而不是再写 = default
    }

    bool MySqlResult::next()
    {
        return false;
    }

    std::optional<std::string> MySqlResult::columnName(const size_t) const
    {
        return std::nullopt;
    }

    std::optional<size_t> MySqlResult::columnIndex(const std::string_view) const
    {
        return std::nullopt;
    }

    DatabaseValue MySqlResult::getValue(const size_t) const
    {
        return std::monostate{};
    }

    void MySqlResult::reset()
    {
        // 桩里没有游标可复位；清空历史错误文本仍然要做，语义与真实实现保持一致
        m_lastError.clear();
    }

    // 仅为满足头文件里的声明而保留：桩构建里没有可转换的列值字节
    DatabaseValue MySqlResult::convertValue(const char *, const size_t, const size_t) const
    {
        return std::monostate{};
    }

#endif // DATABASE_HAS_MYSQL

    // ------------------------------------------------------------------------
    // 以下定义只依赖构造阶段快照下来的成员，不触碰 MySQL C API，
    // 因此真实实现与桩实现共用同一份定义（桩下列数恒为 0，一切自然退化为空集）
    // ------------------------------------------------------------------------

    size_t MySqlResult::rowCount() const
    {
        return m_rowCount;
    }

    size_t MySqlResult::columnCount() const
    {
        return m_columnCount;
    }

    DatabaseValue MySqlResult::getValue(const std::string_view name) const
    {
        // 先按名解析索引再走索引重载，保证两条路径的越界与 NULL 判定完全一致
        const std::optional<size_t> index = columnIndex(name);
        if (!index.has_value())
        {
            // 列名解析失败与列值为 NULL 在契约里同为 monostate，本方法属 const 读取路径，不写错误状态
            return std::monostate{};
        }

        return getValue(*index);
    }

    std::vector<std::string> MySqlResult::columnNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_columnCount);

        for (size_t index = 0; index < m_columnCount; ++index)
        {
            // 复用 columnName()，列名来源只有一处真值；缺名列补空串占位，
            // 保证返回列表长度恒等于 columnCount() 且下标与列序严格对齐
            names.push_back(columnName(index).value_or(std::string{}));
        }

        return names;
    }

    bool MySqlResult::isEmpty() const
    {
        // 预读结果的行数恒为精确值（写回执没有游标，行数也是 0），因此空与非空直接由行数判定，
        // 不再另存一份标志位——同一事实两处真值来源迟早会对不上
        return m_rowCount == 0;
    }

} // namespace AsynGyanis::Database
