/**
 * @file SqliteConnection.h
 * @brief SQLite 嵌入式数据库连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseConnection.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// sqlite3 与 sqlite3_stmt 是 SQLite 头文件中定义的全局 C 结构体，前置声明统一集中写在本头的全局作用域：
// 只有 .cpp 才包含 <sqlite3.h>，避免第三方 C 头顺着包含链传染给所有使用方。
// SqliteResult.h 通过包含本头复用这两行声明，不得重复声明。
struct sqlite3;
struct sqlite3_stmt;

namespace AsynGyanis::Database
{
    /**
     * @brief SQLite 嵌入式数据库连接
     *
     * @details 封装 SQLite C API，实现 DatabaseConnection 抽象接口。SQLite 是进程内引擎，
     *          数据库就是一个文件（或 ":memory:" 代表的内存库），没有服务进程、没有网络往返，
     *          因此 ConnectionConfig 中只有 database 字段会被读取，
     *          host / port / userName / password 一律忽略。
     *
     * connect() 在打开句柄之后还会做两件配置：
     * - 把基类的 queryTimeout() 映射成 sqlite3_busy_timeout，命令遇到表锁时最多等待该毫秒数；
     * - 尝试启用 WAL 日志模式与外键约束。两条 PRAGMA 失败都不致命，原因只写入 lastError()。
     *
     * 生命周期：构造 → connect() → execute() / 事务 → disconnect() → 析构。
     *          析构自动调用 disconnect()，因此按智能指针或栈对象使用都不会泄漏句柄。
     *          本对象独占 sqlite3* 句柄，任何副本或移动后的源对象都会重复关闭同一句柄，故拷贝与移动一律禁止。
     *
     * 错误信息统一写入基类的 m_lastError，lastError() 沿用基类实现，本类不做重复覆写。
     *
     * @warning execute() 交出的 SqliteResult 保存本连接句柄的非拥有指针，
     *          结果集必须严格早于连接对象销毁，否则游标会访问已释放的 sqlite3*。
     *
     * @code
     *   auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault());
     *   if (connection->connect())
     *   {
     *       connection->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
     *       auto result = connection->execute("SELECT id, name FROM users");
     *       while (result != nullptr && result->next())
     *       {
     *           const DatabaseValue name = result->getValue("name");
     *       }
     *   }
     * @endcode
     */
    class SqliteConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 SQLite 连接，此阶段不打开任何数据库文件
         * @param configuration 连接配置，只有 configuration.database 会被使用；
         *                      为空串时 connect() 按内存库处理
         */
        explicit SqliteConnection(const ConnectionConfig &configuration);

        /**
         * @brief 析构时自动断开连接，释放底层 sqlite3 句柄
         */
        ~SqliteConnection() override;

        // 连接独占 sqlite3 句柄的所有权：拷贝会出现两个对象 close 同一句柄，
        // 移动会让源对象析构时重复关闭，因此拷贝与移动一律禁止（基类同样已删除，这里显式写清意图）。
        SqliteConnection(const SqliteConnection &)            = delete;
        SqliteConnection &operator=(const SqliteConnection &) = delete;
        SqliteConnection(SqliteConnection &&)                 = delete;
        SqliteConnection &operator=(SqliteConnection &&)      = delete;

        /**
         * @brief 打开（或创建）SQLite 数据库文件
         * @details 重写 DatabaseConnection::connect()：SQLite 不需要握手与认证，打开失败只来自
         *          文件系统或文件本身（路径非法、目录不可写、文件损坏等）。相较基类契约的额外行为：
         *          1) 已连接时直接返回 true，保持幂等；
         *          2) 打开失败时立刻关闭 sqlite3_open 可能已分配的半开句柄，绝不留下泄漏；
         *          3) 成功后应用 busy_timeout 与两条 PRAGMA，PRAGMA 失败不影响返回值；
         *          4) 基类的 connectTimeout() 在此无对应能力（进程内没有网络等待），故不参与配置。
         * @return true 连接已建立
         * @return false 打开失败，具体原因（含 SQLite 错误码与路径）见 lastError()
         * @note 路径必须是 UTF-8 字节序列；Windows 下由调用方负责从宽字符路径转换而来
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放底层 sqlite3 句柄
         * @details 重写 DatabaseConnection::disconnect()：与基类的差异在于使用 sqlite3_close_v2
         *          而非 sqlite3_close——后者遇到尚未 finalize 的语句会返回 SQLITE_BUSY 并拒绝关闭，
         *          从而泄漏句柄；close_v2 会把连接标记为 zombie，等所有语句 finalize 之后再真正释放，
         *          正好覆盖「结果集仍存活于调用方手中」这一场景。
         *          未连接时调用是安全的空操作，析构函数会无条件调用本方法。
         * @note 本方法不等待外部结果集销毁，只保证不再泄漏；游标在连接销毁后使用仍是未定义行为
         */
        void disconnect() override;

        /**
         * @brief 判断连接是否可用
         * @details 重写 DatabaseConnection::isConnected()：SQLite 是进程内引擎，句柄存在即可用，
         *          因此不做任何活性探测（无 mysql_ping 之类的对应 API），只同时校验
         *          基类的 m_isConnected 标志与句柄非空，两者不一致时按未连接处理。
         * @return true 已连接且句柄有效
         */
        [[nodiscard]] bool isConnected() const override;

        /**
         * @brief 执行一条 SQL 命令
         * @details 重写 DatabaseConnection::execute()：统一走 sqlite3_prepare_v2 + sqlite3_step，
         *          不再区分查询与写语句的两套代码路径。与基类约定的差异：
         *          - 每次调用开头清空 m_lastError，成功调用不会残留上一轮的失败文本；
         *          - 带返回列的语句把游标整体交给 SqliteResult（由结果集负责 finalize 与推进）；
         *          - 无返回列的语句一步跑完，返回「执行成功但为空」的结果集，影响行数由结果集快照；
         *          - 一次调用只接受一条语句：先让 SQLite 探测剩余文本，发现额外语句时整次调用直接失败，
         *            一条都不执行，避免旧实现那种「前面的生效、后面的被静默丢掉」。
         * @param command SQL 文本，内部会复制为零终止串后交给 SQLite
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         * @warning 带返回列的写语句（INSERT/UPDATE/DELETE ... RETURNING，以及会回显值的 PRAGMA）
         *          在本函数里不做任何 step，语句真正执行发生在调用方第一次 SqliteResult::next()。
         *          只判返回值非空而不遍历，写副作用就不会发生——需要立即生效的写语句请写成不带
         *          RETURNING 的形式，或显式遍历结果集。
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command) override;

        // 引入基类的全部 execute 重载：本类声明了名为 execute 的成员，按 C++ 名字查找规则
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用两个版本
        using DatabaseConnection::execute;

        /**
         * @brief 执行一条带参数的 SQL 命令（按位置绑定）
         * @details 重写 DatabaseConnection::execute()：与不带参数版本的唯一差异是先把
         *          parameters 逐个绑定到语句占位符上再执行。绑定规则：
         *          - std::monostate → sqlite3_bind_null（真正的 SQL NULL，而不是空串）；
         *          - bool → sqlite3_bind_int 的 1/0（SQLite 没有布尔存储类）；
         *          - std::int64_t → sqlite3_bind_int64；
         *          - double → sqlite3_bind_double；
         *          - std::string → sqlite3_bind_text，按字节长度传递且用 SQLITE_TRANSIENT 复制，
         *            因为语句的 step 可能晚于本调用返回（结果集存活期间），不能引用调用方的缓冲区；
         *          - 容器类型（List/Hash）无法映射成标量参数，直接失败并给出中文原因。
         *          此外还会校验「占位符个数 == 参数个数」：SQLite 对未绑定的占位符按 NULL 处理，
         *          少给参数会静默变成永假条件，因此宁可当场报错。
         *          参数值一律以绑定方式送入，不拼进 SQL 文本，含单引号、"--"、分号的字符串
         *          因此只是普通文本（见 SqlStatement.h 的说明）。
         * @param command    带占位符的 SQL 文本，内部会复制为零终止串后交给 SQLite
         * @param parameters 按占位符出现顺序排列的绑定参数，第 i 个元素绑定到第 i 个占位符
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         * @note 传空 parameters 时与不带参数的 execute() 完全等价（两条路径共用同一实现）
         * @warning 与不带参数版本一致：带返回列的写语句（INSERT ... RETURNING）真正的执行
         *          发生在调用方第一次 SqliteResult::next()，只判非空而不遍历则写副作用不会发生
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command,
                                                              std::span<const DatabaseValue> parameters) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::Sqlite，
         *          不依赖连接状态，未连接时同样可用于日志与断言。
         * @return DatabaseType DatabaseType::Sqlite
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 开启一个事务
         * @details 执行本方言（SqliteDialect）给出的开启语句 "BEGIN IMMEDIATE"，是
         *          SQLite 专有能力的便捷封装，不覆盖基类任何虚函数。
         *          语句文本刻意取自方言而不是硬编码：方言给的是 IMMEDIATE（BEGIN 时立刻取写锁，
         *          失败当场暴露），硬编码成 "BEGIN TRANSACTION" 则是 DEFERRED（写锁推迟到第一条
         *          写语句，多连接并发时必然撞上无法靠重试化解的 SQLITE_BUSY）。
         *          与 Transaction + 方言这条路径用的是同一份语句来源，二者不会漂移。
         * @return true 事务已开启
         * @return false 已处于事务中或未连接，原因见 lastError()
         */
        bool beginTransaction();

        /**
         * @brief 提交当前事务
         * @details 执行本方言给出的提交语句 "COMMIT"，语句文本同样以方言为唯一来源。
         * @return true 提交成功
         * @return false 没有活动事务或未连接，原因见 lastError()
         */
        bool commit();

        /**
         * @brief 回滚当前事务
         * @details 执行本方言给出的回滚语句 "ROLLBACK"，与 commit() 一样属于事务控制便捷封装，
         *          语句文本同样以方言为唯一来源。
         * @return true 回滚成功
         * @return false 没有活动事务或未连接，原因见 lastError()
         */
        bool rollback();

        /**
         * @brief 获取数据库版本字符串
         * @details SQLite 没有服务端进程，因此返回的是链接进来的 SQLite 库版本；
         *          sqlite3_libversion() 不依赖句柄，未连接时同样返回有效文本（旧实现在未连接时返回空串）。
         * @return std::string 形如 "3.45.0" 的版本号
         */
        [[nodiscard]] std::string serverVersion() const;

        /**
         * @brief 获取本连接最近一次插入操作生成的 rowid
         * @details 读的是 SQLite 的连接级计数器，任何 INSERT 都会刷新它，与结果集无关。
         * @return std::int64_t 最近插入的 rowid；未连接或从未插入过时为 0
         */
        [[nodiscard]] std::int64_t lastInsertRowId() const;

        /**
         * @brief 获取底层 sqlite3 句柄，供需要直接使用 SQLite C API 的高级场景使用
         * @warning 返回的句柄所有权始终属于本连接，调用方不得 sqlite3_close，
         *          也不得在连接销毁后继续使用
         * @return sqlite3* 未连接时为 nullptr
         */
        [[nodiscard]] sqlite3 *nativeHandle() const noexcept { return m_database; }

    private:
        /**
         * @brief 把参数按位置绑定到已编译的语句上
         * @param statement 已 prepare 的语句句柄，绑定失败时由调用方负责 finalize
         * @param parameters 待绑定的参数列表，第 i 个元素绑定到第 i 个占位符（SQLite 序号从 1 起）
         * @return true 全部参数绑定成功
         * @return false 参数个数不匹配、参数类型不受支持或底层绑定失败，原因见 lastError()
         */
        [[nodiscard]] bool bindParameters(sqlite3_stmt *statement, std::span<const DatabaseValue> parameters);

        /**
         * @brief 采集 SQLite 的错误文本与错误码并写入 m_lastError
         * @param description 面向使用者的中文动作说明，例如「编译 SQL 语句失败」
         */
        void captureError(std::string_view description);

        /**
         * @brief 执行连接初始化阶段的 PRAGMA，失败只记录原因不中断连接
         * @param pragmaText 完整的 PRAGMA 语句文本
         * @param description 失败信息里展示的中文动作名
         */
        void applyStartupPragma(std::string_view pragmaText, std::string_view description);

        sqlite3 *m_database{nullptr}; ///< SQLite C API 数据库句柄，本对象独占所有权
    };

} // namespace AsynGyanis::Database
