/**
 * @file AssertionSupport.h
 * @brief 错误路径用例的共用断言：按类型捕获，把原因正文交回调用方继续核对
 * @author Gyanis
 * @date 2026-10-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 只断言「抛了」的用例改掉文案、漏掉资源回收都照样绿。这里把「类型对不对」
 *          与「原因说了什么」拆成两步：本函数负责前者并交回正文，调用方再按模块
 *          断言正文里必须出现的键名、取值或修复动作。
 */

#pragma once

#include <gtest/gtest.h>

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::TestSupport
{
    /**
     * @brief 断言动作按指定类型抛出，并交回原因正文
     * @tparam ExceptionType 期望的异常类型
     * @tparam Action 无参可调用对象
     * @param action 被测动作
     * @param context 失败时附在断言后面的上下文（哪个取值、哪一步）
     * @return std::string 异常正文；没有抛出或类型不符时为空（已记失败）
     */
    template<typename ExceptionType, typename Action>
    std::string requireFailureText(Action &&action, const std::string &context)
    {
        try
        {
            action();
        } catch (const ExceptionType &failure)
        {
            return failure.what();
        } catch (const std::exception &unexpected)
        {
            ADD_FAILURE() << context << "：抛出的异常类型不符：" << unexpected.what();
            return {};
        }
        ADD_FAILURE() << context << "：本该抛出异常，却正常返回了";
        return {};
    }

    /**
     * @brief 断言动作按指定类型拒绝，并核对原因里出现给定片段
     * @tparam ExceptionType 期望的异常类型
     * @tparam Action 无参可调用对象
     * @param action 被测动作
     * @param expected 原因里必须出现的片段（键名、取值或规则）
     * @param context 失败时附在断言后面的上下文
     * @return std::string 异常正文，便于继续断言别的片段
     */
    template<typename ExceptionType, typename Action>
    std::string expectFailure(Action &&action, const std::string_view expected, const std::string &context)
    {
        const std::string reason = requireFailureText<ExceptionType>(std::forward<Action>(action), context);
        EXPECT_NE(reason.find(expected), std::string::npos) << "原因没有点明拒绝的是哪一项（缺少「" << expected << "」）：" << reason;
        return reason;
    }

    /**
     * @brief 断言异常可按指定基类捕获，且捕获到的正文与对象自身逐字一致
     * @tparam CatchType 捕获时写的类型
     * @tparam ExceptionType 异常的具体类型（按它抛出，避免按基类抛出时切片）
     * @param exception 被抛出的异常对象
     * @details 只判「能按基类捕获」看不出捕获后正文有没有被包一层或截断；分类前缀与抛出点
     *          正是下游日志依赖的部分，两条一起判
     */
    template<typename CatchType, typename ExceptionType>
    void expectCatchableWithSameText(const ExceptionType &exception)
    {
        try
        {
            throw exception;
        } catch (const CatchType &caught)
        {
            EXPECT_STREQ(caught.what(), exception.what()) << "按基类捕获后正文变了";
            return;
        } catch (...)
        {
            ADD_FAILURE() << "异常没有落在期望的基类上";
        }
    }
} // namespace AsynGyanis::TestSupport
