#include "Database/Sqlite/SqliteResult.h"

#include "Database/Common/BinaryBytes.h"

#include <sqlite3.h>

#include <string>

namespace AsynGyanis::Database
{
    SqliteResult::SqliteResult(sqlite3_stmt *const statement, sqlite3 *const database)
        : m_statement(statement), m_database(database)
    {
        // 影响行数与最近插入 rowid 都是 SQLite 的连接级计数器：必须在构造这一刻快照，
        // 否则调用方之后在本连接上再执行一条写语句，本对象读到的就是别人的计数
        if (m_database != nullptr)
        {
            m_affectedRowCount = sqlite3_changes(m_database);
            m_lastInsertRowId  = sqlite3_last_insert_rowid(m_database);
        }

        // 空游标即「写操作成功回执」：没有列也没有行，isEmpty() 保持构造默认值 true
        if (m_statement == nullptr)
        {
            return;
        }

        // 列数在 prepare 之后即固定，与是否 step 无关，这里一次性缓存下来供全部 const 接口复用
        m_columnCount = static_cast<size_t>(sqlite3_column_count(m_statement));
        if (m_columnCount == 0)
        {
            return;
        }

        // 只要语句带返回列就先假设非空，随后由预扫描纠正
        m_isEmpty = false;

        // 只有只读语句能被安全地跑两遍（预扫描一遍、next() 再遍历一遍）；
        // 带写副作用的语句（例如 INSERT ... RETURNING）预扫描会把数据改两次，坚决不做，
        // 此时按基类契约让 rowCount() 返回 0 表示「无法预先得知全部行」
        if (sqlite3_stmt_readonly(m_statement) != 0)
        {
            countRows();
        }
    }

    SqliteResult::~SqliteResult()
    {
        // 语句由结果集独占所有权：finalize 会释放游标并归还它持有的读锁。
        // 这里忽略 finalize 的返回码——析构路径既不能向调用方报错也不该抛异常，
        // 未跑完的语句其错误已在上一次 next() 的 false 返回值上体现
        if (m_statement != nullptr)
        {
            sqlite3_finalize(m_statement);
            m_statement = nullptr;
        }
        // m_database 是非拥有指针，连接负责关闭，这里既不能 free 也不能置空后使用
    }

    bool SqliteResult::next()
    {
        // 写回执没有游标，永远「没有下一行」
        if (m_statement == nullptr)
        {
            m_hasCurrentRow = false;
            return false;
        }

        // SQLite 的约定是：对已返回 SQLITE_DONE 的语句再 step 一次，会隐式 reset 并从头重跑查询。
        // 不加这道闸门，遍历结束后多调用一次 next() 就会让结果集「复活」，把整条语句白执行第二遍
        if (m_scanCompleted)
        {
            m_hasCurrentRow = false;
            return false;
        }

        // 只有 SQLITE_ROW 代表还有数据；SQLITE_DONE 是正常耗尽，SQLITE_ERROR/SQLITE_BUSY 是出错，
        // 三者一律返回 false——按基类契约 next() 属于读取路径，不改写 m_lastError
        const int stepResult = sqlite3_step(m_statement);
        m_hasCurrentRow      = (stepResult == SQLITE_ROW);
        if (!m_hasCurrentRow)
        {
            // 无论耗尽还是出错都记为遍历结束，后续读取稳定返回「没有下一行」
            m_scanCompleted = true;
        }
        return m_hasCurrentRow;
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
        // 用缓存的无符号列数比较上界：旧实现把 index 强转成 int 再比较，
        // 传入 SIZE_MAX 时会回绕成 -1 从而绕过检查，导致越界调用 SQLite
        if (m_statement == nullptr || index >= m_columnCount)
        {
            return std::nullopt;
        }

        // 列名指针的生命周期到 finalize 或下一次改变语句结构为止，立刻拷成 std::string 交出去
        const char *rawName = sqlite3_column_name(m_statement, static_cast<int>(index));
        if (rawName == nullptr)
        {
            return std::nullopt;
        }
        return std::string(rawName);
    }

    std::optional<size_t> SqliteResult::columnIndex(const std::string_view name) const
    {
        if (m_statement == nullptr || name.empty())
        {
            return std::nullopt;
        }

        for (size_t index = 0; index < m_columnCount; ++index)
        {
            const char *rawName = sqlite3_column_name(m_statement, static_cast<int>(index));
            // 先到先得：SELECT name, name 这类同名列只返回第一个匹配，与 SQLite 按名取值的规则一致
            if (rawName != nullptr && name == rawName)
            {
                return index;
            }
        }
        return std::nullopt;
    }

    DatabaseValue SqliteResult::getValue(const size_t index) const
    {
        // 游标没停在有效行上时 SQLite 的列读取接口属于未定义行为（旧实现会读到上一次 step 的残值），
        // 统一按「无值」返回 monostate，让调用方与读到 NULL 列的表现一致
        if (!m_hasCurrentRow)
        {
            return std::monostate{};
        }

        // 同样走无符号比较，避免大索引强转 int 回绕后绕过边界检查
        if (m_statement == nullptr || index >= m_columnCount)
        {
            return std::monostate{};
        }

        return convertValue(static_cast<int>(index));
    }

    DatabaseValue SqliteResult::getValue(const std::string_view name) const
    {
        const auto index = columnIndex(name);
        // 列名解析失败与列值为 NULL 在契约里同为 monostate，不额外区分错误状态
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

        names.reserve(m_columnCount);
        for (size_t index = 0; index < m_columnCount; ++index)
        {
            const char *rawName = sqlite3_column_name(m_statement, static_cast<int>(index));
            // 表达式列（SELECT COUNT(*)）可能没有名字，补空串占位以保证下标与列序严格对齐
            names.emplace_back(rawName != nullptr ? rawName : "");
        }
        return names;
    }

    void SqliteResult::reset()
    {
        // 写回执本来就是空集，reset 是安全的空操作
        if (m_statement == nullptr)
        {
            return;
        }

        // 先清掉上一轮的错误：本函数代表一次新的尝试，不能让历史文本冒充本次结果
        m_lastError.clear();

        // 游标退回首行之前，当前行随之失效：不清这个标志会让 getValue() 继续读已被释放的列值
        m_hasCurrentRow = false;
        // 解除耗尽闸门：基类契约要求 reset() 之后可以重新完整遍历一遍
        m_scanCompleted = false;

        // reset 会把语句恢复到可再次 step 的状态，其返回码代表「上一次执行遗留的错误」，
        // 例如 SQLITE_BUSY；本函数是非 const 写路径，允许把原因记录进 m_lastError
        const int resetResult = sqlite3_reset(m_statement);
        if (resetResult != SQLITE_OK)
        {
            m_lastError = std::string("重置 SQLite 游标失败：") + sqlite3_errstr(resetResult);
        }
    }

    bool SqliteResult::isEmpty() const
    {
        return m_isEmpty;
    }

    std::int64_t SqliteResult::affectedRowCount() const noexcept
    {
        // 把 SQLite 连接级计数器的 int 快照按驱动层统一的有符号 64 位宽度交出去；
        // 这里只是类型加宽，不与连接交互，因此 noexcept 成立
        return static_cast<std::int64_t>(m_affectedRowCount);
    }

    void SqliteResult::countRows()
    {
        m_rowCount = 0;
        while (true)
        {
            const int stepResult = sqlite3_step(m_statement);
            if (stepResult == SQLITE_ROW)
            {
                ++m_rowCount;
                continue;
            }

            if (stepResult == SQLITE_DONE)
            {
                break;
            }

            // 预扫描中途出错（例如锁超时）：已数到的行数只是下界，构造阶段属于写路径，
            // 允许记录错误文本；SQLite 的 errmsg 指针会在下一次 API 调用后失效，必须立即拷贝。
            // 句柄缺失（构造时只传了语句）时不能调 sqlite3_errmsg，退回不依赖句柄的全局 sqlite3_errstr
            m_lastError = std::string("统计 SQLite 结果集行数失败：") +
                          (m_database != nullptr ? sqlite3_errmsg(m_database) : sqlite3_errstr(stepResult));
            break;
        }

        m_isEmpty = (m_rowCount == 0);

        // 计数只是探路，必须把游标退回首行之前，next() 才能从第一行开始遍历；
        // 若上一步已经记录了错误，就不用 reset 的结果覆盖它，保留更接近根因的文本
        const int resetResult = sqlite3_reset(m_statement);
        if (resetResult != SQLITE_OK && m_lastError.empty())
        {
            m_lastError = std::string("重置 SQLite 游标失败：") + sqlite3_errstr(resetResult);
        }
    }

    DatabaseValue SqliteResult::convertValue(const int index) const
    {
        // 调用方（getValue 系列）已做过空句柄与越界检查，这里不再重复防御，保持单次判定
        const int columnType = sqlite3_column_type(m_statement, index);

        switch (columnType)
        {
            case SQLITE_NULL:
                // NULL 映射成 monostate 而不是空串或 0，调用方才能区分「没有值」与「值为 0/空」
                return std::monostate{};

            case SQLITE_INTEGER:
                // 统一收进 64 位整数：SQLite 的 INTEGER 本身就是最多 8 字节有符号整数
                return static_cast<std::int64_t>(sqlite3_column_int64(m_statement, index));

            case SQLITE_FLOAT:
                return sqlite3_column_double(m_statement, index);

            case SQLITE_TEXT:
            {
                // sqlite3_column_text 返回 UTF-8 的 unsigned char*，且指针只在下一次 step/finalize 前有效，
                // 必须按长度立刻拷进 std::string；列长度要在取到指针之后再问 sqlite3_column_bytes（官方约定该调用会固定前一次转换的结果）
                const auto *rawText = reinterpret_cast<const char *>(sqlite3_column_text(m_statement, index));
                const int byteCount = sqlite3_column_bytes(m_statement, index);
                if (rawText != nullptr && byteCount > 0)
                {
                    return std::string(rawText, static_cast<size_t>(byteCount));
                }
                // 空字符串与 NULL 是两回事：TEXT 列取到 0 字节时给出空串而不是 monostate
                return std::string{};
            }

            case SQLITE_BLOB:
            {
                // BLOB 按原始字节搬进二进制备选：不做十六进制转写（那会翻倍体积并让调用方
                // 拿不到原始二进制），也不再塞进 std::string——二进制与文本分开后，类型本身
                // 就是绑定线索，写回时驱动才知道该用 sqlite3_bind_blob 而不是按文本绑定
                const auto *rawBlob = static_cast<const unsigned char *>(sqlite3_column_blob(m_statement, index));
                const int byteCount = sqlite3_column_bytes(m_statement, index);
                if (rawBlob != nullptr && byteCount > 0)
                {
                    return BinaryBytes(rawBlob, rawBlob + static_cast<size_t>(byteCount));
                }
                // 零长度 BLOB 与 NULL 是两回事：给出空序列而不是 monostate
                return BinaryBytes{};
            }

            default:
                // 未来 SQLite 若新增存储类，这里按空值兜底而不是猜类型，避免把未知数据误当整数或文本
                return std::monostate{};
        }
    }

} // namespace AsynGyanis::Database
