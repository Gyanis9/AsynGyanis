/**
 * @file ExceptionStackTrace.h
 * @brief 从异常对象取回它携带的调用栈 —— 供「只知道 std::exception」的场合使用
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 三条异常链（派生自 std::runtime_error 的 Exception、派生自 std::logic_error 的
 *          LogicException、派生自 std::invalid_argument 的 InvalidArgumentException）无法共享
 *          基类，而调用方常常只 catch 了 std::exception；这里用 dynamic_cast 逐个识别。
 *          只应在错误路径上调用——正常路径不该为它付出类型检查的开销。
 */

#pragma once

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Base/Exception/StackTrace.h"

#include <exception>

namespace AsynGyanis::Base
{
    /**
     * @brief 取异常携带的抛出点调用栈
     * @param exception 待检查的异常对象
     * @return const CapturedStackTrace* 异常携带的栈；不属于框架异常家族时返回 nullptr
     */
    [[nodiscard]] inline const CapturedStackTrace *tryStackTrace(const std::exception &exception) noexcept
    {
        // 三条链互不派生，只能逐个试；顺序按运行期故障在前、用法错误在后的使用频率排
        if (const auto *frameworkException = dynamic_cast<const Exception *>(&exception); frameworkException != nullptr)
        {
            return &frameworkException->stackTrace();
        }
        if (const auto *logicException = dynamic_cast<const LogicException *>(&exception); logicException != nullptr)
        {
            return &logicException->stackTrace();
        }
        if (const auto *argumentException = dynamic_cast<const InvalidArgumentException *>(&exception); argumentException != nullptr)
        {
            return &argumentException->stackTrace();
        }
        return nullptr;
    }
} // namespace AsynGyanis::Base
