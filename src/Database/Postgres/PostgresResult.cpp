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

            std::int64_t                 parsedValue = 0;
            const std::from_chars_result result      = std::from_chars(countText.data(), countText.data() + countText.size(), parsedValue);

            // 解析失败或尾部有余文说明文本不是纯十进制数：这是服务端形态不符预期，
            // 按「未知」处理比抛异常或钳位更符合 affectedRowCount() 的 noexcept 契约
            if (result.ec != std::errc{} || result.ptr != countText.data() + countText.size())
            {
                return 0;
            }

            return parsedValue;
        }

        /**
         * @brief 裁掉 libpq 文本的行尾换行（与 PostgresConnection.cpp 的同名助手同款）
         * @details PQresultErrorMessage / PQerrorMessage 的文本惯例以换行结尾，直接拼进中文句子
         *          会让日志出现断行；只裁行尾，中间的换行（服务端 DETAIL/HINT 行）保留有助于定位
         * @param rawMessage libpq 给出的 C 字符串，可为空指针
         * @return std::string 去掉行尾换行后的文本；入参为空指针时返回空串
         */
        [[nodiscard]] std::string trimmedLibraryMessage(const char *rawMessage)
        {
            std::string message(rawMessage != nullptr ? rawMessage : "");
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
            {
                message.pop_back();
            }
            return message;
        }

        /**
         * @brief 把一块「只有一行」的流式结果转成 DatabaseValue 行
         * @details 与预读构造同一套值映射（Detail::convertColumnText），NULL 与空串照旧分开。
         *          列 OID 每格现取：单行模式下每块独立，在本文件存一份列类型快照只会多一份
         *          要同步的状态，而 PQftype 读的只是 libpq 本地数组。
         * @param chunk 单行模式交出的块，调用方保证非空且已含行
         * @param rowIndex 该块内的行下标（单行模式恒为 0，保留形参是为了与预读的取值规则同形）
         * @param columnTotal 列数（来自列名快照的长度）
         * @return std::vector<DatabaseValue> 该行的全部取值，长度恒等于列数
         */
        [[nodiscard]] std::vector<DatabaseValue> makeRowFromChunk(PGresult *chunk, const int rowIndex, const std::size_t columnTotal)
        {
            std::vector<DatabaseValue> row;
            row.reserve(columnTotal);

            for (std::size_t columnIndex = 0; columnIndex < columnTotal; ++columnIndex)
            {
                const int rawColumnIndex = static_cast<int>(columnIndex);

                if (PQgetisnull(chunk, rowIndex, rawColumnIndex) != 0)
                {
                    // 与预读同款：SQL NULL 与「空串」「0」是三件不同的事，只有 NULL 映射成 monostate
                    row.push_back(std::monostate{});
                    continue;
                }

                const char *rawValue = PQgetvalue(chunk, rowIndex, rawColumnIndex);
                if (rawValue == nullptr)
                {
                    row.push_back(std::monostate{});
                    continue;
                }

                // 长度必须参与构造：含 '\0' 的字节序列不会因零终止而截断
                const int         rawLength  = PQgetlength(chunk, rowIndex, rawColumnIndex);
                const std::size_t byteLength = rawLength > 0 ? static_cast<std::size_t>(rawLength) : 0U;

                row.push_back(Detail::convertColumnText(PQftype(chunk, rawColumnIndex), rawValue, byteLength));
            }

            return row;
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
                const int         rawLength  = PQgetlength(m_result, rawRowIndex, rawColumnIndex);
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

        // 流式那份多一步：没读完就把剩余块读干再通知连接放行——半途而废的流会让下一条语句
        // 读到上一查询的行（协议错位），而析构是调用方表达「剩下的不要了」的唯一入口。
        // 收干过程中的错误不写 m_lastError：对象正在析构，写在上面的原因没人读得到
        if (m_isStreaming && !m_isStreamEndNotified)
        {
            while (PGresult *remainingChunk = PQgetResult(m_connectionHandle))
            {
                PQclear(remainingChunk);
            }
            noteStreamEnd();
        }
    }

    PostgresResult::PostgresResult(PGconn *const connectionHandle, PostgresConnection *const owner, PGresult *const firstRowResult, const StreamingTag) noexcept :
        m_connectionHandle(connectionHandle), m_streamOwner(owner), m_isStreaming(true)
    {
        // 所有块（含首块与最终的收尾块）都带完整列定义，列名与列数因此照常快照；
        // 行数不快照——按基类契约回 0，含义是「无法预先知道」，比假装精确诚实
        const int         rawColumnCount = firstRowResult != nullptr ? PQnfields(firstRowResult) : 0;
        const std::size_t columnTotal    = rawColumnCount > 0 ? static_cast<std::size_t>(rawColumnCount) : 0U;

        m_columnNames.reserve(columnTotal);
        for (std::size_t columnIndex = 0; columnIndex < columnTotal; ++columnIndex)
        {
            const char *rawColumnName = PQfname(firstRowResult, static_cast<int>(columnIndex));
            m_columnNames.emplace_back(rawColumnName != nullptr ? rawColumnName : "");
        }

        // 首块在这里就转成当前行缓冲并在构造内释放：它只是「一块只有一行的结果」，
        // 数据取完就可以 PQclear，调用方的第一次 next() 交付的就是这份缓冲
        if (firstRowResult != nullptr && PQntuples(firstRowResult) > 0)
        {
            m_streamingRow        = makeRowFromChunk(firstRowResult, 0, columnTotal);
            m_hasBufferedFirstRow = true;
        }
        if (firstRowResult != nullptr)
        {
            PQclear(firstRowResult);
        }
    }

    std::unique_ptr<PostgresResult> PostgresResult::forStreaming(PGconn *const connectionHandle, PostgresConnection *const owner, PGresult *const firstRowResult)
    {
        // 与 MySqlResult::forStreaming 同一形状：new 表达式写在成员函数里，私有构造的访问权
        // 只落在这一类；失败路径的所有权仍在调用方（它带着 ResultReleaser 守卫）
        return std::unique_ptr<PostgresResult>(new PostgresResult(connectionHandle, owner, firstRowResult, StreamingTag{}));
    }

    void PostgresResult::noteStreamEnd() noexcept
    {
        if (!m_isStreaming || m_isStreamEndNotified)
        {
            return;
        }
        m_isStreamEndNotified = true;
        if (m_streamOwner != nullptr)
        {
            m_streamOwner->noteStreamClosed();
        }
    }

    bool PostgresResult::advanceStreaming()
    {
        // 首块缓冲：第一次 next() 直接交付（构造后未 next() 时不许有「当前行」，见头文件游标语义）
        if (m_hasBufferedFirstRow)
        {
            m_hasBufferedFirstRow = false;
            m_currentRow          = &m_streamingRow;
            return true;
        }

        // 已经收尾（读完、中断或收干过）：不再触碰连接，语义与预读走完一致
        if (m_isStreamEndNotified)
        {
            m_currentRow = nullptr;
            return false;
        }

        PGresult *chunk = PQgetResult(m_connectionHandle);
        if (chunk == nullptr)
        {
            // 空指针不是「读完了」：读完整的一轮以一块非空的 TUPLES_OK 收尾。这里要么是链路中断，
            // 要么是连接状态被写坏——剩下的行取不到了，已读到的部分不构成完整结果集
            const std::string reason = m_connectionHandle != nullptr ? trimmedLibraryMessage(PQerrorMessage(m_connectionHandle)) : std::string{};
            m_lastError              = "PostgreSQL 流式读取中断：剩下的行取不到了，已读到的部分不构成完整结果集。这不是「读完了」，"
                                       "请按需要重查或改用 execute() 的预读路径" +
                                       (reason.empty() ? std::string{} : "（" + reason + "）");
            noteStreamEnd();
            m_currentRow = nullptr;
            return false;
        }

        const ExecStatusType chunkStatus = PQresultStatus(chunk);

        // 一块一行（单行模式的正常产出）
        if (chunkStatus == PGRES_SINGLE_TUPLE)
        {
            m_streamingRow = makeRowFromChunk(chunk, 0, m_columnNames.size());
            PQclear(chunk);
            m_currentRow = &m_streamingRow;
            return true;
        }

        // 收尾块：0 行的 TUPLES_OK，标志一轮读取到此结束（此时回复流已排空，连接重新可发命令）
        if (chunkStatus == PGRES_TUPLES_OK)
        {
            PQclear(chunk);
            noteStreamEnd();
            m_currentRow = nullptr;
            return false;
        }

        // 其余状态（FATAL_ERROR、BAD_RESPONSE……）一律算中断：错误文本与 SQLSTATE 都挂在这块上，
        // 与连接侧的 captureResultError 同一形态拼出原因（结果集自持 m_lastError，不改连接的那份）
        const char *rawMessage = PQresultErrorMessage(chunk);
        std::string message    = rawMessage != nullptr ? rawMessage : "";
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        {
            message.pop_back();
        }
        if (message.empty())
        {
            message = "服务端未给出原因";
        }
        const char       *rawSqlState = PQresultErrorField(chunk, PG_DIAG_SQLSTATE);
        const std::string sqlState    = rawSqlState != nullptr ? std::string(rawSqlState) : std::string{};

        m_lastError = "PostgreSQL 流式读取中断：" + message + (sqlState.empty() ? std::string{} : "（SQLSTATE " + sqlState + "）") +
                      "。剩下的行取不到了，已读到的部分不构成完整结果集；请按需要重查或改用 execute() 的预读路径";
        PQclear(chunk);
        noteStreamEnd();
        m_currentRow = nullptr;
        return false;
    }

#else // DATABASE_HAS_POSTGRES —— 桩实现：没有 libpq，结果集退化成永远为空的只读对象

    // 桩构建里不可能有 PGresult，构造函数刻意不使用参数值（也就无需 PQclear），
    // 全部状态保持默认：0 行 0 列、isEmpty() 为 true、影响行数为 0。
    // 桩下 connect() 必失败，任何写语句都执行不了，报出非零行数只会是假信息
    PostgresResult::PostgresResult(PGresult *)
    {
    }

    // 桩构建里既没有 PGresult 也没有连接句柄：流式构造退化成与预读构造同一份「永远为空」的
    // 对象（m_isStreaming 恒为假），因此 forStreaming() 给出的仍是可用的空结果集
    std::unique_ptr<PostgresResult> PostgresResult::forStreaming(PGconn *, PostgresConnection *, PGresult *)
    {
        return std::make_unique<PostgresResult>(nullptr);
    }

    bool PostgresResult::advanceStreaming()
    {
        // 桩里 m_isStreaming 恒为假，本函数正常路径不可达；给出定义是为了把共用区的 next()
        // 引用的符号链上，语义与「没有更多行」一致
        m_currentRow = nullptr;
        return false;
    }

    void PostgresResult::noteStreamEnd() noexcept
    {
        // 桩里没有连接可通知（m_isStreaming 恒为假，真实实现的入口判断会先返回）
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
        // 流式那份逐块向连接取行（首块已缓冲在构造里），推进逻辑全部在 advanceStreaming
        if (m_isStreaming)
        {
            return advanceStreaming();
        }

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
        // 流式档恒为 0，含义是「无法预先知道」而不是「没有行」（判口径与 isEmpty() 一致）
        if (m_isStreaming)
        {
            return 0;
        }

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

        // 流式结果没有可退回的位置：行已经从服务端流过来就收不回去。这里不假装能重扫
        // （游标原地不动、继续 next() 仍按流式推进），并把原因留给调用方
        if (m_isStreaming)
        {
            m_currentRow = nullptr;
            m_lastError  = "流式结果集不支持重扫：行是一次性流过来的，收不回也定位不了；"
                           "要重扫请把数据读进自己的容器，或改用 execute() 的预读路径";
            return;
        }

        // 既没有游标可复位（写回执），也没有第三方状态要清理：行数据在构造后不再变动，
        // 复位就是纯粹地回到「首行之前」
        m_nextRowIndex = 0;
        m_currentRow   = nullptr;
    }

    bool PostgresResult::isEmpty() const
    {
        // 预读结果的行数恒为精确值（写回执没有游标，行数也是 0），因此空与非空直接由行数判定，
        // 不再另存一份标志位——同一事实两处真值来源迟早会对不上。
        // 流式档反过来：没读完就没有「这堆行是空的」的依据，一律回 false（不宣称自己空）。
        // 误报「空」会让调用方整段跳过它本该处理的数据，这个方向比白跑一次 next() 贵得多
        if (m_isStreaming)
        {
            return false;
        }

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
