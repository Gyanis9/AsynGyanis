/**
 * @file PostgresResult.cpp
 * @brief PostgreSQL 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// min / max 函数式宏会破坏 std::numeric_limits<T>::max() 等写法，必须在任何头之前挡住它们，
// 理由与写法说明见 PostgresConnection.cpp 同一位置的中文注释
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Database/Postgres/PostgresResult.h"

#ifdef DATABASE_HAS_POSTGRES

// libpq 与取值映射头只在本实现文件里包含，前置声明见 PostgresConnection.h 的全局作用域
#include <libpq-fe.h>

#include "Database/Postgres/PostgresValueConversion.h"

#include <charconv>
#include <system_error>

#endif // DATABASE_HAS_POSTGRES

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_POSTGRES

    namespace
    {
        /**
         * @brief 把 PQcmdTuples 给出的行计数文本解析成整数
         * @details PQcmdTuples 对没有行计数的命令（SELECT、BEGIN、SET 等）返回空串，
         *          对写语句返回十进制文本（如 "3"）。空串是「本命令没有行计数」的官方表达，
         *          因此映射成 0（与基类「0 表示未知」的约定一致），而不是当成错误。
         * @param rawCountText PQcmdTuples 的返回值，可为空指针
         * @return std::int64_t 行数；没有行计数或文本形态不符预期时返回 0
         */
        [[nodiscard]] std::int64_t parseAffectedRowCount(const char *rawCountText)
        {
            // 空指针与空串都表示「该命令没有行计数」：如实回 0，不编造一个非零值
            if (rawCountText == nullptr || *rawCountText == '\0')
            {
                return 0;
            }

            const std::string_view countText(rawCountText);

            std::int64_t                parsedValue = 0;
            const std::from_chars_result result     = std::from_chars(countText.data(), countText.data() + countText.size(), parsedValue);

            // 解析失败或尾部有余文说明文本不是纯十进制数：这是服务端形态不符预期，
            // 按「未知」处理比抛异常或钳位更符合 affectedRowCount() 的 noexcept 契约
            if (result.ec != std::errc{} || result.ptr != countText.data() + countText.size())
            {
                return 0;
            }

            return parsedValue;
        }
    } // namespace

    PostgresResult::PostgresResult(PGresult *const ownedResult) : m_result(ownedResult)
    {
        // 空句柄即「写操作的成功回执」：没有列也没有行，全部计数保持默认 0，isEmpty() 因此恒为 true
        if (m_result == nullptr)
        {
            return;
        }

        // 行数与列数在构造时一并快照：PQntuples / PQnfields 读的是 libpq 已取回本地的数据，
        // 不是游标；之后所有 const 接口都只读这两个计数，不再触碰句柄
        const int rawRowCount    = PQntuples(m_result);
        const int rawColumnCount = PQnfields(m_result);

        // 负值只可能来自无效句柄（正常路径不可达）：按 0 处理，避免下面按负数下标循环
        const std::size_t rowTotal    = rawRowCount > 0 ? static_cast<std::size_t>(rawRowCount) : 0U;
        const std::size_t columnTotal = rawColumnCount > 0 ? static_cast<std::size_t>(rawColumnCount) : 0U;

        // 影响行数必须在这里就地快照：PQcmdTuples 的结果是挂在 PGresult 上的一段文本，
        // PQclear 之后就没了，而本类的读取接口不再触碰句柄。SELECT 一类命令返回空串，
        // parseAffectedRowCount 会把它折成 0，符合「只读结果集返回 0」的基类约定
        m_affectedRowCount = parseAffectedRowCount(PQcmdTuples(m_result));

        m_columnNames.reserve(columnTotal);
        for (std::size_t columnIndex = 0; columnIndex < columnTotal; ++columnIndex)
        {
            const char *rawColumnName = PQfname(m_result, static_cast<int>(columnIndex));
            // 表达式列在部分场景下可能没有名字（libpq 给出空指针），补空串占位，
            // 保证列名列表长度恒等于列数、下标与列序严格对齐
            m_columnNames.emplace_back(rawColumnName != nullptr ? rawColumnName : "");
        }

        // 每列的 OID 只在构造时取一次：行循环里逐格调用 PQftype 是纯粹的重复劳动，
        // 而 PQftype 的结果在整个结果集生命周期内不会变
        std::vector<Oid> columnTypes(columnTotal);
        for (std::size_t columnIndex = 0; columnIndex < columnTotal; ++columnIndex)
        {
            columnTypes[columnIndex] = PQftype(m_result, static_cast<int>(columnIndex));
        }

        m_rows.reserve(rowTotal);
        for (std::size_t rowIndex = 0; rowIndex < rowTotal; ++rowIndex)
        {
            std::vector<DatabaseValue> currentRow;
            currentRow.reserve(columnTotal);

            for (std::size_t columnIndex = 0; columnIndex < columnTotal; ++columnIndex)
            {
                // 行列下标在 int 范围内：上面的总量已由 PQntuples / PQnfields 的 int 返回值界定
                const int rawRowIndex    = static_cast<int>(rowIndex);
                const int rawColumnIndex = static_cast<int>(columnIndex);

                if (PQgetisnull(m_result, rawRowIndex, rawColumnIndex) != 0)
                {
                    // 列值为 SQL NULL：与「空串」「0」是三件不同的事，只有 NULL 才映射成 monostate
                    currentRow.push_back(std::monostate{});
                    continue;
                }

                // 值一律按「指针 + 长度」取出：PQgetvalue 的返回值保证非空（SQL NULL 已在上一步挡掉），
                // 但仍留一道兜底，避免第三方库行为变化时把空指针交给下面的字符串构造
                const char *rawValue = PQgetvalue(m_result, rawRowIndex, rawColumnIndex);
                if (rawValue == nullptr)
                {
                    currentRow.push_back(std::monostate{});
                    continue;
                }

                // 长度为 0 表示空串（不是 NULL）：长度必须参与构造，否则含 '\0' 的字节序列会被截断
                const int rawLength = PQgetlength(m_result, rawRowIndex, rawColumnIndex);
                const std::size_t byteLength = rawLength > 0 ? static_cast<std::size_t>(rawLength) : 0U;

                currentRow.push_back(Detail::convertColumnText(columnTypes[columnIndex], rawValue, byteLength));
            }

            m_rows.push_back(std::move(currentRow));
        }
    }

    PostgresResult::~PostgresResult()
    {
        // 结果集由本对象独占所有权：PQclear 一次性回收行缓冲与列元数据。
        // 该接口没有返回码，析构路径也无从向调用方报错，因此这里不做额外检查。
        // 注意本对象的所有取值接口都只读内存快照，m_result 在构造之后就不再参与任何语义
        if (m_result != nullptr)
        {
            PQclear(m_result);
            m_result = nullptr;
        }
    }

#else // DATABASE_HAS_POSTGRES —— 桩实现：没有 libpq，结果集退化成永远为空的只读对象

    // 桩构建里不可能有 PGresult，构造函数刻意不使用参数值（也就无需 PQclear），
    // 全部状态保持默认：0 行 0 列、isEmpty() 为 true、影响行数为 0。
    // 桩下 connect() 必失败，任何写语句都执行不了，报出非零行数只会是假信息
    PostgresResult::PostgresResult(PGresult *)
    {
    }

    PostgresResult::~PostgresResult()
    {
        // 桩构建里没有 PGresult，也就没有 PQclear 要做的事；
        // 虚析构只能在类内首次声明处 = default，类外定义必须给出函数体而不是再写 = default
    }

#endif // DATABASE_HAS_POSTGRES

    // ------------------------------------------------------------------------
    // 以下定义只依赖构造阶段快照下来的成员，不触碰 libpq，
    // 因此真实实现与桩实现共用同一份定义（桩下列数恒为 0，一切自然退化为空集）
    // ------------------------------------------------------------------------

    bool PostgresResult::next()
    {
        // 游标已走到末尾（或本结果集是写回执）时没有下一行：顺手把当前行指针置空，
        // 保证 getValue() 的判定与之一致，不会读出上一行的残值
        if (m_nextRowIndex >= m_rows.size())
        {
            m_currentRow = nullptr;
            return false;
        }

        // 行数据在构造后不再变动，指向容器元素的指针因此始终有效，不需要额外的有效性标志位。
        // 按基类契约本方法属只读路径，不改写 m_lastError
        m_currentRow = &m_rows[m_nextRowIndex];
        ++m_nextRowIndex;
        return true;
    }

    std::optional<std::string> PostgresResult::columnName(const size_t index) const
    {
        // 越界一律回空值：列名快照的长度就是列数，判界后取元素不存在读越界
        if (index >= m_columnNames.size())
        {
            return std::nullopt;
        }

        return m_columnNames[index];
    }

    std::optional<size_t> PostgresResult::columnIndex(const std::string_view name) const
    {
        // 空列名一律视为不存在：避免把「调用方没填列名」误判成命中某个空名表达式列
        if (name.empty())
        {
            return std::nullopt;
        }

        for (size_t index = 0; index < m_columnNames.size(); ++index)
        {
            // 逐字节精确匹配（区分大小写）：元数据里的列名是服务端按查询原文保留的名字，
            // 归一化会把服务端的标识符规则复制一份到客户端，两边迟早不一致。同名列先到先得
            if (name == std::string_view(m_columnNames[index]))
            {
                return index;
            }
        }

        return std::nullopt;
    }

    DatabaseValue PostgresResult::getValue(const size_t index) const
    {
        // 游标没停在有效行上（未 next()、已走完、刚 reset()）时没有可读的当前行，
        // 统一按「无值」返回，与读到 NULL 列的表现一致
        if (m_currentRow == nullptr || index >= m_currentRow->size())
        {
            return std::monostate{};
        }

        // 取的是构造时已转好的值，因此不存在解析失败、截断或二次转换
        return (*m_currentRow)[index];
    }

    size_t PostgresResult::rowCount() const
    {
        return m_rows.size();
    }

    size_t PostgresResult::columnCount() const
    {
        return m_columnNames.size();
    }

    DatabaseValue PostgresResult::getValue(const std::string_view name) const
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

    std::vector<std::string> PostgresResult::columnNames() const
    {
        // 列名在构造时已按列序补齐（缺名列补空串），这里直接拷一份副本交给调用方，
        // 不泄漏内部容器的引用
        return m_columnNames;
    }

    void PostgresResult::reset()
    {
        // 先清掉上一轮的错误：本函数代表一次新的尝试，不能让历史文本冒充本次结果
        m_lastError.clear();

        // 既没有游标可复位（写回执），也没有第三方状态要清理：行数据在构造后不再变动，
        // 复位就是纯粹地回到「首行之前」
        m_nextRowIndex = 0;
        m_currentRow   = nullptr;
    }

    bool PostgresResult::isEmpty() const
    {
        // 行数在构造时已是精确值（写回执为 0），因此空与非空直接由行数判定，
        // 不再另存一份标志位——同一事实两处真值来源迟早会对不上
        return m_rows.empty();
    }

    std::int64_t PostgresResult::affectedRowCount() const noexcept
    {
        // 只把构造时快照的语句级影响行数交出去：本方法不触碰任何句柄，因此 noexcept 成立。
        // 查询结果集构造时解析到的是空串（PQcmdTuples 对 SELECT 不给行计数），即 0，
        // 符合基类「只读结果集返回 0」的约定
        return m_affectedRowCount;
    }

} // namespace AsynGyanis::Database
