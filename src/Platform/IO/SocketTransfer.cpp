#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <limits>

#if !ASYN_PLATFORM_WIN32
#include <sys/sendfile.h>
#endif

namespace AsynGyanis::Platform
{
    ssize_t Socket::writeVectored(const int descriptor, const WriteBuffer *const buffers, const std::size_t bufferCount) noexcept
    {
        // 段数与长度先按平台上限校验：这两处超限都必须当场失败。
        // 段数超限静默拆分会让「一次系统调用」的收益悄悄消失；单段长度被截断更糟——
        // 线上字节流会缺一截，而返回值看起来一切正常
        if (!FileDescriptor::isValid(descriptor) || buffers == nullptr || bufferCount == 0 || bufferCount > kMaximumVectorCount)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        // 「有长度却没有数据」这一非法形状在两个平台上都得当场拒掉：交给 Windows 是 WSASend 报错，
        // 交给 POSIX 的 sendmsg 则只得到一个依赖地址取值的 EFAULT——判据放在平台分支之前，
        // 才不会同一份参数在一侧返回 kInvalidArgument、另一侧返回系统错误
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            if (!buffers[index].data && buffers[index].length != 0)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
        }

#if ASYN_PLATFORM_WIN32
        WSABUF windowsBuffers[kMaximumVectorCount]{};
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            // 长度形参是 ULONG：超限直接失败，静默截断会让线上字节流缺一截而返回值看着正常
            if (buffers[index].length > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            windowsBuffers[index].buf = static_cast<char *>(const_cast<void *>(buffers[index].data));
            windowsBuffers[index].len = static_cast<ULONG>(buffers[index].length);
        }

        DWORD sentLength = 0;
        if (::WSASend(static_cast<SOCKET>(descriptor), windowsBuffers, static_cast<DWORD>(bufferCount), &sentLength, 0, nullptr, nullptr) == SOCKET_ERROR)
        {
            return -1;
        }
        return static_cast<ssize_t>(sentLength);
#else
        iovec vectors[kMaximumVectorCount]{};
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            vectors[index].iov_base = const_cast<void *>(buffers[index].data);
            vectors[index].iov_len  = buffers[index].length;
        }

        // 用 sendmsg 而不是 writev：只有它带 MSG_NOSIGNAL，对端已关闭时不会把进程打死
        msghdr message{};
        message.msg_iov    = vectors;
        message.msg_iovlen = bufferCount;
        return ::sendmsg(descriptor, &message, MSG_NOSIGNAL);
#endif
    }

#if !ASYN_PLATFORM_WIN32
    ssize_t Socket::sendFileChunk(const int socketDescriptor, const int fileDescriptor, const std::uint64_t offset, const std::size_t length) noexcept
    {
        if (!FileDescriptor::isValid(socketDescriptor) || !FileDescriptor::isValid(fileDescriptor) || length == 0)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

        // 超限会让内核直接 EINVAL 而不是部分写入，先钳制到安全上限；调用方按返回值推进偏移
        const std::size_t clampedLength = length > kMaximumSendFileChunk ? kMaximumSendFileChunk : length;

        // 显式传偏移指针：传空指针会让 sendfile 改用并推进描述符自身的文件偏移，而同一个
        // 文件可能被多条响应并发发送，动共享偏移会让它们读到彼此的位置
        off_t sendOffset = static_cast<off_t>(offset);
        return ::sendfile(socketDescriptor, fileDescriptor, &sendOffset, clampedLength);
    }
#endif
} // namespace AsynGyanis::Platform
