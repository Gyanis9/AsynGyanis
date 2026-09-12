/**
 * @file CoreException.h
 * @brief Core 模块异常基类
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/Exception.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Core
{
    /**
     * @brief Core 模块异常基类
     *
     * @details 本模块抛出的**运行期故障**（TLS 上下文/会话创建、握手、TLS 读写失败）一律派生
     *          自本类，调用方用一条 `catch (const Base::Exception &)` 即可兜住整个框架的运行期
     *          错误。按调用方的处置方式划分，本模块当前只有一种处置——记日志、放弃本次 TLS 操作
     *          并关闭连接、不重试，故不再细分；将来出现「可退避重试」这类处置时再加子类。
     *
     * @note 调用方用错接口（参数非法等）抛的是 `Base::InvalidArgumentException`，
     *       它派生自 `std::invalid_argument` 而**不在**本类的继承链上：那类问题是用法的 bug，
     *       不该被「可恢复的运行期故障」这一捕获面吞掉。要一次网住两者请捕获
     *       `std::exception` 或分别捕获。
     * @note 消息一律中文，且写清「原因 + 替代做法」；`what()` 的文本格式与 `Base::Exception`
     *       一致（`[异常] 消息 [文件:行 in 函数]`），因为两者共用同一份格式化实现。
     */
    class CoreException : public Base::Exception
    {
    public:
        /**
         * @brief 构造 Core 模块异常
         * @param message 异常描述消息（中文，写清原因与替代做法）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit CoreException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Core
