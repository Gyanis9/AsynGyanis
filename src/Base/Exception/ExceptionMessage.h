/**
 * @file ExceptionMessage.h
 * @brief 异常消息的统一文本生成 —— 供异常体系的两个分支共用
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 异常文本的格式（"[异常] 消息 [文件:行 in 函数]"）是被用例钉住的契约
 *          （见 tests/Base/Exception/TestException.cpp），而异常体系的两条继承链
 *          （std::runtime_error 的 Exception 与 std::logic_error 的 LogicException）无法共享基类，
 *          因此把这段文本生成上收成自由函数，避免格式串被抄成多份后各自漂移。
 *
 * @note 只对本模块内部可见：放在 Detail 命名空间下，不作为对外接口的一部分。
 */
#pragma once

#include <format>
#include <source_location>
#include <string>

namespace AsynGyanis::Base::Detail
{
    /**
     * @brief 把异常消息与抛出位置拼成统一格式的文本
     * @param message 异常描述消息
     * @param sourceLocation 异常抛出位置
     * @return std::string 形如 "[异常] 消息 [文件:行 in 函数]" 的完整文本
     */
    [[nodiscard]] inline std::string formatExceptionMessage(const std::string &message,
                                                            const std::source_location &sourceLocation)
    {
        return std::format("[异常] {} [{}:{} in {}]",
                           message,
                           sourceLocation.file_name(),
                           sourceLocation.line(),
                           sourceLocation.function_name());
    }
} // namespace AsynGyanis::Base::Detail
