/**
 * @file MySqlStreamingStatementResult.h
 * @brief MySQL 带参流式结果集（逐行取数；预取窗口由连接在取数前设置）
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 预处理语句与大结果集的组合：连接在 mysql_stmt_execute 之后**不** store_result，
 *          行留在服务端按预取窗口逐批取回（STMT_ATTR_PREFETCH_ROWS，窗口按**行数**计），
 *          客户端内存只装窗口与当前行。与 MySqlResult 的流式档同一套契约差异：
 *          rowCount() 回 0（未知）、isEmpty() 回 false（不宣称自己空）、不可重扫、
 *          结果集必须比产出它的连接短命，且在它收口之前那条连接不能再发任何语句。
 *
 * @note 语句句柄归连接的语句缓存所有，本对象只借用：收口（读完、中断或析构）时
 *       mysql_stmt_free_result 把没读完的回复流吃掉并通知连接放行；清不干净（连接已坏）
 *       时按命令文本请连接弃掉缓存里那一条。本对象**不**关闭任何句柄；取数期间连接被
 *       在途流计数锁住，缓存不会逐出这条语句。
 * @warning 生命周期与文本协议流式档同一条（详见 MySqlResult 的 @warning）：回复流没走完的
 *          期间断开连接属违反契约的用法；本对象比连接短命是它的前提。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseResult.h"
#include "Database/MySql/MySqlConnection.h" // 复用其中全局作用域的 MYSQL_STMT 前置声明

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    class MySqlConnection; ///< 收口时要回去对账（在途未清流计数归位；清不干净时弃掉缓存语句）

    /**
     * @brief MySQL 带参流式结果集（行留在服务端，逐行取回）
     */
    class ASYN_DATABASE_API MySqlStreamingStatementResult final : public DatabaseResult
    {
    public:
        /**
         * @brief 由连接在 mysql_stmt_bind_result 成功之后构造：结果缓冲与元数据随构造一并移交
         * @param statement 已执行、未 store 的语句句柄（归连接的语句缓存所有，本对象只借用）
         * @param owner 产出本条结果的连接（收口时通知；只读路径不触碰它之外的任何连接状态）
         * @param commandText 命令文本副本：收口清不干净时按它弃掉缓存里的那条语句
         * @param columnNames 按列序排列的列名
         * @param columnTypes 每列的 MYSQL_FIELD.type 取值（取值映射的判据之一）
         * @param columnCharacterSets 每列的字符集号（区分二进制列与文本列的唯一依据）
         * @param columnBuffers 每列的取值缓冲（绑定指针指向它们；超容量的列在取行时按列补取）
         * @param columnLengths 每列本次取回的真实长度（mysql_stmt_fetch 写入）
         * @param columnNullFlags 每列本行的 NULL 标记（mysql_stmt_fetch 写入）
         */
        MySqlStreamingStatementResult(MYSQL_STMT *statement, MySqlConnection *owner, std::string commandText, std::vector<std::string> columnNames, std::vector<int> columnTypes,
                                      std::vector<unsigned int> columnCharacterSets, std::vector<std::vector<char>> columnBuffers, std::vector<unsigned long> columnLengths,
                                      std::unique_ptr<bool[]> columnNullFlags);

        /**
         * @brief 析构：收口（free_result 吃掉剩余回复流 + 通知连接放行后续语句）
         * @details 「只想要前几行就收手」的安全性正是建立在这一步上：不读完的回复流由
         *          free_result 一次吞掉，连接不会停在协议错位的中途
         */
        ~MySqlStreamingStatementResult() override;

        MySqlStreamingStatementResult(const MySqlStreamingStatementResult &)            = delete;
        MySqlStreamingStatementResult &operator=(const MySqlStreamingStatementResult &) = delete;
        MySqlStreamingStatementResult(MySqlStreamingStatementResult &&)                 = delete;
        MySqlStreamingStatementResult &operator=(MySqlStreamingStatementResult &&)      = delete;

        /**
         * @brief 取下一行并当场按列转换成值
         * @details 重写 DatabaseResult::next()：mysql_stmt_fetch 一次取一行（预取窗口由连接设置），
         *          本行当场逐列转换；超缓冲的列就地 mysql_stmt_fetch_column 补取（绝不让截断值过界）。
         *          结束时（MYSQL_NO_DATA）收口；取数中断（错误码非零）按**残缺结果**处置：
         *          写好 m_lastError 并收口，调用方从 lastError() 看到「这不是读完了」。
         * @return true 游标停在有效行上
         * @return false 读完、中断或已收口
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：流式档行数未知，恒回 0（歧义哨兵取不漏检一侧：
         *          「未知」不冒充「空」，isEmpty() 与之配对）。
         * @return size_t 恒为 0
         */
        [[nodiscard]] size_t rowCount() const override;

        /**
         * @brief 获取结果集列数
         * @details 重写 DatabaseResult::columnCount()：构造时快照的列名个数。
         * @return size_t 列数
         */
        [[nodiscard]] size_t columnCount() const override;

        /**
         * @brief 按列索引取列名
         * @details 重写 DatabaseResult::columnName()：越界返回空值。
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 列名；越界时为空
         */
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;

        /**
         * @brief 按列名取列索引
         * @details 重写 DatabaseResult::columnIndex()：逐字节精确匹配（区分大小写），同名列先到先得，
         *          与 MySqlResult / MySqlStatementResult 同一约定。
         * @param name 列名；空串视为不存在
         * @return std::optional<size_t> 列索引；不存在时为空
         */
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;

        /**
         * @brief 按列索引读取当前行的值
         * @details 重写 DatabaseResult::getValue()：游标不在有效行上（未 next()、已结束或已收口）
         *          或列值为 SQL NULL 时回 std::monostate，与流式档的其余实现同一约定。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；无当前行或越界时回 monostate
         */
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;

        /**
         * @brief 按列名读取当前行的值
         * @details 重写 DatabaseResult::getValue()：先解析列名再走索引重载，两条路径判定一致。
         * @param name 列名（区分大小写）
         * @return DatabaseValue 列值；列不存在或无当前行时回 monostate
         */
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;

        /**
         * @brief 获取全部列名
         * @details 重写 DatabaseResult::columnNames()：返回构造时快照的副本，下标与列序严格对齐。
         * @return std::vector<std::string> 按列序排列的列名
         */
        [[nodiscard]] std::vector<std::string> columnNames() const override;

        /**
         * @brief 重置游标：流式档不支持
         * @details 重写 DatabaseResult::reset()：行是一次性流过来的，收不回也定位不了；
         *          把原因写进 m_lastError 并保持游标失效（与 MySqlResult 流式档同款）。
         */
        void reset() override;

        /**
         * @brief 结果集是否为空
         * @details 重写 DatabaseResult::isEmpty()：行数未知，恒回 false——不宣称自己空；
         *          误报「空」会让调用方整段跳过它本该处理的数据，这个方向比白跑一次 next() 贵得多。
         * @return false
         */
        [[nodiscard]] bool isEmpty() const override;

    private:
        /**
         * @brief 流收口（幂等）：free_result 吃掉剩余回复流 + 通知连接；清不干净则弃掉缓存里的那条语句
         */
        void endStream() noexcept;

        /**
         * @brief 把本行的各列转成 DatabaseValue；值超缓冲的列就地按实际长度补取
         * @return true 本行可读；false 补取出错（已写好 m_lastError 并收口）
         */
        bool buildCurrentRow();

        MYSQL_STMT      *m_statement{nullptr}; ///< 借用的语句句柄（归连接的语句缓存所有）
        MySqlConnection *m_owner{nullptr};     ///< 产出本结果的连接（收口时回去对账）

        std::string m_commandText; ///< 命令文本副本：清不干净时按键弃掉缓存里的那条语句

        std::vector<std::string>       m_columnNames;         ///< 按列序排列的列名
        std::vector<int>               m_columnTypes;         ///< 每列的 MYSQL_FIELD.type
        std::vector<unsigned int>      m_columnCharacterSets; ///< 每列的字符集号
        std::vector<std::vector<char>> m_columnBuffers;       ///< 每列的取值缓冲（mysql_stmt_fetch 写入）
        std::vector<unsigned long>     m_columnLengths;       ///< 每列本次的真实长度
        std::unique_ptr<bool[]>        m_columnNullFlags;     ///< 每列本行的 NULL 标记

        std::vector<DatabaseValue> m_currentRow;           ///< 当前行的转换结果；空 = 游标不在有效行上
        bool                       m_isStreamEnded{false}; ///< 收口是否已做过（幂等标记）
    };

} // namespace AsynGyanis::Database
