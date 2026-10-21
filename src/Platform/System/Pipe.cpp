#include "Platform/System/Pipe.h"

#include "Platform/System/PlatformError.h"

#include <utility>

#if !ASYN_PLATFORM_WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
#if !ASYN_PLATFORM_WIN32
    namespace
    {
        /// 两端先都建成 CLOEXEC，要交给子进程的那一端再单独清掉
        constexpr int kPipeCreationFlags = O_CLOEXEC;
    } // namespace
#endif

    Pipe::~Pipe()
    {
        closeReadEnd();
        closeWriteEnd();
    }

    Pipe::Pipe(Pipe &&other) noexcept : m_readEnd(std::exchange(other.m_readEnd, FileDescriptor::kInvalid)), m_writeEnd(std::exchange(other.m_writeEnd, FileDescriptor::kInvalid))
    {
    }

    Pipe &Pipe::operator=(Pipe &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        closeReadEnd();
        closeWriteEnd();
        m_readEnd  = std::exchange(other.m_readEnd, FileDescriptor::kInvalid);
        m_writeEnd = std::exchange(other.m_writeEnd, FileDescriptor::kInvalid);
        return *this;
    }

    std::optional<Pipe> Pipe::create(const bool writeEndSurvivesExec) noexcept
    {
#if ASYN_PLATFORM_WIN32
        // 唯一消费方是 systemd 看门狗，而它不存在于 Windows；伪造一条不能走 FileDescriptor
        // （Windows 侧只认 socket）的管道，只会把不一致留给下一个人
        static_cast<void>(writeEndSurvivesExec);
        PlatformError::setLastErrorCode(PlatformError::kNotSupported);
        return std::nullopt;
#else
        int descriptors[2]{FileDescriptor::kInvalid, FileDescriptor::kInvalid};
        if (::pipe2(descriptors, kPipeCreationFlags) != 0)
        {
            return std::nullopt;
        }
        if (writeEndSurvivesExec && ::fcntl(descriptors[1], F_SETFD, 0) != 0)
        {
            // 写端清不掉 CLOEXEC 就等于交不到子进程手里：两端都不留，别让调用方拿到半条通道
            const int failure = errno;
            static_cast<void>(::close(descriptors[0]));
            static_cast<void>(::close(descriptors[1]));
            PlatformError::setLastErrorCode(failure);
            return std::nullopt;
        }
        Pipe pipe;
        pipe.m_readEnd  = descriptors[0];
        pipe.m_writeEnd = descriptors[1];
        return pipe;
#endif
    }

    bool Pipe::isValid() const noexcept
    {
        return FileDescriptor::isValid(m_readEnd) && FileDescriptor::isValid(m_writeEnd);
    }

    int Pipe::readEnd() const noexcept
    {
        return m_readEnd;
    }

    int Pipe::writeEnd() const noexcept
    {
        return m_writeEnd;
    }

    int Pipe::releaseReadEnd() noexcept
    {
        return std::exchange(m_readEnd, FileDescriptor::kInvalid);
    }

    int Pipe::releaseWriteEnd() noexcept
    {
        return std::exchange(m_writeEnd, FileDescriptor::kInvalid);
    }

    void Pipe::closeReadEnd() noexcept
    {
        if (FileDescriptor::isValid(m_readEnd))
        {
            static_cast<void>(FileDescriptor::close(m_readEnd));
            m_readEnd = FileDescriptor::kInvalid;
        }
    }

    void Pipe::closeWriteEnd() noexcept
    {
        if (FileDescriptor::isValid(m_writeEnd))
        {
            static_cast<void>(FileDescriptor::close(m_writeEnd));
            m_writeEnd = FileDescriptor::kInvalid;
        }
    }
} // namespace AsynGyanis::Platform
