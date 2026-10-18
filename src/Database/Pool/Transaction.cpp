#include "Database/Pool/Transaction.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Dialect/DialectRegistry.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace AsynGyanis::Database
{
    Transaction::Transaction(ConnectionPool &pool) : Transaction(pool, TransactionIsolationLevel::Default)
    {
    }

    Transaction::Transaction(ConnectionPool &pool, const TransactionIsolationLevel isolationLevel) : Transaction(pool, isolationLevel, TransactionAccessMode::ReadWrite)
    {
    }

    Transaction::Transaction(ConnectionPool &pool, const TransactionIsolationLevel isolationLevel, const TransactionAccessMode accessMode)
    {
        // 构造的第一步就锁定连接：事务期间的每一条语句都必须落在同一条连接上，
        // 若拖到第一条语句再去池里取，BEGIN 与后续语句就可能分属不同连接（见文件头说明）
        m_connection = pool.acquire();
        if (!m_connection)
        {
            throw ConnectionUnavailableException("数据库事务：无法从连接池获取连接（池已达上限且等待超时、"
                                                 "连接工厂创建失败或等待名额被拒），事务未开启");
        }

        // 事务控制语句的文本由方言提供：SQLite 用 BEGIN IMMEDIATE、MySQL 用 START TRANSACTION，
        // 写死在事务对象里等于把方言知识放到了错误的层。方言在此解析一次并缓存，
        // 未实现的类型（MySQL / Redis）会在这里抛出带中文提示的 Base::InvalidArgumentException
        m_dialect = DialectRegistry::dialectFor(m_connection->databaseType());

        // 只读形态在任何语句发出之前就判定：引擎给不出那句话就当场拒绝，而不是开一笔普通事务
        // 「反正也读不出区别」。顺序排在隔离级别语句之前还有第二层理由——SET TRANSACTION 一旦发出去
        // 就落在连接会话上，此时再抛出，那条连接带着残留设置归还池，下一个借用者读到的是别人的一档
        const std::optional<std::string_view> beginStatement =
                accessMode == TransactionAccessMode::ReadOnly ? m_dialect->readOnlyTransactionStatement() : std::optional<std::string_view>{m_dialect->beginTransactionStatement()};
        if (!beginStatement.has_value())
        {
            throw Base::InvalidArgumentException("数据库事务：引擎 " + std::string(databaseTypeName(m_connection->databaseType())) +
                                                 " 没有只读事务形态，无法按 ReadOnly 开启。SQLite 的 BEGIN 只有 deferred / immediate / exclusive 三种，"
                                                 "都不区分读写；要防误写请在同一连接上开事务后立刻执行只读约束（应用层判据），或换用 MySQL / PostgreSQL");
        }

        // 请求了非默认隔离级别：取出方言的设置语句并检查可用性。语句文本的执行位置由方言决定
        // （MySQL / SQLite 的 SET TRANSACTION 必须在事务开始前生效；PostgreSQL 恰恰相反，
        // 事务块外发它只会得到警告且不生效，见 SqlDialect::isolationLevelStatementFollowsBegin()）。
        // 引擎给不出设置语句（SQLite 不支持按事务请求）时拒绝并点名，不静默降级；
        // 此处抛出安全：连接是成员，栈展开时它会照常归还池，不会留下半开状态
        const bool             isolationRequested  = isolationLevel != TransactionIsolationLevel::Default;
        const bool             isolationAfterBegin = isolationRequested && m_dialect->isolationLevelStatementFollowsBegin();
        const std::string_view isolationStatement  = isolationRequested ? m_dialect->isolationLevelStatement(isolationLevel) : std::string_view{};

        if (isolationRequested && isolationStatement.empty())
        {
            throw Base::InvalidArgumentException("数据库事务：引擎 " + std::string(databaseTypeName(m_connection->databaseType())) + " 不支持按事务请求隔离级别「" +
                                                 std::string(isolationLevelName(isolationLevel)) + "」；请用默认级别（Default），或换用支持 SET TRANSACTION 的引擎");
        }

        // BEGIN 前置的引擎：设置语句先发，失败时事务尚未开始，直接抛出即可
        if (isolationRequested && !isolationAfterBegin && !executeControlStatement(isolationStatement))
        {
            throw QueryExecutionException(m_lastError, m_connection->lastNativeErrorCode());
        }

        // BEGIN 失败（例如同一连接上已有未结束的事务）必须让构造失败：返回一个「看起来在事务里、
        // 实际没有事务」的对象，会让后续每一条语句都静默运行在自动提交模式下
        if (!executeControlStatement(*beginStatement))
        {
            // 带上连接的原生码：m_lastError 这段文本正是从这条连接抄来的（见
            // executeControlStatement），因此两者的配对关系成立
            throw QueryExecutionException(m_lastError, m_connection->lastNativeErrorCode());
        }

        // BEGIN 成功即已进入事务：先标记为活动，使随后任何失败路径（含下面 BEGIN 后置的隔离级别
        // 语句）在栈展开时都被析构回滚，而不是把开着的事务随连接还回池
        m_isActive = true;

        // BEGIN 后置的引擎（PostgreSQL）：必须在事务第一条语句之前发出，失败同样让构造抛出，
        // 由上面的标记保证回滚
        if (isolationRequested && isolationAfterBegin && !executeControlStatement(isolationStatement))
        {
            throw QueryExecutionException(m_lastError, m_connection->lastNativeErrorCode());
        }

        // 时限的计时起点：从「BEGIN 真的成功了」这一刻算，而不是从对象构造算——构造里那段取连接与
        // 发 BEGIN 本身可能等了一段时间（池被借干时的等待、驱动建连），那部分不该吃掉事务的额度
        m_beganAt = std::chrono::steady_clock::now();
    }

    Transaction::~Transaction()
    {
        // 未提交就回滚是唯一安全的默认动作：
        // - 已提交/已回滚的事务 m_isActive 为假，这里不会发出多余的 ROLLBACK，不影响已落库的数据；
        // - 异常穿过事务作用域时来不及调用 rollback()，由这里补上，避免半成品数据残留
        if (m_isActive)
        {
            // 析构不能抛异常：失败只记录在 lastError() 里（对象随即销毁），
            // rollback() 内部会断开连接，让池在归还时丢弃这条状态不明的连接
            static_cast<void>(rollback());
        }

        // 函数体结束后才轮到成员析构：m_connection 在此把连接归还池，
        // 因此顺序天然是「先回滚，再归还连接」
    }

    bool Transaction::commit()
    {
        // 幂等：事务已经结束（提交过或回滚过）时不再发送 COMMIT
        if (!m_isActive)
        {
            return true;
        }

        // 整笔时限已到期就不发 COMMIT：这行的意义是把「一笔跑飞了的事务还能落库」变成不可能。
        // 不发语句也不改 m_isActive——剩下的工作交给析构去 ROLLBACK，因此这条路是「拒提交」而不是「拒收尾」
        if (isTimeLimitExceeded())
        {
            m_lastError = timeLimitExceededReason();
            return false;
        }

        if (!executeControlStatement(m_dialect->commitStatement()))
        {
            // 提交失败时保持 m_isActive：事务是否仍在进行不可知，
            // 让析构阶段再尝试一次 ROLLBACK，比把失败静默成成功安全得多
            return false;
        }

        // 提交成功，事务结束；此后再调用 commit()/rollback() 都是无操作
        m_isActive = false;
        return true;
    }

    bool Transaction::rollback()
    {
        // 幂等：事务已经结束时直接返回成功
        if (!m_isActive)
        {
            return true;
        }

        if (!executeControlStatement(m_dialect->rollbackStatement()))
        {
            // 回滚失败意味着连接上可能还挂着一个未结束的事务。这样的连接绝不能还给池被别人复用，
            // 否则下一个使用者会莫名其妙地落在别人的事务里；主动断开后，池归还时的
            // isConnected() 检查会判定它不健康并丢弃它，宁可废掉一条连接也不留下脏会话
            m_connection->disconnect();
            return false;
        }

        m_isActive = false;
        return true;
    }

    DatabaseConnection &Transaction::connection() const
    {
        // 正常流程下构造成功即持有连接，此分支仅为防御：把空指针解引用换成可定位的中文异常
        if (!m_connection)
        {
            throw Base::LogicException("数据库事务：事务对象未持有连接，无法提供连接（可能构造失败后被继续使用）");
        }
        return *m_connection;
    }

    std::unique_lock<std::mutex> Transaction::acquireStatementLock() const
    {
        // 独占语义由 unique_lock 自己承担：拿到即独占这条连接，离开作用域即交还
        return std::unique_lock<std::mutex>(m_statementMutex);
    }

    bool Transaction::savepoint(const std::string_view name)
    {
        if (!checkSavepointPremises(name, "建立"))
        {
            return false;
        }
        return executeControlStatement(m_dialect->savepointStatement(name));
    }

    bool Transaction::rollbackToSavepoint(const std::string_view name)
    {
        if (!checkSavepointPremises(name, "回退到"))
        {
            return false;
        }
        // 这里不动 m_isActive：回退到保存点撤掉的是那一点之后的写入，事务本身仍然开着，
        // 之后 commit() 照常把那一点之前的工作落库——这正是它能替代「嵌套事务」的原因
        return executeControlStatement(m_dialect->rollbackToSavepointStatement(name));
    }

    bool Transaction::releaseSavepoint(const std::string_view name)
    {
        if (!checkSavepointPremises(name, "丢弃"))
        {
            return false;
        }
        return executeControlStatement(m_dialect->releaseSavepointStatement(name));
    }

    bool Transaction::checkSavepointPremises(const std::string_view name, const std::string_view action)
    {
        // 坏名字是调用方的输入问题，当场抛而不是回 false：回 false 会把注意力引向数据库，
        // 而那两条语句本身都没错。NUL 单独判是因为它会让串在驱动侧被截断成另一个名字
        if (name.empty() || name.find('\0') != std::string_view::npos)
        {
            throw Base::InvalidArgumentException("数据库事务：" + std::string(action) + "保存点需要一个非空且不含 NUL 的名字");
        }
        // 空白判定只认 ASCII 的那几个字符，不用 std::isspace：它跟着 locale 走，
        // 换一档 locale 就可能多认或少认一个字符，而「这个名字能不能发出去」不该随环境变
        const bool isAllWhitespace =
                std::ranges::all_of(name, [](const char character)
                                    { return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' || character == '\v'; });
        if (isAllWhitespace)
        {
            throw Base::InvalidArgumentException("数据库事务：" + std::string(action) + "保存点的名字不能全是空白字符（它会被原样引起来送进语句）");
        }

        // 事务已结束就一条都不发。这一步不是形式主义：SQLite 上没有活动事务时 SAVEPOINT 会另起一个
        // 事务（文档明写等价于 BEGIN DEFERRED），而这个对象的 m_isActive 已假、析构不再回滚也不再提交，
        // 那条连接就会带着一个没人负责的事务回到池里——把「保存点」变成一次意外的开事务
        if (!m_isActive)
        {
            m_lastError = "数据库事务：事务已结束（已提交或已回滚），无法" + std::string(action) + "保存点";
            return false;
        }
        // 整笔时限已到期：保存点也是往前走的一步（它要在同一笔事务里再发一条语句），所以与时限同判。
        // 判据放在名字检查之后：坏名字是使用错误，无论时限有没有到都该当场抛
        if (isTimeLimitExceeded())
        {
            m_lastError = timeLimitExceededReason();
            return false;
        }
        return true;
    }

    bool Transaction::isTimeLimitExceeded() const noexcept
    {
        // 没设限（0 或负数）与已结束的事务一律回 false：后者已经有「已结束」那条判据在前面挡着，
        // 把两条原因混成一句会让人去查错的分支
        if (!m_isActive || m_timeLimit <= std::chrono::milliseconds::zero())
        {
            return false;
        }
        return std::chrono::steady_clock::now() - m_beganAt >= m_timeLimit;
    }

    std::string Transaction::timeLimitExceededReason() const
    {
        const auto usedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_beganAt).count();
        return "数据库事务：这一整笔事务已越过 setTimeLimit 设的 " + std::to_string(m_timeLimit.count()) + " 毫秒时限（从 BEGIN 起实际用去 " + std::to_string(usedMilliseconds) +
               " 毫秒），该事务的后续语句与提交都不再被接受。请另起一笔，或把额度放宽到实际需要的时长；已经发出的语句靠"
               "透传下去的语句级超时被打断（掩盖不住的时刻以引擎的中断报错为准）";
    }

    void Transaction::applyRemainingTimeBudget(DatabaseConnection &connection) const noexcept
    {
        // 没有预算要守（不设限，或事务已结束）就别碰连接的会话级设置：不设限时误把「预算 0」
        // 写回去，正好会把调用方设过的更严超时静默放松成无限
        if (!m_isActive || m_timeLimit <= std::chrono::milliseconds::zero())
        {
            return;
        }

        const auto elapsed   = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_beganAt);
        const auto remaining = m_timeLimit - elapsed;
        // 夹到 [1, INT_MAX]：非正（取锁复查通过后的瞬时越限）按 1 毫秒下发——0 在 queryTimeout 的
        // 口径里是「不设限」，把「几乎没有预算」写回成 0 正好把这道界翻反；超出 int 的长预算夹到
        // 上限，差异只出现在以天计的额度上
        const auto budget             = std::clamp<std::int64_t>(remaining.count(), 1, std::numeric_limits<int>::max());
        const int  budgetMilliseconds = static_cast<int>(budget);

        const int currentTimeout = connection.queryTimeout();
        // 取更严者：调用方（或池基线）设过的更小值不放大；会话级 0＝不设限，直接让预算顶上
        const int effectiveTimeout = currentTimeout > 0 ? std::min(currentTimeout, budgetMilliseconds) : budgetMilliseconds;
        // 相等就不重发：改值要走驱动的一条 SET（PostgreSQL / MySQL 会话变量），白付一趟往返没有回报
        if (effectiveTimeout != currentTimeout)
        {
            connection.setQueryTimeout(effectiveTimeout);
        }
    }

    bool Transaction::executeControlStatement(const std::string_view statement)
    {
        // BEGIN / COMMIT / ROLLBACK 也是「走这条连接的语句」，因此与查询共用同一把使用权锁：
        // 不与并发中的业务语句同时踩同一个驱动句柄，也不出现「语句还在跑就被 COMMIT」
        const std::unique_lock<std::mutex> statementLock = acquireStatementLock();

        // 每次尝试都先清掉上一轮的失败文本：本函数代表一次新的尝试，
        // 历史错误不能冒充本次结果
        m_lastError.clear();

        // BEGIN / COMMIT / ROLLBACK 都是无参数语句，走驱动的普通执行接口即可；
        // 它们本身没有返回列，返回非空结果集就代表语句已被引擎接受
        if (const std::unique_ptr<DatabaseResult> result = m_connection->execute(statement); result == nullptr)
        {
            m_lastError = "数据库事务：" + std::string(statement) + " 执行失败：" + m_connection->lastError();
            return false;
        }

        return true;
    }

} // namespace AsynGyanis::Database
