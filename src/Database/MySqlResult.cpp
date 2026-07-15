/**
 * @file MySqlResult.cpp
 * @brief MySQL 结果集实现
 * @copyright Copyright (c) 2026
 */

#include "MySqlResult.h"

#ifdef DATABASE_HAS_MYSQL
#include <mysql/mysql.h>
#include <cstring>
#endif

namespace Database
{

#ifdef DATABASE_HAS_MYSQL

    MySqlResult::MySqlResult(MYSQL_RES *const resultPointer, MYSQL *const connection) :
        m_resultPointer(resultPointer),
        m_connection(connection)
    {
        if (m_resultPointer == nullptr)
        {
            m_isEmpty = true;
            return;
        }

        m_rowCount    = mysql_num_rows(m_resultPointer);
        m_columnCount = mysql_num_fields(m_resultPointer);
        m_isEmpty     = (m_rowCount == 0);
    }

    MySqlResult::~MySqlResult()
    {
        if (m_resultPointer != nullptr)
        {
            mysql_free_result(m_resultPointer);
            m_resultPointer = nullptr;
        }
    }

    bool MySqlResult::next()
    {
        if (m_resultPointer == nullptr) return false;
        m_currentRow = mysql_fetch_row(m_resultPointer);
        return m_currentRow != nullptr;
    }

    size_t MySqlResult::rowCount() const { return m_rowCount; }
    size_t MySqlResult::columnCount() const { return m_columnCount; }

    std::optional<std::string> MySqlResult::columnName(const size_t index) const
    {
        if (m_resultPointer == nullptr || index >= m_columnCount) return std::nullopt;
        const MYSQL_FIELD *field = mysql_fetch_field_direct(
            m_resultPointer, static_cast<unsigned int>(index));
        return field ? std::optional<std::string>(field->name) : std::nullopt;
    }

    std::optional<size_t> MySqlResult::columnIndex(const std::string_view name) const
    {
        if (m_resultPointer == nullptr) return std::nullopt;
        const unsigned int count = mysql_num_fields(m_resultPointer);
        const MYSQL_FIELD *fields = mysql_fetch_fields(m_resultPointer);
        for (unsigned int i = 0; i < count; ++i)
            if (fields[i].name == name) return i;
        return std::nullopt;
    }

    DatabaseValue MySqlResult::getValue(const size_t index) const
    {
        if (m_currentRow == nullptr || index >= m_columnCount)
            return std::monostate{};
        return convertValue(m_currentRow, index);
    }

    DatabaseValue MySqlResult::getValue(const std::string_view name) const
    {
        const auto idx = columnIndex(name);
        return idx.has_value() ? getValue(idx.value()) : std::monostate{};
    }

    std::vector<std::string> MySqlResult::columnNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_columnCount);
        for (size_t i = 0; i < m_columnCount; ++i)
            if (auto n = columnName(i); n.has_value())
                names.push_back(std::move(n.value()));
        return names;
    }

    void MySqlResult::reset()
    {
        if (m_resultPointer != nullptr)
        {
            mysql_data_seek(m_resultPointer, 0);
            m_currentRow = nullptr;
        }
    }

    bool MySqlResult::isEmpty() const { return m_isEmpty; }

    DatabaseValue MySqlResult::convertValue(char **const row, const size_t index) const
    {
        if (row == nullptr || row[index] == nullptr) return std::monostate{};
        const MYSQL_FIELD *field = mysql_fetch_field_direct(
            m_resultPointer, static_cast<unsigned int>(index));
        if (field == nullptr) return std::string(row[index]);

        switch (field->type)
        {
            case MYSQL_TYPE_TINY: case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG: case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24: case MYSQL_TYPE_YEAR:
            {
                const unsigned long *lengths = mysql_fetch_lengths(m_resultPointer);
                return (lengths && lengths[index] > 0)
                    ? static_cast<int64_t>(std::strtoll(row[index], nullptr, 10))
                    : static_cast<int64_t>(0);
            }
            case MYSQL_TYPE_FLOAT: case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_NEWDECIMAL:
            {
                const unsigned long *lengths = mysql_fetch_lengths(m_resultPointer);
                return (lengths && lengths[index] > 0) ? std::strtod(row[index], nullptr) : 0.0;
            }
            default:
                return std::string(row[index]);
        }
    }

#else // DATABASE_HAS_MYSQL — 桩实现

    MySqlResult::MySqlResult(MYSQL_RES *, MYSQL *) { m_isEmpty = true; }
    MySqlResult::~MySqlResult() = default;
    bool MySqlResult::next() { return false; }
    size_t MySqlResult::rowCount() const { return 0; }
    size_t MySqlResult::columnCount() const { return 0; }
    std::optional<std::string> MySqlResult::columnName(size_t) const { return std::nullopt; }
    std::optional<size_t> MySqlResult::columnIndex(std::string_view) const { return std::nullopt; }
    DatabaseValue MySqlResult::getValue(size_t) const { return std::monostate{}; }
    DatabaseValue MySqlResult::getValue(std::string_view) const { return std::monostate{}; }
    std::vector<std::string> MySqlResult::columnNames() const { return {}; }
    void MySqlResult::reset() {}
    bool MySqlResult::isEmpty() const { return true; }
    DatabaseValue MySqlResult::convertValue(char **, size_t) const { return std::monostate{}; }

#endif

} // namespace Database
