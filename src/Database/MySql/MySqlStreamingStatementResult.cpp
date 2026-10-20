// 与 MySqlConnection.cpp 同一条 NOMINMAX 前置：libmysqlclient 的头会间接包含 windows.h，
// 必须在任何头文件之前挡住 min/max 函数式宏（理由与写法的完整说明见 MySqlConnection.cpp 顶部）
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Database/MySql/MySqlStreamingStatementResult.h"

#include <utility>

#ifdef DATABASE_HAS_MYSQL

#include "Database/MySql/MySqlConnection.h"
#include "Database/MySql/MySqlValueConversion.h"

// 客户端头的两种发行布局与 MySqlConnection.cpp 同一份探测（扁平与带 mysql/ 子目录）
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif

#include <format>

namespace AsynGyanis::Database
{
    MySqlStreamingStatementResult::MySqlStreamingStatementResult(MYSQL_STMT *const statement, MySqlConnection *const owner, std::string commandText,
                                                                 std::vector<std::string> columnNames, std::vector<int> columnTypes, std::vector<unsigned int> columnCharacterSets,
                                                                 std::vector<std::vector<char>> columnBuffers, std::vector<unsigned long> columnLengths,
                                                                 std::unique_ptr<bool[]> columnNullFlags) :
        m_statement(statement), m_owner(owner), m_commandText(std::move(commandText)), m_columnNames(std::move(columnNames)), m_columnTypes(std::move(columnTypes)),
        m_columnCharacterSets(std::move(columnCharacterSets)), m_columnBuffers(std::move(columnBuffers)), m_columnLengths(std::move(columnLengths)),
        m_columnNullFlags(std::move(columnNullFlags))
    {
    }

    MySqlStreamingStatementResult::~MySqlStreamingStatementResult()
    {
        endStream();
    }

    void MySqlStreamingStatementResult::endStream() noexcept
    {
        // 幂等：读完的 next()、补取失败的 next() 与析构都可能走到这里，收口只做一次
        if (m_isStreamEnded)
        {
            return;
        }
        m_isStreamEnded = true;

        if (m_statement != nullptr && m_owner != nullptr)
        {
            // free_result：已读完的场合只清本地缓冲；中途收手时把没读完的回复流一并吃掉
            // （与文本协议档的 mysql_free_result 同一职责）——「只想要前几行就收手」安全的前提
            if (mysql_stmt_free_result(m_statement) != 0)
            {
                // 清不干净的语句不能再留在缓存里被下一位复用（它会一直失败）；
                // 按命令文本弃掉是安全的：取数期间连接被在途流计数锁住，缓存没有任何并发活动
                m_owner->discardCachedStatement(m_commandText);
            }
            m_owner->noteStreamClosed();
        }
        m_currentRow.clear();
    }

    bool MySqlStreamingStatementResult::next()
    {
        // 游标先失效：本次取数失败时当前行不该再可读
        m_currentRow.clear();

        if (m_isStreamEnded || m_statement == nullptr)
        {
            return false;
        }

        const int fetchResult = mysql_stmt_fetch(m_statement);
        if (fetchResult == MYSQL_NO_DATA)
        {
            // 正常读完：收口放行连接
            endStream();
            return false;
        }
        if (fetchResult == 1)
        {
            // 中断与「读完了」是两件事：前者交出的是残缺结果，如实说明（与文本协议流式档同一判据；
            // 这里错误码直接来自语句句柄，不必再猜连接状态）
            const unsigned int errorNumber = mysql_stmt_errno(m_statement);
            const char        *nativeText  = mysql_stmt_error(m_statement);
            m_lastError                    = std::format("MySQL 流式读取中断（错误码 {}{}）：剩下的行取不到了，"
                                                         "已读到的部分不构成完整结果集。这不是「读完了」，请按需要重查或改用 execute() 的预读路径",
                                                         errorNumber, nativeText != nullptr && nativeText[0] != '\0' ? std::format("，{}", nativeText) : std::string{});
            endStream();
            return false;
        }

        // 其余两种返回都算「本行有效」：0 是常规路径，MYSQL_DATA_TRUNCATED 只表示个别列超出绑定缓冲，
        // 下面逐列按真实长度补取，绝不把截断值交出去
        return buildCurrentRow();
    }

    bool MySqlStreamingStatementResult::buildCurrentRow()
    {
        const std::size_t columnCount = m_columnNames.size();
        m_currentRow.reserve(columnCount);

        for (std::size_t index = 0; index < columnCount; ++index)
        {
            if (m_columnNullFlags[index])
            {
                // SQL NULL：与「空串」「0」是三件不同的事，只有 NULL 才映射成 monostate
                m_currentRow.emplace_back(std::monostate{});
                continue;
            }

            if (m_columnLengths[index] > m_columnBuffers[index].size())
            {
                // 流式路径取不到 store_result 更新过的 max_length，缓冲从最小容量起步；
                // 超出的列按实际长度单独补取（文档给出的长数据读取方式），绝不交出截断的数据
                const unsigned long actualLength = m_columnLengths[index];
                std::vector<char>   exactBuffer(static_cast<std::size_t>(actualLength));

                MYSQL_BIND columnBinding{};
                // 补取同样按列的真实性质选类型：二进制列若按 MYSQL_TYPE_STRING 重取，
                // 客户端库会按字符集转换一次，拿回来的就不再是原始字节
                columnBinding.buffer_type   = Detail::isBinaryColumn(m_columnTypes[index], m_columnCharacterSets[index]) ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
                columnBinding.buffer        = exactBuffer.data();
                columnBinding.buffer_length = actualLength;
                columnBinding.length        = &m_columnLengths[index];

                if (mysql_stmt_fetch_column(m_statement, &columnBinding, static_cast<unsigned int>(index), 0) != 0)
                {
                    const unsigned int errorNumber = mysql_stmt_errno(m_statement);
                    const char        *nativeText  = mysql_stmt_error(m_statement);
                    m_lastError                    = std::format("MySQL 流式读取中断（错误码 {}{}）：补取超长列失败，当前行不完整。"
                                                                 "已读到的部分不构成完整结果集，这不是「读完了」",
                                                                 errorNumber, nativeText != nullptr && nativeText[0] != '\0' ? std::format("，{}", nativeText) : std::string{});
                    endStream();
                    return false;
                }

                m_currentRow.emplace_back(
                        Detail::convertColumnText(m_columnTypes[index], m_columnCharacterSets[index], exactBuffer.data(), static_cast<std::size_t>(actualLength)));
                continue;
            }

            m_currentRow.emplace_back(
                    Detail::convertColumnText(m_columnTypes[index], m_columnCharacterSets[index], m_columnBuffers[index].data(), static_cast<std::size_t>(m_columnLengths[index])));
        }
        return true;
    }

    size_t MySqlStreamingStatementResult::rowCount() const
    {
        // 流式档行数未知：回 0 是这个歧义哨兵「不漏检」的一侧（与 isEmpty() 配对，见头文件说明）
        return 0U;
    }

    bool MySqlStreamingStatementResult::isEmpty() const
    {
        // 没读完就没有「这堆行是空的」的依据，一律回 false，不宣称自己空
        return false;
    }

    size_t MySqlStreamingStatementResult::columnCount() const
    {
        return m_columnNames.size();
    }

    std::optional<std::string> MySqlStreamingStatementResult::columnName(const size_t index) const
    {
        if (index >= m_columnNames.size())
        {
            return std::nullopt;
        }
        return m_columnNames[index];
    }

    std::optional<size_t> MySqlStreamingStatementResult::columnIndex(const std::string_view name) const
    {
        // 空列名一律视为不存在：本驱动不会给列起空名（无名表达式列补的是空串占位，
        // 那种「空名」不可寻址，与 MySqlResult / MySqlStatementResult 同一口径）
        if (name.empty())
        {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < m_columnNames.size(); ++index)
        {
            if (m_columnNames[index] == name)
            {
                return index;
            }
        }
        return std::nullopt;
    }

    DatabaseValue MySqlStreamingStatementResult::getValue(const size_t index) const
    {
        // 游标不在有效行上（未 next()、已结束或已收口）时 m_currentRow 为空，一律回「无值」
        if (index >= m_currentRow.size())
        {
            return std::monostate{};
        }
        return m_currentRow[index];
    }

    DatabaseValue MySqlStreamingStatementResult::getValue(const std::string_view name) const
    {
        const std::optional<size_t> index = columnIndex(name);
        return index.has_value() ? getValue(*index) : DatabaseValue{std::monostate{}};
    }

    std::vector<std::string> MySqlStreamingStatementResult::columnNames() const
    {
        return m_columnNames;
    }

    void MySqlStreamingStatementResult::reset()
    {
        // 先清掉上一轮的错误：本函数代表一次新的尝试，不能让历史文本冒充本次结果
        m_lastError.clear();
        m_currentRow.clear();

        // 流式结果没有可退回的位置：行是一次性流过来的，收不回也定位不了。
        // 与文本协议流式档同款处置：不假装能重扫，把原因留给调用方
        m_lastError = "流式结果集不支持重扫：行是一次性流过来的，收不回也定位不了；"
                      "要重扫请把数据读进自己的容器，或改用 execute() 的预读路径";
    }

} // namespace AsynGyanis::Database

#endif // DATABASE_HAS_MYSQL

namespace AsynGyanis::Database
{
    void *MySqlStreamingStatementResult::adoptResultBindings(std::shared_ptr<void> bindings) noexcept
    {
        m_resultBindingsOwner = std::move(bindings);
        return m_resultBindingsOwner.get();
    }
} // namespace AsynGyanis::Database
