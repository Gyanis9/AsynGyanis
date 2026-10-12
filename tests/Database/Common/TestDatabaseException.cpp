// 数据库异常体系测试：家族继承关系与调用方实际可用的捕获面。
// 覆盖场景：
// - EveryRuntimeFailureDerivesFromProjectExceptionBase / UsageErrorsStayOutsideTheRuntimeFailureFamily
// - OneCatchOfProjectBaseCoversEverySubclass / EachSubclassIsCatchableOnItsOwn / RootAndStandardBaseAlsoCatchSubclasses
// - MessageKeepsOriginalTextAndCarriesThrowSite
// 钉住的契约：运行期故障家族（DatabaseException 及其全部子类）都派生自 Base::Exception，调用方能用一个 catch
// 网住整个框架的可恢复故障，且都能被根类型与标准库 std::runtime_error 捕获；用法错误（Base::LogicException /
// Base::InvalidArgumentException）刻意不在本家族内，该边界由本文件与 tests/Base/Exception/TestException.cpp 共同钉住。
// 子类之间也不互相捕获——「整笔事务超时」不是「语句执行失败」，重试逻辑按后者分支时不能被前者误命中。
// 全部用例都不触碰数据库：只构造异常对象并断言继承关系与捕获结果。
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseException.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Common/RowMappingException.h"
#include "Database/Common/TransactionTimeoutException.h"

#include <gtest/gtest.h>

#include <array>
#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace AsynGyanis::Database
{
    namespace
    {
        /**
         * @brief 断言文本中包含给定子串
         * @param haystack 待检查文本
         * @param needle 期望出现的子串
         * @return true 出现
         */
        bool contains(const std::string &haystack, const std::string &needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    // ============================================================================
    // 家族继承关系
    // ============================================================================

    /** @brief 钉住运行期故障家族的继承面：家族里的每个类型都能被 Base::Exception 一条 catch 网住，且不脱离 std::runtime_error 分类 */
    TEST(DatabaseExceptionFamily, EveryRuntimeFailureDerivesFromProjectExceptionBase)
    {
        // 断言这些都派生自框架异常基类：调用方才能用一条 catch 兜住框架的运行期故障
        static_assert(std::is_base_of_v<Base::Exception, DatabaseException>);
        static_assert(std::is_base_of_v<Base::Exception, ConnectionUnavailableException>);
        static_assert(std::is_base_of_v<Base::Exception, QueryExecutionException>);
        static_assert(std::is_base_of_v<Base::Exception, RowMappingException>);
        static_assert(std::is_base_of_v<Base::Exception, TransactionTimeoutException>);

        // 同时仍是标准库的 runtime_error：按标准分类的上游处理器不受影响
        static_assert(std::is_base_of_v<std::runtime_error, DatabaseException>);

        // 子类经由根类间接派生，不是各自独立挂在 Base::Exception 下
        static_assert(std::is_base_of_v<DatabaseException, ConnectionUnavailableException>);
        static_assert(std::is_base_of_v<DatabaseException, QueryExecutionException>);
        static_assert(std::is_base_of_v<DatabaseException, RowMappingException>);
        static_assert(std::is_base_of_v<DatabaseException, TransactionTimeoutException>);
    }

    /** @brief 钉住用法错误与运行期故障家族互不相交：参数/状态错误不得被「可恢复故障」的捕获面吞掉 */
    TEST(DatabaseExceptionFamily, UsageErrorsStayOutsideTheRuntimeFailureFamily)
    {
        // 用法错误（参数非法、对象状态不允许）刻意留在 std::logic_error 分支：
        // 它们是调用方的 bug，不该被「可恢复故障」的捕获面吞掉并据此重试
        static_assert(!std::is_base_of_v<Base::Exception, Base::LogicException>);
        static_assert(!std::is_base_of_v<Base::Exception, Base::InvalidArgumentException>);
        static_assert(!std::is_base_of_v<DatabaseException, Base::LogicException>);
        static_assert(!std::is_base_of_v<DatabaseException, Base::InvalidArgumentException>);
    }

    // ============================================================================
    // 捕获面
    // ============================================================================

    /** @brief 钉住一条 catch 兜住全家族的捕获面：按值抛出后仍能取回具体子类的消息，不因切片丢动态类型 */
    TEST(DatabaseExceptionFamily, OneCatchOfProjectBaseCoversEverySubclass)
    {
        // 调用方只需要这一个 catch 分支就能兜住本模块的全部运行期故障，不必逐个枚举子类——
        // 这正是引入家族根类型的意义。参数按值接收（auto），throw 时静态类型即具体子类，
        // 不会切成基类；若形参写成 const DatabaseException& 则会被切片，动态类型丢失
        const auto catchThroughProjectBase = [](auto failure) -> std::string
        {
            try
            {
                throw failure;
            } catch (const Base::Exception &exception)
            {
                return exception.what();
            }
        };

        EXPECT_TRUE(contains(catchThroughProjectBase(ConnectionUnavailableException("池已达上限")), "池已达上限"));
        EXPECT_TRUE(contains(catchThroughProjectBase(QueryExecutionException("表不存在")), "表不存在"));
        EXPECT_TRUE(contains(catchThroughProjectBase(RowMappingException("列类型不符")), "列类型不符"));
        EXPECT_TRUE(contains(catchThroughProjectBase(TransactionTimeoutException("整笔事务超时")), "整笔事务超时"));
    }

    /** @brief 钉住各类故障各自可精确捕获，且超时不落在「语句执行失败」的捕获面里——不同处置方式的前提 */
    TEST(DatabaseExceptionFamily, EachSubclassIsCatchableOnItsOwn)
    {
        // 各类失败的处置方式不同（重试 / 记日志失败 / 对齐结构体 / 另起一笔），因此必须能各自精确捕获
        EXPECT_THROW(throw ConnectionUnavailableException("池已达上限"), ConnectionUnavailableException);
        EXPECT_THROW(throw QueryExecutionException("语句被拒"), QueryExecutionException);
        EXPECT_THROW(throw RowMappingException("列类型不符"), RowMappingException);
        EXPECT_THROW(throw TransactionTimeoutException("整笔事务超时"), TransactionTimeoutException);

        // 子类之间也不互相捕获。这条边界是给重试逻辑划的：那些代码按 QueryExecutionException 分支、
        // 再查 isRetryable() 决定重来一次；若超时异常也是它的子类，一笔注定被拒的事务就会被拿去
        // 重试同一条语句，把连接与行锁一直占着
        static_assert(!std::is_base_of_v<QueryExecutionException, TransactionTimeoutException>);
        const bool timeoutCaughtAsStatementFailure = []
        {
            try
            {
                throw TransactionTimeoutException("整笔事务超时");
            } catch (const QueryExecutionException &)
            {
                // 被语句失败的捕获面接走了，正是这条断言要拒绝的形状
                return true;
            } catch (const Base::Exception &)
            {
                return false;
            }
        }();
        EXPECT_FALSE(timeoutCaughtAsStatementFailure) << "整笔事务超时不该落进「语句执行失败」的捕获面——重试逻辑会据此重试一条注定被拒的语句";
    }

    /** @brief 钉住放宽捕获面不漏失败：子类同样能被根类型与 std::exception 捕获 */
    TEST(DatabaseExceptionFamily, RootAndStandardBaseAlsoCatchSubclasses)
    {
        // 捕获根类型或 std::exception 同样能命中子类：放宽捕获面不会漏掉任何一类失败
        EXPECT_THROW(throw RowMappingException("列类型不符"), DatabaseException);
        EXPECT_THROW(throw QueryExecutionException("语句被拒"), DatabaseException);
        EXPECT_THROW(throw TransactionTimeoutException("整笔事务超时"), DatabaseException);
        EXPECT_THROW(throw ConnectionUnavailableException("池已达上限"), std::exception);
    }

    // ============================================================================
    // 消息与位置
    // ============================================================================

    /** @brief 钉住消息保留原文、不加领域前缀，且抛出点与文本格式与 Base::Exception 完全一致 */
    TEST(DatabaseExceptionFamily, MessageKeepsOriginalTextAndCarriesThrowSite)
    {
        // 消息不额外加领域前缀（调用点的文本本身已带上下文标签），
        // 但位置捕获与文本格式与 Base::Exception 完全一致——两者共用同一份格式化实现
        const std::source_location    throwSite = std::source_location::current();
        const QueryExecutionException exception("语句执行失败：no such table", throwSite);

        const std::string message = exception.what();
        EXPECT_TRUE(contains(message, "[异常]")) << message;
        EXPECT_TRUE(contains(message, "语句执行失败：no such table")) << message;
        EXPECT_EQ(exception.location().line(), throwSite.line());
        EXPECT_STREQ(exception.location().file_name(), throwSite.file_name());
    }

    /**
     * @brief 原生码进出一致；不带码的旧签名退化为「未知」
     * @details 旧签名必须继续可用且把码标成未知——那 6 处既有抛出点与它们的断言都不该被改动。
     */
    TEST(QueryExecutionNativeCode, NativeCodeRoundTripsAndDefaultsToUnknown)
    {
        const QueryExecutionException withCode("语句被拒", 1213);
        EXPECT_EQ(withCode.nativeErrorCode(), 1213);

        const QueryExecutionException withoutCode("语句被拒");
        EXPECT_EQ(withoutCode.nativeErrorCode(), QueryExecutionException::kUnknownNativeErrorCode) << "旧签名的码必须显式标为未知，不能留 0——0 在两个驱动里都是「成功」的意思";
    }

    /**
     * @brief 可重试判定表：只收「服务端明确让这条语句重来」的冲突类
     * @details 判错的代价不对称：少重试一次只是一趟往返，多重试一条注定的失败写语句则是二次写入，
     *          因此默认答案是 false，表里只列白名单。
     */
    TEST(QueryExecutionNativeCode, RetryabilityWhitelist)
    {
        // MySQL ER_LOCK_WAIT_TIMEOUT(1205) / ER_LOCK_DEADLOCK(1213)；SQLite BUSY(5) / LOCKED(6)；
        // PostgreSQL 40P01（deadlock_detected，服务端已回滚本事务、与 MySQL 1213 同语义）与
        // 40001（serialization_failure）——后两者的码是 SQLSTATE 文本的打包值，见
        // postgresSqlStateNativeCode；两处必须同解，调用方对表比较才成立。
        // 表用 std::array 显式定长：花括号列表混着 LL 字面量与 int64_t 时 GCC 会
        // 因「long long 与 long 推导冲突」拒编（MSVC 的 int64_t 恰是 long long，两处不同形）
        const std::array<std::int64_t, 6> retryableCodes{1205, 1213, 5, 6, postgresSqlStateNativeCode("40P01"), postgresSqlStateNativeCode("40001")};
        for (const std::int64_t retryableCode: retryableCodes)
        {
            const QueryExecutionException exception("语句被拒", retryableCode);
            EXPECT_TRUE(exception.isRetryable()) << "码 " << retryableCode << " 应判可重试";
        }

        // 约束冲突（MySQL 1062 / SQLite 19 / PostgreSQL 23505）、语法与对象不存在（SQLite 1）、
        // 权限不足（MySQL 1142）、连接类（2002/2003/2005）、超时打断（MySQL 3024 / SQLite 9 INTERRUPT）、
        // 连接中断（2006/2013，语句可能已在服务端生效，跨连接重放不是本函数能替调用方拍的）
        const std::array<std::int64_t, 12> nonRetryableCodes{1062, 19, 1, 1142, 2002, 2003, 2005, 3024, 9, 2006, 2013, postgresSqlStateNativeCode("23505")};
        for (const std::int64_t nonRetryableCode: nonRetryableCodes)
        {
            const QueryExecutionException exception("语句被拒", nonRetryableCode);
            EXPECT_FALSE(exception.isRetryable()) << "码 " << nonRetryableCode << " 不该判可重试";
        }

        EXPECT_FALSE(QueryExecutionException("语句被拒").isRetryable()) << "码未知时必须判不可重试";
    }

    /**
     * @brief PostgreSQL 的 SQLSTATE 文本按「各字符一字节、高位在前」打包成原生码
     * @details libpq 没有数字形式的原生码，SQLSTATE 字符串就是它的原生码；打包规则与
     *          isRetryable() 的白名单必须同解——两处各写一份公式时，按码分支的调用方
     *          就会对着一个永远不成立的常量比较
     */
    TEST(QueryExecutionNativeCode, PostgresSqlStatePacksIntoNativeCode)
    {
        // "23505" 的五个 ASCII 字节 0x32 0x33 0x35 0x30 0x35 依次入位
        EXPECT_EQ(postgresSqlStateNativeCode("23505"), 0x3233353035LL);
        EXPECT_EQ(postgresSqlStateNativeCode("40P01"), 0x3430503031LL);

        // 规则不依赖长度：短串与空串同样可打包（调用方不该依赖长度行为，这里只是钉住规则本身）
        EXPECT_EQ(postgresSqlStateNativeCode("40"), 0x3430LL);
        EXPECT_EQ(postgresSqlStateNativeCode(""), 0LL);
    }

    /**
     * @brief 码不改变消息文本：既有断言的比对对象仍是原文
     */
    TEST(QueryExecutionNativeCode, NativeCodeDoesNotAlterMessageText)
    {
        const QueryExecutionException withCode("Queryable: 语句执行失败：死锁", 1213);
        EXPECT_NE(std::string(withCode.what()).find("Queryable: 语句执行失败：死锁"), std::string::npos) << "带上原生码不得改写消息文本——那是三条链共用的对外契约";
    }

} // namespace AsynGyanis::Database
