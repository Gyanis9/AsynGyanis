/**
 * @file SqliteResult.cpp
 * @brief SQLite 结果集实现
 * @copyright Copyright (c) 2026
 */

#include "SqliteResult.h"

#include <sqlite3.h>

#include <cstring>

namespace Database
{

    SqliteResult::SqliteResult(sqlite3_stmt *const statement, sqlite3 *const database) :
        m_statement(statement),
        m_database(database)
    {
        if (m_statement == nullptr)
        {
            m_isEmpty = true;
            return;
        }

        m_columnCount = sqlite3_column_count(m_statement);
        m_isEmpty     = (m_columnCount == 0);

        // 计数所有行（next() 首次调用时才开始遍历）
        countRows();
    }

    SqliteResult::~SqliteResult()
    {
        if (m_statement != nullptr)
        {
            sqlite3_finalize(m_statement);
            m_statement = nullptr;
        }
    }

    bool SqliteResult::next()
    {
        if (m_statement == nullptr) return false;
        m_hasRow = (sqlite3_step(m_statement) == SQLITE_ROW);
        return m_hasRow;
    }

    size_t SqliteResult::rowCount() const
    {
        return m_rowCount;
    }

    size_t SqliteResult::columnCount() const
    {
        return m_columnCount;
    }

    std::optional<std::string> SqliteResult::columnName(const size_t index) const
    {
        if (m_statement == nullptr || static_cast<int>(index) >= sqlite3_column_count(m_statement))
        {
            return std::nullopt;
        }

        const char *name = sqlite3_column_name(m_statement, static_cast<int>(index));
        if (name == nullptr)
        {
            return std::nullopt;
        }
        return std::string(name);
    }

    std::optional<size_t> SqliteResult::columnIndex(const std::string_view name) const
    {
        if (m_statement == nullptr)
        {
            return std::nullopt;
        }

        const int count = sqlite3_column_count(m_statement);
        for (int i = 0; i < count; ++i)
        {
            const char *columnNamePtr = sqlite3_column_name(m_statement, i);
            if (columnNamePtr != nullptr && name == columnNamePtr)
            {
                return static_cast<size_t>(i);
            }
        }
        return std::nullopt;
    }

    DatabaseValue SqliteResult::getValue(const size_t index) const
    {
        if (m_statement == nullptr || static_cast<int>(index) >= sqlite3_column_count(m_statement))
        {
            return std::monostate{};
        }

        return convertValue(static_cast<int>(index));
    }

    DatabaseValue SqliteResult::getValue(const std::string_view name) const
    {
        const auto index = columnIndex(name);
        if (!index.has_value())
        {
            return std::monostate{};
        }
        return getValue(index.value());
    }

    std::vector<std::string> SqliteResult::columnNames() const
    {
        std::vector<std::string> names;
        if (m_statement == nullptr)
        {
            return names;
        }

        const int count = sqlite3_column_count(m_statement);
        names.reserve(count);
        for (int i = 0; i < count; ++i)
        {
            const char *name = sqlite3_column_name(m_statement, i);
            names.emplace_back(name != nullptr ? name : "");
        }
        return names;
    }

    void SqliteResult::reset()
    {
        if (m_statement != nullptr)
        {
            sqlite3_reset(m_statement);
        }
    }

    bool SqliteResult::isEmpty() const
    {
        return m_isEmpty;
    }

    void SqliteResult::countRows()
    {
        if (m_statement == nullptr)
        {
            m_rowCount = 0;
            m_isEmpty = true;
            return;
        }

        // 从初始位置开始遍历计数
        m_rowCount = 0;
        while (sqlite3_step(m_statement) == SQLITE_ROW)
        {
            ++m_rowCount;
        }

        if (m_rowCount == 0)
        {
            m_isEmpty = true;
        }

        // 重置到初始位置，准备让 next() 从第一行开始
        sqlite3_reset(m_statement);
    }

    DatabaseValue SqliteResult::convertValue(const int index) const
    {
        if (m_statement == nullptr)
        {
            return std::monostate{};
        }

        const int type = sqlite3_column_type(m_statement, index);

        switch (type)
        {
            case SQLITE_NULL:
                return std::monostate{};

            case SQLITE_INTEGER:
                return static_cast<int64_t>(sqlite3_column_int64(m_statement, index));

            case SQLITE_FLOAT:
                return sqlite3_column_double(m_statement, index);

            case SQLITE_TEXT:
            {
                const char *text = reinterpret_cast<const char *>(
                    sqlite3_column_text(m_statement, index));
                const int byteCount = sqlite3_column_bytes(m_statement, index);
                if (text != nullptr && byteCount > 0)
                {
                    return std::string(text, static_cast<size_t>(byteCount));
                }
                return std::string{};
            }

            case SQLITE_BLOB:
            {
                // BLOB 数据转为十六进制字符串
                const auto *blob = static_cast<const unsigned char *>(
                    sqlite3_column_blob(m_statement, index));
                const int byteCount = sqlite3_column_bytes(m_statement, index);
                if (blob != nullptr && byteCount > 0)
                {
                    return std::string(reinterpret_cast<const char *>(blob),
                                      static_cast<size_t>(byteCount));
                }
                return std::string{};
            }

            default:
                return std::monostate{};
        }
    }

} // namespace Database
