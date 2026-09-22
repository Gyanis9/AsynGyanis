#include "Base/Exception/SystemException.h"
#include "Platform/System/PlatformError.h"

#include <string>
#include <system_error>
#include <utility>

namespace AsynGyanis::Base
{
    SystemException::SystemException(const std::string &context, const std::source_location &sourceLocation) :
        SystemException(context, lastErrnoErrorCode(), sourceLocation)
    {
        // errno 在委托构造的参数求值期读取，无法在本体内补救读取时序，
        // 只能提示调用方优先用显式 error_code 的重载
    }

    SystemException::SystemException(const std::string &context, std::error_code errorCode, const std::source_location &sourceLocation) :
        Exception(context + ": [" + std::to_string(errorCode.value()) + "] " + errorCode.message(), sourceLocation)
        , m_errorCode(errorCode)
    {
    }

    std::error_code SystemException::lastErrnoErrorCode() noexcept
    {
        // 取的是 errno，类别就必须按 errno 解释：Windows 上 std::system_category() 把数值当
        // **Win32 错误码**查表，而 kernel32 / winsock 的失败根本不写 errno——「errno 的值 +
        // system_category」会给出与本次失败无关的描述（实测发送失败被报成「[112] 磁盘空间不足」）。
        // generic_category 在两侧都按 errno 语义解释，数值与描述才自洽。
        // 因此 kernel32 / socket 调用失败必须走显式 error_code 重载，由调用点给出对应空间的码
        return std::error_code{Platform::PlatformError::lastErrorCode(), std::generic_category()};
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
