/**
 * @file MySqlResult.h
 * @brief MySQL / MariaDB 查询结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseResult.h"
#include "Database/MySql/MySqlConnection.h" // 复用其中全局作用域的 MYSQL / MYSQL_RES / MYSQL_ROW 前置声明

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    namespace Detail
    {
        /**
         * @brief 流式结果集构造的区分标签
         * @details 只用来让「流式那份 MYSQL_RES」走一个语义明确的构造重载，而不是往既有构造后面
         *          再挂两个默认参数——挂默认参数会让老调用点在改顺序时悄悄换掉含义。
         */
        struct StreamingTag
        {
        };
    } // namespace Detail

    /**
     * @brief MySQL / MariaDB 查询结果集
     *
     * @details 封装 MySQL 客户端库交出的 MYSQL_RES（本对象持有唯一所有权，析构时 mysql_free_result），
     *          传空句柄即「写回执」：0 行 0 列、isEmpty() 恒为 true，表示执行成功但没有任何数据。
     *          m_currentRow 为空与「无当前行」是同一件事，走到末尾或 reset() 之后取值一律回 std::monostate。
     * @details 同一份实现覆盖两种数据来源，差别只在 MYSQL_RES 由哪一句产出：
     *          @li **预读**（mysql_store_result，公开构造那一条）：整份行数据一次复制进客户端内存，
     *              此后取数不再有网络往返，行数与列数在构造时就快照定死，也可以重扫；
     *          @li **流式**（mysql_use_result，见 forStreaming()）：行留在服务端，每调用一次 next()
     *              才取一行，客户端内存只装当前这一行——大结果集不再等额占内存，代价是下面那几条契约差异。
     * @note 文本与参数化两条协议路径共用 MySqlValueConversion.h 解析列值，因此不会出现取值分歧；值一律按
     *       (指针, 长度) 拷贝，内嵌 '\0' 与 BLOB 不被截断。MySQL 没有布尔存储类，TINYINT(1) 同样映射成
     *       std::int64_t，由调用方自行收窄。
     * @warning 数值解析失败或超出 int64 范围（例如 BIGINT UNSIGNED 上界到 2^64-1）时按原始十进制文本交出，
     *          而不是钳成 LLONG_MAX 或返回空值：凭空造出的错误数值比类型不稳定危险得多。
     *
     * @note 与 SqliteResult 的差异：SQLite 的游标挂在连接上，连接必须先于结果集销毁；
     *       MySQL 的**预读**结果已由 mysql_store_result 完整复制进 MYSQL_RES 自有内存，本类不持有任何连接指针，
     *       因此结果集可以比连接对象活得更久（与 RedisResult 同语义）。
     * @warning **流式那份不适用上一条**：它的 MYSQL_RES 背后是服务端还没发完的回复流，因此
     *          ①结果集必须比产出它的连接短命；②在它消费完（或析构）之前，那条连接不能再执行任何语句，
     *          服务端会回「Commands out of sync」；③析构里的 mysql_free_result 会把没读完的回复流吃掉，
     *          这是「只想要前几行就收手」仍然安全的唯一原因。
     */
    class ASYN_DATABASE_API MySqlResult : public DatabaseResult
    {
    public:
        /**
         * @brief 用 mysql_store_result 预读出的结果集构造，并接管其所有权
         * @details 构造阶段一次性快照行数与列数（之后不再调用 mysql_num_rows / mysql_num_fields），
         *          传 nullptr 表示「写操作的空回执」，此时不建立游标，全部计数保持为 0。
         *          除「自增标识宽不到有符号 64 位」这一种情况外不报告失败：数据已由客户端库完整读出，
         *          没有可摘取的服务端错误。
         * @param ownedResult MySQL C API 交出的 MYSQL_RES 指针，所有权移交本对象；可为 nullptr
         * @param affectedRowCount 本条语句影响的行数（WHERE 匹配到多少行；连接在握手里开了
         *                         CLIENT_FOUND_ROWS，值改回原样也算一行），由连接在 mysql_affected_rows /
         *                         mysql_stmt_affected_rows 之后传入；只读结果集按约定传 0
         * @param generatedInsertId 本条语句带回的自增标识，由连接在 mysql_insert_id /
         *                          mysql_stmt_insert_id 之后传入；只读结果集与非插入语句按约定传 0
         */
        explicit MySqlResult(MYSQL_RES *ownedResult, std::int64_t affectedRowCount = 0, std::uint64_t generatedInsertId = 0);

        /**
         * @brief 接管一份**流式**结果集（由 mysql_use_result 产出）并交出所有权
         * @details 与公开构造的唯一差别是数据来源：行留在服务端，每调用一次 next() 才取一行，
         *          因此客户端内存只装当前这一行。列元数据在构造时就绪（列数与列名照常可得），
         *          行数则不然——`rowCount()` 在这种情况下按基类契约回 0，含义是「无法预先知道」。
         * @param ownedResult mysql_use_result 交出的句柄，所有权移交本对象。传 nullptr 也构得出
         *        「一份永远取不到行的流式空结果」，但没有返回列的语句根本走不到这里——那一判在执行路径上
         *        （见 MySqlConnection::executeStreaming），本工厂不负责重复它
         * @param connectionHandle 产出这份结果的连接句柄（**非拥有**）：流式读到 NULL 时，
         *        「取完最后一行」与「链路中断」只能靠 mysql_errno(连接) 区分，故必须带着它。
         *        结果集因此必须比这条连接短命
         * @return std::unique_ptr<MySqlResult> 结果集（构造私有，只有本工厂能造，见私有构造的说明）
         */
        [[nodiscard]] static std::unique_ptr<MySqlResult> forStreaming(MYSQL_RES *ownedResult, MYSQL *connectionHandle);

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
         * @details 重写 DatabaseResult::next()：mysql_fetch_row 推进游标。两种来源的差别在这里最要紧——
         *          预读结果的行都在客户端内存里，返回空指针只可能是「已到末尾」；流式结果的空指针
         *          还可能是链路中断，因此本方法在流式档上会拿 mysql_errno(连接) 分辨二者，把后者写进
         *          lastError()（**这是本驱动唯一一处 next() 会改写错误状态的实现**，基类「只读路径不改
         *          错误状态」的纪律在这里让位于「不能把断链说成读完了」）。
         *          两种情形都回 false：调用方要么先判 lastError() 再决定重查，要么按「没有更多行」收尾。
         * @return true 游标停在有效行上，可以读取列值
         * @return false 已无更多行，或本结果集是写回执（没有游标可言），或流式读取途中链路中断
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：预读结果取构造时快照的 mysql_num_rows，是精确值；
         *          **流式结果回 0，含义是基类契约里的「无法预先得知」而不是「一行都没有」**——
         *          客户端库里那份 MYSQL_RES 在取完全部行之前根本没有这个数，凭空给一个非零值或让调用方
         *          把 0 读成空集都危险，所以老实回 0 并由 isEmpty() 另走一条不谎报的判定。
         * @return size_t 行数；流式结果按「未知」回 0
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
         * @brief 重置游标到首行之前，使结果集可重新遍历（**只对预读结果有效**）
         * @details 预读档：用 mysql_data_seek 退回第 0 行（官方保证随机定位只对 mysql_store_result 的
         *          预读结果有效，该接口是 void、无法报告失败）；还要把 m_currentRow 置空，否则
         *          getValue() 会继续读到上一行遗留的数据。
         * @warning 流式档没有可退回的位置：行已经从服务端发过来了就再也拿不回来，官方也明说定位
         *          只适用于预读结果。因此这一档上本方法**不做任何定位**，只清掉当前行指针并把
         *          「不支持重扫」写进 lastError()——游标保持在上次停下的地方，继续 next() 还是 false。
         *          要重扫就把数据读进自己的容器，或改用 execute() 的预读路径。
         */
        void reset() override;

        /**
         * @brief 判断结果集是否为空
         * @details 预读档：由构造时快照的行数判定（预读结果的行数恒精确，同一事实只保留一处真值来源），
         *          语义是「结果集本身有没有行」而非「还剩多少行可读」，遍历完仍为 false。
         * @warning 流式档**一律回 false**，含义是「不宣称自己是空的」：没读完就没有任何依据说这堆行是空的，
         *          而把一份非空结果误报成空会让调用方整段跳过它该做的事——那是比「多跑一次空循环」贵得多的
         *          错误方向。要知道有没有读到行，判 next() 的返回值。
         * @return true 没有任何数据行（流式结果恒为 false）
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

        /**
         * @brief 流式结果的构造：只由 forStreaming() 调用
         * @details 私有是有意为之：流式那份必须带着产出它的连接句柄才有意义（next() 靠它分辨
         *          「取完」与「断链」），让调用点自由拼这两个参数就会造出一个自己判不出截断的结果集。
         * @param ownedResult 移交所有权的 MYSQL_RES（由 mysql_use_result 交出）
         * @param connectionHandle 非拥有的连接句柄
         * @param tag 与预读构造区分的标签，无它义
         */
        MySqlResult(MYSQL_RES *ownedResult, MYSQL *connectionHandle, Detail::StreamingTag tag) noexcept;

        MYSQL_RES   *m_result{nullptr};           ///< MySQL 结果集句柄（预读或流式），非空时由本对象负责 mysql_free_result
        MYSQL_ROW    m_currentRow{nullptr};       ///< 当前行的列指针数组，空表示游标未停在有效行上
        size_t       m_rowCount{0};               ///< 构造时快照的行数；写回执与**流式结果**都是 0（后者含义是「未知」）
        size_t       m_columnCount{0};            ///< 构造时快照的列数，写回执结果为 0
        std::int64_t m_affectedRowCount{0};       ///< 构造时快照的语句级影响行数，只读结果集与写回执之外恒为 0
        std::int64_t m_lastInsertRowId{0};        ///< 构造时快照的语句级自增标识，非插入语句与宽不进 int64 时为 0
        MYSQL       *m_connectionHandle{nullptr}; ///< 流式结果的生产者连接（非拥有），只用于 next() 分辨「取完」与「断链」
        bool         m_isStreaming{false};        ///< 数据来源是否为 mysql_use_result；决定 rowCount/isEmpty/reset 的三处口径
    };

} // namespace AsynGyanis::Database
