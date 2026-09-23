/**
 * @file MySqlResult.h
 * @brief MySQL / MariaDB 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseResult.h"
#include "Database/MySql/MySqlConnection.h" // 复用其中全局作用域的 MYSQL / MYSQL_RES / MYSQL_ROW 前置声明

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief MySQL / MariaDB 查询结果集
     *
     * @details 封装 mysql_store_result 预读出来的 MYSQL_RES（本对象持有唯一所有权，析构时 mysql_free_result），
     *          传空句柄即「写回执」：0 行 0 列、isEmpty() 恒为 true，表示执行成功但没有任何数据。
     *          m_currentRow 为空与「无当前行」是同一件事，走到末尾或 reset() 之后取值一律回 std::monostate。
     * @note 文本与参数化两条协议路径共用 MySqlValueConversion.h 解析列值，因此不会出现取值分歧；值一律按
     *       (指针, 长度) 拷贝，内嵌 '\0' 与 BLOB 不被截断。MySQL 没有布尔存储类，TINYINT(1) 同样映射成
     *       std::int64_t，由调用方自行收窄。
     * @warning 数值解析失败或超出 int64 范围（例如 BIGINT UNSIGNED 上界到 2^64-1）时按原始十进制文本交出，
     *          而不是钳成 LLONG_MAX 或返回空值：凭空造出的错误数值比类型不稳定危险得多。
     *
     * @note 与 SqliteResult 的差异：SQLite 的游标挂在连接上，连接必须先于结果集销毁；
     *       MySQL 的数据已由 mysql_store_result 完整复制进 MYSQL_RES 自有内存，本类不持有任何连接指针，
     *       因此结果集可以比连接对象活得更久（与 RedisResult 同语义）。
     */
    class MySqlResult : public DatabaseResult
    {
    public:
        /**
         * @brief 用 mysql_store_result 预读出的结果集构造，并接管其所有权
         * @details 构造阶段一次性快照行数与列数（之后不再调用 mysql_num_rows / mysql_num_fields），
         *          传 nullptr 表示「写操作的空回执」，此时不建立游标，全部计数保持为 0。
         *          除「自增标识宽不到有符号 64 位」这一种情况外不报告失败：数据已由客户端库完整读出，
         *          没有可摘取的服务端错误。
         * @param ownedResult MySQL C API 交出的 MYSQL_RES 指针，所有权移交本对象；可为 nullptr
         * @param affectedRowCount 本条语句实际改动的行数，由连接在 mysql_affected_rows /
         *                         mysql_stmt_affected_rows 之后传入；只读结果集按约定传 0
         * @param generatedInsertId 本条语句带回的自增标识，由连接在 mysql_insert_id /
         *                          mysql_stmt_insert_id 之后传入；只读结果集与非插入语句按约定传 0
         */
        explicit MySqlResult(MYSQL_RES *ownedResult, std::int64_t affectedRowCount = 0, std::uint64_t generatedInsertId = 0);

        /**
         * @brief 析构时释放所持有的 MYSQL_RES（行缓冲与列元数据一并回收）
         */
        ~MySqlResult() override;

        // MYSQL_RES 所有权唯一：拷贝会让同一份结果被 mysql_free_result 两次；
        // 移动则让源对象析构时再次释放已经交给目标对象的句柄。基类同样已删除拷贝与移动。
        MySqlResult(const MySqlResult &) = delete;

        MySqlResult &operator=(const MySqlResult &) = delete;

        MySqlResult(MySqlResult &&) = delete;

        MySqlResult &operator=(MySqlResult &&) = delete;

        /**
         * @brief 将游标移动到下一行
         * @details 重写 DatabaseResult::next()：mysql_fetch_row 推进缓冲游标；预读结果下返回空指针只可能是
         *          「已到末尾」，不存在网络往返导致的「耗尽与出错无从区分」；属只读路径、不改写 m_lastError。
         *          其余与基类一致。
         * @return true 游标停在有效行上，可以读取列值
         * @return false 已无更多行，或本结果集是写回执（没有游标可言）
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：取构造时快照的 mysql_num_rows，预读结果下是精确值
         *          （不是基类那种「无法预先得知」的 0）；写回执结果没有游标，返回 0。
         * @return size_t 行数
         */
        [[nodiscard]] size_t rowCount() const override;

        /**
         * @brief 获取结果集列数
         * @details 重写 DatabaseResult::columnCount()：取构造时快照的 mysql_num_fields，不再每次调用第三方 API；写回执返回 0。
         * @return size_t 列数
         */
        [[nodiscard]] size_t columnCount() const override;

        /**
         * @brief 按列索引取列名
         * @details 重写 DatabaseResult::columnName()：先用缓存的无符号列数判界——mysql_fetch_field_direct
         *          的列号形参是 unsigned int，越界值不经判界会被截断成另一个合法索引，从而读到别的列。
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 列名；无结果集、索引越界或该列没有名字时返回空值
         */
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;

        /**
         * @brief 按列名取列索引
         * @details 重写 DatabaseResult::columnIndex()：先判空 mysql_fetch_fields 的返回（无列或元数据读取
         *          失败时它是空指针）；列名大小写敏感性由服务端排序规则决定、客户端不复制，同名列取第一个。
         * @param name 列名；空串一律视为不存在
         * @return std::optional<size_t> 列索引；无结果集或列不存在时返回空值
         */
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;

        /**
         * @brief 按列索引读取当前行的值
         * @details 重写 DatabaseResult::getValue()：游标必须停在有效行上（mysql_fetch_row 的行指针与
         *          mysql_fetch_lengths 的长度表都只在下一次推进之前有效）；取值以 (指针, 长度) 构造，
         *          TEXT/BLOB 内嵌的 '\0' 不被截断。const 读取路径，绝不改写 m_lastError。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；无当前行、索引越界、列值为 SQL NULL 或长度表不可用时返回 std::monostate
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
         * @details 重写 DatabaseResult::columnNames()：逐项复用 columnName()（列名来源只有一处真值）；
         *          缺名列补空串占位，保证长度恒等于 columnCount() 且下标与列序严格对齐。
         * @return std::vector<std::string> 按列顺序排列的列名；写回执结果为空向量
         */
        [[nodiscard]] std::vector<std::string> columnNames() const override;

        /**
         * @brief 重置游标到首行之前，使结果集可重新遍历
         * @details 重写 DatabaseResult::reset()：用 mysql_data_seek 退回第 0 行（官方保证随机定位只对
         *          mysql_store_result 的预读结果有效，该接口是 void、无法报告失败）；还要把 m_currentRow
         *          置空，否则 getValue() 会继续读到上一行遗留的数据。
         */
        void reset() override;

        /**
         * @brief 判断结果集是否为空
         * @details 重写 DatabaseResult::isEmpty()：由构造时快照的行数判定（预读结果的行数恒精确，同一事实
         *          只保留一处真值来源），语义是「结果集本身有没有行」而非「还剩多少行可读」，遍历完仍为 false。
         * @return true 没有任何数据行
         */
        [[nodiscard]] bool isEmpty() const override;

        /**
         * @brief 获取最近一次写语句实际改动的行数
         * @details 重写 DatabaseResult::affectedRowCount()：返回连接在执行本条语句后立即快照的语句级计数
         *          （mysql_affected_rows / mysql_stmt_affected_rows 会被下一条命令覆盖，因此由连接一次取好
         *          传进来）；只读结果集与驱动未提供时为 0。
         * @return std::int64_t 影响行数；只读结果集或驱动未提供时为 0
         */
        [[nodiscard]] std::int64_t affectedRowCount() const noexcept override;

        /**
         * @brief 获取最近一次插入生成的自增标识
         * @details 重写 DatabaseResult::lastInsertRowId()：返回连接在本条语句执行完立即快照的
         *          mysql_insert_id / mysql_stmt_insert_id（同样是语句级值，下一条命令就覆盖）。
         *          BIGINT UNSIGNED 的自增列可以从 2^63 起播种，那种值宽不进有符号 64 位，构造时按 0
         *          交出并把原因写进 lastError()——回绕成负数比报不出来更危险。
         * @return std::int64_t 自增标识；只读结果集、非插入语句与不提供该信息时为 0
         */
        [[nodiscard]] std::int64_t lastInsertRowId() const noexcept override;

    private:
        /**
         * @brief 按列的声明类型把一段 (指针, 长度) 的原始字节转换成统一的 DatabaseValue
         * @details 纯函数：只读列元数据与传入字节，不写 m_lastError，因此可以安全地在 const 取值路径上调用。
         * @param rawValue 列值首地址，调用方保证非空
         * @param byteLength 列值字节长度，来自 mysql_fetch_lengths，可为 0（表示空串而不是 NULL）
         * @param index 已通过上层判界的列索引，用于取该列的元数据类型
         * @return DatabaseValue 映射后的值
         */
        [[nodiscard]] DatabaseValue convertValue(const char *rawValue, size_t byteLength, size_t index) const;

        MYSQL_RES *  m_result{nullptr};     ///< MySQL 预读结果集句柄，非空时由本对象负责 mysql_free_result
        MYSQL_ROW    m_currentRow{nullptr}; ///< 当前行的列指针数组，空表示游标未停在有效行上
        size_t       m_rowCount{0};         ///< 构造时快照的行数，写回执结果为 0
        size_t       m_columnCount{0};      ///< 构造时快照的列数，写回执结果为 0
        std::int64_t m_affectedRowCount{0}; ///< 构造时快照的语句级影响行数，只读结果集与写回执之外恒为 0
        std::int64_t m_lastInsertRowId{0};  ///< 构造时快照的语句级自增标识，非插入语句与宽不进 int64 时为 0
    };

} // namespace AsynGyanis::Database
