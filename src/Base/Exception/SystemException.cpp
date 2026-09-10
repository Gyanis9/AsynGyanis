/**
 * @file SystemException.cpp
 * @brief 系统调用失败异常的实现
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/SystemException.h"

#include <cerrno>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    SystemException::SystemException(const std::string &context, const std::source_location &sourceLocation) :
        SystemException(context, std::error_code(errno, std::system_category()), sourceLocation)
    {
        // 注意：errno 在委托构造函数参数求值期间被读取，
        // 与 context 参数的 string 构造存在竞争。
        // 调用者应优先使用显式传递 error_code 的双参数构造函数。
    }

    SystemException::SystemException(const std::string &context, const std::error_code errorCode, const std::source_location &sourceLocation) :
        Exception(context + ": [" + std::to_string(errorCode.value()) + "] " + errorCode.message(), sourceLocation)
        , m_errorCode(errorCode)
    {
    }

    const std::error_code &SystemException::errorCode() const noexcept
    {
        return m_errorCode;
    }

    int SystemException::nativeError() const noexcept
    {
        return m_errorCode.value();
    }
} // namespace AsynGyanis::Base
