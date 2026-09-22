/**
 * @file SqliteResult.h
 * @brief SQLite 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseResult.h"
#include "Database/Sqlite/SqliteConnection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief SQLite 查询结果集
     *
     * @details SQLite 用预编译语句（sqlite3_stmt）承载游标：构造时读取列数并对只读语句预扫描行数，
     *          构造时传入空游标表示写语句（INSERT/UPDATE/DELETE/DDL）的成功回执——列数与行数均为 0，
     *          但仍快照连接级的影响行数与最近插入 rowid。该快照必须落在构造这一刻，否则调用方之后在
     *          本连接上再执行一条写语句，本对象读到的就是别人的计数。
     *
     * @warning 本类持有 sqlite3_stmt 的所有权（析构 finalize），同时持有 sqlite3 的非拥有指针；
     *          连接对象必须比结果集活得更久。基类已删除拷贝与移动，这里不再放开。
     */
    class SqliteResult : public DatabaseResult
    {
    public:
        /// 行值快照的行数上限：只读结果集不超过这个行数时整份存进内存（遍历因此不再执行第二遍），
        /// 超过则退回游标遍历，以此保证「读一行就丢弃结果集」的用法不必为整表正文买单
        static constexpr size_t kMaximumMaterializedRowCount = 256;

        /**
         * @brief 用已编译的预编译语句构造结果集
         * @details statement 为空表示「写操作的成功回执」，此时不建立游标，只快照连接级计数器。
         * @param statement 已 prepare 的 SQLite 语句句柄，所有权移交本对象；可为 nullptr
         * @param database  语句所属的数据库句柄，仅用于读取错误文本与连接级计数器，不接管生命周期
         */
        explicit SqliteResult(sqlite3_stmt *statement, sqlite3 *database = nullptr);

        /**
         * @brief 析构时 finalize 语句，释放游标占用的语句与页锁资源
         */
        ~SqliteResult() override;

        // 语句句柄所有权唯一，拷贝会导致双重 finalize；基类同样已删除拷贝与移动。
        SqliteResult(const SqliteResult &) = delete;

        SqliteResult &operator=(const SqliteResult &) = delete;

        SqliteResult(SqliteResult &&) = delete;

        SqliteResult &operator=(SqliteResult &&) = delete;

        /**
         * @brief 将游标推进到下一行
         * @details 重写 DatabaseResult::next()：只读语句的行已在构造期预扫描并存进快照，这里只把下标
         *          往前拨一格，不再执行语句；其余情况 sqlite3_step 一次，只有 SQLITE_ROW 才算成功。
         *          SQLITE_ERROR / SQLITE_BUSY 与游标耗尽都返回 false，按基类契约属只读路径、
         *          不改写 m_lastError，需要区分时请检查语句是否已被连接侧报错。其余与基类一致。
         * @return true 游标停在有效行上，可以读取列值
         * @return false 已无更多行，或推进过程中出错
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：只读语句构造时预扫描得到精确行数；带写副作用的语句
         *          （如 INSERT ... RETURNING）绝不重复执行，按基类契约返回 0（未知）；写回执同样返回 0。
         * @return size_t 行数，0 表示空集或无法预先得知
         */
        [[nodiscard]] size_t rowCount() const override;

        /**
         * @brief 获取结果集列数
         * @details 重写 DatabaseResult::columnCount()：取构造时缓存的 sqlite3_column_count 快照，
         *          不再每次调用第三方 API；写回执结果没有游标，返回 0。
         * @return size_t 列数
         */
        [[nodiscard]] size_t columnCount() const override;

        /**
         * @brief 按列索引取列名
         * @details 重写 DatabaseResult::columnName()：用缓存的 size_t 列数做上界判断，
         *          避免把无符号索引强转成 int 后回绕成负数（回绕后的索引会读到别的列）。
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 列名；无游标或索引越界返回空值
         */
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;

        /**
         * @brief 按列名取列索引
         * @details 重写 DatabaseResult::columnIndex()：顺序扫描列名，同名列（SELECT name, name）
         *          返回第一个匹配，与 SQLite 自身按名取值的规则一致；空列名一律视为不存在。
         * @param name 列名，区分大小写（与 SQLite 的列别名原文一致）
         * @return std::optional<size_t> 列索引；无游标或列不存在返回空值
         */
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;

        /**
         * @brief 按列索引读取当前行的值
         * @details 重写 DatabaseResult::getValue()：游标未停在有效行上（未调用 next()、已走完或已 reset()）
         *          时直接返回 std::monostate——SQLite 规定列读取接口只能在 step 返回 SQLITE_ROW 之后使用，
         *          否则是未定义行为。其余与基类一致。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；无当前行、索引越界或列为 NULL 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;

        /**
         * @brief 按列名读取当前行的值
         * @details 重写 DatabaseResult::getValue()：先按名解析列索引，再走索引重载，
         *          保证两条路径的越界与「无当前行」判定完全一致。
         * @param name 列名
         * @return DatabaseValue 列值；列不存在、无当前行或值为 NULL 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;

        /**
         * @brief 获取全部列名
         * @details 重写 DatabaseResult::columnNames()：按列顺序返回，长度恒等于 columnCount()；
         *          SQLite 对表达式列可能给出空名，这里保留空串而不是丢弃，确保下标对齐。
         * @return std::vector<std::string> 列名列表；无游标时为空向量
         */
        [[nodiscard]] std::vector<std::string> columnNames() const override;

        /**
         * @brief 重置游标到首行之前，使结果集可以重新遍历
         * @details 重写 DatabaseResult::reset()：除 sqlite3_reset 外还要清掉「当前行有效」标志（否则
         *          getValue() 会去读已经失效的列值）与上一轮遗留的错误文本；reset 失败（例如 SQLITE_BUSY）
         *          时属非 const 写路径，会把原因写入 lastError()。其余与基类一致。
         */
        void reset() override;

        /**
         * @brief 判断结果集是否为空
         * @details 重写 DatabaseResult::isEmpty()：取构造阶段确定的快照，不随游标推进改变；带写副作用
         *          因而未预扫描的语句按「可能有行」处理，其真实是否有行由 next() 的返回值决定。
         * @return true 没有任何数据行
         */
        [[nodiscard]] bool isEmpty() const override;

        /**
         * @brief 获取底层 SQLite 语句句柄，供高级场景使用
         * @warning 所有权仍属于本结果集，调用方不得 sqlite3_finalize，也不得在结果集销毁后使用
         * @return sqlite3_stmt* 写回执结果为 nullptr；行已整份物化的快照结果在语句缓存收回游标后同样是
         *         nullptr（那条游标已不属于本结果集，取它没有意义）
         */
        [[nodiscard]] sqlite3_stmt *nativeHandle() const noexcept
        {
            return m_statement;
        }

        /**
         * @brief 获取最近一次插入操作生成的 rowid
         * @details SQLite 的该计数器是连接级状态，构造时快照，之后连接上的新写入不会反映到本对象。
         * @return std::int64_t rowid，从未插入过时为 0
         */
        [[nodiscard]] std::int64_t lastInsertRowId() const noexcept
        {
            return m_lastInsertRowId;
        }

        /**
         * @brief 获取影响行数
         * @details 重写 DatabaseResult::affectedRowCount()：返回构造时快照的 sqlite3_changes()——写回执
         *          是本条语句的影响行数，查询结果则是该连接上一条写语句的计数（SQLite 只有连接级计数器，
         *          与基类「只读结果集返回 0」的一般约定不同，这里如实返回快照值）。其余与基类一致。
         * @return std::int64_t 受影响行数；构造时未持有连接句柄（只传了语句）时为 0
         */
        [[nodiscard]] std::int64_t affectedRowCount() const noexcept override;

    private:
        // 语句缓存收回游标是「连接 ↔ 结果集」之间的协作，不对外放开：
        // 只有 SqliteConnection::execute() 会在确认游标已 idle 之后取走它
        friend class SqliteConnection;

        /**
         * @brief 行是否已整份物化（此时游标已被 reset 且本对象此后再不碰它）
         * @return true 可以安全交还语句缓存复用
         */
        [[nodiscard]] bool isCursorIdle() const noexcept
        {
            return m_isMaterializedRowsValid;
        }

        /**
         * @brief 交出游标的所有权，本对象不再 finalize 它
         * @warning 仅在 isCursorIdle() 为真时调用；此时列名与行值都已存进快照，交出游标不影响读取
         * @return sqlite3_stmt* 已 reset 到可重跑状态的游标
         */
        [[nodiscard]] sqlite3_stmt *releaseCursor() noexcept
        {
            sqlite3_stmt *const statement = m_statement;
            m_statement                   = nullptr;
            return statement;
        }

        /**
         * @brief 把 SQLite 的列值按存储类转换成统一的 DatabaseValue
         * @param index 已通过上层校验的列索引（int 是 SQLite API 的原生索引类型）
         * @return DatabaseValue 列值，NULL 或未知存储类返回 std::monostate
         */
        [[nodiscard]] DatabaseValue convertValue(int index) const;

        /**
         * @brief 预扫描只读语句：统计行数并顺带把行值收进快照，结束后把游标复位
         * @details 只读语句本来就要为 rowCount() 整趟走一遍，这一步把看到的值一并存下，
         *          之后 next()/getValue() 直接读快照，同一条查询不再执行第二遍。
         *          行数上限之外的行只计数不存值，超限就丢掉快照退回游标遍历，
         *          以免「读一行就丢弃结果集」的用法为整表正文买单。
         */
        void prefetchRows();

        sqlite3_stmt *m_statement{nullptr};   ///< 预编译语句句柄，非空时由本对象负责 finalize
        sqlite3 *     m_database{nullptr};    ///< 所属连接的句柄，只读引用，不接管生命周期
        size_t        m_columnCount{0};       ///< 列数快照，0 表示这是没有游标的写回执
        size_t        m_rowCount{0};          ///< 预扫描得到的行数快照，未预扫描时为 0
        int           m_affectedRowCount{0};  ///< 构造时快照的连接级 sqlite3_changes（int 是 SQLite API 的原生类型）
        std::int64_t  m_lastInsertRowId{0};   ///< 构造时快照的连接级 sqlite3_last_insert_rowid
        bool          m_hasCurrentRow{false}; ///< 游标当前是否停在有效行上，决定能否读取列值
        bool          m_scanCompleted{false}; ///< 游标是否已走到末尾；SQLite 会对已 DONE 的语句再次 step 而重跑查询，必须显式记住耗尽
        bool          m_isEmpty{true};        ///< 结果集是否为空（写回执恒为 true）
        /// 预扫描顺带存下的行值，摊成「行优先、行内按列序」的一整块（第 r 行第 c 列在
        /// r * m_columnCount + c）；m_isMaterializedRowsValid 为真时 next()/getValue() 只读这里。
        /// 不用 vector<vector<...>>：那等于每行一次堆分配，256 行的快照就是 257 次
        std::vector<DatabaseValue> m_materializedCells;
        /// 快照里存了几行。不拿 m_materializedCells.size() / m_columnCount 反推：写回执的列数是 0，
        /// 那个除法会炸，而快照与写回执共用这一段代码
        size_t m_materializedRowCount{0};
        /// 快照模式下的列名表：游标交还语句缓存后列名不能再从它身上问，故与行值同时存下
        std::vector<std::string> m_columnNames;
        bool m_isMaterializedRowsValid{false}; ///< 快照是否可用（未预扫描或行数超限则为假，退回游标遍历）
        bool m_isCurrentRowMaterialized{false}; ///< 游标当前停的这一行是否来自快照（决定 getValue 走哪条路）
        size_t m_materializedRowCursor{0};      ///< 快照模式下的下一次读取下标
    };

} // namespace AsynGyanis::Database
