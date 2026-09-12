/**
 * @file TestCoreException.cpp
 * @brief CoreException 单元测试：继承关系与调用方实际可用的捕获面
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 全部用例都不触碰网络或 TLS：只构造异常对象并断言继承关系与捕获结果。
 *          Core 的运行期故障统一派生自 CoreException（进而 Base::Exception），调用方能用一条
 *          `catch (const Base::Exception &)` 网住全部；它同时仍是标准库的 std::runtime_error；
 *          用法错误（Base::InvalidArgumentException）刻意不在本链上，不被「可恢复故障」的捕获面吞掉。
 */
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Exception/CoreException.h"

#include <gtest/gtest.h>

#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace AsynGyanis::Core
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
    // 继承关系
    // ============================================================================

    /**
     * @brief CoreException 同时站在两条继承链上：既能被框架基类 Base::Exception 捕获，也仍是标准库的 std::runtime_error
     */
    TEST(CoreExceptionFamily, DerivesFromProjectBaseAndStandardRuntimeError)
    {
        // 这两条断言就是本次改造的验收点：Core 的运行期故障现在能被框架基类捕获
        static_assert(std::is_base_of_v<Base::Exception, CoreException>);
        static_assert(std::is_base_of_v<std::runtime_error, CoreException>);
        static_assert(std::is_base_of_v<std::exception, CoreException>);
    }

    /**
     * @brief 用法错误（Base::InvalidArgumentException）刻意不在运行期故障链上
     * @details 若并入，「可恢复故障」的捕获面 catch (const Base::Exception &) 会把调用方的 bug 一起吞掉
     */
    TEST(CoreExceptionFamily, UsageErrorsStayOutsideTheRuntimeFailureChain)
    {
        // 参数非法这类用法错误走 std::invalid_argument 分支，刻意不并入运行期故障家族：
        // 否则 catch (const Base::Exception &) 会把调用方的 bug 当成可重试故障
        static_assert(!std::is_base_of_v<Base::Exception, Base::InvalidArgumentException>);
        static_assert(!std::is_base_of_v<CoreException, Base::InvalidArgumentException>);
    }

    // ============================================================================
    // 捕获面
    // ============================================================================

    /**
     * @brief 一个 catch (const Base::Exception &) 即可兜住 Core 的运行期故障，且异常文本原样保留
     */
    TEST(CoreExceptionFamily, OneCatchOfProjectBaseCoversCoreFailures)
    {
        const CoreException failure("创建 TLS 会话失败：SSL_new 返回空（上下文无效或内存不足）");

        try
        {
            throw failure;
        } catch (const Base::Exception &exception)
        {
            EXPECT_TRUE(contains(exception.what(), "创建 TLS 会话失败")) << exception.what();
        }
    }

    /**
     * @brief CoreException 可分别按模块类型、框架基类、std::exception 捕获：替换裸标准异常后上游捕获面不缩水
     */
    TEST(CoreExceptionFamily, CatchableAsModuleTypeAndStandardBase)
    {
        const CoreException failure("TLS 握手失败：对端证书不受信");

        EXPECT_THROW(throw failure, CoreException);
        EXPECT_THROW(throw failure, Base::Exception);
        EXPECT_THROW(throw failure, std::exception);
    }

    // ============================================================================
    // 消息与位置
    // ============================================================================

    /**
     * @brief 异常消息不加领域前缀、逐字保留调用点文本，并如实记录抛出位置（文件与行号）
     */
    TEST(CoreExceptionFamily, MessageKeepsOriginalTextAndCarriesThrowSite)
    {
        // 消息不额外加领域前缀（调用点的文本已自带「创建 TLS 会话失败：」这类标签），
        // 位置捕获与文本格式与 Base::Exception 一致——两者共用同一份格式化实现
        const std::source_location throwSite = std::source_location::current();
        const CoreException        failure("TLS 写入失败：连接已被对端关闭", throwSite);

        const std::string message = failure.what();
        EXPECT_TRUE(contains(message, "[异常]")) << message;
        EXPECT_TRUE(contains(message, "TLS 写入失败：连接已被对端关闭")) << message;
        EXPECT_EQ(failure.location().line(), throwSite.line());
        EXPECT_STREQ(failure.location().file_name(), throwSite.file_name());
    }

} // namespace AsynGyanis::Core
