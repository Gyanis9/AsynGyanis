#include "Database/Sqlite/SqliteConnection.h"

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ErrorText.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/Sqlite/SqliteResult.h"

#include <sqlite3.h>

#include <limits>
#include <string>

namespace AsynGyanis::Database
{
    namespace
    {
        // 用 constexpr 常量取代宏：类型安全、作用域受控，且不会污染全局命名空间
        constexpr auto kInMemoryDatabasePath = ":memory:";
        constexpr auto kWriteAheadLogPragma  = "PRAGMA journal_mode=WAL;";
        constexpr auto kForeignKeysPragma    = "PRAGMA foreign_keys=ON;";

        // 语句长度（sqlite3_prepare_v2）与参数长度（sqlite3_bind_text / sqlite3_bind_blob）
        // 的长度形参都是 int，超限会被静默截断成半条语句或半段数据，因此共用同一上限
        constexpr size_t kMaximumNativeLength = static_cast<size_t>(std::numeric_limits<int>::max());
    } // namespace

    SqliteConnection::SqliteConnection(const ConnectionConfig &configuration)
    {
        // 基类的 m_configuration 是唯一真值来源，这里只登记配置，不提前打开文件：
        // 构造阶段做任何 IO 都会让「构造连接」这件事带上失败语义
        m_configuration = configuration;
    }

    SqliteConnection::~SqliteConnection()
    {
        // RAII 收尾：析构阶段虚表已回到本类，直接调用 disconnect() 而不经虚接口，
        // 保证无论调用方是否显式断开都不会漏掉 sqlite3_close
        SqliteConnection::disconnect();
    }

    bool SqliteConnection::connect()
    {
        // 已连接时直接返回，满足基类对 connect() 幂等的要求；
        // 重复 sqlite3_open 同一个文件会拿到第二个句柄，旧句柄就此泄漏
        if (m_isConnected)
        {
            return true;
        }

        // 清掉上一轮的失败文本，避免成功路径上 lastError() 仍报旧错
        m_lastError.clear();

        // 未配置库路径时按内存库处理，与 ConnectionConfig::sqliteDefault() 的默认值保持一致
        const std::string databasePath = m_configuration.database.empty() ? kInMemoryDatabasePath : m_configuration.database;

        if (const int openResult = sqlite3_open(databasePath.c_str(), &m_database); openResult != SQLITE_OK)
        {
            // sqlite3_open 即使返回失败也可能已经分配出句柄，官方约定必须由调用方关闭，否则直接泄漏；
            // 错误文本要趁句柄仍然有效时取，关闭之后再读就是悬垂指针
            std::string failureReason = (m_database != nullptr) ? sqlite3_errmsg(m_database) : sqlite3_errstr(openResult);
            if (failureReason.empty())
            {
                failureReason = sqlite3_errstr(openResult);
            }

            // sqlite3_close(nullptr) 是文档保证的安全空操作，因此无需先判空
            sqlite3_close(m_database);
            m_database    = nullptr;
            m_isConnected = false;
            m_lastError   = "打开 SQLite 数据库失败：" + failureReason + "（路径：" + databasePath + "）";
            return false;
        }

        // SQLite 是进程内引擎，没有网络握手，基类的 connectTimeout() 在这里没有对应能力；
        // queryTimeout() 则映射成 busy_timeout：表被其他连接占用时最多等待这么多毫秒再报 SQLITE_BUSY。
        // 单位与基类一致（毫秒），非正值按「不等待、立即返回 SQLITE_BUSY」处理，避免负数被底层当成特殊值
        const int busyTimeoutMilliseconds = queryTimeout() > 0 ? queryTimeout() : 0;
        sqlite3_busy_timeout(m_database, busyTimeoutMilliseconds);

        // 两条 PRAGMA 属于「尽力而为」的初始化：内存库改不了 WAL、只读目录改不了日志模式都属正常场景，
        // 失败只把原因留在 lastError()，不改变连接结果
        applyStartupPragma(kWriteAheadLogPragma, "启用 WAL 日志模式");
        applyStartupPragma(kForeignKeysPragma, "启用外键约束");

        // 状态位最后置位：前面任何一步没走完都不算连接成功，isConnected() 不会读到中间态
        m_isConnected = true;
        return true;
    }

    void SqliteConnection::disconnect()
    {
        // 既没连过也没有残留句柄时是安全的空操作，析构函数会无条件调用本方法
        if (!m_isConnected && m_database == nullptr)
        {
            return;
        }

        // 先落状态再关句柄：关闭过程中若有回调读 isConnected()，也应看到「已断开」
        m_isConnected = false;
        if (m_database == nullptr)
        {
            return;
        }

        // 缓存里的游标属于**当前这个**数据库句柄，必须在关句柄前全部 finalize：
        // 键只有 SQL 文本，重连之后拿旧游标去 step 就是对已释放对象的访问
        clearStatementCache();

        // 交给外层的 SqliteResult 可能仍持有本连接的语句：
        // sqlite3_close 遇到未 finalize 的语句会返回 SQLITE_BUSY 并拒绝关闭，句柄就此泄漏；
        // sqlite3_close_v2 把连接标记为 zombie，待所有语句 finalize 后再真正释放，正是这种场景的官方用法
        const int closeResult = sqlite3_close_v2(m_database);
        m_database            = nullptr;

        if (closeResult != SQLITE_OK)
        {
            // 只可能是 SQLITE_MISUSE（句柄已被外部释放），用不依赖句柄的 sqlite3_errstr 给出原因
            m_lastError = std::string("关闭 SQLite 连接失败：") + sqlite3_errstr(closeResult);
        }
    }

    bool SqliteConnection::isConnected() const
    {
        // 双判据：m_isConnected 是逻辑状态，句柄非空才是物理事实；
        // 两者不一致说明有路径漏置位，一律按未连接处理更安全
        return m_isConnected && m_database != nullptr;
    }

    std::unique_ptr<DatabaseResult> SqliteConnection::execute(const std::string_view command)
    {
        // 不带参数的路径等价于「参数列表为空」的参数化路径：共用同一份实现，
        // 语句边界检查、错误处理、结果集构造三条行为完全一致，不会出现两套逻辑漂移
        return execute(command, std::span<const DatabaseValue>{});
    }

    std::unique_ptr<DatabaseResult> SqliteConnection::execute(const std::string_view command, const std::span<const DatabaseValue> parameters)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        // 未连接时绝不触碰 prepare，避免把空句柄交给 SQLite
        if (!isConnected())
        {
            m_lastError = "未连接到 SQLite，命令未执行：请先调用 connect() 建立连接";
            return nullptr;
        }

        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        // std::string_view 不保证以 '\0' 结尾，而 sqlite3_prepare_v2 依赖零终止符界定语句边界，
        // 直接把 data() 交给它可能越界读取调用方缓冲区；这里落一份带终止符的副本
        const std::string commandText(command);

        // 长度超过 int 上限会被静默截断，宁可报错也不执行半条语句
        if (commandText.size() > kMaximumNativeLength)
        {
            m_lastError = "数据库命令过长：" + std::to_string(commandText.size()) + " 字节，超出 SQLite 单条语句上限";
            return nullptr;
        }

        // 写语句缓存命中就整趟跳过编译：一条参数化语句里 sqlite3_prepare_v2 占约八成耗时
        // （单文件探针实测：只编译再释放 2613 ns、编译+绑定+推进+取列 3219 ns、复用游标 605 ns）
        sqlite3_stmt *statement   = findCachedStatement(commandText);
        const bool    isFromCache = (statement != nullptr);

        if (!isFromCache)
        {
            const char *unusedTail    = commandText.c_str();
            const int   prepareResult = sqlite3_prepare_v2(m_database, commandText.c_str(), static_cast<int>(commandText.size()), &statement, &unusedTail);
            if (prepareResult != SQLITE_OK)
            {
                // 编译失败时 SQLite 约定把 *ppStmt 置空，无需再 finalize
                captureError("编译 SQL 语句失败");
                return nullptr;
            }

            if (statement == nullptr)
            {
                // 编译成功却没产出语句：输入只剩空白、注释或分号，对数据库没有任何作用。
                // 这里报错而不是返回空结果集，否则调用方会把「什么都没执行」当成执行成功
                m_lastError = "数据库命令中没有可执行的 SQL 语句";
                return nullptr;
            }

            // 一次调用只执行一条语句：让 SQLite 自己再编译一次剩余文本来判断其后是否还有语句。
            // 手工裁剪 tail 需要重造词法器（要认得 -- 行注释与 /* */ 块注释），交给库判断最不容易出错。
            // 缓存命中时这一步整个跳过：同一段文本上次已确认只含一条语句，语法不会自己变
            if (unusedTail != nullptr && *unusedTail != '\0')
            {
                const int     remainingLength   = static_cast<int>(commandText.c_str() + commandText.size() - unusedTail);
                sqlite3_stmt *trailingStatement = nullptr;
                // pTail 出参传 nullptr 是 SQLite 明确允许的：本次探测只关心「还有没有语句」，不需要剩余位置
                const int     trailingResult    = sqlite3_prepare_v2(m_database, unusedTail, remainingLength, &trailingStatement, nullptr);
                const bool    hasExtraStatement = (trailingResult == SQLITE_OK && trailingStatement != nullptr);

                if (trailingStatement != nullptr)
                {
                    sqlite3_finalize(trailingStatement); // 探测用的游标绝不外抛
                }

                if (trailingResult != SQLITE_OK)
                {
                    // 先抓错误文本再释放首条语句：finalize 会重置连接的错误状态，顺序反了就取不到真实原因
                    captureError("额外的 SQL 语句编译失败");
                    sqlite3_finalize(statement);
                    return nullptr;
                }

                if (hasExtraStatement)
                {
                    // 静默丢掉后半段语句会让调用方误以为整段脚本都已生效
                    sqlite3_finalize(statement);
                    m_lastError = "一次调用只执行一条 SQL 语句，检测到额外语句，请拆成多次 execute() 调用";
                    return nullptr;
                }

                // 走到这里说明剩余文本只有空白、注释或多余分号：首条语句依然有效，继续正常执行
            }
        }

        // 绑定参数必须发生在语句首次 step 之前（写语句的副作用就发生在 step 上），
        // 也必须发生在语句边界检查之后：参数个数要与最终确认执行的那一条语句对齐。
        // 绑定失败说明参数与占位符不匹配或类型无法映射，此时一条语句都不执行，
        // 避免出现「SQL 执行了但参数全是 NULL」这种静默错误的中间态
        if (!bindParameters(statement, parameters))
        {
            // 收尾按来源分岔：缓存来的必须 reset 回表（finalize 掉它等于把缓存挖了个洞），
            // 而错误文本已由 bindParameters 先行写好（finalize/reset 会重置连接的错误状态）
            retireStatement(statement, isFromCache);
            return nullptr;
        }

        // 带返回列 = 查询：把游标整体交给结果集，由 SqliteResult 负责推进与 finalize。
        // 构造期会预扫描数行，中途出错（锁超时、IO 错误）时结果集看起来只是「0 行」——
        // 直接交出去，调用方拿到的是「查询没有返回数据」，真错误被静默吞掉。
        // 与写路径同一口径：如实报失败，错误文本先取回本连接，结果集随指针析构一并 finalize
        if (sqlite3_column_count(statement) > 0)
        {
            auto result = std::make_unique<SqliteResult>(statement, m_database);
            if (const std::string preScanError = result->lastError(); !preScanError.empty())
            {
                m_lastError = preScanError;
                return nullptr;
            }
            return result;
        }

        // 无返回列 = 写操作（INSERT/UPDATE/DELETE/DDL）：SQLite 保证一条写语句一次 step 即可跑完
        const int stepResult = sqlite3_step(statement);
        if (stepResult != SQLITE_DONE)
        {
            // SQLITE_BUSY 表示等锁超过了 busy_timeout，SQLITE_ERROR/SQLITE_CONSTRAINT 是语句本身的问题；
            // 先取错误文本再收尾，因为 reset 与 finalize 都会重置语句与连接的错误状态
            captureError("执行 SQL 语句失败");
            retireStatement(statement, isFromCache);
            return nullptr;
        }

        // 收尾一律 reset 而不是 finalize：这条游标下一步要进缓存复用，先 finalize 再把指针存进表里
        // 就是一个已释放对象的地址。reset 的返回码与 finalize 同义——DEFERRABLE 外键这类推迟到语句
        // 收尾才失败的错误就落在这一格上（step 已经返回 SQLITE_DONE，不看这一格就彻底丢了）
        const int resetResult = sqlite3_reset(statement);
        if (resetResult != SQLITE_OK)
        {
            // 用不依赖句柄状态的 sqlite3_errstr：收尾之后连接的 errmsg 可能已被改写
            m_lastError = composeNativeErrorText("收尾 SQL 语句失败", sqlite3_errstr(resetResult), sqlite3_errstr(resetResult), resetResult);

            // 报错的这条不入表：命中来的那份上面那次 reset 已把它放回可用状态，留在表里没有问题；
            // 第一次编译出来的这条必须释放，否则一个不登记在表里的游标就一直占到连接关闭
            if (!isFromCache)
            {
                sqlite3_finalize(statement);
            }
            return nullptr;
        }

        // 游标已被 reset，处于「可从头重跑」的状态，到这里才允许交给缓存
        cacheStatement(std::move(commandText), statement);

        // 写操作没有游标，用空语句构造「执行成功但为空」的结果集；
        // SqliteResult 构造时会立刻快照 sqlite3_changes()，本条语句的影响行数因此不会丢
        return std::make_unique<SqliteResult>(nullptr, m_database);
    }

    DatabaseType SqliteConnection::databaseType() const
    {
        return DatabaseType::Sqlite;
    }

    bool SqliteConnection::beginTransaction()
    {
        // 语句文本取自方言而不是写死在这里：同一件事（开启事务）若有两份语句源，
        // 就会随方言演进而漂移——方言给出的是 BEGIN IMMEDIATE（立刻取写锁），
        // 硬编码的 "BEGIN TRANSACTION" 是 DEFERRED，写锁推迟到第一条写语句才取
        const SqliteDialect dialect;
        // 事务语句本身没有返回列，execute() 非空即代表 BEGIN 已被 SQLite 接受
        return execute(dialect.beginTransactionStatement()) != nullptr;
    }

    bool SqliteConnection::commit()
    {
        const SqliteDialect dialect;
        return execute(dialect.commitStatement()) != nullptr;
    }

    bool SqliteConnection::rollback()
    {
        const SqliteDialect dialect;
        // 没有活动事务时 SQLite 会在 step 阶段报错，这里如实返回 false 并由 lastError() 给出原因
        return execute(dialect.rollbackStatement()) != nullptr;
    }

    void SqliteConnection::resetSessionState() noexcept
    {
        // 未连接，或引擎报告当前处于自动提交（即没有活动事务）：没有要复位的东西。
        // 判据取自 sqlite3_get_autocommit 而不是本类记账，手工执行的 "BEGIN" 也能被认出来
        if (m_database == nullptr || ::sqlite3_get_autocommit(m_database) != 0)
        {
            return;
        }

        // 滚掉事务：失败只记在 lastError() 里（与 rollback() 同一口径），归还路径不看返回码。
        // try/catch 是必需的：rollback() 会构造 std::string（内存分配失败即抛），
        // 而本方法按接口约定是 noexcept，异常穿出去就是 terminate
        try
        {
            [[maybe_unused]] const bool isRolledBack = rollback();
        } catch (...)
        {
            // 归还路径绝不抛出：最坏情况是连接带着未复位的事务回到池里
        }
    }

    std::string SqliteConnection::serverVersion() const
    {
        // SQLite 没有服务端进程，返回链接进来的库版本；sqlite3_libversion() 不依赖句柄，
        // 未连接时同样有效，不因缺少句柄而白白丢掉可用信息
        return sqlite3_libversion();
    }

    std::int64_t SqliteConnection::lastInsertRowId() const
    {
        // 未连接时没有「最近插入」可言，返回 0（SQLite 的 rowid 从 1 起，不会与 0 混淆）
        return m_database != nullptr ? sqlite3_last_insert_rowid(m_database) : 0;
    }

    sqlite3_stmt *SqliteConnection::findCachedStatement(const std::string &sqlText) const noexcept
    {
        const auto entry = m_statementCache.find(sqlText);
        return entry == m_statementCache.end() ? nullptr : entry->second;
    }

    void SqliteConnection::cacheStatement(std::string sqlText, sqlite3_stmt *statement)
    {
        // 命中缓存的这条本来就在表里，此时表满也不该清表——那会把 64 条可用游标为一件本来就不必做的
        // 事扔掉，所以先用一次查找把它排除掉；只有真正新增一条键才可能触到上限
        if (m_statementCache.size() >= kMaximumCachedStatements && m_statementCache.find(sqlText) == m_statementCache.end())
        {
            // 到上限就整表清空：会涨到上限的负载说明「同一句 SQL 被反复执行」这个前提已经不成立，
            // 缓存对它本来就没收益；换来的是游标数有常数上界，且省掉一套 LRU 簿记
            clearStatementCache();
        }

        // emplace 对已存在的键是空操作：既不会把表里那份换成同一个指针，也不会漏 finalize 谁
        static_cast<void>(m_statementCache.emplace(std::move(sqlText), statement));
    }

    int SqliteConnection::retireStatement(sqlite3_stmt *statement, const bool isFromCache) noexcept
    {
        if (isFromCache)
        {
            // reset 把游标放回「可从头重跑」的状态，并释放它这一步拿到的读锁；
            // 它的返回码与 finalize 同义——延迟外键这类推迟到收尾才失败的错误就在这里报出
            return sqlite3_reset(statement);
        }

        return sqlite3_finalize(statement);
    }

    void SqliteConnection::clearStatementCache() noexcept
    {
        for (const auto &[sqlText, statement] : m_statementCache)
        {
            static_cast<void>(sqlText);
            sqlite3_finalize(statement);
        }
        m_statementCache.clear();
    }

    bool SqliteConnection::bindParameters(sqlite3_stmt *statement, const std::span<const DatabaseValue> parameters)
    {
        // 参数个数必须与占位符个数严格相等：SQLite 对未绑定的占位符按 NULL 参与运算，
        // 少给参数会让条件静默变成永假（WHERE "id" = NULL），几乎不可能从结果上反推原因，
        // 因此这里宁可当场失败也不做任何「缺省补 NULL」的宽容处理
        const int expectedParameterCount = sqlite3_bind_parameter_count(statement);
        if (static_cast<std::size_t>(expectedParameterCount) != parameters.size())
        {
            m_lastError = parameterCountMismatchText(static_cast<std::size_t>(expectedParameterCount), parameters.size());
            return false;
        }

        for (std::size_t index = 0; index < parameters.size(); ++index)
        {
            // SQLite 的绑定序号从 1 开始，C++ 下标从 0 开始，这里显式 +1 并保留转换意图
            const int            parameterIndex = static_cast<int>(index) + 1;
            const DatabaseValue &parameterValue = parameters[index];
            int                  bindResult     = SQLITE_OK;

            // 用 std::get_if 取指针而不是 std::get：类型不符时得到空指针并走 else 分支给出中文错误，
            // 而 std::get 会抛 std::bad_variant_access，把「参数类型不对」变成难以处理的异常
            if (std::holds_alternative<std::monostate>(parameterValue))
            {
                // NULL 必须用 sqlite3_bind_null 表达：绑成空字符串后 "IS NULL" 不再成立，
                // 与调用方传空值的意图直接冲突
                bindResult = sqlite3_bind_null(statement, parameterIndex);
            } else if (const auto *booleanValue = std::get_if<bool>(&parameterValue))
            {
                // SQLite 没有独立的布尔存储类，按官方建议用整数 0/1 表示真假
                bindResult = sqlite3_bind_int(statement, parameterIndex, *booleanValue ? 1 : 0);
            } else if (const auto *integerValue = std::get_if<std::int64_t>(&parameterValue))
            {
                bindResult = sqlite3_bind_int64(statement, parameterIndex, *integerValue);
            } else if (const auto *realValue = std::get_if<double>(&parameterValue))
            {
                bindResult = sqlite3_bind_double(statement, parameterIndex, *realValue);
            } else if (const auto *textValue = std::get_if<std::string>(&parameterValue))
            {
                // sqlite3_bind_text 的长度参数是 int，超长文本会被静默截断成半条数据，直接拒绝
                if (textValue->size() > kMaximumNativeLength)
                {
                    m_lastError = parameterTooLongText(index, false, textValue->size(), "SQLite");
                    return false;
                }

                // SQLITE_TRANSIENT 让 SQLite 立刻复制一份文本：语句的 step 可能晚于本函数返回
                // （带返回列的语句要等调用方遍历结果集才真正执行），若用 SQLITE_STATIC，
                // 数据库读到的会是调用方早已释放的缓冲区。文本按字节长度传递，内嵌 '\0' 不丢失
                bindResult = sqlite3_bind_text(statement, parameterIndex, textValue->data(),
                                               static_cast<int>(textValue->size()), SQLITE_TRANSIENT);
            } else if (const auto *byteValue = std::get_if<BinaryBytes>(&parameterValue))
            {
                // sqlite3_bind_blob 的长度参数同样是 int，超长二进制照样会被静默截断
                if (byteValue->size() > kMaximumNativeLength)
                {
                    m_lastError = parameterTooLongText(index, true, byteValue->size(), "SQLite");
                    return false;
                }

                // 空载荷必须走 zeroblob：sqlite3_bind_blob 收到空指针会绑成 SQL NULL，而
                // 「零长度 BLOB」与 NULL 是两件事。std::vector 在为空时 data() 可能返回空指针
                // （这一点与 std::string 不同，后者即使为空也指向内部缓冲），因此不能侥幸
                if (byteValue->empty())
                {
                    bindResult = sqlite3_bind_zeroblob(statement, parameterIndex, 0);
                } else
                {
                    // SQLITE_TRANSIENT 的理由与文本分支相同：语句可能晚于本函数返回才真正执行
                    bindResult = sqlite3_bind_blob(statement, parameterIndex, byteValue->data(),
                                                   static_cast<int>(byteValue->size()), SQLITE_TRANSIENT);
                }
            } else
            {
                // 容器的正确用法是展开成多个标量参数（如 IN 列表），方言层已把 IN 集合展开，
                // 走到这里说明调用方传了非标量值
                m_lastError = containerParameterRejectedText(index, parameterValue);
                return false;
            }

            if (bindResult != SQLITE_OK)
            {
                // 先取错误文本再返回：绑定失败已让语句处于不可用状态，错误文本是唯一可用的线索
                captureError("绑定第 " + std::to_string(index) + " 个参数失败");
                return false;
            }
        }

        return true;
    }

    void SqliteConnection::captureError(const std::string_view description)
    {
        // sqlite3_errmsg 的返回指针只在下一次使用同一连接的 API 之前有效，必须立刻拷进 std::string；
        // 句柄为空时（连接根本没建起来）不能调用它，改用不依赖句柄的全局 sqlite3_errstr
        const int   errorCode  = (m_database != nullptr) ? sqlite3_errcode(m_database) : SQLITE_ERROR;
        const char *rawMessage = (m_database != nullptr) ? sqlite3_errmsg(m_database) : sqlite3_errstr(errorCode);

        // errmsg 可能给出空串，errstr 只需错误码且恒有文案，作为兜底
        m_lastError = composeNativeErrorText(description, rawMessage != nullptr ? rawMessage : "", sqlite3_errstr(errorCode), errorCode);
    }

    void SqliteConnection::applyStartupPragma(const std::string_view pragmaText, const std::string_view description)
    {
        // sqlite3_exec 依赖零终止符，string_view 未必带，落一份副本再用
        const std::string statementText(pragmaText);

        char *            rawError      = nullptr;
        // 错误出参由 SQLite 分配，官方约定必须由调用方 sqlite3_free 释放
        const int         execResult    = sqlite3_exec(m_database, statementText.c_str(), nullptr, nullptr, &rawError);
        const std::string failureReason = (rawError != nullptr) ? rawError : "";
        sqlite3_free(rawError); // sqlite3_free(nullptr) 合法，无需判空

        if (execResult != SQLITE_OK)
        {
            // 不覆盖已经存在的错误文本：先出现的失败更接近根因
            if (m_lastError.empty())
            {
                m_lastError = std::string(description) + "失败：" + (failureReason.empty() ? sqlite3_errstr(execResult) : failureReason);
            }
        }
    }

} // namespace AsynGyanis::Database
