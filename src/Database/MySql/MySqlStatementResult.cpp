#include "Database/MySql/MySqlStatementResult.h"

#include <utility>

namespace AsynGyanis::Database
{
    MySqlStatementResult::MySqlStatementResult(std::vector<std::string> columnNames, std::vector<std::vector<DatabaseValue>> rows) :
        m_columnNames(std::move(columnNames)), m_rows(std::move(rows))
    {
        // 行数据与列名在连接预读阶段就是按同一份列元数据产出的，这里不再做对齐修正：
        // 靠「悄悄补齐」掩盖列数不一致，只会把预读阶段的缺陷藏起来
    }

    MySqlStatementResult::~MySqlStatementResult() = default;

    bool MySqlStatementResult::next()
    {
        // 已到末尾：清掉当前行标志，保证 getValue() 的判定与本方法一致（不再读到最后一行的残留值）
        if (m_nextRowIndex >= m_rows.size())
        {
            m_hasCurrentRow = false;
            return false;
        }

        // 下标先取后加：m_nextRowIndex 始终指向「下一次要交出的行」，从中可反推当前行位置
        m_hasCurrentRow = true;
        ++m_nextRowIndex;
        return true;
    }

    size_t MySqlStatementResult::rowCount() const
    {
        return m_rows.size();
    }

    size_t MySqlStatementResult::columnCount() const
    {
        return m_columnNames.size();
    }

    std::optional<std::string> MySqlStatementResult::columnName(const size_t index) const
    {
        // 越界索引直接返回空值，不做任何回绕或钳制：调用方应据此判断列名拼写
        if (index >= m_columnNames.size())
        {
            return std::nullopt;
        }

        return m_columnNames[index];
    }

    std::optional<size_t> MySqlStatementResult::columnIndex(const std::string_view name) const
    {
        if (name.empty())
        {
            return std::nullopt;
        }

        for (size_t index = 0; index < m_columnNames.size(); ++index)
        {
            // 显式构造 std::string_view 而不是依赖隐式转换：比较语义写明白，也确保是逐字节比较。
            // 列标识符的大小写敏感性由服务端排序规则决定，本方法按原文精确匹配；同名列先到先得
            if (name == std::string_view(m_columnNames[index]))
            {
                return index;
            }
        }

        return std::nullopt;
    }

    DatabaseValue MySqlStatementResult::getValue(const size_t index) const
    {
        // 游标没停在有效行上时一律按「无值」返回：构造后与 reset() 后都会走这条分支
        if (!m_hasCurrentRow || index >= m_columnNames.size())
        {
            return std::monostate{};
        }

        const std::vector<DatabaseValue> &currentRow = m_rows[m_nextRowIndex - 1];
        // 行内元素个数理论上等于列数（预读时严格按列序产出），但仍按行自身的长度判界，
        // 避免将来有人构造出列数不齐的行时发生越界读取
        if (index >= currentRow.size())
        {
            return std::monostate{};
        }

        return currentRow[index];
    }

    DatabaseValue MySqlStatementResult::takeValue(const size_t index)
    {
        // 判界三条与 getValue() 逐字一致：两处判定一旦分岔，「取一次」与「读一次」就会给出不同结果
        if (!m_hasCurrentRow || index >= m_columnNames.size())
        {
            return std::monostate{};
        }

        std::vector<DatabaseValue> &currentRow = m_rows[m_nextRowIndex - 1];
        if (index >= currentRow.size())
        {
            return std::monostate{};
        }

        // 直接把格子的载荷移动给调用方，不在本函数里造中间量再清空源：源会留成一个已搬空的
        // 同类型值（长文本/字节序列被搬走后长度为 0），基类契约写明「取过的格不得再读」。
        // 显式再赋一个 monostate 反而会让 GCC 13 在 SSO 缓冲上误判出 -Wfree-nonheap-object
        return std::move(currentRow[index]);
    }

    DatabaseValue MySqlStatementResult::getValue(const std::string_view name) const
    {
        // 先按名解析索引再走索引重载，保证两条路径的「无当前行」判定与取值规则完全一致
        const std::optional<size_t> index = columnIndex(name);
        if (!index.has_value())
        {
            // 列名解析失败与列值为 NULL 在契约里同为 monostate，本方法属 const 读取路径，不写错误状态
            return std::monostate{};
        }

        return getValue(*index);
    }

    std::vector<std::string> MySqlStatementResult::columnNames() const
    {
        // 直接返回快照的副本：列名来源只有构造时那一处真值，长度与列序天然对齐
        return m_columnNames;
    }

    void MySqlStatementResult::reset()
    {
        // 先清掉上一轮的错误文本：本函数代表一次新的遍历尝试
        m_lastError.clear();

        m_nextRowIndex  = 0;
        m_hasCurrentRow = false;
    }

    bool MySqlStatementResult::isEmpty() const
    {
        // 预读快照的行数恒为精确值，因此空与非空直接由行数判定，不再另存一份标志位——
        // 同一事实两处真值来源迟早会对不上
        return m_rows.empty();
    }

} // namespace AsynGyanis::Database
