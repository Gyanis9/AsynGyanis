/**
 * @file LogicException.h
 * @brief 编程/用法错误异常 —— 调用方以无效方式使用接口
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/ExceptionPayload.h"

#include <source_location>
#include <stdexcept>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 编程/用法错误异常
     *
     * @details 用于「调用方把接口用错了」这一类失败，重试与降级都无济于事，只能改代码或改配置。
     *          **刻意不派生自 Base::Exception**：那会归入 std::runtime_error，分开才能让
     *          `catch (const Base::Exception &)` 不把调用方的 bug 一并吞掉；要一次网住「所有用法错误」
     *          需捕获两条链的共同基类 std::logic_error。
     *
     * @note 抛出点快照与调用栈继承自 Detail::ExceptionPayload，与 Exception 是同一份实现，
     *       因此 what() 的文本格式与 Exception 完全一致。
     */
    class ASYN_BASE_API LogicException : public std::logic_error, public Detail::ExceptionPayload
    {
    public:
        /**
         * @brief 构造编程/用法错误异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit LogicException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        // location() 与 stackTrace() 继承自 Detail::ExceptionPayload：三条链不再各抄一份
    };
} // namespace AsynGyanis::Base
