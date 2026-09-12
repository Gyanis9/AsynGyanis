/**
 * @file PostgresResult.h
 * @brief PostgreSQL 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseResult.h"
#include "Database/Postgres/PostgresConnection.h" // 复用其中全局作用域的 PGconn / PGresult 前置声明

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief PostgreSQL 查询结果集
     *
     * @details 封装 PQexec / PQexecParams 交出的 PGresult，把 PostgreSQL 的列类型映射到
     *          DatabaseResult 的统一接口。本对象持有 PGresult 的唯一所有权，析构时 PQclear
     *          一次性回收行缓冲与列元数据。
     *
     * ## 为什么构造时就把整份数据读进内存
     * PGresult 里的行数据虽然已经由 libpq 完整取回客户端内存（不是游标），但它的每个取值
     * 都只能通过 PQgetvalue / PQfname 这类句柄接口拿到，也就是说「读一行」这件事始终牵着
     * 一个第三方句柄。构造时把列名、每格已转好的 DatabaseValue 与影响行数全部快照下来之后：
     * - 结果集**不持有连接**，也不依赖语句句柄，可以比连接对象活得更久（与 MySqlStatementResult
     *   在参数化路径上的取舍一致，两条路径的结果集语义因此统一）；
     * - 全部 const 取值接口都不再触碰 libpq，越界、NULL 之类的判定完全由本类的内存快照决定，
     *   不存在「句柄先被释放、读取时才炸」的时序陷阱；
     * - OID 到 DatabaseValue 的解析只在构造时做一遍，不随调用方的取值次数重复解析。
     * 代价与大结果集的取舍：整份数据等额占用内存（与 MySQL 驱动的 mysql_store_result 预读同源）。
     *
     * ## 两种形态
     * - 查询结果：行数与列数在构造时快照，可反复遍历；
     * - 写回执：INSERT/UPDATE/DELETE/DDL/事务语句没有返回列，0 行 0 列，isEmpty() 恒为 true，
     *   affectedRowCount() 给出 PQcmdTuples 快照下来的语句级影响行数。
     *
     * ## 游标语义
     * m_currentRow 指向内存快照里的一行，为空表示游标没有停在有效行上（未 next()、已走完或
     * 刚 reset()），此时取值一律回 std::monostate。行数据在构造后不再变动，指向元素的指针
     * 因此始终有效，"有没有当前行" 与 "指针是否为空" 是同一件事，不需要额外的有效标志。
     *
     * ## 值映射规则（实现全部收敛在 PostgresValueConversion.h，只有一份真值）
     * SQL NULL→std::monostate、BOOLEAN→bool（文本形态是 't'/'f'）、
     * SMALLINT/INTEGER/BIGINT→std::int64_t、REAL/DOUBLE PRECISION→double、
     * NUMERIC→十进制文本 std::string（精确定小数，转 double 会丢精度）、
     * TEXT/VARCHAR/NAME/BYTEA/日期时间/JSON/UUID 等其余类型→std::string（按「指针 + 长度」拷贝，不丢字节）。
     *
     * @warning 数值解析失败（或解析结果超出 double 能表达的范围，如 "Infinity" / "NaN"：
     *          标准库的 std::from_chars 不识别这两种浮点文本）时按原始文本交出，
     *          而不是钳成极值或返回空值：凭空造出的错误数值比类型不稳定危险得多。
     *
     * @note PGresult 的句柄保留到本对象析构为止（所有权语义与 MySqlResult 对 MYSQL_RES 一致），
     *       但构造之后不再被任何接口读取——所有数据都来自内存快照。
     *
     * @code
     *   auto result = connection->execute("SELECT id, payload FROM records");
     *   while (result != nullptr && result->next())
     *   {
     *       const DatabaseValue identifier = result->getValue("id");
     *       const DatabaseValue payload    = result->getValue(1);
     *   }
     * @endcode
     */
    class PostgresResult : public DatabaseResult
    {
    public:
        /**
         * @brief 用 libpq 交出的结果集构造，并接管其所有权
         * @details 构造阶段一次性完成三件事：快照列名、把每格文本按列的 OID 转成 DatabaseValue、
         *          用 PQcmdTuples 快照语句级影响行数。影响行数必须在这里取而不是等到调用方读取：
         *          它是 PGresult 上的一份文本，PQclear 之后就没了，而本类的全部读取接口都不再
         *          触碰句柄（这也让本类的 const 接口天然不需要任何句柄有效性前提）。
         *          本构造函数不报告失败：数据已由 libpq 完整取回，没有可摘取的服务端错误。
         *          传 nullptr 表示「写操作的空回执」，此时不建立游标，全部计数保持为 0。
         * @param ownedResult libpq 交出的 PGresult，所有权移交本对象；可为 nullptr
         */
        explicit PostgresResult(PGresult *ownedResult);

        /**
         * @brief 析构时释放所持有的 PGresult（行缓冲与列元数据一并回收）
         */
        ~PostgresResult() override;

        // PGresult 所有权唯一：拷贝会让同一份结果被 PQclear 两次；
        // 移动则让源对象析构时再次释放已经交给目标对象的句柄。基类同样已删除拷贝与移动。
        PostgresResult(const PostgresResult &)            = delete;
        PostgresResult &operator=(const PostgresResult &) = delete;
        PostgresResult(PostgresResult &&)                 = delete;
        PostgresResult &operator=(PostgresResult &&)      = delete;

        /**
         * @brief 将游标移动到下一行
         * @details 重写 DatabaseResult::next()：在内存快照上推进下标，不再有任何 libpq 调用，
         *          因此也不存在「句柄已释放却仍在读」的时序问题。与基类的差异——
         *          返回 false 只可能是「已到末尾」（数据在构造时就从 libpq 一次性取回，
         *          没有「取到一半出错」这一分支）。按基类契约本方法属只读路径，不会改写 m_lastError。
         * @return true 游标停在有效行上，可以读取列值
         * @return false 已无更多行，或本结果集是写回执（没有游标可言）
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：返回构造时快照的真实行数。PostgreSQL 的
         *          简单查询与扩展查询都是一次性把全部行取回，不是游标，因此这里是精确值，
         *          而不是基类注释里「无法预先得知」的那个 0；写回执结果没有游标，返回 0。
         * @return size_t 行数
         */
        [[nodiscard]] size_t rowCount() const override;

        /**
         * @brief 获取结果集列数
         * @details 重写 DatabaseResult::columnCount()：返回构造时快照的列数，不再每次调用 libpq；
         *          写回执结果返回 0。
         * @return size_t 列数
         */
        [[nodiscard]] size_t columnCount() const override;

        /**
         * @brief 按列索引取列名
         * @details 重写 DatabaseResult::columnName()：直接读构造时快照的列名列表，
         *          先用列表长度判界再取（越界一律回空值），不把索引下传给第三方接口。
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 列名；索引越界时返回空值（无名的表达式列在快照里为空串，不在此列）
         */
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;

        /**
         * @brief 按列名取列索引
         * @details 重写 DatabaseResult::columnIndex()：在快照的列名列表上做逐字节精确匹配
         *          （区分大小写）。PostgreSQL 会把**未加双引号**的标识符折叠成小写，而元数据里
         *          的列名是服务端按查询原文保留的名字，因此本方法按原文比较，不做任何大小写归一化
         *          ——归一化会把服务端的标识符规则复制一份到客户端，两边迟早不一致。
         *          同名列（SELECT name, name）取第一个匹配。
         * @param name 列名；空串一律视为不存在
         * @return std::optional<size_t> 列索引；列不存在时返回空值
         */
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;

        /**
         * @brief 按列索引读取当前行的值
         * @details 重写 DatabaseResult::getValue()：与基类的额外约束——游标必须停在有效行上
         *          （未调用 next()、已走完或刚 reset() 时返回 std::monostate），随后从内存快照
         *          读取已转好的 DatabaseValue。本方法是 const 只读路径，绝不改写 m_lastError。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；无当前行、索引越界或值为 SQL NULL 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;

        /**
         * @brief 按列名读取当前行的值
         * @details 重写 DatabaseResult::getValue()：先把列名解析成索引，再走索引重载，
         *          保证两条路径的越界判定、「无当前行」判定与 NULL 映射完全一致。
         * @param name 列名（区分大小写，判定规则见 columnIndex()）
         * @return DatabaseValue 列值；列不存在、无当前行或值为 SQL NULL 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;

        /**
         * @brief 获取全部列名
         * @details 重写 DatabaseResult::columnNames()：返回构造时快照的列名列表副本。列表在构造时
         *          已按列序补齐（缺名列补空串占位），因此长度恒等于 columnCount()，
         *          下标与列序严格对齐。
         * @return std::vector<std::string> 按列顺序排列的列名；写回执结果为空向量
         */
        [[nodiscard]] std::vector<std::string> columnNames() const override;

        /**
         * @brief 重置游标到首行之前，使结果集可重新遍历
         * @details 重写 DatabaseResult::reset()：把「下一行下标」归零并把当前行指针置空
         *          （不清指针会让 getValue() 继续读到上一行的数据）；行数据本身在构造后不再变动，
         *          因此复位是纯粹的本地状态操作，没有任何第三方调用，也不可能失败。
         *          与基类的差异：顺带清空上一轮遗留的错误文本（本方法属非 const 写路径）。
         */
        void reset() override;

        /**
         * @brief 判断结果集是否为空
         * @details 重写 DatabaseResult::isEmpty()：直接由构造时快照的行数判定（行数恒精确，
         *          因此不需要另存一份空标志，同一事实只保留一处真值来源），不随游标推进改变——
         *          遍历完之后 isEmpty() 依然返回 false，因为它的语义是「结果集本身有没有行」，
         *          而不是「还剩多少行可读」。写回执结果没有行，因此恒为空。
         * @return true 没有任何数据行
         */
        [[nodiscard]] bool isEmpty() const override;

        /**
         * @brief 获取最近一次写语句实际改动的行数
         * @details 重写 DatabaseResult::affectedRowCount()：返回构造时快照下来的 PQcmdTuples 取值
         *          （PQcmdTuples 对没有行计数的命令——如 SELECT、BEGIN——返回空串，此时按 0 处理）。
         *          该接口给出的是「语句级」结果，必须在结果集被释放之前读取，因此由构造函数一次取好
         *          存进成员，本方法只做读取，天然满足 noexcept。
         *          与 MySqlResult 的差异：MySQL 侧的计数由连接另行取好再传进构造函数，
         *          这里直接在结果集上解析——PQcmdTuples 本来就挂在本结果集上，让连接再取一次
         *          等于同一件事有两份取值来源，迟早会漂移。
         * @return std::int64_t 影响行数；只读结果集或服务端未提供行计数时为 0
         */
        [[nodiscard]] std::int64_t affectedRowCount() const noexcept override;

    private:
        PGresult *m_result{nullptr}; ///< libpq 结果集句柄，非空时由本对象负责 PQclear（数据已在构造时读进内存）

        std::vector<std::string>                m_columnNames; ///< 构造时快照的列名，长度即列数，缺名列补空串
        std::vector<std::vector<DatabaseValue>> m_rows;        ///< 构造时快照的全部行，每行长度恒等于列数

        const std::vector<DatabaseValue> *m_currentRow{nullptr}; ///< 指向当前行（内存快照中的一行），空表示游标未停在有效行上
        size_t                            m_nextRowIndex{0};    ///< 下一次 next() 将返回的行下标，走完即等于行数

        std::int64_t m_affectedRowCount{0}; ///< 构造时快照的语句级影响行数；只读结果集（含写回执之外的查询）恒为 0
    };

} // namespace AsynGyanis::Database
