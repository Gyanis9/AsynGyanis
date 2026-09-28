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
 *          基类，而调用方常常只 catch 了 std::exception；本头文件用一次 dynamic_cast 认出来。
 *          只应在错误路径上调用——正常路径不该为它付出类型检查的开销。
 * @note 上面那三个异常头的 include 是**对外兼容面**而不是本文件自身需要的：大量现有翻译单元
 *       经 Logger.h 间接拿到这三个类型，抽掉它们会把几十处 catch 打成编译错误。
 *       要按「谁用谁 include」收口，应当单独立一笔逐个补直接包含，而不是混在本轮改造里顺手做。
 */

#pragma once

#include "Base/Exception/Exception.h"
#include "Base/Exception/ExceptionPayload.h"
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
     *
     * @details 三条链的载荷都来自 Detail::ExceptionPayload，因此一次 dynamic_cast 就够。
     *          原先要按链串行试三次，而那意味着新增第四条链时必须记得回来加一次——
     *          漏加的表征不是编译失败也不是报错，是「日志照常输出、只是不带栈」。
     */
    [[nodiscard]] inline const CapturedStackTrace *tryStackTrace(const std::exception &exception) noexcept
    {
        const auto *const payload = dynamic_cast<const Detail::ExceptionPayload *>(&exception);
        return payload == nullptr ? nullptr : &payload->stackTrace();
    }
} // namespace AsynGyanis::Base
