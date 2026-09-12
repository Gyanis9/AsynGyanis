#include "Database/Pool/Transaction.h"

#include "Base/Exception/LogicException.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Dialect/DialectRegistry.h"

#include <string>

namespace AsynGyanis::Database
{
    Transaction::Transaction(ConnectionPool &pool)
    {
        // 构造的第一步就锁定连接：事务期间的每一条语句都必须落在同一条连接上，
        // 若拖到第一条语句再去池里取，BEGIN 与后续语句就可能分属不同连接（见文件头说明）
        m_connection = pool.acquire();
        if (!m_connection)
        {
            throw ConnectionUnavailableException("数据库事务：无法从连接池获取连接（池已达上限且等待超时，"
                                                 "或连接工厂创建失败），事务未开启");
        }

        // 事务控制语句的文本由方言提供：SQLite 用 BEGIN IMMEDIATE、MySQL 用 START TRANSACTION，
        // 写死在事务对象里等于把方言知识放到了错误的层。方言在此解析一次并缓存，
        // 未实现的类型（MySQL / Redis）会在这里抛出带中文提示的 Base::InvalidArgumentException
        m_dialect = DialectRegistry::dialectFor(m_connection->databaseType());

        // BEGIN 失败（例如同一连接上已有未结束的事务）必须让构造失败：返回一个「看起来在事务里、
        // 实际没有事务」的对象，会让后续每一条语句都静默运行在自动提交模式下
        if (!executeControlStatement(m_dialect->beginTransactionStatement()))
        {
            throw QueryExecutionException(m_lastError);
        }

        // 走到这里数据库确实进入了事务，标记为活动；析构时会据此决定是否回滚
        m_isActive = true;
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
            // isConnected() 探活会判定它不健康并丢弃它，宁可废掉一条连接也不留下脏会话
            m_connection->disconnect();
            return false;
        }

        m_isActive = false;
        return true;
    }

    DatabaseConnection &Transaction::connection()
    {
        // 正常流程下构造成功即持有连接，此分支仅为防御：把空指针解引用换成可定位的中文异常
        if (!m_connection)
        {
            throw Base::LogicException("数据库事务：事务对象未持有连接，无法提供连接（可能构造失败后被继续使用）");
        }
        return *m_connection;
    }

    bool Transaction::executeControlStatement(const std::string_view statement)
    {
        // 每次尝试都先清掉上一轮的失败文本：本函数代表一次新的尝试，
        // 历史错误不能冒充本次结果
        m_lastError.clear();

        // BEGIN / COMMIT / ROLLBACK 都是无参数语句，走驱动的普通执行接口即可；
        // 它们本身没有返回列，返回非空结果集就代表语句已被引擎接受
        const std::unique_ptr<DatabaseResult> result = m_connection->execute(statement);
        if (result == nullptr)
        {
            m_lastError = "数据库事务：" + std::string(statement) + " 执行失败：" + m_connection->lastError();
            return false;
        }

        return true;
    }

} // namespace AsynGyanis::Database
