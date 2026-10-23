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
} // namespace AsynGyanis::TestSupport
