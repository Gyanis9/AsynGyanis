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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

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
     * @details 封装 SQLite C API，实现 DatabaseConnection 抽象接口：进程内引擎、没有网络往返，因此
     *          ConnectionConfig 只有 database 字段被读取。基类 queryTimeout() 在这里落成两道界，都在
     *          execute() 入口按当次取值现读（改超时不必重连），非正值一律按「不设界」处理：等锁上限走
     *          sqlite3_busy_timeout，语句执行时限由进度回调打断。一条参数化语句里「编译」占约八成耗时，
     *          故跑完的游标按 SQL 文本缓存在 m_statementCache 里复用，表满时逐出最久没被读到的一条。
     *
     * @warning execute() 交出的 SqliteResult 保存本连接句柄的非拥有指针，结果集必须严格早于连接对象销毁，
     *          否则游标会访问已释放的 sqlite3*。语句时限同样只管 execute() 同步执行那一段：交出游标之后的
     *          next() 不受它约束，慢速遍历不会被当成超时打断。
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
        SqliteConnection(const SqliteConnection &) = delete;

        SqliteConnection &operator=(const SqliteConnection &) = delete;

        SqliteConnection(SqliteConnection &&) = delete;

        SqliteConnection &operator=(SqliteConnection &&) = delete;

        /**
         * @brief 打开（或创建）SQLite 数据库文件
         * @details 重写 DatabaseConnection::connect()：SQLite 不需要握手与认证，打开失败只来自文件系统或
         *          文件本身（路径非法、目录不可写、文件损坏等）。与基类的差异：已连接时直接返回 true 保持幂等；
         *          打开失败立刻关闭 sqlite3_open 可能已分配的半开句柄；成功后应用 busy_timeout、挂上语句时限
         *          的进度回调、执行两条 PRAGMA（PRAGMA 失败不影响返回值）；connectTimeout() 无对应能力，不参与配置。
         *          其余与基类一致。
         * @return true 连接已建立
         * @return false 打开失败，具体原因（含 SQLite 错误码与路径）见 lastError()
         * @note 路径必须是 UTF-8 字节序列；Windows 下由调用方负责从宽字符路径转换而来
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放底层 sqlite3 句柄
         * @details 重写 DatabaseConnection::disconnect()：用 sqlite3_close_v2 而非 sqlite3_close——后者遇到
         *          尚未 finalize 的语句会返回 SQLITE_BUSY 并拒绝关闭，从而泄漏句柄；close_v2 把连接标记为
         *          zombie，等所有语句 finalize 之后再真正释放，正好覆盖「结果集仍存活」这一场景。
         *          未连接时调用是安全的空操作。其余与基类一致。
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
         * @details 重写 DatabaseConnection::execute()：统一走 sqlite3_prepare_v2 + sqlite3_step，每次调用
         *          开头清空 m_lastError；带返回列的语句把游标整体交给 SqliteResult，无返回列的语句一步跑完并
         *          返回「执行成功但为空」的结果集。一次调用只接受一条语句，发现额外语句时整次失败，避免
         *          「前面的生效、后面的被静默丢掉」这种半执行状态。其余与基类一致。
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
         * @details 重写 DatabaseConnection::execute()：按位置绑定参数后执行——std::monostate→SQL NULL、
         *          bool→0/1、字符串按字节长度并用 SQLITE_TRANSIENT 复制（step 可能晚于本调用返回，不能引用
         *          调用方的缓冲区）。容器类型无法映射成标量参数，直接失败；绑定前校验「占位符个数 == 参数
         *          个数」（SQLite 对未绑定的占位符按 NULL 处理，少给会静默变成永假条件）。其余与基类一致。
         * @param command    带占位符的 SQL 文本，内部会复制为零终止串后交给 SQLite
         * @param parameters 按占位符出现顺序排列的绑定参数，第 i 个元素绑定到第 i 个占位符
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         * @note 传空 parameters 时与不带参数的 execute() 完全等价（两条路径共用同一实现）
         * @warning 与不带参数版本一致：带返回列的写语句（INSERT ... RETURNING）真正的执行
         *          发生在调用方第一次 SqliteResult::next()，只判非空而不遍历则写副作用不会发生
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command, std::span<const DatabaseValue> parameters) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::Sqlite，未连接时同样可用。
         * @return DatabaseType DatabaseType::Sqlite
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 开启一个事务
         * @details 执行本方言（SqliteDialect）给出的开启语句 "BEGIN IMMEDIATE"，是 SQLite 专有能力的便捷
         *          封装，不覆盖基类任何虚函数。语句文本刻意取自方言而不是硬编码：IMMEDIATE 在 BEGIN 时立刻取
         *          写锁、失败当场暴露，硬编码的 "BEGIN TRANSACTION" 是 DEFERRED，写锁推迟到第一条写语句，
         *          多连接并发时必然撞上无法靠重试化解的 SQLITE_BUSY。
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
         * @details 执行本方言给出的回滚语句 "ROLLBACK"，语句文本同样以方言为唯一来源。
         * @return true 回滚成功
         * @return false 没有活动事务或未连接，原因见 lastError()
         */
        bool rollback();

        /**
         * @brief 归还连接池时复位会话状态：把未提交的事务滚掉
         * @details 残留的事务会跟着连接串给下一个借用者：对方的语句悄悄并进上一笔事务，BEGIN IMMEDIATE
         *          取到的写锁会一直握到那条连接被回收，别的连接全被挡在门外。判定直接问引擎
         *          （sqlite3_get_autocommit），因此手工执行的 "BEGIN" 同样能被认出，不依赖本类另记状态。
         *          其余与基类契约一致。
         * @note 与基类契约一致：不抛异常、幂等；未连接或本就没有活动事务时不做任何事
         */
        void resetSessionState() noexcept override;

        /**
         * @brief 获取数据库版本字符串
         * @details SQLite 没有服务端进程，因此返回的是链接进来的 SQLite 库版本；
         *          sqlite3_libversion() 不依赖句柄，未连接时同样返回有效文本。
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
        [[nodiscard]] sqlite3 *nativeHandle() const noexcept
        {
            return m_database;
        }

        /**
         * @brief 语句缓存当前的游标条数
         * @details 上界是 kMaximumCachedStatements。表满时逐出的是「最久没被读到」的那一条，
         *          条数因此稳定停在上界，不会像整表清空那样一夜回到 0。
         * @return std::size_t 表里的游标条数
         */
        [[nodiscard]] std::size_t cachedStatementCount() const noexcept
        {
            return m_statementCache.size();
        }

        /**
         * @brief 语句缓存自本连接构造以来的累计命中次数
         * @details 一次命中省下的是一整趟 sqlite3_prepare 编译，因此这个计数就是「热语句有没有被
         *          逐出」的可判定证据（用例与基准都按它来判，不靠测时间猜）。
         * @return std::uint64_t 累计命中次数
         */
        [[nodiscard]] std::uint64_t statementCacheHitCount() const noexcept
        {
            return m_statementCacheHits;
        }

    private:
        /**
         * @brief 语句时限的闸门：进入 execute() 时按当次 queryTimeout() 装上，离开作用域一律撤下
         * @details 必须成对：时限留在连接上，调用方之后慢速遍历游标就会被上一次语句的截止时刻打断，
         *          而 SqliteResult::next() 按契约不写错误文本——那会表现为「结果集悄悄少了若干行」。
         */
        class StatementDeadlineGuard
        {
        public:
            /**
             * @brief 用归属连接的当前超时值装上时限
             * @param owner 执行这条语句的连接
             */
            explicit StatementDeadlineGuard(SqliteConnection &owner) noexcept : m_owner(owner)
            {
                m_owner.armStatementDeadline(m_owner.queryTimeout());
            }

            /**
             * @brief 离开作用域时无条件撤下时限
             */
            ~StatementDeadlineGuard()
            {
                m_owner.disarmStatementDeadline();
            }

            StatementDeadlineGuard(const StatementDeadlineGuard &) = delete;

            StatementDeadlineGuard &operator=(const StatementDeadlineGuard &) = delete;

        private:
            SqliteConnection &m_owner; ///< 被装上/撤下时限的连接，活在本闸门外层作用域里
        };

        /**
         * @brief 把「本条语句最迟何时结束」记进连接状态
         * @details 由 SQLite 的进度回调读取，因此每次执行都要重装一次：这样 setQueryTimeout() 改完
         *          下一条语句即生效，不必像 MySQL 那样等到重连。
         * @param milliseconds 时限毫秒数，取自 queryTimeout()；非正值表示不装时限（与 MySQL/Redis 对
         *                     非正值的解读一致，此处刻意不钳成 1 毫秒去伪造一个上界）
         */
        void armStatementDeadline(const int milliseconds) noexcept;

        /**
         * @brief 撤下语句时限
         * @details 时限不再参与判定，进度回调回到「一次布尔比较就返回」的零成本路径
         */
        void disarmStatementDeadline() noexcept
        {
            m_statementDeadlineArmed = false;
        }

        /**
         * @brief SQLite 进度回调：到点就请求打断本条语句
         * @details 回调按 kProgressHandlerInterval 个虚拟机指令的间隔被调用，返回非 0 时 SQLite 以
         *          SQLITE_INTERRUPT 中止当前操作。签名必须是 C 函数指针形态，故不带 noexcept。
         * @param ownerPointer 注册时传入的 SqliteConnection 指针，SQLite 原样送回
         * @return int 0 表示继续执行，1 表示要求打断
         */
        static int enforceStatementDeadline(void *ownerPointer);

        /**
         * @brief 拼装「被语句时限打断」的中文错误文本
         * @return std::string 带上限毫秒数与出路的文案，交进 m_lastError
         */
        [[nodiscard]] std::string statementDeadlineErrorText() const;

        /**
         * @brief 把参数按位置绑定到已编译的语句上
         * @param statement 已 prepare 的语句句柄，绑定失败时由调用方负责收尾
         * @param parameters 待绑定的参数列表，第 i 个元素绑定到第 i 个占位符（SQLite 序号从 1 起）
         * @return true 全部参数绑定成功
         * @return false 参数个数不匹配、参数类型不受支持或底层绑定失败，原因见 lastError()
         */
        [[nodiscard]] bool bindParameters(sqlite3_stmt *statement, std::span<const DatabaseValue> parameters);

        /**
         * @brief 在语句缓存里找一条已编译的游标，并把这条记为「刚被用到」
         * @param sqlText 语句文本（本次调用已有的那份带零终止符的副本，不再另造键）
         * @return sqlite3_stmt* 命中时返回已 reset 到可重跑状态的游标；未命中为 nullptr
         */
        [[nodiscard]] sqlite3_stmt *findCachedStatement(const std::string &sqlText) noexcept;

        /**
         * @brief 把一条查询游标从表里摘走，但不释放它
         * @details 查询游标可能随结果集活到调用方手里（行数超过快照上限时不物化），
         *          届时结果集析构会 finalize 它；表里若还留着同一地址就是悬空键。
         *          写完收回的游标没有这个问题（本函数执行完一定回到表里），故只有查询路径摘。
         * @param sqlText 语句文本
         */
        void detachCachedStatement(const std::string &sqlText) noexcept;

        /**
         * @brief 把一条跑完并 reset 过的游标放进缓存
         * @details 表内已有同文本条目时空操作（缓存命中的那条本就在表里）。
         * @param sqlText 语句文本，接管其内容作键
         * @param statement 可复用的游标，所有权移交缓存
         */
        void cacheStatement(std::string sqlText, sqlite3_stmt *statement);

        /**
         * @brief 收尾一条**不再复用**的游标：缓存来的 reset 归还，新编译的 finalize 释放
         * @details 只在出错、这条不打算入表的路上用；成功路径的收尾直接 sqlite3_reset
         *          （见 execute()），因为那条游标下一步就要交给缓存。
         * @param statement 待收尾的游标，不可为空
         * @param isFromCache 该游标是否来自 m_statementCache
         * @return sqlite3_reset 或 sqlite3_finalize 的返回码；两者都是「延迟外键等 step 之后
         *         才浮现的错误」的报出点，因此这一格返回码在两条路上都必须查
         */
        int retireStatement(sqlite3_stmt *statement, bool isFromCache) noexcept;

        /**
         * @brief finalize 掉缓存里全部游标并清空表
         * @details 必须在换掉 m_database 之前调用：缓存的键只有 SQL 文本，而游标句柄属于
         *          具体的那个数据库连接，重连之后旧游标一律不可再用。
         */
        void clearStatementCache() noexcept;

        /**
         * @brief 逐出缓存里最久没被读到的那一条游标
         * @details 只在表已满、正要新增一个键时调用（调用方保证表非空）。逐出而不是清空，是因为
         *          「少数热语句 + 大量一次性语句」才是常态：整表清空会把热的那批一起扔掉，
         *          接下来每一次热语句执行都要重新编译，代价比省下的那点内存大得多。
         */
        void evictLeastRecentlyUsedStatement() noexcept;

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

        /**
         * @brief 语句缓存的容量上限
         * @details 到上限时逐出最久没被读到的一条（见 evictLeastRecentlyUsedStatement()），
         *          游标数与内存因此仍有常数上界，而热语句不会被一次性语句挤掉。
         */
        static constexpr std::size_t kMaximumCachedStatements = 64;

        /// 缓存里的一条游标连同它的使用记号
        struct CachedStatement
        {
            sqlite3_stmt *statement{nullptr};  ///< 已编译且处于 reset 态的游标，所有权在表
            std::uint64_t lastUseStamp{0};     ///< 最近一次被读到或写入时的戳记，越小越先被逐出
        };

        sqlite3 *m_database{nullptr}; ///< SQLite C API 数据库句柄，本对象独占所有权

        /// SQL 文本 → 已编译且已 reset 的游标。写语句跑完即回表；查询语句只有「行已整份物化进快照」
        /// 时才回表（那种游标此后不再被任何人引用），行没跑完的查询游标随结果集走、由结果集 finalize
        std::unordered_map<std::string, CachedStatement> m_statementCache;

        /// 单调递增的使用计数器，充当「最近使用」的比较依据：只用于逐出排序，不参与任何正确性判定
        std::uint64_t m_statementCacheUseStamp{0};

        /// 缓存命中累计次数：命中一次即少编译一条语句，供用例与基准判定逐出策略是否留住了热语句
        std::uint64_t m_statementCacheHits{0};

        /// 进度回调的触发间隔（虚拟机指令条数）：1000 条一次，检查成本相对指令执行可忽略，
        /// 而超时的最大过冲只有一个指令批次的粒度
        static constexpr int kProgressHandlerInterval = 1000;

        std::chrono::steady_clock::time_point m_statementDeadline{}; ///< 本条语句的最迟结束时刻，仅 m_statementDeadlineArmed 为真时有意义
        int  m_statementDeadlineMilliseconds{0};                     ///< 装时限时使用毫秒数，只用于错误文案里报出上限
        bool m_statementDeadlineArmed{false};                        ///< 时限是否生效，false 时回调不做任何时钟读取
        bool m_statementDeadlineHit{false};                          ///< 本次执行是否真的因超时被打断，用于把 SQLITE_INTERRUPT 翻成可操作的中文原因
    };

} // namespace AsynGyanis::Database
