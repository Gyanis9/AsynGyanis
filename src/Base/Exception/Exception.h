/**
 * @file Exception.h
 * @brief 项目统一异常基类，记录异常消息与抛出位置
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/ExceptionPayload.h"

#include <source_location>
#include <stdexcept>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 项目统一异常基类
     *
     * @details 继承 std::runtime_error，把消息包成「[异常] 消息 [文件:行 in 函数]」——抛出点
     *          （文件、行号、函数名）附在消息**之后**而不是之前，见 Detail::formatExceptionMessage。
     *          抛出点快照与调用栈由 Detail::ExceptionPayload 提供，与 LogicException、
     *          InvalidArgumentException 共享同一份实现（三条链分属标准库的两条分支，共享不了
     *          异常基类，但都继承这一个非异常基类）。运行期故障都归入本类家族，
     *          使上层能用一条 catch 兜住框架错误。
     * @note 消息格式化在构造期完成，抛出后不依赖任何外部状态。
     */
    class Exception : public std::runtime_error, public Detail::ExceptionPayload
    {
    public:
        /**
         * @brief 构造统一异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit Exception(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        // location() 与 stackTrace() 继承自 Detail::ExceptionPayload：三条链不再各抄一份
    };
} // namespace AsynGyanis::Base
