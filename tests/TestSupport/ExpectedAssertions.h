/**
 * @file ExpectedAssertions.h
 * @brief 结果断言宏：判「成不成」的同时，把失败载荷的类别与中文文案一并写进诊断
 * @author Gyanis
 * @date 2026-10-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 光板一句 ASSERT_TRUE(result.has_value()) 红的时候只报 expected true == false，定位还得重跑一遍。
 *          GoogleTest 的 `<<` 尾链只在失败分支求值，因此带值成功时不会去碰并不存在的 `error()`。
 * @note 实参必须是不带副作用的表达式：宏体里它出现两次（一次判 `has_value()`，一次取失败说明）。
 */

#pragma once

#include <gtest/gtest.h>

#include <string>
#include <system_error>
#include <type_traits>

namespace AsynGyanis::TestSupport
{
    /**
     * @brief 把一份失败载荷摊成一句人可读的文本
     * @param error 失败载荷：框架错误结构、裸 std::string、std::error_code 或只有类别的枚举
     * @return std::string 有文案的给「类别 + 文案」，只有类别的给类别取值，都没有的明说到哪一格没文案
     * @details 类别按底层整数报：本仓的 kind 枚举取值追加在末尾且不重排，号对得上头文件就能定位。
     */
    template<typename Error>
    [[nodiscard]] std::string describeFailurePayload(const Error &error)
    {
        if constexpr (std::is_same_v<std::decay_t<Error>, std::string>)
        {
            return error;
        } else if constexpr (std::is_same_v<std::decay_t<Error>, std::error_code>)
        {
            return "error_code=" + std::to_string(error.value()) + "：" + error.message();
        } else if constexpr (requires { error.kind; } && requires { error.message; })
        {
            return "kind=" + std::to_string(static_cast<long long>(error.kind)) + "：" + static_cast<std::string>(error.message);
        } else if constexpr (requires { error.kind; })
        {
            return "kind=" + std::to_string(static_cast<long long>(error.kind)) + "（该载荷不带文案）";
        } else if constexpr (std::is_enum_v<std::decay_t<Error>>)
        {
            return "类别=" + std::to_string(static_cast<long long>(error)) + "（该载荷不带文案）";
        } else
        {
            return "（该载荷类型既无类别也无文案）";
        }
    }

    /**
     * @brief 取一个结果对象的失败说明
     * @param result 结果对象：`std::expected` 取它的 `error()`，`std::optional` 这类没有失败载荷的另说
     * @return std::string 失败载荷的摊平文本
     */
    template<typename Result>
    [[nodiscard]] std::string describeRejectedResult(const Result &result)
    {
        if constexpr (requires { result.error(); })
        {
            return describeFailurePayload(result.error());
        } else
        {
            return "（该结果类型不带失败载荷）";
        }
    }
} // namespace AsynGyanis::TestSupport

/**
 * @brief 断言结果落在成功分支；红的时候把失败载荷一并报出
 * @note 与 ASSERT_TRUE(x.has_value()) 同为致命断言，失败即结束本用例
 */
#define ASSERT_HAS_VALUE(result) ASSERT_TRUE((result).has_value()) << #result << " 落进了失败分支：" << ::AsynGyanis::TestSupport::describeRejectedResult(result)

/**
 * @brief 非致命版的成功分支断言，用于一条报文里多处判成不成
 */
#define EXPECT_HAS_VALUE(result) EXPECT_TRUE((result).has_value()) << #result << " 落进了失败分支：" << ::AsynGyanis::TestSupport::describeRejectedResult(result)
