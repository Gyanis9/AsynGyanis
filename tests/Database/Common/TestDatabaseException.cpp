// 数据库异常体系测试：家族继承关系与调用方实际可用的捕获面。
// 覆盖场景：
// - EveryRuntimeFailureDerivesFromProjectExceptionBase / UsageErrorsStayOutsideTheRuntimeFailureFamily
// - OneCatchOfProjectBaseCoversEverySubclass / EachSubclassIsCatchableOnItsOwn / RootAndStandardBaseAlsoCatchSubclasses
// - MessageKeepsOriginalTextAndCarriesThrowSite
// 钉住的契约：运行期故障家族（DatabaseException 及其三个子类）全部派生自 Base::Exception，调用方能用一个 catch 网住
// 整个框架的可恢复故障，且都能被根类型与标准库 std::runtime_error 捕获；用法错误（Base::LogicException /
// Base::InvalidArgumentException）刻意不在本家族内，该边界由本文件与 tests/Base/Exception/TestException.cpp 共同钉住。
// 全部用例都不触碰数据库：只构造异常对象并断言继承关系与捕获结果。
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseException.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Common/RowMappingException.h"

#include <gtest/gtest.h>

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

    /** @brief 钉住运行期故障家族的继承面：四类故障都能被 Base::Exception 一条 catch 网住，且不脱离 std::runtime_error 分类 */
    TEST(DatabaseExceptionFamily, EveryRuntimeFailureDerivesFromProjectExceptionBase)
    {
        // 断言这四条都派生自框架异常基类：调用方才能用一条 catch 兜住框架的运行期故障
        static_assert(std::is_base_of_v<Base::Exception, DatabaseException>);
        static_assert(std::is_base_of_v<Base::Exception, ConnectionUnavailableException>);
        static_assert(std::is_base_of_v<Base::Exception, QueryExecutionException>);
        static_assert(std::is_base_of_v<Base::Exception, RowMappingException>);

        // 同时仍是标准库的 runtime_error：按标准分类的上游处理器不受影响
        static_assert(std::is_base_of_v<std::runtime_error, DatabaseException>);

        // 子类经由根类间接派生，不是各自独立挂在 Base::Exception 下
        static_assert(std::is_base_of_v<DatabaseException, ConnectionUnavailableException>);
        static_assert(std::is_base_of_v<DatabaseException, QueryExecutionException>);
        static_assert(std::is_base_of_v<DatabaseException, RowMappingException>);
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
    }

    /** @brief 钉住三类故障各自可精确捕获——重试、记日志、对齐结构体等不同处置方式的前提 */
    TEST(DatabaseExceptionFamily, EachSubclassIsCatchableOnItsOwn)
    {
        // 三类失败的处置方式不同（重试 / 记日志失败 / 对齐结构体），因此必须能各自精确捕获
        EXPECT_THROW(throw ConnectionUnavailableException("池已达上限"), ConnectionUnavailableException);
        EXPECT_THROW(throw QueryExecutionException("语句被拒"), QueryExecutionException);
        EXPECT_THROW(throw RowMappingException("列类型不符"), RowMappingException);
    }

    /** @brief 钉住放宽捕获面不漏失败：子类同样能被根类型与 std::exception 捕获 */
    TEST(DatabaseExceptionFamily, RootAndStandardBaseAlsoCatchSubclasses)
    {
        // 捕获根类型或 std::exception 同样能命中子类：放宽捕获面不会漏掉任何一类失败
        EXPECT_THROW(throw RowMappingException("列类型不符"), DatabaseException);
        EXPECT_THROW(throw QueryExecutionException("语句被拒"), DatabaseException);
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

} // namespace AsynGyanis::Database
