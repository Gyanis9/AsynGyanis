/**
 * @file Exception.h
 * @brief 项目统一异常基类，记录异常消息与抛出位置
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/StackTrace.h"

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
     *          同时捕获抛出点调用栈（原始帧，解析时机见 StackTrace.h）。运行期故障都归入本类家族，
     *          使上层能用一条 catch 兜住框架错误。
     * @note 消息格式化在构造期完成，抛出后不依赖任何外部状态。
     */
    class Exception : public std::runtime_error
    {
    public:
        /**
         * @brief 构造统一异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit Exception(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取异常抛出位置
         * @return const std::source_location& 构造时捕获的源位置快照
         */
        [[nodiscard]] const std::source_location &location() const noexcept;

        /**
         * @brief 获取抛出点的调用栈（原始帧，未解析符号）
         * @return const CapturedStackTrace& 构造时捕获的调用栈；降级平台恒为空
         */
        [[nodiscard]] const CapturedStackTrace &stackTrace() const noexcept;

    private:
        std::source_location m_location;   ///< 异常抛出时的源码位置快照
        CapturedStackTrace   m_stackTrace; ///< 异常抛出时的调用栈（原始帧，解析推迟到输出时）
    };
} // namespace AsynGyanis::Base
