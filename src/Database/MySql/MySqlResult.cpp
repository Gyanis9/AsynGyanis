/**
 * @file MySqlResult.cpp
 * @brief MySQL / MariaDB 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// min / max 函数式宏会破坏 std::numeric_limits<T>::max() 等写法，必须在任何头之前挡住它们，
// 理由与写法说明见 MySqlConnection.cpp 同一位置的中文注释
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Database/MySql/MySqlResult.h"

#ifdef DATABASE_HAS_MYSQL

// MySQL C API 头只在本实现文件里包含，前置声明见 MySqlConnection.h 的全局作用域。
// 两种发行布局（顶层 mysql.h / mysql/ 子目录 mysql.h）的兼容写法必须与 MySqlConnection.cpp 保持一致，
// 说明见该文件同一位置的中文注释
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif

// 列值到 DatabaseValue 的类型映射与参数化执行路径共用一份实现，保证两条协议路径取值语义一致
#include "Database/MySql/MySqlValueConversion.h"

#endif // DATABASE_HAS_MYSQL

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_MYSQL

    MySqlResult::MySqlResult(MYSQL_RES *const ownedResult, const std::int64_t affectedRowCount)
        : m_result(ownedResult), m_affectedRowCount(affectedRowCount)
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

        // 列类型到 DatabaseValue 的映射与参数化执行路径共用 Detail::convertColumnText 一份实现：
        // 列类型以 int 传递是为了不在该共享头的签名里暴露第三方枚举，转换规则见那里
        return Detail::convertColumnText(static_cast<int>(currentField->type), rawValue, byteLength);
    }

#else // DATABASE_HAS_MYSQL —— 桩实现：没有客户端库，结果集退化成永远为空的只读对象

    // 桩构建里不可能有 MYSQL_RES，构造函数刻意不使用参数值（也就无需 mysql_free_result），
    // 全部状态保持默认：0 行 0 列、isEmpty() 为 true。影响行数同样按默认 0 处理——
    // 桩下 connect() 必失败，任何写语句都执行不了，报出非零行数只会是假信息
    MySqlResult::MySqlResult(MYSQL_RES *, const std::int64_t)
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

    std::int64_t MySqlResult::affectedRowCount() const noexcept
    {
        // 只把构造时快照的语句级影响行数交出去：本方法不触碰任何句柄，因此 noexcept 成立。
        // 查询结果集构造时传的是 0，符合基类「只读结果集返回 0」的约定
        return m_affectedRowCount;
    }

} // namespace AsynGyanis::Database
