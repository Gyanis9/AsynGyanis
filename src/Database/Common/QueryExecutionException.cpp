#include "Database/Common/QueryExecutionException.h"

namespace AsynGyanis::Database
{
    QueryExecutionException::QueryExecutionException(const std::string &message, const std::source_location &sourceLocation) : DatabaseException(message, sourceLocation)
    {
    }

    QueryExecutionException::QueryExecutionException(const std::string &message, const std::int64_t nativeErrorCode, const std::source_location &sourceLocation) :
        DatabaseException(message, sourceLocation), m_nativeErrorCode(nativeErrorCode)
    {
    }

    std::int64_t QueryExecutionException::nativeErrorCode() const noexcept
    {
        return m_nativeErrorCode;
    }

    bool QueryExecutionException::isRetryable() const noexcept
    {
        // 取值一律按「服务端/驱动明确说这条语句可以重来」的那几条列，其余走默认的 false：
        // 判错的代价不对称——少重试一次只是一趟往返，多重试一条注定失败的语句会把
        // 一个写操作重放到可能已经生效的状态上。
        switch (m_nativeErrorCode)
        {
            // MySQL：ER_LOCK_WAIT_TIMEOUT(1205)、ER_LOCK_DEADLOCK(1213)。死锁时 InnoDB 已经
            // 主动回滚本事务并期望客户端重放，这条是明文的可重试
            case 1205:
            case 1213:
                return true;

            // SQLite：SQLITE_BUSY(5)、SQLITE_LOCKED(6)——都是「别人正占着写锁，稍后再来就行」。
            // 这里按主码判：启用扩展结果码时调用方拿到的是 5/6 加扩展位，那种取值不在表里，
            // 因此只会退化成「不重试」，方向是安全的
            case 5:
            case 6:
                return true;

            default:
                return false;
        }
    }

} // namespace AsynGyanis::Database
