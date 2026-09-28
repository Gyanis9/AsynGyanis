#include "Base/Exception/ExceptionPayload.h"

namespace AsynGyanis::Base::Detail
{
    ExceptionPayload::ExceptionPayload(const std::source_location &sourceLocation, const std::size_t framesToSkip) :
        // 帧数口径与三条链原先各自捕获时逐帧对齐：captureStackTrace 内部已隐去自身一帧，
        // 默认的 2 再隐去「本载荷的构造函数」与「调用本初始化式的异常构造函数」，
        // 于是首个保留帧仍是抛出点。派生类构造函数多出的帧会出现在最前面，属可接受的噪声——
        // 换来的是不必让每个派生类各捕获一次（口径与原 Exception 完全相同）
        m_location(sourceLocation), m_stackTrace(captureStackTrace(framesToSkip))
    {
    }

    const std::source_location &ExceptionPayload::location() const noexcept
    {
        return m_location;
    }

    const CapturedStackTrace &ExceptionPayload::stackTrace() const noexcept
    {
        return m_stackTrace;
    }

} // namespace AsynGyanis::Base::Detail
