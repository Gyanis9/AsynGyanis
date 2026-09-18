#include "Base/Exception/StackTrace.h"

#if defined(ASYN_HAS_STACKTRACE)

namespace AsynGyanis::Base
{
    CapturedStackTrace captureStackTrace(const std::size_t framesToSkip, const std::size_t maximumDepth)
    {
        // +1 跳过本函数自身的帧：调用方传 0 时首个保留帧就是它的调用者
        return std::stacktrace::current(framesToSkip + 1, maximumDepth);
    }

    std::string formatStackTrace(const CapturedStackTrace &stackTrace)
    {
        // 空栈直接返回：避免为「没有栈」的场合也去初始化调试信息子系统
        return stackTrace.empty() ? std::string{} : std::to_string(stackTrace);
    }
} // namespace AsynGyanis::Base

#endif
