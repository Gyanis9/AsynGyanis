#include "Platform/FileSystem/FileWatcher.h"

#if ASYN_PLATFORM_WIN32
#include "Platform/FileSystem/Win32FileWatcher.h"
#else
#include "Platform/FileSystem/InotifyFileWatcher.h"
#endif

namespace AsynGyanis::Platform
{
    std::unique_ptr<FileWatcher> FileWatcher::create()
    {
#if ASYN_PLATFORM_WIN32
        return std::make_unique<Win32FileWatcher>();
#else
        return std::make_unique<InotifyFileWatcher>();
#endif
    }
} // namespace AsynGyanis::Platform
